#include "https_client.h"

#include "http_client.h"

#include <cctype>
#include <chrono>
#include "net_time.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(UFIN_TLS_MBEDTLS)
#include "dns_client.h"
#include "tls_conn.h"
#include <memory>
#elif defined(UFIN_TLS_NSSL)
#include <nsysnet/nssl.h>
#include <mutex>
#endif

namespace https {

static void (*g_trace)(const char*) = nullptr;
void setTrace(void (*hook)(const char*)) { g_trace = hook; }
static void trace(const char* m) { if (g_trace) g_trace(m); }

static std::string lowerCopy(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// ---- parsing ---------------------------------------------------------------

// Decodes a chunked body. false if it does not end with its final (0-size) chunk.
static bool dechunk(const std::string& in, std::string& out) {
    size_t pos = 0;
    out.clear();
    while (true) {
        size_t eol = in.find("\r\n", pos);
        if (eol == std::string::npos) return false;
        std::string sizeLine = in.substr(pos, eol - pos);
        size_t semi = sizeLine.find(';'); // chunk extensions
        if (semi != std::string::npos) sizeLine.resize(semi);
        if (sizeLine.empty()) return false;
        char* end = nullptr;
        unsigned long size = strtoul(sizeLine.c_str(), &end, 16);
        if (end == sizeLine.c_str()) return false;
        pos = eol + 2;
        if (size == 0) return true;
        if (pos + size > in.size()) return false;
        out.append(in, pos, size);
        pos += size;
        if (in.compare(pos, 2, "\r\n") != 0) return false;
        pos += 2;
    }
}

bool parseResponse(const std::string& raw, Response& out) {
    out = Response();
    const size_t headEnd = raw.find("\r\n\r\n");
    if (headEnd == std::string::npos) {
        out.error = "incomplete response headers";
        return false;
    }
    const std::string head = raw.substr(0, headEnd);
    if (head.compare(0, 5, "HTTP/") != 0) {
        out.error = "not an HTTP response";
        return false;
    }
    size_t sp = head.find(' ');
    if (sp == std::string::npos) {
        out.error = "bad status line";
        return false;
    }
    out.status = atoi(head.c_str() + sp + 1);
    if (out.status < 100 || out.status > 599) {
        out.error = "bad status line";
        return false;
    }

    bool chunked = false;
    long long contentLength = -1;
    size_t lineStart = head.find("\r\n");
    while (lineStart != std::string::npos) {
        lineStart += 2;
        size_t lineEnd = head.find("\r\n", lineStart);
        std::string line = head.substr(lineStart, lineEnd == std::string::npos ? std::string::npos : lineEnd - lineStart);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = lowerCopy(line.substr(0, colon));
            size_t v = colon + 1;
            while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) v++;
            std::string value = line.substr(v);
            if (name == "location") out.location = value;
            else if (name == "transfer-encoding" && lowerCopy(value).find("chunked") != std::string::npos) chunked = true;
            else if (name == "content-length") contentLength = atoll(value.c_str());
        }
        lineStart = lineEnd;
    }

    const std::string body = raw.substr(headEnd + 4);
    if (chunked) {
        if (!dechunk(body, out.body)) {
            out.body.clear();
            out.error = "response body ended early (chunked)";
            return false;
        }
    } else {
        if (contentLength >= 0 && (long long)body.size() < contentLength) {
            out.error = "response body ended early";
            return false;
        }
        out.body = contentLength >= 0 ? body.substr(0, (size_t)contentLength) : body;
    }
    out.ok = true;
    return true;
}

bool splitUrl(const std::string& url, std::string& host, int& port, std::string& path) {
    static const char kScheme[] = "https://";
    if (url.compare(0, sizeof(kScheme) - 1, kScheme) != 0) return false;
    size_t start = sizeof(kScheme) - 1;
    size_t slash = url.find('/', start);
    std::string authority = url.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    path = slash == std::string::npos ? "/" : url.substr(slash);
    port = 443;
    size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        port = atoi(authority.c_str() + colon + 1);
        authority.resize(colon);
        if (port <= 0 || port > 65535) return false;
    }
    if (authority.empty()) return false;
    host = authority;
    return true;
}

// ---- the request -----------------------------------------------------------

using Deadline = int64_t; // nettime::nowMs() value

