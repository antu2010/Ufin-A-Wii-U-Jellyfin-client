// One client connection to a Jellyfin server -- plain TCP, or TCP + TLS when that server was set up for https --
// for http_client.h (JSON requests, images) and http_stream_reader.h (video / audio streams).
//
// Which servers use TLS is a small process-wide table (setServerTls), filled in when the user signs in to / switches to
// a server: every request to that host:port then goes over TLS, whichever code makes it, so none of the call sites
// has to know. A server that is not in the table is spoken to in plain HTTP exactly as before.
#pragma once
#include <functional>
#include <memory>
#include <string>

#include "tls_conn.h"

namespace net {

// host:port speaks https. `pin` = the SHA-256 fingerprint (hex) of the one certificate to accept (a self-signed
// certificate the user chose to trust); "" = the certificate must be one a public authority signed for the host name.
void setServerTls(const std::string& host, int port, bool https, const std::string& pin);
// Is host:port set up for https? `pin` receives its pin (if any).
bool serverTls(const std::string& host, int port, std::string* pin = nullptr);

// What certificate does host:port show? (TLS handshake in report-only mode: nothing is refused, nothing is sent.) true
// when a handshake happened -- `info` then describes the certificate and says whether it is one we would accept without
// asking (`info.trusted`). false + `error` when the server could not be reached or does not speak TLS.
bool probeCertificate(const std::string& host, int port, int timeoutSeconds, tlsconn::CertInfo& info, std::string& error);

class Conn {
public:
    Conn();
    ~Conn();
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;

    // Connects (and does the TLS handshake when the server is set up for it). false + `error` on failure.
    bool open(const std::string& host, int port, int timeoutSeconds, std::string& error);
    bool isOpen() const { return fd_ >= 0 || tls_; }
    bool secure() const { return (bool)tls_; }

    bool sendAll(const void* data, size_t len);

    enum { Closed = 0, Error = -1, TimedOut = -3, Aborted = -4 };
    // Waits up to `timeoutSeconds` for data, then reads up to `len` bytes: >0 bytes, Closed (the peer ended the
    // stream), Error, TimedOut, or Aborted (`abort` returned true; polled every ~50 ms while waiting, when given).
    int recv(void* buf, int len, int timeoutSeconds, const std::function<bool()>* abort = nullptr);
    // Is there something to read right now, or does the next one arrive within `seconds`?
    bool waitReadable(int seconds);

    void close();

private:
    int fd_ = -1;
    std::shared_ptr<tlsconn::Session> tls_;
};

} // namespace net
