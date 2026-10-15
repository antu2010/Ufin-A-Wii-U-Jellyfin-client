// The HTTPS client's core (https_client.h) against small scripted servers: the
// request it sends, response parsing (Content-Length, chunked, truncated),
// redirects, the size cap, the deadline and cancel. The TLS layer is swapped for
// plain TCP here (the console's system TLS cannot run on a PC); the glue that
// wraps NSSL has its own test, test_https_nssl.
#include "check.h"
#include "http_client.h"
#include "https_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

using Clock = std::chrono::steady_clock;
static double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }
static bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Accepts connections one after another; behave(fd, requestText) decides the answer, then the fd is closed.
class Server {
public:
    explicit Server(std::function<void(int, const std::string&)> behave) : behave_(std::move(behave)) {}
    ~Server() { stop(); }
    bool start() {
        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listenFd_, (sockaddr*)&a, sizeof(a)) < 0 || listen(listenFd_, 4) < 0) return false;
        socklen_t l = sizeof(a);
        getsockname(listenFd_, (sockaddr*)&a, &l);
        port_ = ntohs(a.sin_port);
        running_ = true;
        thread_ = std::thread([this] {
            while (running_) {
                int fd = accept(listenFd_, nullptr, nullptr);
                if (fd < 0) break;
                std::string req;
                char b[2048];
                while (req.find("\r\n\r\n") == std::string::npos) {
                    ssize_t n = recv(fd, b, sizeof(b), 0);
                    if (n <= 0) break;
                    req.append(b, (size_t)n);
                }
                requests_++;
                lastRequest_ = req;
                behave_(fd, req);
                close(fd);
            }
        });
        return true;
    }
    void stop() {
        if (!running_) return;
        running_ = false;
        shutdown(listenFd_, SHUT_RDWR);
        close(listenFd_);
        if (thread_.joinable()) thread_.join();
    }
    int port() const { return port_; }
    int requests() const { return requests_; }
    std::string lastRequest() const { return lastRequest_; }
private:
    std::function<void(int, const std::string&)> behave_;
    int listenFd_ = -1, port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> requests_{0};
    std::string lastRequest_;
    std::thread thread_;
};

static void sendAll(int fd, const std::string& s) { send(fd, s.data(), s.size(), MSG_NOSIGNAL); }

// "TLS" that is really plain TCP to 127.0.0.1:port.
static https::Opener plainOpener() {
    return [](const std::string&, int port, int timeoutSeconds, https::Connection& out, std::string& error) {
        int fd = http_connect_tcp("127.0.0.1", port, timeoutSeconds, error);
        if (fd < 0) return false;
        out.read = [fd](void* buf, int len) -> int {
            ssize_t n = recv(fd, buf, (size_t)len, 0);
            return n > 0 ? (int)n : (n == 0 ? 0 : -1);
        };
        out.write = [fd](const void* buf, int len) { return send(fd, buf, (size_t)len, MSG_NOSIGNAL) == len; };
        out.close = [fd]() { close(fd); };
        return true;
    };
}

static std::string ok200(const std::string& body, const std::string& extra = "") {
    return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extra +
           "\r\n" + body;
}