static Response getOnce(const Opener& open, const std::string& host, int port, const std::string& path,
                        const std::string& extraHeaders, const Options& options, Deadline deadline) {
    Response fail;
    Connection conn;
    std::string err;
    const int connectSeconds = options.timeoutSeconds > 0 ? options.timeoutSeconds : 8;
    if (!open(host, port, connectSeconds, conn, err)) {
        fail.error = err.empty() ? "could not connect" : err;
        return fail;
    }
    struct Closer {
        Connection& c;
        ~Closer() { if (c.close) c.close(); }
    } closer{conn};

    const std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\n" + extraHeaders +
                                "Accept-Encoding: identity\r\nConnection: close\r\n\r\n";
    trace("sending the request");
    if (!conn.write || !conn.write(request.data(), (int)request.size())) {
        fail.error = "could not send the request";
        return fail;
    }
    trace("request sent, waiting for the answer");
    bool firstBytes = true;

    std::string raw;
    std::vector<char> buf(16384); // heap: this runs on a background thread with a small stack
    while (true) {
        if (options.cancelled && options.cancelled()) {
            fail.error = "cancelled";
            return fail;
        }
        if (nettime::nowMs() > deadline) {
            fail.error = "timed out";
            return fail;
        }
        const int n = conn.read(buf.data(), (int)buf.size());
        if (n > 0) {
            if (firstBytes) { trace("the answer is arriving"); firstBytes = false; }
            raw.append(buf.data(), (size_t)n);
            if (raw.size() > options.maxBytes + 16384) {
                fail.error = "response too large";
                return fail;
            }
        } else if (n == 0) {
            trace("the server closed the connection");
            break; // the server closed: that is the end of a "Connection: close" response
        } else if (n == -2) {
            nettime::sleepMs(20);
        } else {
            // A TLS peer that just drops the connection after the last record looks like an
            // error here; if a complete response is already in hand, take it.
            Response done;
            if (parseResponse(raw, done)) return done;
            fail.error = "connection error while reading";
            return fail;
        }
    }

    Response resp;
    if (!parseResponse(raw, resp)) {
        fail.status = resp.status;
        fail.error = resp.error.empty() ? "could not read the response" : resp.error;
        return fail;
    }
    if (resp.body.size() > options.maxBytes) {
        fail.error = "response too large";
        return fail;
    }
    return resp;
}

Response getWith(const Opener& open, const std::string& host, const std::string& path,
                 const std::string& extraHeaders, const Options& options) {
    const int total = options.timeoutSeconds > 0 ? options.timeoutSeconds : 8;
    const Deadline deadline = nettime::nowMs() + (int64_t)total * 1000;
    std::string curHost = host, curPath = path;
    int curPort = options.port;
    for (int hop = 0;; hop++) {
        Response r = getOnce(open, curHost, curPort, curPath, extraHeaders, options, deadline);
        const bool redirect = r.ok && (r.status == 301 || r.status == 302 || r.status == 307 || r.status == 308) && !r.location.empty();
        if (!redirect) return r;
        if (hop >= options.maxRedirects) {
            Response f;
            f.error = "too many redirects";
            return f;
        }
        std::string h, p;
        int port = 443;
        if (r.location[0] == '/') {
            h = curHost; p = r.location; port = curPort;
        } else if (!splitUrl(r.location, h, port, p)) {
            Response f;
            f.error = "redirect to something other than https";
            return f;
        }
        curHost = h; curPath = p; curPort = port;
    }
}

Response get(const std::string& host, const std::string& path, const std::string& extraHeaders, const Options& options) {
    return getWith(systemOpener(), host, path, extraHeaders, options);
}

// ---- system TLS (the console's NSSL) ---------------------------------------

#if defined(UFIN_TLS_MBEDTLS)

// ---- our own TLS (Mbed TLS, vendored in src/vendor/mbedtls; the session itself is tls_conn.h) -------------------
//
// The console's system TLS (NSSL) cannot be relied on: it is not emulated by Cemu at all and, on a real
// console, its old protocol / cipher set is refused by modern servers. So this is a plain TLS 1.2 client:
// our own DNS lookup (dns_client.h), a bounded TCP connect (http_client.h), the handshake over that socket,
// and the server certificate checked against the roots in tls_roots.cpp.

void setExtraRootsForTests(const std::string& pem) { tlsconn::setExtraRootsForTests(pem); }

Opener systemOpener() {
    return [](const std::string& host, int port, int timeoutSeconds, Connection& out, std::string& error) {
        trace("DNS");
        std::string ip;
        std::string dnsError;
        if (!dns::resolveA(host, dns::defaultOptions(), ip, dnsError)) {
            if (dnsError == "cancelled") { error = dnsError; return false; }
            // The public resolvers did not answer (blocked on this network, or UDP refused): use the
            // system's own lookup, which the console does well. (Cemu's crashes, which is why it is
            // second: our own query already works there.)
            trace("DNS: own lookup failed, using the system resolver");
            ip = host;
        }
        trace("TCP connect");
        std::string tcpError;
        const int fd = http_connect_tcp(ip, port, timeoutSeconds > 0 ? timeoutSeconds : 8, tcpError);
        if (fd < 0) {
            error = ip == host && !dnsError.empty() ? dnsError + "; " + tcpError : tcpError;
            return false;
        }
        trace("TLS handshake");
        std::shared_ptr<tlsconn::Session> tls = tlsconn::connect(fd, host, tlsconn::Policy(), timeoutSeconds, nullptr, error);
        if (!tls) return false;
        trace("TLS ready");
        out.read = [tls](void* buf, int len) -> int { return tls->read(buf, len); };
        out.write = [tls](const void* buf, int len) -> bool { return tls->write(buf, len); };
        out.close = [tls]() {
            tls->close(); // the socket goes when the last copy of this closure does
        };
        return true;
    };
}

#elif defined(UFIN_TLS_NSSL)

