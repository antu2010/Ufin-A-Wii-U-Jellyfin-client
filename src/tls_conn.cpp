#include "tls_conn.h"

#include <cstdio>
#include <cstring>
#include <unistd.h>

#if defined(UFIN_TLS_MBEDTLS)
#include "net_time.h"
#include "tls_roots.h"
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mutex>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace tlsconn {

// ---- fingerprints (pure) ------------------------------------------------------------------------------------

std::string normalizeFingerprint(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == ':' || c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return "";
        out.push_back(c);
    }
    return out.size() == 64 ? out : "";
}

std::string formatFingerprint(const std::string& hex) {
    const std::string n = normalizeFingerprint(hex);
    if (n.empty()) return hex;
    std::string out;
    for (size_t i = 0; i < n.size(); i += 2) {
        if (i) out.push_back(':');
        out.push_back((char)toupper((unsigned char)n[i]));
        out.push_back((char)toupper((unsigned char)n[i + 1]));
    }
    return out;
}

#if defined(UFIN_TLS_MBEDTLS)

namespace {

std::string g_extraRoots; // tests: an additional root, as PEM

// ---- randomness ----------------------------------------------------------------------------------------------
//
// There is no /dev/urandom here. One process-wide pool accumulates everything unpredictable the run offers:
//   * the clocks, the address-space layout and a counter (all of it guessable by someone who knows the console),
//   * timing jitter: how long a small amount of work takes, measured with the fast clock, many times over,
//   * whatever the input code feeds in through addEntropy() (touch positions, button timings).
// The pool is hashed forward (never reset), so every connection starts from everything seen before it. That is
// still not a hardware random generator, and this is documented as such: it is what keeps session keys unguessable
// for anyone who is not already inside the console, which is the threat a LAN password needs protecting from.
std::mutex g_poolMutex;
unsigned char g_pool[32];
bool g_poolInit = false;
unsigned long g_poolCounter = 0;

void poolMix(const void* data, size_t len) { // g_poolMutex held
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts_ret(&sha, 0);
    mbedtls_sha256_update_ret(&sha, g_pool, sizeof(g_pool));
    mbedtls_sha256_update_ret(&sha, (const unsigned char*)data, len);
    mbedtls_sha256_finish_ret(&sha, g_pool);
    mbedtls_sha256_free(&sha);
}

// Times a little work with the fast clock; the low bits of those times are where the noise is.
uint64_t jitterSample() {
    uint64_t acc = 0;
    volatile unsigned scratch[64];
    for (int round = 0; round < 48; round++) {
        const auto t0 = std::chrono::steady_clock::now();
        unsigned x = (unsigned)round * 2654435761u;
        for (int i = 0; i < 64; i++) { x = x * 1664525u + 1013904223u; scratch[i] = x; }
        const auto t1 = std::chrono::steady_clock::now();
        acc = (acc << 5) ^ (acc >> 59) ^ (uint64_t)(t1 - t0).count() ^ (uint64_t)t1.time_since_epoch().count();
    }
    return acc;
}

int ufinEntropy(void*, unsigned char* out, size_t len, size_t* olen) {
    int stackProbe = 0;
    void* heapProbe = malloc(24);
    unsigned long long mix[10];
    mix[0] = (unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count();
    mix[1] = (unsigned long long)std::chrono::system_clock::now().time_since_epoch().count();
    mix[2] = (unsigned long long)(size_t)&stackProbe;
    mix[3] = (unsigned long long)(size_t)heapProbe;
    mix[5] = (unsigned long long)clock();
    mix[6] = (unsigned long long)time(nullptr);
    mix[7] = (unsigned long long)(size_t)&mix;
    mix[8] = jitterSample();
    mix[9] = jitterSample();
    free(heapProbe);
    unsigned char digest[32];
    size_t done = 0;
    std::lock_guard<std::mutex> l(g_poolMutex);
    mix[4] = ++g_poolCounter;
    poolMix(mix, sizeof(mix));
    for (unsigned char block = 0; done < len; block++) {
        mbedtls_sha256_context sha;
        mbedtls_sha256_init(&sha);
        mbedtls_sha256_starts_ret(&sha, 0);
        mbedtls_sha256_update_ret(&sha, g_pool, sizeof(g_pool));
        mbedtls_sha256_update_ret(&sha, &block, 1);
        mbedtls_sha256_finish_ret(&sha, digest);
        mbedtls_sha256_free(&sha);
        const size_t n = len - done < sizeof(digest) ? len - done : sizeof(digest);
        memcpy(out + done, digest, n);
        done += n;
    }
    // The output is never the pool itself: step it so two requests never see the same bytes.
    poolMix("out", 3);
    *olen = len;
    return 0;
}

std::string tlsError(const char* what, int code) {
    char buf[128];
    mbedtls_strerror(code, buf, sizeof(buf));
    char num[24];
    snprintf(num, sizeof(num), " (-0x%04X)", (unsigned)-code);
    return std::string(what) + ": " + buf + num;
}

int sendCb(void* ctx, const unsigned char* buf, size_t len) {
    const int fd = *(int*)ctx;
    const ssize_t n = send(fd, buf, len, 0);
    if (n >= 0) return (int)n;
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS) ? MBEDTLS_ERR_SSL_WANT_WRITE : -0x004E;
}
int recvCb(void* ctx, unsigned char* buf, size_t len) {
    const int fd = *(int*)ctx;
    const ssize_t n = recv(fd, buf, len, 0);
    if (n > 0) return (int)n;
    if (n == 0) return 0; // closed
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_READ : -0x004C;
}

std::string hex(const unsigned char* p, size_t n) {
    static const char d[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s.push_back(d[p[i] >> 4]); s.push_back(d[p[i] & 15]); }
    return s;
}

class MbedSession : public Session {
public:
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    mbedtls_x509_crt roots;
    int sock = -1;
    bool established = false;

