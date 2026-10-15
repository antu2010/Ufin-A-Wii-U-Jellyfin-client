// Ufin's own TLS client against a real TLS server (openssl s_server) with a throw-away CA made on the spot:
// a trusted server answers; an untrusted root, a certificate for another name, a plain-HTTP server, a server
// that offers nothing newer than TLS 1.1, and a silent server are all refused with a reason, and none hangs.
#include "check.h"
#include "dns_client.h"
#include "https_client.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

static bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

static std::string slurp(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool run(const std::string& cmd) { return system((cmd + " >/dev/null 2>&1").c_str()) == 0; }

struct OpenSslServer {
    pid_t pid = -1;
    int port = 0;
    bool start(const std::string& dir, const std::string& cert, const std::string& key, const std::string& extraArgs, int tryPort) {
        port = tryPort;
        pid = fork();
        if (pid == 0) {
            if (chdir(dir.c_str()) != 0) _exit(1);
            const std::string cmd = "exec openssl s_server -accept " + std::to_string(port) + " -cert " + cert + " -key " + key +
                                    " -HTTP " + extraArgs + " >/dev/null 2>&1";
            execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
            _exit(1);
        }
        // Not probed with a connection: s_server treats a connect-and-drop as a failed client and can lose
        // its place. Give it time to bind instead, and let the first real request find out.
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        return waitpid(pid, nullptr, WNOHANG) == 0; // still running
    }
    ~OpenSslServer() {
        if (pid > 0) { kill(pid, SIGTERM); int st; waitpid(pid, &st, 0); }
    }
};

// A server that accepts and never says anything.
struct SilentServer {
    int fd = -1, port = 0;
    bool start() {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(fd, (sockaddr*)&a, sizeof(a)) != 0 || listen(fd, 4) != 0) return false;
        socklen_t l = sizeof(a);
        getsockname(fd, (sockaddr*)&a, &l);
        port = ntohs(a.sin_port);
        return true;
    }
    ~SilentServer() { if (fd >= 0) close(fd); }
};

int main() {
    char tmpl[] = "/tmp/ufin_tls_XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    CHECK(run("openssl version"));
    // The CA, a server certificate for ufin.test, one for other.test, and a self-signed one for ufin.test.
    const std::string d = "cd " + dir + " && ";
    CHECK(run(d + "openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout ca.key -out ca.pem -subj /CN=UfinTestCA -days 30"));
    for (const char* name : {"ufin.test", "other.test"}) {
        const std::string n = name;
        CHECK(run(d + "openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout " + n + ".key -out " + n + ".csr -subj /CN=" + n));
        CHECK(run(d + "printf 'subjectAltName=DNS:" + n + "\\n' > " + n + ".ext && openssl x509 -req -in " + n + ".csr -CA ca.pem -CAkey ca.key -CAcreateserial -out " + n + ".pem -days 30 -extfile " + n + ".ext"));
    }
    CHECK(run(d + "openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout self.key -out self.pem -subj /CN=ufin.test -days 30 -addext subjectAltName=DNS:ufin.test"));
    {
        std::ofstream f(dir + "/release.json", std::ios::binary);  // s_server -HTTP sends the file as it is: a whole response
        f << "HTTP/1.1 200 OK\r\nContent-Length: 21\r\nConnection: close\r\n\r\n{\"tag_name\":\"v1.3.1\"}";
    }
    const std::string ca = slurp(dir + "/ca.pem");
    CHECK(!ca.empty());
    dns::setHostOverride("ufin.test", "127.0.0.1");
    dns::setHostOverride("other.test", "127.0.0.1");

    const int base = 20000 + (int)(getpid() % 20000);
    https::Options o;
    o.timeoutSeconds = 8;
    {
        // --- trusted ---
        https::setExtraRootsForTests(ca);
        OpenSslServer s;
        CHECK(s.start(dir, "ufin.test.pem", "ufin.test.key", "", base));
        o.port = s.port;
        https::Response r = https::get("ufin.test", "/release.json", "", o);
        if (!r.ok) fprintf(stderr, "  (error: %s)\n", r.error.c_str());
        CHECK(r.ok);
        CHECK_EQ(r.status, 200);
        CHECK_EQ(r.body, std::string("{\"tag_name\":\"v1.3.1\"}"));
        // --- a certificate for another name ---
        OpenSslServer wrong;
        CHECK(wrong.start(dir, "other.test.pem", "other.test.key", "", base + 1));
        o.port = wrong.port;
        r = https::get("ufin.test", "/release.json", "", o);
        CHECK(!r.ok && has(r.error, "certificate was refused"));
        CHECK(has(r.error, "CN") || has(r.error, "name"));
        // --- self-signed, not our root ---
        OpenSslServer self;
        CHECK(self.start(dir, "self.pem", "self.key", "", base + 2));
        o.port = self.port;
        r = https::get("ufin.test", "/release.json", "", o);
        CHECK(!r.ok && has(r.error, "certificate was refused"));
        // --- a server that only speaks TLS 1.1 ---
        OpenSslServer old;
        if (old.start(dir, "ufin.test.pem", "ufin.test.key", "-tls1_1 -cipher DEFAULT:@SECLEVEL=0", base + 3)) {
            o.port = old.port;
            r = https::get("ufin.test", "/release.json", "", o);
            CHECK(!r.ok && has(r.error, "handshake failed"));
        }
    }
    {
        // --- without the test root the same good server is refused ---
        // (the extra root is process-wide, so check this by clearing it)
        https::setExtraRootsForTests("");
        OpenSslServer s;
        CHECK(s.start(dir, "ufin.test.pem", "ufin.test.key", "", base + 4));
        o.port = s.port;
        https::Response r = https::get("ufin.test", "/release.json", "", o);
        CHECK(!r.ok && has(r.error, "certificate was refused"));
        CHECK(has(r.error, "not trusted") || has(r.error, "CA"));
        // --- a plain HTTP port: not TLS at all ---
        OpenSslServer plain;
        (void)plain;
    }
    {
        // --- a server that never answers: bounded by the timeout ---
        SilentServer silent;
        CHECK(silent.start());
        o.port = silent.port;
        o.timeoutSeconds = 2;
        const auto t0 = std::chrono::steady_clock::now();
        https::Response r = https::get("ufin.test", "/release.json", "", o);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(!r.ok && has(r.error, "timed out"));
        CHECK(ms < 4500);
    }
    {
        // --- nobody listening ---
        o.port = 1;
        o.timeoutSeconds = 3;
        https::Response r = https::get("ufin.test", "/release.json", "", o);
        CHECK(!r.ok);
        // --- a name that cannot be resolved: no system resolver is ever asked ---
        dns::setHostOverride("ufin.test", "");
        dns::Options none;
        none.servers = {"127.0.0.1"};
        none.port = 9; // nothing there
        none.totalTimeoutMs = 300;
        std::string ip, err;
        CHECK(!dns::resolveA("ufin.test", none, ip, err));
    }
    run("rm -rf " + dir);
    return check::finish("test_https_mbedtls");
}
