// A tiny DNS client (one UDP query for an A record), used by the HTTPS path of the update check.
//
// Why not gethostbyname(): on Cemu it crashes the whole emulator (the call ends in an unimplemented
// get_h_errno), and the system resolver gives no timeout or cancel. A DNS query is one small UDP packet, so it
// is simple to do here, bounded and cancelable. Servers: the public resolvers below, tried in turn. Only the
// update check uses this; Ufin's Jellyfin connection keeps using the system resolver as before.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dns {

// ---- pure packet code (host-tested) ----------------------------------------

// A standard recursive query for `host`, type A, class IN. Empty if the name cannot be encoded.
std::vector<uint8_t> buildQuery(const std::string& host, uint16_t id);
// The first IPv4 address in the answer section, as "a.b.c.d" (CNAME records are skipped over: the
// resolver already followed them). false with `error` for a wrong id, a refusal / NXDOMAIN, a
// truncated or malformed packet, or no A record.
bool parseAnswer(const uint8_t* data, size_t len, uint16_t id, std::string& ipv4, std::string& error);

// ---- the lookup ------------------------------------------------------------

struct Options {
    std::vector<std::string> servers = {"1.1.1.1", "8.8.8.8", "9.9.9.9"};
    int port = 53;
    int perServerTimeoutMs = 2000;
    int totalTimeoutMs = 6000;
    std::function<bool()> cancelled;  // polled about every 50 ms
};

// The options the HTTPS path uses (the struct's own defaults unless a test changes them).
Options& defaultOptions();

// "a.b.c.d" is returned as it is. Otherwise queries the servers in turn.
bool resolveA(const std::string& host, const Options& options, std::string& ipv4, std::string& error);

// For tests: make `host` resolve to `ipv4` without any network (an empty ipv4 removes the entry).
void setHostOverride(const std::string& host, const std::string& ipv4);

} // namespace dns
