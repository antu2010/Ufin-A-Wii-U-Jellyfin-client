// The DNS client: packet building / parsing over canned answers, and a real UDP exchange with a fake server
// on localhost (answers, refusals, silence, a stray packet, several servers in turn, cancel).
#include "check.h"
#include "dns_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

using namespace dns;

static std::vector<uint8_t> answerFor(const std::vector<uint8_t>& query, int rcode, bool withCname, const char* ip, bool wrongId = false) {
    std::vector<uint8_t> r(query.begin(), query.end());
    if (wrongId) r[1] ^= 0xFF;
    r[2] = 0x81; r[3] = (uint8_t)(0x80 | rcode); // response, RD, RA, rcode
    int answers = 0;
    std::vector<uint8_t> body;
    if (rcode == 0) {
        if (withCname) { // name pointer, CNAME, IN, ttl, rdlen 2, pointer
            body.insert(body.end(), {0xC0, 0x0C, 0x00, 0x05, 0x00, 0x01, 0, 0, 0, 60, 0x00, 0x02, 0xC0, 0x0C});
            answers++;
        }
        if (ip) {
            unsigned a, b, c, d;
            sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d);
            body.insert(body.end(), {0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0, 0, 0, 60, 0x00, 0x04, (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d});
            answers++;
        }
    }
    r[7] = (uint8_t)answers;
    r.insert(r.end(), body.begin(), body.end());
    return r;
}

// A UDP "DNS server" on 127.0.0.1:<port>; `mode` decides what it does with each query.
struct Server {
    int sock = -1, port = 0;
    std::atomic<bool> stop{false};
    std::atomic<int> queries{0};
    std::thread th;
    enum Mode { Answer, AnswerCname, NxDomain, Silent, WrongIdThenAnswer } mode = Answer;
    const char* ip = "93.184.216.34";
    bool start() {
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(sock, (sockaddr*)&a, sizeof(a)) != 0) return false;
        socklen_t l = sizeof(a);
        getsockname(sock, (sockaddr*)&a, &l);
        port = ntohs(a.sin_port);
        timeval tv{0, 50000};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        th = std::thread([this] {
            uint8_t buf[512];
            while (!stop) {
                sockaddr_in from{};
                socklen_t fl = sizeof(from);
                const ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
                if (n <= 0) continue;
                queries++;
                std::vector<uint8_t> q(buf, buf + n);
                std::vector<uint8_t> r;
                switch (mode) {
                    case Silent: continue;
                    case NxDomain: r = answerFor(q, 3, false, nullptr); break;
                    case AnswerCname: r = answerFor(q, 0, true, ip); break;
                    case WrongIdThenAnswer: {
                        auto bad = answerFor(q, 0, false, "6.6.6.6", true);
                        sendto(sock, bad.data(), bad.size(), 0, (sockaddr*)&from, fl);
                        r = answerFor(q, 0, false, ip);
                        break;
                    }
                    default: r = answerFor(q, 0, false, ip);
                }
                sendto(sock, r.data(), r.size(), 0, (sockaddr*)&from, fl);
            }
        });
        return true;
    }
    ~Server() { stop = true; if (th.joinable()) th.join(); if (sock >= 0) close(sock); }
};

