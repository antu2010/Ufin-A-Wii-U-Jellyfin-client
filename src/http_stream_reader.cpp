#include "http_stream_reader.h"

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <algorithm>
#include <coreinit/debug.h>

static bool resolveHost(const std::string& host, struct in_addr* out) {
    if (inet_pton(AF_INET, host.c_str(), out) == 1) return true;
    struct hostent* he = gethostbyname(host.c_str());
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) return false;
    memcpy(out, he->h_addr_list[0], sizeof(struct in_addr));
    return true;
}

static std::string toLower(const std::string& s) {
    std::string out = s;
    for (auto& c : out) c = (char)tolower((unsigned char)c);
    return out;
}

// wut doesn't expose SO_RCVTIMEO (confirmed earlier -- it fails to
// compile), so timeouts are done manually via select() instead: wait for
// the socket to become readable before ever calling the blocking recv().
// Without this, a slow-to-respond server (plausible here -- Jellyfin has
// to spin up FFmpeg and start actually encoding before the first
// response bytes exist) leaves us blocked in recv() indefinitely, which
// is what appears to be crashing Cemu and freezing real hardware. This
// turns that into a clean, visible timeout error instead.
static bool waitReadable(int sock, int timeoutSeconds) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(sock, &readSet);

    struct timeval tv;
    tv.tv_sec = timeoutSeconds;
    tv.tv_usec = 0;

    int result = select(sock + 1, &readSet, nullptr, nullptr, &tv);
    return result > 0;
}

HttpStreamReader::HttpStreamReader(std::string host, int port, std::string path)
    : host_(std::move(host)), port_(port), path_(std::move(path)) {}

HttpStreamReader::~HttpStreamReader() {
    if (sock_ >= 0) close(sock_);
}

bool HttpStreamReader::fillSockBuf() {
    if (sock_buf_pos_ < sock_buf_len_) return true; // still have unconsumed bytes

    // 15 seconds is generous enough to cover a slow transcode start-up,
    // but still finite -- if this is ever too short/long in practice,
    // this constant is the first thing to tune.
    if (!waitReadable(sock_, 15)) {
        last_error_ = "timed out waiting for data from server (15s)";
        return false;
    }

    sock_buf_.resize(4096);
    ssize_t n = recv(sock_, sock_buf_.data(), sock_buf_.size(), 0);
    if (n <= 0) return false; // 0 = peer closed, <0 = error -- both mean "no more data" here
    sock_buf_len_ = (size_t)n;
    sock_buf_pos_ = 0;
    return true;
}

bool HttpStreamReader::readRawBytes(uint8_t* buf, int n) {
    int got = 0;
    while (got < n) {
        if (!fillSockBuf()) return false;
        int avail = (int)(sock_buf_len_ - sock_buf_pos_);
        int toCopy = std::min(avail, n - got);
        memcpy(buf + got, sock_buf_.data() + sock_buf_pos_, toCopy);
        sock_buf_pos_ += toCopy;
        got += toCopy;
    }
    return true;
}

bool HttpStreamReader::readLine(std::string& outLine) {
    outLine.clear();
    while (true) {
        if (!fillSockBuf()) return false;
        uint8_t byte = sock_buf_[sock_buf_pos_++];
        if (byte == '\n') {
            if (!outLine.empty() && outLine.back() == '\r') outLine.pop_back();
            return true;
        }
        outLine.push_back((char)byte);
    }
}

