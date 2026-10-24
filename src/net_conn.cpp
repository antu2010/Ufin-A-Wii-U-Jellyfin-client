#include "net_conn.h"

#include "http_client.h"
#include "net_time.h"
#include "tls_conn.h"

#include <cctype>
#include <cerrno>
#include <map>
#include <mutex>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace net {

namespace {
struct ServerTls {
    std::string pin;
};
std::mutex g_mutex;
std::map<std::string, ServerTls>& table() {
    static std::map<std::string, ServerTls> t;
    return t;
}
std::string key(const std::string& host, int port) {
    std::string k;
    for (char c : host) k.push_back((char)tolower((unsigned char)c));
    return k + ":" + std::to_string(port);
}
} // namespace

void setServerTls(const std::string& host, int port, bool https, const std::string& pin) {
    std::lock_guard<std::mutex> l(g_mutex);
    if (!https) { table().erase(key(host, port)); return; }
    table()[key(host, port)].pin = tlsconn::normalizeFingerprint(pin);
}

bool serverTls(const std::string& host, int port, std::string* pin) {
    std::lock_guard<std::mutex> l(g_mutex);
    auto it = table().find(key(host, port));
    if (it == table().end()) return false;
    if (pin) *pin = it->second.pin;
    return true;
}

bool probeCertificate(const std::string& host, int port, int timeoutSeconds, tlsconn::CertInfo& info, std::string& error) {
    info = tlsconn::CertInfo();
    const int fd = http_connect_tcp(host, port, timeoutSeconds, error);
    if (fd < 0) return false;
    tlsconn::Policy policy;
    policy.reportOnly = true;
    std::shared_ptr<tlsconn::Session> s = tlsconn::connect(fd, host, policy, timeoutSeconds, &info, error); // (closes fd)
    if (!s) return false;
    s->close();
    return info.have;
}

Conn::Conn() {}
Conn::~Conn() { close(); }

bool Conn::open(const std::string& host, int port, int timeoutSeconds, std::string& error) {
    close();
    std::string pin;
    const bool useTls = serverTls(host, port, &pin);
    const int fd = http_connect_tcp(host, port, timeoutSeconds, error);
    if (fd < 0) return false;
    if (!useTls) {
        fd_ = fd;
        return true;
    }
    tlsconn::Policy policy;
    policy.pin = pin;
    tls_ = tlsconn::connect(fd, host, policy, timeoutSeconds, nullptr, error); // (closes fd on failure)
    return (bool)tls_;
}

bool Conn::sendAll(const void* data, size_t len) {
    if (tls_) return tls_->write(data, (int)len);
    const char* p = (const char*)data;
    size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::send(fd_, p + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static bool selectReadable(int fd, int ms) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(fd, &readSet);
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return select(fd + 1, &readSet, nullptr, nullptr, &tv) > 0;
}

bool Conn::waitReadable(int seconds) {
    if (tls_ && tls_->pending() > 0) return true;
    return selectReadable(tls_ ? tls_->fd() : fd_, seconds * 1000);
}

int Conn::recv(void* buf, int len, int timeoutSeconds, const std::function<bool()>* abort) {
    const bool abortable = abort && *abort;
    const int64_t deadline = nettime::nowMs() + (int64_t)timeoutSeconds * 1000;
    for (;;) {
        if (tls_ && tls_->pending() > 0) {
            const int r = tls_->read(buf, len);
            if (r != -2) return r > 0 ? r : (r == 0 ? Closed : Error);
        }
        if (abortable && (*abort)()) return Aborted;
        const int fd = tls_ ? tls_->fd() : fd_;
        // Without an abort check this is the single select() the plain code always had.
        int sliceMs = abortable ? 50 : timeoutSeconds * 1000;
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(fd, &readSet);
        struct timeval tv;
        tv.tv_sec = sliceMs / 1000;
        tv.tv_usec = (sliceMs % 1000) * 1000;
        const int sel = select(fd + 1, &readSet, nullptr, nullptr, &tv);
        if (sel > 0) {
            if (!tls_) {
                const ssize_t n = ::recv(fd_, buf, (size_t)len, 0);
                return n > 0 ? (int)n : (n == 0 ? Closed : Error);
            }
            const int r = tls_->read(buf, len);
            if (r == -2) { // part of a record arrived: keep waiting for the rest
                if (nettime::nowMs() >= deadline) return TimedOut;
                continue;
            }
            return r > 0 ? r : (r == 0 ? Closed : Error);
        }
        if (sel < 0 && errno != EINTR) return TimedOut; // select itself failed: "not readable", as before
        if (!abortable || nettime::nowMs() >= deadline) return TimedOut;
    }
}

void Conn::close() {
    if (tls_) {
        tls_->close();
        tls_.reset(); // closes the socket
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

} // namespace net
