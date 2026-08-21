#include "http_client.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>

static bool resolve_host(const std::string& host, struct in_addr* out) {
    if (inet_pton(AF_INET, host.c_str(), out) == 1) {
        return true;
    }
    struct hostent* he = gethostbyname(host.c_str());
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        return false;
    }
    memcpy(out, he->h_addr_list[0], sizeof(struct in_addr));
    return true;
}

static std::string to_lower(const std::string& s) {
    std::string out = s;
    for (auto& c : out) c = (char)tolower((unsigned char)c);
    return out;
}

static std::string dechunk(const std::string& body) {
    std::string out;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t line_end = body.find("\r\n", pos);
        if (line_end == std::string::npos) break;

        std::string size_line = body.substr(pos, line_end - pos);
        size_t semi = size_line.find(';');
        if (semi != std::string::npos) size_line = size_line.substr(0, semi);

        long chunk_size = strtol(size_line.c_str(), nullptr, 16);
        pos = line_end + 2;

        if (chunk_size <= 0) break;
        if (pos + (size_t)chunk_size > body.size()) break;

        out.append(body, pos, (size_t)chunk_size);
        pos += (size_t)chunk_size;

        if (pos + 2 <= body.size() && body[pos] == '\r' && body[pos + 1] == '\n') {
            pos += 2;
        }
    }
    return out;
}

static HttpResponse do_request(const std::string& host, int port, const std::string& request) {
    HttpResponse resp;

    struct in_addr addr;
    if (!resolve_host(host, &addr)) {
        resp.body = "could not resolve host: " + host;
        return resp;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        resp.body = "socket() failed";
        return resp;
    }

    struct sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons((uint16_t)port);
    server.sin_addr = addr;

    if (connect(sock, (struct sockaddr*)&server, sizeof(server)) < 0) {
        resp.body = "connect() failed to " + host;
        close(sock);
        return resp;
    }

    size_t sent = 0;
    while (sent < request.size()) {
        ssize_t n = send(sock, request.c_str() + sent, request.size() - sent, 0);
        if (n <= 0) {
            resp.body = "send() failed";
            close(sock);
            return resp;
        }
        sent += (size_t)n;
    }

    std::string raw;
    char buf[2048];
    ssize_t n;
    while ((n = recv(sock, buf, sizeof(buf), 0)) > 0) {
        raw.append(buf, (size_t)n);
    }
    close(sock);

    if (raw.empty()) {
        resp.body = "empty response from server";
        return resp;
    }

    size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        resp.body = "malformed HTTP response (no header terminator)";
        return resp;
    }

    std::string status_line = raw.substr(0, raw.find("\r\n"));
    size_t sp1 = status_line.find(' ');
    if (sp1 != std::string::npos) {
        resp.status_code = atoi(status_line.c_str() + sp1 + 1);
    }

    std::string headers = to_lower(raw.substr(0, header_end));
    resp.body = raw.substr(header_end + 4);

    if (headers.find("transfer-encoding: chunked") != std::string::npos) {
        resp.body = dechunk(resp.body);
    }

    resp.success = resp.status_code >= 200 && resp.status_code < 300;
    return resp;
}

HttpResponse http_get(const std::string& host, int port, const std::string& path,
                       const std::string& extra_headers) {
    char req[4096];
    snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n",
        path.c_str(), host.c_str(), extra_headers.c_str());
    return do_request(host, port, req);
}

HttpResponse http_post(const std::string& host, int port, const std::string& path,
                        const std::string& body, const std::string& content_type,
                        const std::string& extra_headers) {
    char header[4096];
    snprintf(header, sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n",
        path.c_str(), host.c_str(), content_type.c_str(), body.size(), extra_headers.c_str());
    std::string request = std::string(header) + body;
    return do_request(host, port, request);
}
