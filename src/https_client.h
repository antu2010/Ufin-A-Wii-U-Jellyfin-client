// A small HTTPS GET for the startup update check (update_check.h).
//
// Ufin's normal HTTP client (http_client.h) is plain sockets, which is right for
// a Jellyfin server on the LAN but cannot talk to GitHub, which only serves
// HTTPS. This adds just enough for one GET: on the Wii U the TLS comes from the
// console's own system library (NSSL, part of wut -- nothing extra to install
// or link), and everything around it (request, response parsing, chunked
// bodies, redirects, size cap, deadline, cancel) is plain code that the host
// tests run against a local server.
//
// Built with UFIN_TLS_NSSL (CMake defines it for the console) the system TLS is
// used; without it https_get() fails with "TLS not available" and only the
// injectable core (https_get_with) works, which is what the host tests use.
#pragma once

#include <functional>
#include <string>

namespace https {

struct Response {
    bool ok = false;        // a complete HTTP response was received
    int status = 0;
    std::string body;
    std::string location;   // Location header, if any
    std::string error;      // when !ok
};

// ---- pure parsing (host-tested) -------------------------------------------

// raw = everything the server sent until it closed the connection. false when the
// header block is not complete or is not HTTP. A body that ends early (shorter than
// Content-Length, or a chunked body without its final chunk) is reported through
// out.error with out.ok == false, never silently accepted.
bool parseResponse(const std::string& raw, Response& out);

// "https://host[:port]/path" -> pieces. false for anything not https://.
bool splitUrl(const std::string& url, std::string& host, int& port, std::string& path);

// ---- a connection, as far as the core is concerned ------------------------

struct Connection {
    // >0 bytes read, 0 the peer closed, -1 error, -2 nothing yet (try again shortly).
    std::function<int(void* buf, int len)> read;
    // true if all bytes were written.
    std::function<bool(const void* buf, int len)> write;
    std::function<void()> close;
};
using Opener = std::function<bool(const std::string& host, int port, int timeoutSeconds, Connection& out,
                                  std::string& error)>;

struct Options {
    int port = 443;
    int timeoutSeconds = 8;                 // the whole request, connect included
    size_t maxBytes = 3 * 1024 * 1024;      // response size cap (the App Store's repo.json is the big one)
    int maxRedirects = 2;
    std::function<bool()> cancelled;        // polled between reads
};

// Optional step-by-step trace ("connecting", "TLS handshake" ...), for finding where a console or
// emulator stops. Never carries URLs' query strings or headers. Unset = silent.
void setTrace(void (*hook)(const char* message));

// Tests only: trust one more root certificate (PEM) besides the built-in ones. Has an effect with the
// Mbed TLS build only.
void setExtraRootsForTests(const std::string& pem);

// GET https://host + path. extraHeaders: "Name: value\r\n" lines (never logged by this code).
Response getWith(const Opener& open, const std::string& host, const std::string& path,
                 const std::string& extraHeaders, const Options& options);

// getWith() over the system TLS of the build (see the top of this file).
Response get(const std::string& host, const std::string& path, const std::string& extraHeaders,
             const Options& options = Options());

// The opener of this build: system TLS on the console, a "not available" failure elsewhere.
Opener systemOpener();

} // namespace https
