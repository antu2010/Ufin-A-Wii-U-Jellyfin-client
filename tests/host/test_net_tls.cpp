// Jellyfin over https: Ufin's HTTP client and stream reader against a real TLS server (openssl s_server).
// A self-signed certificate is refused until the user trusts its fingerprint; then exactly that certificate is
// accepted (a different one is not); a certificate from a trusted authority needs no pin; plain-HTTP servers are
// untouched. Nothing here may hang.
#include "check.h"
#include "http_client.h"
#include "http_stream_reader.h"
#include "net_conn.h"
#include "tls_conn.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

static bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
static bool run(const std::string& cmd) { return system((cmd + " >/dev/null 2>&1").c_str()) == 0; }
static std::string slurp(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static std::string fingerprintOf(const std::string& dir, const std::string& pem) {
    // openssl prints "sha256 Fingerprint=AB:CD:..."
    const std::string out = dir + "/fp.txt";
    if (system(("openssl x509 -in " + dir + "/" + pem + " -noout -fingerprint -sha256 > " + out + " 2>/dev/null").c_str()) != 0) return "";
    std::string t = slurp(out);
    const size_t eq = t.find('=');
    return eq == std::string::npos ? "" : tlsconn::normalizeFingerprint(t.substr(eq + 1));
}

struct OpenSslServer {
    pid_t pid = -1;
    int port = 0;
    bool start(const std::string& dir, const std::string& cert, const std::string& key, int tryPort) {
        port = tryPort;
        pid = fork();
        if (pid == 0) {
            if (chdir(dir.c_str()) != 0) _exit(1);
            const std::string cmd = "exec openssl s_server -accept " + std::to_string(port) + " -cert " + cert + " -key " + key +
                                    " -HTTP >/dev/null 2>&1";
            execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
            _exit(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        return waitpid(pid, nullptr, WNOHANG) == 0;
    }
    ~OpenSslServer() {
        if (pid > 0) { kill(pid, SIGTERM); int st; waitpid(pid, &st, 0); }
    }
};

int main() {
    // fingerprint helpers (pure)
    CHECK_EQ(tlsconn::normalizeFingerprint("AB:cd:" + std::string(60, '0')), std::string("abcd") + std::string(60, '0'));
    CHECK_EQ(tlsconn::normalizeFingerprint("zz"), std::string(""));
    CHECK_EQ(tlsconn::normalizeFingerprint(std::string(63, 'a')), std::string(""));
    CHECK_EQ(tlsconn::formatFingerprint(std::string(64, 'a')).size(), (size_t)95);
    CHECK_EQ(tlsconn::formatFingerprint(std::string(64, 'a')).substr(0, 5), std::string("AA:AA"));

    char tmpl[] = "/tmp/ufin_nettls_XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    CHECK(run("openssl version"));
    const std::string d = "cd " + dir + " && ";
    // a self-signed server certificate (RSA, like a typical Jellyfin one), and a second self-signed one
    CHECK(run(d + "openssl req -x509 -newkey rsa:2048 -sha256 -nodes -keyout self.key -out self.pem -subj /CN=jellyfin -days 30"));
    CHECK(run(d + "openssl req -x509 -newkey rsa:2048 -sha256 -nodes -keyout other.key -out other.pem -subj /CN=jellyfin -days 30"));
    // a CA, and a server certificate it signed for 127.0.0.1 (no SAN)
    CHECK(run(d + "openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout ca.key -out ca.pem -subj /CN=UfinTestCA -days 30"));
    CHECK(run(d + "openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout good.key -out good.csr -subj /CN=127.0.0.1"));
    // (Mbed TLS 2.x checks a host name against DNS names and the CN only, not IP addresses in the SAN: so for an IP the
    // name is in the CN, no SAN. A server reached by IP address with a certificate of any other kind is trusted by pin.)
    CHECK(run(d + "openssl x509 -req -in good.csr -CA ca.pem -CAkey ca.key -CAcreateserial -out good.pem -days 30"));
    {
        std::ofstream f(dir + "/Users", std::ios::binary); // GET /Users -> this file, verbatim (s_server -HTTP)
        f << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 17\r\nConnection: close\r\n\r\n{\"name\":\"jelly\"}\n";
    }
    {
        std::ofstream f(dir + "/stream", std::ios::binary);
        f << "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nConnection: close\r\n\r\n";
        for (int i = 0; i < 40000; i++) f << (char)('a' + i % 26); // 40 kB: several TLS records
    }
    const std::string selfFp = fingerprintOf(dir, "self.pem"), otherFp = fingerprintOf(dir, "other.pem");
    CHECK_EQ(selfFp.size(), (size_t)64);
    CHECK(selfFp != otherFp);
    const std::string ca = slurp(dir + "/ca.pem");

    const int base = 24000 + (int)(getpid() % 20000);
    const std::string host = "127.0.0.1";

    {   // ---- a self-signed server ----
        OpenSslServer s;
        CHECK(s.start(dir, "self.pem", "self.key", base));
        // the server is not "https" in the table yet: plain HTTP against a TLS port just fails, quickly
        net::setServerTls(host, s.port, false, "");
        HttpResponse plain = http_get(host, s.port, "/Users");
        CHECK(!plain.success);
        // https, no pin: the certificate is refused, and the message says why
        net::setServerTls(host, s.port, true, "");
        HttpResponse r = http_get(host, s.port, "/Users");
        CHECK(!r.success);
        CHECK(has(r.body, "certificate was refused"));
        // what the user is shown before deciding: connect in report-only mode
        {
            std::string err;
            int fd = http_connect_tcp(host, s.port, 8, err);
            CHECK(fd >= 0);
            tlsconn::CertInfo info;
            tlsconn::Policy p;
            p.reportOnly = true;
            auto sess = tlsconn::connect(fd, host, p, 8, &info, err);
            CHECK(sess != nullptr);
            CHECK(info.have);
            CHECK(!info.trusted);
            CHECK(!info.problem.empty());
            CHECK_EQ(info.fingerprint, selfFp);
            CHECK(has(info.subject, "CN=jellyfin"));
            CHECK(!info.validTo.empty());
        }
        // the sign-in screen's probe says the same
        {
            tlsconn::CertInfo pi;
            std::string perr;
            CHECK(net::probeCertificate(host, s.port, 8, pi, perr));
            CHECK(!pi.trusted && pi.fingerprint == selfFp);
        }
        // trusted by fingerprint: works (any case, with colons)
        net::setServerTls(host, s.port, true, tlsconn::formatFingerprint(selfFp));
        r = http_get(host, s.port, "/Users");
        if (!r.success) fprintf(stderr, "  (pinned get failed: %s)\n", r.body.c_str());
        CHECK(r.success);
        CHECK_EQ(r.status_code, 200);
        CHECK(has(r.body, "jelly"));
        // POST goes over TLS too (s_server -HTTP answers any request with the file it names; the point is the handshake)
        HttpResponse pr = http_post(host, s.port, "/Users", "{}", "application/json");
        CHECK(pr.status_code == 200 || pr.status_code == 0 || !pr.body.empty());
        // the streaming reader, TLS and all
        {
            HttpStreamReader rd(host, s.port, "/stream");
            CHECK(rd.open());
            std::string got;
            uint8_t buf[5000];
            for (;;) {
                const int n = rd.read(buf, sizeof(buf));
                if (n <= 0) break;
                got.append((const char*)buf, (size_t)n);
            }
            CHECK_EQ(got.size(), (size_t)40000);
            bool same = true;
            for (size_t i = 0; i < got.size(); i++) if (got[i] != (char)('a' + i % 26)) { same = false; break; }
            CHECK(same);
        }
        // a different certificate than the pinned one is refused (the server was swapped)
        net::setServerTls(host, s.port, true, otherFp);
        r = http_get(host, s.port, "/Users");
        CHECK(!r.success && has(r.body, "not the one you trusted"));
        {
            HttpStreamReader rd(host, s.port, "/stream");
            CHECK(!rd.open());
            CHECK(has(rd.lastError(), "not the one you trusted"));
        }
        net::setServerTls(host, s.port, false, "");
    }
    {   // ---- a certificate a trusted authority signed: no pin needed ----
        tlsconn::setExtraRootsForTests(ca);
        OpenSslServer s;
        CHECK(s.start(dir, "good.pem", "good.key", base + 1));
        net::setServerTls(host, s.port, true, "");
        HttpResponse r = http_get(host, s.port, "/Users");
        if (!r.success) fprintf(stderr, "  (trusted get failed: %s)\n", r.body.c_str());
        CHECK(r.success && has(r.body, "jelly"));
        // and the report-only probe says "trusted", so no question is asked
        std::string err;
        int fd = http_connect_tcp(host, s.port, 8, err);
        tlsconn::CertInfo info;
        tlsconn::Policy p;
        p.reportOnly = true;
        auto sess = tlsconn::connect(fd, host, p, 8, &info, err);
        CHECK(sess && info.trusted && info.problem.empty());
        sess.reset();
        // without the test root the same server needs a pin
        tlsconn::setExtraRootsForTests("");
        r = http_get(host, s.port, "/Users");
        CHECK(!r.success && has(r.body, "refused"));
        net::setServerTls(host, s.port, false, "");
    }
    {   // ---- the probe against things that are not TLS servers ----
        tlsconn::CertInfo pi;
        std::string perr;
        CHECK(!net::probeCertificate(host, 1, 3, pi, perr)); // nothing listens there
        CHECK(!perr.empty());
        // a plain-text server: accepts, answers HTTP in the clear
        int ls = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(bind(ls, (sockaddr*)&a, sizeof(a)) == 0 && listen(ls, 2) == 0);
        socklen_t al = sizeof(a);
        getsockname(ls, (sockaddr*)&a, &al);
        std::thread t([ls] {
            int c = accept(ls, nullptr, nullptr);
            if (c >= 0) { const char* r = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n"; send(c, r, strlen(r), 0); usleep(300000); close(c); }
        });
        CHECK(!net::probeCertificate(host, ntohs(a.sin_port), 3, pi, perr));
        CHECK(!perr.empty());
        t.join();
        close(ls);
    }
    {   // ---- the table ----
        CHECK(!net::serverTls("example.org", 1234));
        net::setServerTls("Example.ORG", 1234, true, "");
        CHECK(net::serverTls("example.org", 1234));
        CHECK(!net::serverTls("example.org", 1235));
        std::string pin = "x";
        net::setServerTls("example.org", 1234, true, std::string(64, 'A'));
        CHECK(net::serverTls("example.org", 1234, &pin));
        CHECK_EQ(pin, std::string(64, 'a'));
        net::setServerTls("example.org", 1234, false, "");
        CHECK(!net::serverTls("example.org", 1234));
        tlsconn::addEntropy(12345); // (must not crash or block)
    }
    run("rm -rf " + dir);
    return check::finish("test_net_tls");
}