namespace {

// One NSSL library init and one context holding the system root certificates, for the life of
// the process (the update check runs once). Failures are remembered, not retried.
struct Nssl {
    bool ready = false;
    NSSLContextHandle context = 0;
    std::string error;
};

Nssl& nssl() {
    static Nssl state;
    static std::once_flag once;
    std::call_once(once, [] {
        if (NSSLInit() < 0) {
            state.error = "NSSLInit failed";
            return;
        }
        state.context = NSSLCreateContext(0);
        if (state.context < 0) {
            state.error = "NSSLCreateContext failed";
            return;
        }
        // An emulator without system TLS (Cemu logs "Unsupported lib call: nsysnet.NSSLInit") answers every
        // NSSL call with 0, so everything above "succeeded". A real console hands out a different handle
        // for each context: ask for a second one, and treat the same number twice as "not really there".
        // (Going on would reach name resolution, which crashes Cemu outright.)
        const NSSLContextHandle probe = NSSLCreateContext(0);
        if (probe >= 0 && probe == state.context) {
            state.error = "the system TLS is not available here (an emulator without it?)";
            return;
        }
        if (probe >= 0) NSSLDestroyContext(probe);
        // Every commercial root the console ships (header ids 1001..1033 and the two 4096-bit
        // ones, 1900 and 1901). An id this firmware does not know is simply refused.
        int added = 0;
        for (int id = 1001; id <= 1033; id++) if (NSSLAddServerPKI(state.context, (NSSLServerCertId)id) >= 0) added++;
        for (int id = 1900; id <= 1901; id++) if (NSSLAddServerPKI(state.context, (NSSLServerCertId)id) >= 0) added++;
        if (added == 0) {
            state.error = "no system root certificates could be added";
            return;
        }
        state.ready = true;
    });
    return state;
}

// Closes `fd` only if it is still open (see the note on Connection close below).
void closeIfOpen(int fd) {
    if (fd < 0) return;
    int type = 0;
    socklen_t len = sizeof(type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) == 0) close(fd);
}

} // namespace

Opener systemOpener() {
    return [](const std::string& host, int port, int timeoutSeconds, Connection& out, std::string& error) {
        trace("NSSL init");
        Nssl& n = nssl();
        trace(n.ready ? "NSSL ready" : "NSSL not available");
        if (!n.ready) {
            error = "TLS not available: " + n.error;
            return false;
        }
        // First choice: our own connected socket (bounded connect, shared name lookup), TLS
        // layered on it. `host` is what the certificate has to match.
        std::string tcpError;
        trace("TCP connect");
        int sock = http_connect_tcp(host, port, timeoutSeconds, tcpError);
        trace(sock >= 0 ? "TCP connected" : "TCP connect failed");
        NSSLConnectionHandle h = -1;
        if (sock >= 0) {
            trace("TLS handshake (own socket)");
            h = NSSLCreateConnection(n.context, host.c_str(), (int32_t)host.size(), 0, sock, 1);
            if (h < 0) {
                closeIfOpen(sock);
                sock = -1;
            }
        } else if (port != 443) {
            error = tcpError;
            return false;
        }
        // Fallback, port 443 only: let NSSL open and own the connection itself.
        if (h < 0 && port == 443) {
            trace("TLS handshake (NSSL socket)");
            h = NSSLCreateConnection(n.context, host.c_str(), (int32_t)host.size(), 0, 0, 1);
            sock = -1;
        }
        if (h < 0) {
            char b[96];
            snprintf(b, sizeof(b), "TLS connection failed (NSSL error %d)", (int)h);
            error = sock < 0 && !tcpError.empty() ? tcpError + "; " + b : b;
            return false;
        }
        out.read = [h](void* buf, int len) -> int {
            int32_t got = 0;
            const NSSLError e = NSSLRead(h, buf, len, &got);
            if (e == NSSL_ERROR_OK) return got > 0 ? got : 0;
            if (e == NSSL_ERROR_WANT_READ || e == NSSL_ERROR_WANT_WRITE || e == NSSL_ERROR_NOT_READY) return -2;
            return -1;
        };
        out.write = [h](const void* buf, int len) -> bool {
            const char* p = (const char*)buf;
            int left = len;
            int spins = 0;
            while (left > 0) {
                int32_t sent = 0;
                const NSSLError e = NSSLWrite(h, p, left, &sent);
                if (e == NSSL_ERROR_OK && sent > 0) {
                    p += sent;
                    left -= sent;
                    spins = 0;
                } else if (e == NSSL_ERROR_WANT_READ || e == NSSL_ERROR_WANT_WRITE || e == NSSL_ERROR_NOT_READY) {
                    if (++spins > 500) return false;
                    nettime::sleepMs(10);
                } else {
                    return false;
                }
            }
            return true;
        };
        out.close = [h, sock]() {
            NSSLDestroyConnection(h);
            // Whether destroying the connection also closes a socket it was handed is not
            // documented: close ours only if it is still open, so it is never closed twice.
            closeIfOpen(sock);
        };
        return true;
    };
}

#else

Opener systemOpener() {
    return [](const std::string&, int, int, Connection&, std::string& error) {
        error = "TLS not available in this build";
        return false;
    };
}

#endif

} // namespace https