    MbedSession() {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        mbedtls_ssl_config_init(&conf);
        mbedtls_ssl_init(&ssl);
        mbedtls_x509_crt_init(&roots);
    }
    ~MbedSession() override {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_x509_crt_free(&roots);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
        if (sock >= 0) ::close(sock);
    }
    int read(void* buf, int len) override {
        const int n = mbedtls_ssl_read(&ssl, (unsigned char*)buf, (size_t)len);
        if (n > 0) return n;
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) return -2;
        if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        return -1;
    }
    bool write(const void* buf, int len) override {
        const unsigned char* p = (const unsigned char*)buf;
        int left = len, spins = 0;
        while (left > 0) {
            const int n = mbedtls_ssl_write(&ssl, p, (size_t)left);
            if (n > 0) { p += n; left -= n; spins = 0; }
            else if (n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_WANT_READ) {
                if (++spins > 2000) return false;
                nettime::sleepMs(5);
            } else return false;
        }
        return true;
    }
    int pending() const override { return (int)mbedtls_ssl_get_bytes_avail(&ssl); }
    int fd() const override { return sock; }
    void close() override {
        if (established) mbedtls_ssl_close_notify(&ssl); // best effort
        established = false;
    }
};

void describe(const mbedtls_x509_crt* crt, CertInfo& info) {
    info.have = true;
    unsigned char digest[32];
    mbedtls_sha256_ret(crt->raw.p, crt->raw.len, digest, 0);
    info.fingerprint = hex(digest, 32);
    char buf[256];
    if (mbedtls_x509_dn_gets(buf, sizeof(buf), &crt->subject) > 0) info.subject = buf;
    if (mbedtls_x509_dn_gets(buf, sizeof(buf), &crt->issuer) > 0) info.issuer = buf;
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", crt->valid_to.year, crt->valid_to.mon, crt->valid_to.day);
    info.validTo = buf;
}

} // namespace

void setExtraRootsForTests(const std::string& pem) { g_extraRoots = pem; }

