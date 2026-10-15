// The glue between https_client and the console's system TLS (NSSL), built with
// UFIN_TLS_NSSL against a fake NSSL that passes bytes through a plain socket.
// This does NOT test TLS itself -- certificates, ciphers and the handshake are the
// console's -- only that Ufin drives the NSSL API sensibly: roots added, the host
// name passed for the certificate check, WANT_READ / WANT_WRITE retried, partial
// writes finished, every failure reported as an error, the connection released.
//   test_https_nssl            -> the normal scenarios
//   test_https_nssl noroots    -> the console refuses every root certificate
#include "check.h"
#include "fake_nssl.h"
#include "https_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <thread>

static bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// A server answering every connection with the same bytes, in two pieces.
struct Server {
    int fd = -1, port = 0;
    std::thread th;
    std::string answer;
    bool running = true;
    bool start() {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(fd, (sockaddr*)&a, sizeof(a)) < 0 || listen(fd, 4) < 0) return false;
        socklen_t l = sizeof(a);
        getsockname(fd, (sockaddr*)&a, &l);
        port = ntohs(a.sin_port);
        th = std::thread([this] {
            while (running) {
                int c = accept(fd, nullptr, nullptr);
                if (c < 0) break;
                std::string req;
                char b[1024];
                while (req.find("\r\n\r\n") == std::string::npos) {
                    ssize_t n = recv(c, b, sizeof(b), 0);
                    if (n <= 0) break;
                    req.append(b, (size_t)n);
                }
                lastRequest = req;
                size_t half = answer.size() / 2;
                send(c, answer.data(), half, MSG_NOSIGNAL);
                usleep(20000);
                send(c, answer.data() + half, answer.size() - half, MSG_NOSIGNAL);
                close(c);
            }
        });
        return true;
    }
    void stop() {
        running = false;
        shutdown(fd, SHUT_RDWR);
        close(fd);
        if (th.joinable()) th.join();
    }
    std::string lastRequest;
};

static std::string ok200(const std::string& body) {
    return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "noroots") {
        g_fakeNssl.noRoots = true;
        https::Options o;
        o.port = 1;
        https::Response r = https::get("api.github.com", "/", "", o);
        CHECK(!r.ok);
        CHECK(has(r.error, "TLS not available"));
        CHECK(has(r.error, "no system root certificates"));
        CHECK(g_fakeNssl.rootsAsked == 35);   // it tried every commercial id, then gave up cleanly
        CHECK_EQ(g_fakeNssl.createCalls, 0);  // and never tried to connect without roots
        return check::finish("test_https_nssl (noroots)");
    }

    if (argc > 1 && std::string(argv[1]) == "emulator") {
        // Cemu: every NSSL call "works" with 0. Must be recognised, and must not go on to name resolution.
        g_fakeNssl.emulatorStub = true;
        https::Options o;
        https::Response r = https::get("api.github.com", "/", "", o);
        CHECK(!r.ok);
        CHECK(has(r.error, "TLS not available"));
        CHECK(has(r.error, "not available here"));
        CHECK_EQ(g_fakeNssl.createCalls, 0);
        CHECK_EQ(g_fakeNssl.ownSocketAttempts, 0);
        CHECK_EQ(g_fakeNssl.fallbackAttempts, 0);
        return check::finish("test_https_nssl (emulator)");
    }

    Server s;
    s.answer = ok200("{\"tag_name\":\"v1.3.1\"}");
    CHECK(s.start());
    https::Options o;
    o.port = s.port;
    o.timeoutSeconds = 5;

    // --- the normal path: roots added once, host passed, answer read despite WANT_READ and tiny reads ---
    g_fakeNssl.wantReadEvery = 2;
    g_fakeNssl.maxRead = 7;
    g_fakeNssl.maxWrite = 11;
    g_fakeNssl.wantWriteOnce = true;
    https::Response r = https::get("127.0.0.1", "/repos/a/b/releases/latest", "User-Agent: Ufin/1.3.0\r\n", o);
    CHECK(r.ok && r.status == 200);
    CHECK_EQ(r.body, std::string("{\"tag_name\":\"v1.3.1\"}"));
    CHECK_EQ(g_fakeNssl.initCalls, 1);
    CHECK(g_fakeNssl.rootsAdded > 20 && g_fakeNssl.rootsAdded < 35);       // every id tried, unknown ones skipped
    CHECK_EQ(g_fakeNssl.lastHost, std::string("127.0.0.1"));               // the name the certificate is checked against
    CHECK(has(s.lastRequest, "GET /repos/a/b/releases/latest HTTP/1.1\r\n"));  // all of it arrived despite 11-byte writes
    CHECK(has(s.lastRequest, "User-Agent: Ufin/1.3.0\r\n"));
    CHECK_EQ(g_fakeNssl.destroyCalls, 1);                                   // connection released
    CHECK_EQ(g_fakeNssl.fallbackAttempts, 0);

    // --- a second request reuses the library state (no second init) ---
    g_fakeNssl.wantReadEvery = 0;
    g_fakeNssl.maxRead = 1 << 20;
    g_fakeNssl.maxWrite = 1 << 20;
    r = https::get("127.0.0.1", "/", "", o);
    CHECK(r.ok);
    CHECK_EQ(g_fakeNssl.initCalls, 1);
    CHECK_EQ(g_fakeNssl.destroyCalls, 2);

    // --- NSSL closes the socket itself on destroy: ours is not closed a second time ---
    g_fakeNssl.closeSocketOnDestroy = true;
    r = https::get("127.0.0.1", "/", "", o);   // (a double close would show up as EBADF noise / a bad fd, not here; it must simply work)
    CHECK(r.ok);
    g_fakeNssl.closeSocketOnDestroy = false;

    // --- the handshake is refused (certificate not trusted, protocol too new ...): an error, not a crash ---
    g_fakeNssl.failOnGivenSocket = true;
    r = https::get("127.0.0.1", "/", "", o);   // port != 443: no fallback
    CHECK(!r.ok);
    CHECK(has(r.error, "TLS connection failed"));
    CHECK(has(r.error, "-2621454"));            // 0x28000e, the code a person can look up
    g_fakeNssl.failOnGivenSocket = false;

    // --- port 443: if wrapping our socket fails, let NSSL open the connection itself ---
    {
        g_fakeNssl.failOnGivenSocket = true;
        // Nothing listens on 127.0.0.1:443 for the first (own socket) attempt, so that path fails on connect;
        // NSSL's own connection is then the fallback, here a socket the test opened to the server.
        int fb = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)s.port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(connect(fb, (sockaddr*)&a, sizeof(a)) == 0);
        g_fakeNssl.fallbackFd = fb;
        https::Options o443 = o;
        o443.port = 443;
        r = https::get("127.0.0.1", "/", "", o443);
        CHECK(r.ok && has(r.body, "v1.3.1"));
        CHECK(g_fakeNssl.fallbackAttempts >= 1);
        close(fb);
        g_fakeNssl.fallbackFd = -1;
        g_fakeNssl.failOnGivenSocket = false;
    }

    s.stop();
    return check::finish("test_https_nssl");
}
