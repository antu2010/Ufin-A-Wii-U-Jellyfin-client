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

    // https:// addresses (the https-aware parser)
    {
        bool https = true;
        CHECK(parseServerAddress("192.168.1.100", host, port, https, err)); CHECK(!https); CHECK_EQ(port, 8096);
        CHECK(parseServerAddress("http://jelly.local:8097/", host, port, https, err)); CHECK(!https); CHECK_EQ(port, 8097);
        CHECK(parseServerAddress("https://jelly.example.com", host, port, https, err));
        CHECK(https); CHECK_STR(host, "jelly.example.com"); CHECK_EQ(port, 443);
        CHECK(parseServerAddress("HTTPS://192.168.1.100:8920/web/index.html", host, port, https, err));
        CHECK(https); CHECK_STR(host, "192.168.1.100"); CHECK_EQ(port, 8920);
        CHECK(!parseServerAddress("https://", host, port, https, err));
        CHECK(!parseServerAddress("https://jelly:abc", host, port, https, err));
        // it shows and parses back to the same server
        CHECK_STR(formatServerAddress("jelly.example.com", 443, true), "https://jelly.example.com");
        CHECK_STR(formatServerAddress("192.168.1.100", 8920, true), "https://192.168.1.100:8920");
        CHECK_STR(formatServerAddress("192.168.1.100", 8096, false), "192.168.1.100");
        for (const char* t : {"https://192.168.1.100:8920", "https://jelly.example.com", "192.168.1.100:8097", "192.168.1.100"}) {
            std::string h2; int p2 = 0; bool s2 = false;
            CHECK(parseServerAddress(t, h2, p2, s2, err));
            const std::string shown = formatServerAddress(h2, p2, s2);
            std::string h3; int p3 = 0; bool s3 = true;
            CHECK(parseServerAddress(shown, h3, p3, s3, err));
            CHECK(h2 == h3 && p2 == p3 && s2 == s3);
        }
    }

    return check::finish("test_login_utils");
}
