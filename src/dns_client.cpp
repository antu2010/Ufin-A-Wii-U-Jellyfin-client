#include "dns_client.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include "net_time.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

namespace dns {

namespace {

std::mutex g_overrideMutex;
std::map<std::string, std::string>& overrides() {
    static std::map<std::string, std::string> m;
    return m;
}

bool isDottedQuad(const std::string& s) {
    in_addr a;
    return inet_pton(AF_INET, s.c_str(), &a) == 1;
}

uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

// Skips a (possibly compressed) name starting at `pos`; returns the position after it, or 0 on error.
size_t skipName(const uint8_t* d, size_t len, size_t pos) {
    for (int guard = 0; guard < 128; guard++) {
        if (pos >= len) return 0;
        const uint8_t n = d[pos];
        if (n == 0) return pos + 1;
        if ((n & 0xC0) == 0xC0) return pos + 2 <= len ? pos + 2 : 0; // a pointer ends the name
        if ((n & 0xC0) != 0) return 0;
        pos += 1 + (size_t)n;
    }
    return 0;
}

} // namespace

std::vector<uint8_t> buildQuery(const std::string& host, uint16_t id) {
    std::vector<uint8_t> q = {(uint8_t)(id >> 8), (uint8_t)(id & 0xFF), 0x01, 0x00, // id, flags: RD
                              0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};     // 1 question
    if (host.empty() || host.size() > 253) return {};
    size_t start = 0;
    while (start <= host.size()) {
        size_t dot = host.find('.', start);
        if (dot == std::string::npos) dot = host.size();
        const size_t n = dot - start;
        if (n == 0 || n > 63) return {};
        q.push_back((uint8_t)n);
        for (size_t i = start; i < dot; i++) q.push_back((uint8_t)host[i]);
        if (dot == host.size()) break;
        start = dot + 1;
    }
    q.push_back(0);
    q.insert(q.end(), {0x00, 0x01, 0x00, 0x01}); // type A, class IN
    return q;
}

bool parseAnswer(const uint8_t* d, size_t len, uint16_t id, std::string& ipv4, std::string& error) {
    if (len < 12) { error = "DNS answer too short"; return false; }
    if (be16(d) != id) { error = "DNS answer for another question"; return false; }
    const uint16_t flags = be16(d + 2);
    if (!(flags & 0x8000)) { error = "not a DNS answer"; return false; }
    const int rcode = flags & 0x0F;
    if (rcode == 3) { error = "no such host (NXDOMAIN)"; return false; }
    if (rcode != 0) { error = "DNS error " + std::to_string(rcode); return false; }
    const uint16_t qd = be16(d + 4), an = be16(d + 6);
    size_t pos = 12;
    for (uint16_t i = 0; i < qd; i++) {
        pos = skipName(d, len, pos);
        if (!pos || pos + 4 > len) { error = "malformed DNS answer"; return false; }
        pos += 4;
    }
    for (uint16_t i = 0; i < an; i++) {
        pos = skipName(d, len, pos);
        if (!pos || pos + 10 > len) { error = "malformed DNS answer"; return false; }
        const uint16_t type = be16(d + pos), cls = be16(d + pos + 2), rdlen = be16(d + pos + 8);
        pos += 10;
        if (pos + rdlen > len) { error = "malformed DNS answer"; return false; }
        if (type == 1 && cls == 1 && rdlen == 4) {
            char b[16];
            snprintf(b, sizeof(b), "%u.%u.%u.%u", d[pos], d[pos + 1], d[pos + 2], d[pos + 3]);
            ipv4 = b;
            return true;
        }
        pos += rdlen;
    }
    error = "DNS answer has no address";
    return false;
}

Options& defaultOptions() {
    static Options options;
    return options;
}

void setHostOverride(const std::string& host, const std::string& ipv4) {
    std::lock_guard<std::mutex> lock(g_overrideMutex);
    if (ipv4.empty()) overrides().erase(host);
    else overrides()[host] = ipv4;
}


static bool queryServer(const std::string& server, int port, const std::vector<uint8_t>& query, uint16_t id,
                        int timeoutMs, const std::function<bool()>& cancelled, std::string& ipv4, std::string& error) {
    sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, server.c_str(), &to.sin_addr) != 1) { error = "bad DNS server " + server; return false; }
    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { error = "socket() failed"; return false; }
    struct Closer { int fd; ~Closer() { close(fd); } } closer{sock};
    if (sendto(sock, query.data(), query.size(), 0, (const sockaddr*)&to, sizeof(to)) != (ssize_t)query.size()) {
        error = "could not send the DNS query to " + server;
        return false;
    }
    const int64_t deadline = nettime::nowMs() + timeoutMs;
    uint8_t buf[1500];
    while (nettime::nowMs() < deadline) {
        if (cancelled && cancelled()) { error = "cancelled"; return false; }
        // Poll with a non-blocking read and our own sleep (select() on UDP is not something to bet on here).
        const ssize_t n = recvfrom(sock, buf, sizeof(buf), MSG_DONTWAIT, nullptr, nullptr);
        if (n <= 0) { nettime::sleepMs(20); continue; }
        std::string why;
        if (parseAnswer(buf, (size_t)n, id, ipv4, why)) return true;
        if (why == "DNS answer for another question") continue; // a stray packet: keep waiting
        error = why;
        return false;
    }
    error = "no answer from " + server;
    return false;
}

bool resolveA(const std::string& host, const Options& options, std::string& ipv4, std::string& error) {
    if (isDottedQuad(host)) { ipv4 = host; return true; }
    {
        std::lock_guard<std::mutex> lock(g_overrideMutex);
        auto it = overrides().find(host);
        if (it != overrides().end()) { ipv4 = it->second; return true; }
    }
    static uint16_t counter = 0x5A17;
    const uint16_t id = (uint16_t)(counter += 0x1F3D);
    const std::vector<uint8_t> query = buildQuery(host, id);
    if (query.empty()) { error = "not a host name: " + host; return false; }
    const int64_t deadline = nettime::nowMs() + options.totalTimeoutMs;
    std::string last = "no DNS server answered";
    for (const std::string& server : options.servers) {
        const int left = (int)(deadline - nettime::nowMs());
        if (left <= 0) break;
        std::string why;
        if (queryServer(server, options.port, query, id, left < options.perServerTimeoutMs ? left : options.perServerTimeoutMs,
                        options.cancelled, ipv4, why))
            return true;
        if (why == "cancelled") { error = why; return false; }
        last = why;
        if (why == "no such host (NXDOMAIN)") break; // every resolver will say the same
    }
    error = "could not resolve " + host + ": " + last;
    return false;
}

} // namespace dns