bool HttpStreamReader::open() {
    OSReport("Ufin: HttpStreamReader::open host=%s port=%d\n", host_.c_str(), port_);
    {
        // The request path, with the access token blanked out -- shows
        // StartTimeTicks / PlaySessionId when diagnosing seeks.
        std::string shown = path_;
        size_t k = shown.find("ApiKey=");
        if (k != std::string::npos) {
            size_t end = shown.find('&', k);
            shown.replace(k + 7, (end == std::string::npos ? shown.size() : end) - (k + 7), "***");
        }
        OSReport("Ufin: GET %s\n", shown.c_str());
    }

    struct in_addr addr;
    if (!resolveHost(host_, &addr)) {
        last_error_ = "could not resolve host: " + host_;
        OSReport("Ufin: resolveHost failed for %s\n", host_.c_str());
        return false;
    }
    OSReport("Ufin: resolveHost ok\n");

    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0) {
        last_error_ = "socket() failed";
        OSReport("Ufin: socket() failed\n");
        return false;
    }
    OSReport("Ufin: socket() ok, fd=%d\n", sock_);

    struct sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons((uint16_t)port_);
    server.sin_addr = addr;

    if (connect(sock_, (struct sockaddr*)&server, sizeof(server)) < 0) {
        last_error_ = "connect() failed to " + host_;
        OSReport("Ufin: connect() failed\n");
        return false;
    }
    OSReport("Ufin: connect() ok\n");

    // Deliberately NOT sending "Connection: close" here -- the whole
    // point of this class is to keep the connection open and keep
    // reading as Jellyfin generates more of the transcode.
    char request[4096];
    snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Accept: */*\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        path_.c_str(), host_.c_str());

    std::string req(request);
    OSReport("Ufin: sending request, %zu bytes\n", req.size());
    size_t sent = 0;
    while (sent < req.size()) {
        ssize_t n = send(sock_, req.c_str() + sent, req.size() - sent, 0);
        if (n <= 0) {
            last_error_ = "send() failed";
            OSReport("Ufin: send() failed at offset %zu\n", sent);
            return false;
        }
        sent += (size_t)n;
    }
    OSReport("Ufin: request sent ok\n");

    std::string statusLine;
    if (!readLine(statusLine)) {
        last_error_ = "failed reading status line";
        OSReport("Ufin: readLine(statusLine) failed: %s\n", last_error_.c_str());
        return false;
    }
    OSReport("Ufin: status line: %s\n", statusLine.c_str());

    size_t sp1 = statusLine.find(' ');
    int statusCode = (sp1 != std::string::npos) ? atoi(statusLine.c_str() + sp1 + 1) : 0;
    if (statusCode != 200) {
        // Jellyfin usually says *why* in the body (e.g. the FFmpeg
        // failure), so grab the start of it for the log and the error
        // screen instead of throwing it away.
        std::string line;
        while (readLine(line) && !line.empty()) {}
        std::string body;
        if (sock_buf_pos_ < sock_buf_len_ || waitReadable(sock_, 2)) {
            if (fillSockBuf()) {
                size_t avail = sock_buf_len_ - sock_buf_pos_;
                body.assign((const char*)sock_buf_.data() + sock_buf_pos_,
                            std::min<size_t>(avail, 300));
            }
        }
        for (char& c : body) {
            if ((unsigned char)c < 0x20) c = ' ';
        }
        OSReport("Ufin: unexpected status code %d, body: %s\n", statusCode, body.c_str());
        last_error_ = "server returned status " + std::to_string(statusCode);
        if (!body.empty()) last_error_ += ": " + body.substr(0, 120);
        return false;
    }

    while (true) {
        std::string headerLine;
        if (!readLine(headerLine)) {
            last_error_ = "connection closed while reading headers";
            OSReport("Ufin: connection closed while reading headers\n");
            return false;
        }
        if (headerLine.empty()) break; // blank line = end of headers

        std::string lower = toLower(headerLine);
        if (lower.rfind("transfer-encoding:", 0) == 0 && lower.find("chunked") != std::string::npos) {
            is_chunked_ = true;
        }
    }
    OSReport("Ufin: headers parsed ok, is_chunked=%d\n", (int)is_chunked_);

    // If Jellyfin ever serves this non-chunked (e.g. a future version
    // that supports direct play here), read()/readRawBytes() below fall
    // back to "raw bytes straight from the socket until it closes",
    // which works fine for that case too without any special-casing.
    need_chunk_header_ = is_chunked_;
    return true;
}

int HttpStreamReader::read(uint8_t* buf, int len) {
    if (stream_ended_ || len <= 0) return 0;

    if (!is_chunked_) {
        // Not chunked: just hand back raw socket bytes directly.
        if (!fillSockBuf()) return 0; // treat closed/error as clean EOF here
        int avail = (int)(sock_buf_len_ - sock_buf_pos_);
        int toCopy = std::min(avail, len);
        memcpy(buf, sock_buf_.data() + sock_buf_pos_, toCopy);
        sock_buf_pos_ += toCopy;
        return toCopy;
    }

    int totalRead = 0;
    while (totalRead < len) {
        if (need_chunk_header_) {
            std::string sizeLine;
            if (!readLine(sizeLine)) {
                last_error_ = "failed reading chunk size line";
                return totalRead > 0 ? totalRead : -1;
            }
            size_t semi = sizeLine.find(';'); // chunk extensions, if any
            if (semi != std::string::npos) sizeLine = sizeLine.substr(0, semi);

            long chunkSize = strtol(sizeLine.c_str(), nullptr, 16);
            if (chunkSize <= 0) {
                // Terminating zero-size chunk. There may be trailer
                // headers before the final blank line -- consume and
                // discard them.
                std::string trailerLine;
                while (readLine(trailerLine) && !trailerLine.empty()) {}
                stream_ended_ = true;
                break;
            }
            chunk_remaining_ = chunkSize;
            need_chunk_header_ = false;
        }

        int toRead = (int)std::min<int64_t>(chunk_remaining_, len - totalRead);
        if (toRead > 0) {
            if (!readRawBytes(buf + totalRead, toRead)) {
                last_error_ = "failed reading chunk data";
                return totalRead > 0 ? totalRead : -1;
            }
            totalRead += toRead;
            chunk_remaining_ -= toRead;
        }

        if (chunk_remaining_ == 0) {
            uint8_t crlf[2];
            if (!readRawBytes(crlf, 2)) {
                last_error_ = "failed reading chunk trailing CRLF";
                return totalRead > 0 ? totalRead : -1;
            }
            need_chunk_header_ = true;
        }
    }

    return totalRead;
}
