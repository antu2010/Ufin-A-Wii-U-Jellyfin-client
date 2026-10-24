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
#include <fcntl.h>
#include <cerrno>
#include <chrono>
#include <coreinit/debug.h>

static std::string toLower(const std::string& s) {
    std::string out = s;
    for (auto& c : out) c = (char)tolower((unsigned char)c);
    return out;
}

HttpStreamReader::HttpStreamReader(std::string host, int port, std::string path)
    : host_(std::move(host)), port_(port), path_(std::move(path)) {}

HttpStreamReader::~HttpStreamReader() { conn_.close(); }

bool HttpStreamReader::fillSockBuf() {
    if (sock_buf_pos_ < sock_buf_len_) return true; // still have unconsumed bytes

    // 15 seconds (DEFAULT_TIMEOUT_SECONDS) is generous enough to cover a slow
    // transcode start-up, but still finite -- if this is ever too short/long
    // in practice, this constant is the first thing to tune. The very first
    // wait after the request can be given longer (setStartupTimeoutSeconds)
    // and be cancelled (setAbortCheck); see open(). (wut has no SO_RCVTIMEO,
    // so the wait is a select() inside net::Conn -- over TLS too.)
    sock_buf_.resize(4096);
    const std::function<bool()>* abort = (wait_abortable_ && abort_check_) ? &abort_check_ : nullptr;
    const int n = conn_.recv(sock_buf_.data(), (int)sock_buf_.size(), wait_seconds_, abort);
    if (n == net::Conn::Aborted) {
        cancelled_ = true;
        fill_error_ = "cancelled";
        last_error_ = "cancelled while waiting for the server";
        return false;
    }
    if (n == net::Conn::TimedOut) {
        fill_error_ = "timed out after " + std::to_string(wait_seconds_) + "s";
        last_error_ = "timed out waiting for data from server (" + std::to_string(wait_seconds_) + "s)";
        return false;
    }
    if (n <= 0) {
        // 0 = peer closed, <0 = error -- both mean "no more data" to the
        // caller, but they are different stories in the log.
        fill_error_ = n == 0 ? "connection closed by the server"
                             : (conn_.secure() ? std::string("TLS read failed") : "recv() failed (errno " + std::to_string(errno) + ")");
        return false;
    }
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

    {
        std::string openError;
        if (!conn_.open(host_, port_, 15, openError)) {
            last_error_ = openError.rfind("could not resolve", 0) == 0 ? "could not resolve host: " + host_
                          : openError.rfind("connect() failed", 0) == 0 ? "connect() failed or timed out connecting to " + host_
                                                                         : openError;
            OSReport("Ufin: connect failed: %s\n", last_error_.c_str());
            return false;
        }
        OSReport("Ufin: connect() ok%s\n", conn_.secure() ? " (TLS)" : "");
    }

    // Deliberately NOT sending "Connection: close" here -- the whole
    // point of this class is to keep the connection open and keep
    // reading as Jellyfin generates more of the transcode.
    const std::string hostHeader = (conn_.secure() && port_ != 443) ? host_ + ":" + std::to_string(port_) : host_;
    char request[4096];
    snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Accept: */*\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        path_.c_str(), hostHeader.c_str());

    std::string req(request);
    OSReport("Ufin: sending request, %zu bytes\n", req.size());
    if (!conn_.sendAll(req.data(), req.size())) {
        last_error_ = "send() failed";
        OSReport("Ufin: send() failed\n");
        return false;
    }
    OSReport("Ufin: request sent ok\n");

    // The first response bytes may take a while: Jellyfin does its
    // preparation (starting FFmpeg and, for a burned-in subtitle track,
    // extracting that track and the fonts from the media file) *before* it
    // sends the status line. Only this wait is stretched / cancellable;
    // everything after it uses the normal 15 s again.
    cancelled_ = false;
    fill_error_.clear();
    wait_seconds_ = startup_timeout_seconds_;
    wait_abortable_ = (bool)abort_check_;
    const auto waitStart = std::chrono::steady_clock::now();
    std::string statusLine;
    const bool gotStatusLine = readLine(statusLine);
    const double waitedSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - waitStart).count();
    wait_seconds_ = DEFAULT_TIMEOUT_SECONDS;
    wait_abortable_ = false;
    if (!gotStatusLine) {
        if (cancelled_) {
            last_error_ = "cancelled while waiting for the server to start the stream";
        } else {
            // Keep the reason, not only the stage: a 15 s (or longer) silence
            // from a server that is still busy and a connection the server
            // dropped are different problems and used to read the same.
            last_error_ = "failed reading status line";
            if (!fill_error_.empty()) last_error_ += ": " + fill_error_;
        }
        OSReport("Ufin: readLine(statusLine) failed after %.1fs: %s\n", waitedSeconds, last_error_.c_str());
        return false;
    }
    if (waitedSeconds >= 1.0) OSReport("Ufin: server started answering after %.1fs\n", waitedSeconds);
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
        if (sock_buf_pos_ < sock_buf_len_ || conn_.waitReadable(2)) {
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

    std::string contentType;
    while (true) {
        std::string headerLine;
        if (!readLine(headerLine)) {
            last_error_ = "connection closed while reading headers";
            if (!fill_error_.empty()) last_error_ += ": " + fill_error_;
            OSReport("Ufin: %s\n", last_error_.c_str());
            return false;
        }
        if (headerLine.empty()) break; // blank line = end of headers

        std::string lower = toLower(headerLine);
        if (lower.rfind("transfer-encoding:", 0) == 0 && lower.find("chunked") != std::string::npos) {
            is_chunked_ = true;
        }
        if (lower.rfind("content-type:", 0) == 0) {
            size_t v = headerLine.find(':') + 1;
            while (v < headerLine.size() && headerLine[v] == ' ') v++;
            contentType = headerLine.substr(v, 80); // for the log only
        }
    }
    OSReport("Ufin: headers parsed ok, is_chunked=%d content-type=%s\n", (int)is_chunked_,
             contentType.empty() ? "(none)" : contentType.c_str());

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
                if (!fill_error_.empty()) last_error_ += ": " + fill_error_;
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
                if (!fill_error_.empty()) last_error_ += ": " + fill_error_;
                return totalRead > 0 ? totalRead : -1;
            }
            totalRead += toRead;
            chunk_remaining_ -= toRead;
        }

        if (chunk_remaining_ == 0) {
            uint8_t crlf[2];
            if (!readRawBytes(crlf, 2)) {
                last_error_ = "failed reading chunk trailing CRLF";
                if (!fill_error_.empty()) last_error_ += ": " + fill_error_;
                return totalRead > 0 ? totalRead : -1;
            }
            need_chunk_header_ = true;
        }
    }

    return totalRead;
}