void addEntropy(uint64_t value) {
    if (!g_poolMutex.try_lock()) return; // never make the input code wait
    poolMix(&value, sizeof(value));
    unsigned long long t = (unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count();
    poolMix(&t, sizeof(t));
    g_poolMutex.unlock();
}

std::shared_ptr<Session> connect(int fd, const std::string& host, const Policy& policy, int timeoutSeconds,
                                 CertInfo* info, std::string& error) {
    auto tls = std::make_shared<MbedSession>();
    tls->sock = fd; // from here on the session owns (and closes) the socket
    CertInfo local;
    CertInfo& ci = info ? *info : local;
    ci = CertInfo();
    const int64_t deadline = nettime::nowMs() + (int64_t)(timeoutSeconds > 0 ? timeoutSeconds : 8) * 1000;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    int rc = mbedtls_entropy_add_source(&tls->entropy, ufinEntropy, nullptr, 32, MBEDTLS_ENTROPY_SOURCE_STRONG);
    if (rc == 0) rc = mbedtls_ctr_drbg_seed(&tls->drbg, mbedtls_entropy_func, &tls->entropy, (const unsigned char*)"ufin", 4);
    if (rc != 0) { error = tlsError("TLS random generator", rc); return nullptr; }
    const char* pem = ufinTlsRootsPem();
    rc = mbedtls_x509_crt_parse(&tls->roots, (const unsigned char*)pem, strlen(pem) + 1);
    if (rc < 0) { error = tlsError("TLS root certificates", rc); return nullptr; }
    if (!g_extraRoots.empty()) mbedtls_x509_crt_parse(&tls->roots, (const unsigned char*)g_extraRoots.c_str(), g_extraRoots.size() + 1);
    if (mbedtls_ssl_config_defaults(&tls->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        error = "TLS configuration failed";
        return nullptr;
    }
    mbedtls_ssl_conf_min_version(&tls->conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3); // TLS 1.2
    mbedtls_ssl_conf_authmode(&tls->conf, MBEDTLS_SSL_VERIFY_OPTIONAL); // judged by hand below
    mbedtls_ssl_conf_ca_chain(&tls->conf, &tls->roots, nullptr);
    mbedtls_ssl_conf_rng(&tls->conf, mbedtls_ctr_drbg_random, &tls->drbg);
    // The name is sent (SNI) and checked unless the user pinned this server's certificate: a self-signed certificate
    // made for "jellyfin" or a different address is common, and the pin is stronger than a name anyway.
    if (mbedtls_ssl_setup(&tls->ssl, &tls->conf) != 0 || mbedtls_ssl_set_hostname(&tls->ssl, host.c_str()) != 0) {
        error = "TLS setup failed";
        return nullptr;
    }
    mbedtls_ssl_set_bio(&tls->ssl, &tls->sock, sendCb, recvCb, nullptr);

    while ((rc = mbedtls_ssl_handshake(&tls->ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            error = tlsError("TLS handshake failed", rc);
            return nullptr;
        }
        if (nettime::nowMs() > deadline) { error = "TLS handshake timed out"; return nullptr; }
        nettime::sleepMs(5);
    }

    // What the server showed us.
    const mbedtls_x509_crt* peer = mbedtls_ssl_get_peer_cert(&tls->ssl);
    if (peer) describe(peer, ci);

    uint32_t flags = mbedtls_ssl_get_verify_result(&tls->ssl);
    const uint32_t timeFlags = MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE;
    if (flags & timeFlags) {
        const time_t now = time(nullptr);
        if (now < 1735689600) flags &= ~timeFlags; // before 2025-01-01: the console clock is unset, not the certificate
    }
    ci.trusted = (flags == 0) && peer;
    if (flags != 0) {
        char buf[160];
        mbedtls_x509_crt_verify_info(buf, sizeof(buf), "", flags);
        std::string text = buf;
        while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
        ci.problem = text;
    }

    if (!policy.reportOnly) {
        if (!policy.pin.empty()) {
            if (!peer) { error = "the server sent no certificate"; return nullptr; }
            if (ci.fingerprint != normalizeFingerprint(policy.pin)) {
                error = "the server's certificate is not the one you trusted (its fingerprint changed)";
                return nullptr;
            }
        } else if (flags != 0) {
            error = "the server certificate was refused: " + ci.problem;
            return nullptr;
        }
    }
    tls->established = true;
    return tls;
}

#else // no TLS in this build

void setExtraRootsForTests(const std::string&) {}
void addEntropy(uint64_t) {}

std::shared_ptr<Session> connect(int fd, const std::string&, const Policy&, int, CertInfo* info, std::string& error) {
    if (info) *info = CertInfo();
    error = "TLS not available in this build";
    if (fd >= 0) close(fd);
    return nullptr;
}

#endif

} // namespace tlsconn
