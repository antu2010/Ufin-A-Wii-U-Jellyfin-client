// A TLS connection over an already connected socket, for everything Ufin sends to a Jellyfin server over https:// and
// for the update check (https_client.h). Mbed TLS (vendored in src/vendor/mbedtls), TLS 1.2 client; without
// UFIN_TLS_MBEDTLS (the host tests of the plain HTTP code) connect() simply reports "TLS not available".
//
// Which certificates are accepted:
//   * no pin: the certificate must chain to one of our roots (tls_roots.cpp) and match the host name -- what a server
//     with a real certificate (Let's Encrypt behind a reverse proxy, ...) gets;
//   * a pin (the SHA-256 fingerprint of one certificate): exactly that certificate is accepted, whoever signed it and
//     whatever its name or dates say -- how a self-signed Jellyfin certificate is used safely. The user is shown the
//     fingerprint once and chooses to trust it; a different certificate later is refused, never silently accepted.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace tlsconn {

struct CertInfo {
    bool have = false;         // the server sent a certificate
    std::string fingerprint;   // SHA-256 of the DER certificate: 64 lowercase hex digits
    std::string subject;       // "CN=jellyfin.lan"
    std::string issuer;
    std::string validTo;       // "2027-01-31"
    bool trusted = false;      // chains to a root we trust AND matches the host name
    std::string problem;       // when !trusted: why ("the certificate is not signed by a trusted authority", ...)
};

struct Policy {
    std::string pin;           // see the top of this file; "" = verify normally
    bool reportOnly = false;   // never refuse a certificate: connect() succeeds and `info` says what it was (the
                               // "do you trust this?" probe)
};

class Session {
public:
    virtual ~Session() {}
    // >0 bytes read, 0 the peer closed, -1 error, -2 nothing yet (wait on fd(), try again).
    virtual int read(void* buf, int len) = 0;
    // true if everything was written.
    virtual bool write(const void* buf, int len) = 0;
    // Decrypted bytes already buffered inside the library: read() has them without waiting on the socket.
    virtual int pending() const = 0;
    virtual int fd() const = 0;
    // Tells the peer we are done (best effort). The socket itself is closed when the session is destroyed.
    virtual void close() = 0;
};

// Handshake over `fd` (connected; ownership passes to the session, which closes it -- also when this fails).
// `info`, if given, is filled in as far as the handshake got, also on failure. nullptr + `error` on failure.
std::shared_ptr<Session> connect(int fd, const std::string& host, const Policy& policy, int timeoutSeconds,
                                 CertInfo* info, std::string& error);

// Tests only: one more trusted root (PEM) besides the built-in ones. "" removes it.
void setExtraRootsForTests(const std::string& pem);

// Feeds the random generator something only this run knows (a touch position, a button timing...). Cheap; call it
// freely from the input code.
void addEntropy(uint64_t value);

// "ab12..." (64 hex digits) -> "AB:12:..." for showing; and back (any case, with or without the colons / spaces).
// normalizeFingerprint() returns "" for anything that is not 64 hex digits.
std::string formatFingerprint(const std::string& hex);
std::string normalizeFingerprint(const std::string& text);

} // namespace tlsconn
