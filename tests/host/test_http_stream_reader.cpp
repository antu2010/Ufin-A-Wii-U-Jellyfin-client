// HttpStreamReader against small scripted servers: what open() reports when the
// server stays silent, hangs up, answers late, or answers with an error; that
// the startup wait can be stretched and cancelled; and that a failure keeps its
// *cause* ("timed out", "connection closed") instead of only the name of the
// stage that hit it. The reported bug "failed reading status line" used to hide
// which of those it was.
//
// Needs no FFmpeg: only src/http_stream_reader.cpp and the coreinit stub.
#include "check.h"
#include "http_stream_reader.h"

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

static bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// One-connection-at-a-time TCP server on 127.0.0.1. `behave(fd)` runs after the
// request has been read (up to the blank line) and decides what to send.
class ScriptedServer {
public:
    explicit ScriptedServer(std::function<void(int)> behave) : behave_(std::move(behave)) {}
    ~ScriptedServer() { stop(); }

    bool start() {
        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) return false;
        int one = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listenFd_, (sockaddr*)&addr, sizeof(addr)) < 0 || listen(listenFd_, 4) < 0) return false;
        socklen_t len = sizeof(addr);
        getsockname(listenFd_, (sockaddr*)&addr, &len);
        port_ = ntohs(addr.sin_port);
        running_ = true;
        thread_ = std::thread([this] {
            while (running_) {
                int fd = accept(listenFd_, nullptr, nullptr);
                if (fd < 0) break;
                std::string raw;
                char buf[2048];
                while (raw.find("\r\n\r\n") == std::string::npos) {
                    ssize_t n = recv(fd, buf, sizeof(buf), 0);
                    if (n <= 0) break;
                    raw.append(buf, (size_t)n);
                }
                requests_++;
                behave_(fd);
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

private:
    std::function<void(int)> behave_;
    int listenFd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> requests_{0};
    std::thread thread_;
};

static void sendAll(int fd, const std::string& s) { send(fd, s.data(), s.size(), MSG_NOSIGNAL); }
static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static const char* kChunkedHead =
    "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nTransfer-Encoding: chunked\r\n\r\n";

int main() {
    // The defaults the rest of Ufin relies on stay what they were.
    CHECK_EQ(HttpStreamReader::DEFAULT_TIMEOUT_SECONDS, 15);

    // --- 1. A silent server: the wait ends at the startup timeout, and the error says "timed out". ---
    {
        std::atomic<bool> release{false};
        ScriptedServer server([&](int) { while (!release) sleepMs(20); }); // accept, say nothing
        CHECK(server.start());
        HttpStreamReader r("127.0.0.1", server.port(), "/Videos/v1/stream.mp4?ApiKey=secret");
        r.setStartupTimeoutSeconds(1);
        auto t0 = Clock::now();
        CHECK(!r.open());
        double took = since(t0);
        CHECK(took >= 0.9 && took < 3.0);                       // honoured the 1 s, not the 15 s default
        CHECK(contains(r.lastError(), "failed reading status line"));
        CHECK(contains(r.lastError(), "timed out after 1s"));   // the cause is kept
        CHECK(!r.cancelled());
        release = true;
    }

    // --- 2. A server that hangs up: "connection closed", clearly not a timeout. ---
    {
        ScriptedServer server([](int) {}); // behave() returns -> the fd is closed without a byte
        CHECK(server.start());
        HttpStreamReader r("127.0.0.1", server.port(), "/x");
        auto t0 = Clock::now();
        CHECK(!r.open());
        CHECK(since(t0) < 3.0);                                  // immediate, not a 15 s wait
        CHECK(contains(r.lastError(), "failed reading status line"));
        CHECK(contains(r.lastError(), "connection closed by the server"));
        CHECK(!contains(r.lastError(), "timed out"));
    }

    // --- 3. A slow but healthy server (the burned-in-subtitle case): answers after 2 s. ---
    {
        const std::string payload(5000, 'x');
        ScriptedServer server([&](int fd) {
            sleepMs(2000); // "extracting the subtitle track"
            char len[16];
            snprintf(len, sizeof(len), "%zx", payload.size());
            sendAll(fd, std::string(kChunkedHead) + len + "\r\n" + payload + "\r\n0\r\n\r\n");
        });
        CHECK(server.start());

        // With a 1 s startup wait it gives up -- the failure this change is about...
        {
            HttpStreamReader r("127.0.0.1", server.port(), "/x");
            r.setStartupTimeoutSeconds(1);
            CHECK(!r.open());
            CHECK(contains(r.lastError(), "timed out after 1s"));
        }
        sleepMs(2200); // let the server finish that connection before the next one

        // ...with a longer one it succeeds and the body arrives intact.
        {
            HttpStreamReader r("127.0.0.1", server.port(), "/x");
            r.setStartupTimeoutSeconds(10);
            auto t0 = Clock::now();
            CHECK(r.open());
            double took = since(t0);
            CHECK(took >= 1.8 && took < 6.0);
            uint8_t buf[8192];
            int total = 0, n;
            while ((n = r.read(buf, sizeof(buf))) > 0) total += n;
            CHECK_EQ(n, 0);                       // clean end of stream
            CHECK_EQ(total, (int)payload.size());
        }
        sleepMs(2200);

        // Not setting anything keeps the plain default, which also covers a 2 s answer.
        {
            HttpStreamReader r("127.0.0.1", server.port(), "/x");
            CHECK(r.open());
        }
    }

    // --- 4. The user backs out of a long wait: prompt, flagged as cancelled, not as a failure. ---
    {
        std::atomic<bool> release{false};
        ScriptedServer server([&](int) { while (!release) sleepMs(20); });
        CHECK(server.start());
        HttpStreamReader r("127.0.0.1", server.port(), "/x");
        r.setStartupTimeoutSeconds(30);
        std::atomic<int> polls{0};
        auto t0 = Clock::now();
        r.setAbortCheck([&] { polls++; return since(t0) > 0.4; });
        CHECK(!r.open());
        double took = since(t0);
        CHECK(took >= 0.35 && took < 2.0);          // far below the 30 s
        CHECK(r.cancelled());
        CHECK(contains(r.lastError(), "cancelled"));
        CHECK(!contains(r.lastError(), "timed out"));
        CHECK(polls > 3);                           // polled in short slices, not once
        release = true;
    }

    // --- 5. An abort check that never fires does not disturb a slow, healthy answer. ---
    {
        ScriptedServer server([](int fd) {
            sleepMs(1000);
            sendAll(fd, std::string(kChunkedHead) + "3\r\nabc\r\n0\r\n\r\n");
        });
        CHECK(server.start());
        HttpStreamReader r("127.0.0.1", server.port(), "/x");
        r.setStartupTimeoutSeconds(10);
        std::atomic<int> polls{0};
        r.setAbortCheck([&] { polls++; return false; });
        CHECK(r.open());
        CHECK(!r.cancelled());
        CHECK(polls > 5);
        uint8_t buf[16];
        CHECK_EQ(r.read(buf, sizeof(buf)), 3);
        CHECK(memcmp(buf, "abc", 3) == 0);
    }

    // --- 6. HTTP errors keep reporting the status and the start of the server's explanation. ---
    {
        ScriptedServer server([](int fd) {
            const std::string body = "{\"error\":\"an FFmpeg error occurred\"}";
            sendAll(fd, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\n\r\n" + body);
        });
        CHECK(server.start());
        HttpStreamReader r("127.0.0.1", server.port(), "/x");
        CHECK(!r.open());
        CHECK(contains(r.lastError(), "server returned status 500"));
        CHECK(contains(r.lastError(), "an FFmpeg error occurred"));
    }

    // --- 7. The connection dropping mid-stream: the read error says why. ---
    {
        ScriptedServer server([](int fd) {
            sendAll(fd, std::string(kChunkedHead) + "5\r\nhello\r\n"); // one chunk, then hang up
        });
        CHECK(server.start());
        HttpStreamReader r("127.0.0.1", server.port(), "/x");
        CHECK(r.open());
        uint8_t buf[64];
        CHECK_EQ(r.read(buf, sizeof(buf)), 5);      // what had arrived is delivered...
        CHECK_EQ(r.read(buf, sizeof(buf)), -1);     // ...then an error, not a silent EOF
        CHECK(contains(r.lastError(), "failed reading chunk size line"));
        CHECK(contains(r.lastError(), "connection closed by the server"));
    }

    // --- 8. Connecting to nothing at all still fails fast with its own message. ---
    {
        int port;
        {
            ScriptedServer probe([](int) {});
            CHECK(probe.start());
            port = probe.port();
        } // closed: nothing listens there now
        HttpStreamReader r("127.0.0.1", port, "/x");
        CHECK(!r.open());
        CHECK(contains(r.lastError(), "connect() failed"));
    }

    return check::finish("test_http_stream_reader");
}
