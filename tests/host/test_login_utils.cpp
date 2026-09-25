// Server address parsing for the sign-in screen.
#include "check.h"
#include "login_utils.h"

int main() {
    std::string host, err;
    int port = 0;
    CHECK(parseServerAddress("192.168.1.100", host, port, err));
    CHECK_STR(host, "192.168.1.100"); CHECK_EQ(port, 8096);
    CHECK(parseServerAddress("  192.168.1.100:8097 ", host, port, err));
    CHECK_STR(host, "192.168.1.100"); CHECK_EQ(port, 8097);
    CHECK(parseServerAddress("http://Jelly.local:8096/web/index.html", host, port, err));
    CHECK_STR(host, "Jelly.local"); CHECK_EQ(port, 8096);
    CHECK(parseServerAddress("HTTP://127.0.0.1", host, port, err));
    CHECK_STR(host, "127.0.0.1");
    CHECK(!parseServerAddress("https://jelly.example.com", host, port, err));
    CHECK(err.find("https") != std::string::npos);
    CHECK(!parseServerAddress("192.168.1.100:abc", host, port, err));
    CHECK(!parseServerAddress("192.168.1.100:99999", host, port, err));
    CHECK(!parseServerAddress("192.168.1.100:", host, port, err));
    CHECK(!parseServerAddress("", host, port, err));
    CHECK(!parseServerAddress("my server", host, port, err));
    CHECK_STR(formatServerAddress("192.168.1.100", 8096), "192.168.1.100");
    CHECK_STR(formatServerAddress("192.168.1.100", 8097), "192.168.1.100:8097");
    CHECK_STR(formatServerAddress("", 8096), "");

    return check::finish("test_login_utils");
}