int main() {
    // --- parsing ---------------------------------------------------------------
    {
        https::Response r;
        CHECK(https::parseResponse(ok200("{\"a\":1}"), r));
        CHECK(r.ok && r.status == 200 && r.body == "{\"a\":1}");
        // Content-Length wins over trailing bytes.
        CHECK(https::parseResponse("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabcdef", r) && r.body == "abc");
        // No length: everything until close.
        CHECK(https::parseResponse("HTTP/1.1 200 OK\r\n\r\nrest of it", r) && r.body == "rest of it");
        // Chunked, with an extension and mixed-case header.
        CHECK(https::parseResponse("HTTP/1.1 200 OK\r\ntransfer-encoding: Chunked\r\n\r\n5;x=y\r\nhello\r\n6\r\n world\r\n0\r\n\r\n", r));
        CHECK_EQ(r.body, std::string("hello world"));
        // Truncated bodies are errors, not short answers.
        CHECK(!https::parseResponse("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc", r) && has(r.error, "ended early"));
        CHECK(!https::parseResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel", r) && has(r.error, "ended early"));
        CHECK(!https::parseResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n", r)); // no final chunk
        // Not HTTP / no end of headers / absurd status.
        CHECK(!https::parseResponse("SSH-2.0-OpenSSH\r\n\r\n", r));
        CHECK(!https::parseResponse("HTTP/1.1 200 OK\r\nContent-Le", r) && has(r.error, "incomplete"));
        CHECK(!https::parseResponse("HTTP/1.1 999 Nope\r\n\r\n", r));
        // Non-200 answers parse fine (the caller decides what they mean).
        CHECK(https::parseResponse("HTTP/1.1 403 Forbidden\r\nContent-Length: 2\r\n\r\nno", r) && r.status == 403);
        CHECK(https::parseResponse("HTTP/1.1 301 Moved\r\nLocation: https://x/y\r\nContent-Length: 0\r\n\r\n", r) && r.location == "https://x/y");

        std::string h, p;
        int port = 0;
        CHECK(https::splitUrl("https://api.github.com/repositories/1/releases/latest", h, port, p));
        CHECK(h == "api.github.com" && port == 443 && p == "/repositories/1/releases/latest");
        CHECK(https::splitUrl("https://example.org:8443", h, port, p) && port == 8443 && p == "/");
        CHECK(!https::splitUrl("http://example.org/x", h, port, p));    // never downgrade to plain http
        CHECK(!https::splitUrl("https:///x", h, port, p));
        CHECK(!https::splitUrl("ftp://example.org", h, port, p));
    }

    // --- a normal request ------------------------------------------------------
    {
        Server s([](int fd, const std::string&) { sendAll(fd, ok200("{\"tag_name\":\"v1.3.1\"}")); });
        CHECK(s.start());
        https::Options o;
        o.port = s.port();
        https::Response r = https::getWith(plainOpener(), "api.github.com", "/repos/a/b/releases/latest",
                                           "User-Agent: Ufin/1.3.0\r\nAccept: application/vnd.github+json\r\n", o);
        CHECK(r.ok && r.status == 200 && r.body == "{\"tag_name\":\"v1.3.1\"}");
        const std::string req = s.lastRequest();
        CHECK(req.rfind("GET /repos/a/b/releases/latest HTTP/1.1\r\n", 0) == 0);
        CHECK(has(req, "Host: api.github.com\r\n"));
        CHECK(has(req, "User-Agent: Ufin/1.3.0\r\n"));
        CHECK(has(req, "Connection: close\r\n"));
        CHECK(has(req, "Accept-Encoding: identity\r\n"));   // never a compressed body we cannot read
    }

    // --- chunked, delivered in dribbles ----------------------------------------
    {
        Server s([](int fd, const std::string&) {
            sendAll(fd, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
            sleepMs(30);
            sendAll(fd, "4\r\nWiki\r\n");
            sleepMs(30);
            sendAll(fd, "5\r\npedia\r\n0\r\n\r\n");
        });
        CHECK(s.start());
        https::Options o;
        o.port = s.port();
        https::Response r = https::getWith(plainOpener(), "h", "/", "", o);
        CHECK(r.ok && r.body == "Wikipedia");
    }

    // --- the server hangs up mid-body ------------------------------------------
    {
        Server s([](int fd, const std::string&) { sendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly a little"); });
        CHECK(s.start());
        https::Options o;
        o.port = s.port();
        https::Response r = https::getWith(plainOpener(), "h", "/", "", o);
        CHECK(!r.ok && has(r.error, "ended early"));
    }

    // --- redirects: absolute https, relative, a loop, a downgrade -------------
    {
        Server s([](int fd, const std::string& req) {
            if (req.compare(0, 13, "GET /old HTTP") == 0)
                sendAll(fd, "HTTP/1.1 301 Moved Permanently\r\nLocation: /new\r\nContent-Length: 0\r\n\r\n");
            else if (req.compare(0, 13, "GET /abs HTTP") == 0)
                sendAll(fd, "HTTP/1.1 302 Found\r\nLocation: https://elsewhere.example/new\r\nContent-Length: 0\r\n\r\n");
            else if (req.compare(0, 14, "GET /loop HTTP") == 0)
                sendAll(fd, "HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n");
            else if (req.compare(0, 13, "GET /dwn HTTP") == 0)
                sendAll(fd, "HTTP/1.1 302 Found\r\nLocation: http://plain.example/x\r\nContent-Length: 0\r\n\r\n");
            else
                sendAll(fd, ok200("arrived"));
        });
        CHECK(s.start());
        https::Options o;
        o.port = s.port();
        CHECK_EQ(https::getWith(plainOpener(), "h", "/old", "", o).body, std::string("arrived")); // relative: same host
        https::Response loop = https::getWith(plainOpener(), "h", "/loop", "", o);
        CHECK(!loop.ok && has(loop.error, "too many redirects"));
        https::Response dwn = https::getWith(plainOpener(), "h", "/dwn", "", o);
        CHECK(!dwn.ok && has(dwn.error, "https"));
        // Absolute redirect: the opener is asked for the new host (here it ignores the name and reuses the port).
        std::string seenHost;
        https::Opener spy = [&](const std::string& host, int port, int t, https::Connection& c, std::string& e) {
            seenHost = host;
            return plainOpener()(host, port, t, c, e);
        };
        https::Options o2 = o;
        // port 443 from the redirect would not reach the fake server, so only check where it tried to go.
        https::getWith(spy, "h", "/abs", "", o2);
        CHECK_EQ(seenHost, std::string("elsewhere.example"));
    }

    // --- the deadline: a connection whose read says "nothing yet" (what the console's TLS does) ---
    {
        // The core's own deadline, with a connection whose read says "nothing yet" (what NSSL does).
        https::Opener sleepy = [](const std::string&, int, int, https::Connection& c, std::string&) {
            c.read = [](void*, int) { return -2; };
            c.write = [](const void*, int) { return true; };
            c.close = [] {};
            return true;
        };
        https::Options o;
        o.timeoutSeconds = 1;
        auto t0 = Clock::now();
        https::Response r = https::getWith(sleepy, "h", "/", "", o);
        double took = since(t0);
        CHECK(!r.ok && has(r.error, "timed out"));
        CHECK(took >= 0.9 && took < 3.0);
    }

    // --- cancel ---------------------------------------------------------------
    {
        https::Opener sleepy = [](const std::string&, int, int, https::Connection& c, std::string&) {
            c.read = [](void*, int) { return -2; };
            c.write = [](const void*, int) { return true; };
            c.close = [] {};
            return true;
        };
        https::Options o;
        o.timeoutSeconds = 30;
        auto t0 = Clock::now();
        o.cancelled = [&] { return since(t0) > 0.3; };
        https::Response r = https::getWith(sleepy, "h", "/", "", o);
        CHECK(!r.ok && has(r.error, "cancelled"));
        CHECK(since(t0) < 2.0);
    }

    // --- the size cap ----------------------------------------------------------
    {
        Server s([](int fd, const std::string&) { sendAll(fd, ok200(std::string(200000, 'x'))); });
        CHECK(s.start());
        https::Options o;
        o.port = s.port();
        o.maxBytes = 50000;
        https::Response r = https::getWith(plainOpener(), "h", "/", "", o);
        CHECK(!r.ok && has(r.error, "too large"));
        o.maxBytes = 300000;
        CHECK(https::getWith(plainOpener(), "h", "/", "", o).ok);
    }

    // --- failures to connect / write ------------------------------------------
    {
        int port;
        { Server probe([](int, const std::string&) {}); CHECK(probe.start()); port = probe.port(); } // nothing listens now
        https::Options o;
        o.port = port;
        o.timeoutSeconds = 2;
        https::Response r = https::getWith(plainOpener(), "h", "/", "", o);
        CHECK(!r.ok && has(r.error, "connect"));
        https::Opener nowrite = [](const std::string&, int, int, https::Connection& c, std::string&) {
            c.read = [](void*, int) { return 0; };
            c.write = [](const void*, int) { return false; };
            c.close = [] {};
            return true;
        };
        CHECK(has(https::getWith(nowrite, "h", "/", "", o).error, "send"));
    }

    // --- a connection closed by the opener is always closed ---------------------
    {
        int closes = 0;
        https::Opener counting = [&](const std::string&, int, int, https::Connection& c, std::string&) {
            c.read = [](void*, int) { return 0; };
            c.write = [](const void*, int) { return true; };
            c.close = [&] { closes++; };
            return true;
        };
        https::getWith(counting, "h", "/", "", https::Options());
        CHECK_EQ(closes, 1);
    }

    // --- this host build has no system TLS: a clear failure, not a crash ------------
    {
        https::Response r = https::get("example.org", "/", "");
        CHECK(!r.ok && has(r.error, "TLS not available"));
    }

    return check::finish("test_https_client");
}
