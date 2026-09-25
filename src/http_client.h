#pragma once
#include <string>

struct HttpResponse {
    int status_code = 0;
    std::string body;
    std::string raw_headers; // lowercased raw header block (e.g. for reading Content-Range)
    bool success = false; // true if we got a 2xx status back (206 Partial Content included)
};

// Minimal HTTP/1.1 client built directly on BSD sockets. No TLS support --
// this is meant for talking to a Jellyfin server over plain http:// on
// your LAN, same as your N100 box. If you ever need https://, this client
// won't work as-is (would need mbedtls-wup wired in).
//
// extra_headers, if provided, must be pre-formatted with a trailing
// "\r\n" per header line, e.g. "X-Custom: value\r\n". Range requests
// (used by media/http_stream_io.cpp for streaming playback) work the
// same way: pass "Range: bytes=0-1048575\r\n" as extra_headers.

HttpResponse http_get(const std::string& host, int port, const std::string& path,
                       const std::string& extra_headers = "");

HttpResponse http_post(const std::string& host, int port, const std::string& path,
                        const std::string& body, const std::string& content_type,
                        const std::string& extra_headers = "");

HttpResponse http_delete(const std::string& host, int port, const std::string& path,
                          const std::string& extra_headers = "");

// A connection opened for streaming a large response body (e.g. a movie
// or track) instead of buffering it all into memory like http_get does.
// After a successful http_open_stream(), `sock` is a live, connected
// socket positioned right after the HTTP headers -- the caller reads the
// body directly off it with recv(), and must close(sock) when done.
//
// `leftover` holds any body bytes that were already pulled off the wire
// while we were scanning for the end of the headers -- these must be
// consumed by the caller BEFORE any further recv() calls, or the first
// chunk of the file gets silently dropped.
struct HttpStreamHandle {
    int status_code = 0;
    bool success = false;
    bool chunked = false;      // true if Transfer-Encoding: chunked
    long content_length = -1;  // -1 if unknown (chunked or not sent)
    std::string leftover;      // body bytes already read past the headers
    int sock = -1;
    std::string error;         // set on failure
};

bool http_open_stream(const std::string& host, int port, const std::string& path,
                       const std::string& extra_headers, HttpStreamHandle& out);