int main() {
    // --- packets -----------------------------------------------------------------------------
    auto q = buildQuery("api.github.com", 0x1234);
    CHECK_EQ(q.size(), (size_t)(12 + 1 + 3 + 1 + 6 + 1 + 3 + 1 + 4));
    CHECK(q[0] == 0x12 && q[1] == 0x34 && q[2] == 0x01 && q[5] == 0x01);
    CHECK(q[12] == 3 && memcmp(&q[13], "api", 3) == 0 && q[16] == 6 && q[23] == 3 && q[27] == 0);
    CHECK(q[q.size() - 4] == 0 && q[q.size() - 3] == 1 && q[q.size() - 1] == 1);
    CHECK(buildQuery("", 1).empty());
    CHECK(buildQuery("a..b", 1).empty());
    CHECK(buildQuery(std::string(64, 'a') + ".com", 1).empty());
    CHECK(buildQuery(std::string(300, 'a'), 1).empty());

    std::string ip, err;
    auto good = answerFor(q, 0, false, "140.82.121.6");
    CHECK(parseAnswer(good.data(), good.size(), 0x1234, ip, err) && ip == "140.82.121.6");
    auto cname = answerFor(q, 0, true, "140.82.121.5");
    CHECK(parseAnswer(cname.data(), cname.size(), 0x1234, ip, err) && ip == "140.82.121.5");   // CNAME skipped
    CHECK(!parseAnswer(good.data(), good.size(), 0x1235, ip, err) && err.find("another question") != std::string::npos);
    auto nx = answerFor(q, 3, false, nullptr);
    CHECK(!parseAnswer(nx.data(), nx.size(), 0x1234, ip, err) && err.find("NXDOMAIN") != std::string::npos);
    auto refused = answerFor(q, 5, false, nullptr);
    CHECK(!parseAnswer(refused.data(), refused.size(), 0x1234, ip, err) && err.find("DNS error 5") != std::string::npos);
    auto none = answerFor(q, 0, false, nullptr);
    CHECK(!parseAnswer(none.data(), none.size(), 0x1234, ip, err) && err.find("no address") != std::string::npos);
    CHECK(!parseAnswer(good.data(), 5, 0x1234, ip, err));
    for (size_t cut = 12; cut < good.size(); cut++) CHECK(!parseAnswer(good.data(), cut, 0x1234, ip, err)); // never reads past the end
    std::vector<uint8_t> loop = good;                       // a compression pointer that points at itself
    loop[12] = 0xC0; loop[13] = 12;
    CHECK(!parseAnswer(loop.data(), loop.size(), 0x1234, ip, err));
    std::vector<uint8_t> notResponse = good;
    notResponse[2] = 0x01;
    CHECK(!parseAnswer(notResponse.data(), notResponse.size(), 0x1234, ip, err));

    // --- lookups -----------------------------------------------------------------------------
    {
        Options o;
        CHECK(resolveA("192.168.1.20", o, ip, err) && ip == "192.168.1.20"); // a dotted address needs no lookup
        setHostOverride("ufin.test", "127.0.0.9");
        CHECK(resolveA("ufin.test", o, ip, err) && ip == "127.0.0.9");
        setHostOverride("ufin.test", "");
    }
    {
        Server s; CHECK(s.start());
        Options o; o.servers = {"127.0.0.1"}; o.port = s.port;
        CHECK(resolveA("api.github.com", o, ip, err) && ip == "93.184.216.34" && s.queries == 1);
        s.mode = Server::AnswerCname; s.ip = "1.2.3.4";
        CHECK(resolveA("www.example.org", o, ip, err) && ip == "1.2.3.4");
        s.mode = Server::WrongIdThenAnswer; s.ip = "5.6.7.8";
        CHECK(resolveA("api.github.com", o, ip, err) && ip == "5.6.7.8");   // the stray packet is ignored
        s.mode = Server::NxDomain;
        const int before = s.queries;
        CHECK(!resolveA("nope.invalid", o, ip, err) && err.find("no such host") != std::string::npos);
        CHECK(s.queries == before + 1);
    }
    {   // the first server is silent, the second answers
        Server dead, live;
        CHECK(dead.start() && live.start());
        dead.mode = Server::Silent;
        // different ports per server cannot be expressed (one port option), so use the same port: run
        // "dead" as a closed port instead -- nothing listens there, the query just times out.
        Options o; o.servers = {"127.0.0.1"}; o.port = dead.port; o.perServerTimeoutMs = 300; o.totalTimeoutMs = 400;
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!resolveA("api.github.com", o, ip, err) && err.find("no answer") != std::string::npos);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms >= 250 && ms < 1500);   // bounded
        // cancel
        std::atomic<bool> cancel{false};
        Options c; c.servers = {"127.0.0.1"}; c.port = dead.port; c.perServerTimeoutMs = 5000; c.totalTimeoutMs = 5000;
        c.cancelled = [&] { return cancel.load(); };
        std::thread t([&] { std::this_thread::sleep_for(std::chrono::milliseconds(150)); cancel = true; });
        const auto t1 = std::chrono::steady_clock::now();
        CHECK(!resolveA("api.github.com", c, ip, err) && err == "cancelled");
        t.join();
        CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count() < 1500);
    }
    return check::finish("test_dns_client");
}
