#pragma once
#include <string>

struct HttpResponse {
    int status_code = 0;
    std::string body;
    bool success = false; // true if we got a 2xx status back
};

// Minimal HTTP/1.1 client built directly on BSD sockets. No TLS support --
// this is meant for talking to a Jellyfin server over plain http:// on
// your LAN, same as your N100 box. If you ever need https://, this client
// won't work as-is (would need mbedtls-wup wired in).
//
// extra_headers, if provided, must be pre-formatted with a trailing
// "\r\n" per header line, e.g. "X-Custom: value\r\n".

HttpResponse http_get(const std::string& host, int port, const std::string& path,
                       const std::string& extra_headers = "");

HttpResponse http_post(const std::string& host, int port, const std::string& path,
                        const std::string& body, const std::string& content_type,
                        const std::string& extra_headers = "");
