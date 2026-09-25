// FakeHttpServer -- a very small HTTP/1.1 server on 127.0.0.1 for the
// host tests. Routes are matched on the request path (query string
// stripped); each connection handles one request and is then closed.
// Bodies can be sent with Content-Length or, like Jellyfin's transcode
// endpoint, as Transfer-Encoding: chunked.
#pragma once
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct FakeRoute {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;
    bool chunked = false;
    size_t chunkSize = 64 * 1024;
};

struct FakeRequest {
    std::string method;
    std::string path;     // includes query string
    std::string headers;  // raw header block
    std::string body;
};

class FakeHttpServer {
public:
    ~FakeHttpServer() { stop(); }

    bool start() {
        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) return false;
        int one = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(listenFd_, (sockaddr*)&addr, sizeof(addr)) < 0) return false;
        if (listen(listenFd_, 8) < 0) return false;
        socklen_t len = sizeof(addr);
        getsockname(listenFd_, (sockaddr*)&addr, &len);
        port_ = ntohs(addr.sin_port);
        running_ = true;
        thread_ = std::thread([this] { acceptLoop(); });
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

    // Called for requests no static route matches (outside the lock, so
    // it may be slow -- e.g. run ffmpeg). Return true and fill `out` to
    // answer; false for a 404.
    void setHandler(std::function<bool(const FakeRequest&, FakeRoute&)> h) {
        std::lock_guard<std::mutex> lock(mtx_);
        handler_ = std::move(h);
    }

    void addRoute(const std::string& path, const FakeRoute& route) {
        std::lock_guard<std::mutex> lock(mtx_);
        routes_[path] = route;
    }

    std::vector<FakeRequest> requests() {
        std::lock_guard<std::mutex> lock(mtx_);
        return requests_;
    }

private:
    int listenFd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mtx_;
    std::map<std::string, FakeRoute> routes_;
    std::function<bool(const FakeRequest&, FakeRoute&)> handler_;
    std::vector<FakeRequest> requests_;

    static bool sendAll(int fd, const char* data, size_t len) {
        while (len > 0) {
            ssize_t n = send(fd, data, len, 0);
            if (n <= 0) return false;
            data += n;
            len -= (size_t)n;
        }
        return true;
    }

    void acceptLoop() {
        while (running_) {
            int fd = accept(listenFd_, nullptr, nullptr);
            if (fd < 0) {
                if (!running_) break;
                continue;
            }
            handle(fd);
            close(fd);
        }
    }

    void handle(int fd) {
        std::string raw;
        char buf[4096];
        size_t headerEnd = std::string::npos;
        while (headerEnd == std::string::npos) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) return;
            raw.append(buf, (size_t)n);
            headerEnd = raw.find("\r\n\r\n");
        }

        FakeRequest req;
        req.headers = raw.substr(0, headerEnd);
        size_t sp1 = req.headers.find(' ');
        size_t sp2 = req.headers.find(' ', sp1 + 1);
        req.method = req.headers.substr(0, sp1);
        req.path = req.headers.substr(sp1 + 1, sp2 - sp1 - 1);

        size_t contentLength = 0;
        std::string lower = req.headers;
        for (auto& c : lower) c = (char)tolower((unsigned char)c);
        size_t cl = lower.find("content-length:");
        if (cl != std::string::npos) contentLength = (size_t)atol(lower.c_str() + cl + 15);

        req.body = raw.substr(headerEnd + 4);
        while (req.body.size() < contentLength) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            req.body.append(buf, (size_t)n);
        }

        FakeRoute route;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            requests_.push_back(req);
            std::string key = req.path.substr(0, req.path.find('?'));
            auto it = routes_.find(key);
            if (it != routes_.end()) {
                route = it->second;
                found = true;
            }
        }
        if (!found) {
            std::function<bool(const FakeRequest&, FakeRoute&)> h;
            {
                std::lock_guard<std::mutex> lock(mtx_);
                h = handler_;
            }
            if (h) found = h(req, route);
        }
        if (!found) {
            route.status = 404;
            route.body = "{\"error\":\"not found\"}";
        }

        std::string head = "HTTP/1.1 " + std::to_string(route.status) + (route.status == 200 ? " OK" : " Error") +
                           "\r\nContent-Type: " + route.contentType + "\r\nConnection: close\r\n";
        if (route.chunked) {
            head += "Transfer-Encoding: chunked\r\nAccept-Ranges: none\r\n\r\n";
            if (!sendAll(fd, head.data(), head.size())) return;
            size_t pos = 0;
            while (pos < route.body.size()) {
                size_t n = std::min(route.chunkSize, route.body.size() - pos);
                char sz[32];
                snprintf(sz, sizeof(sz), "%zx\r\n", n);
                if (!sendAll(fd, sz, strlen(sz))) return;
                if (!sendAll(fd, route.body.data() + pos, n)) return;
                if (!sendAll(fd, "\r\n", 2)) return;
                pos += n;
            }
            sendAll(fd, "0\r\n\r\n", 5);
        } else {
            head += "Content-Length: " + std::to_string(route.body.size()) + "\r\n\r\n";
            if (!sendAll(fd, head.data(), head.size())) return;
            sendAll(fd, route.body.data(), route.body.size());
        }
    }
};
