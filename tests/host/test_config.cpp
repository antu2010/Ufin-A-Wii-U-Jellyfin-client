#include "check.h"
#include "config_loader.h"

#include <cstdio>
#include <string>

static std::string writeTemp(const char* name, const std::string& content) {
    std::string path = std::string("/tmp/ufin_test_") + name + ".json";
    FILE* f = fopen(path.c_str(), "wb");
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
    return path;
}

int main() {
    UfinConfig cfg;
    std::string err;

    // Missing file.
    CHECK(!loadConfigFromFile("/tmp/ufin_definitely_missing.json", cfg, err));
    CHECK_STR(err, "config.json not found");

    // Full config with optional video keys.
    std::string full = writeTemp("full",
        "{\"host\":\"10.0.0.5\",\"port\":8096,\"username\":\"u\",\"password\":\"p\","
        "\"video_bitrate\":4000000,\"video_profile\":\"main\"}");
    CHECK(loadConfigFromFile(full.c_str(), cfg, err));
    CHECK_STR(cfg.host, "10.0.0.5");
    CHECK_EQ(cfg.port, 8096);
    CHECK_STR(cfg.username, "u");
    CHECK_STR(cfg.password, "p");
    CHECK_EQ(cfg.videoBitrate, 4000000);
    CHECK_STR(cfg.videoProfile, "main");

    // Minimal config keeps video defaults.
    UfinConfig minimal;
    std::string min = writeTemp("min", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\"}");
    CHECK(loadConfigFromFile(min.c_str(), minimal, err));
    CHECK_EQ(minimal.videoBitrate, 2500000);
    CHECK_STR(minimal.videoProfile, "baseline");

    // Bad optional values are ignored, not fatal.
    UfinConfig odd;
    std::string oddPath = writeTemp("odd",
        "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"video_bitrate\":\"fast\",\"video_profile\":\"\"}");
    CHECK(loadConfigFromFile(oddPath.c_str(), odd, err));
    CHECK_EQ(odd.videoBitrate, 2500000);
    CHECK_STR(odd.videoProfile, "baseline");

    // No password: still loads (the login screen fills the gap), but
    // isn't enough to sign in on its own.
    std::string missing = writeTemp("missing", "{\"host\":\"h\",\"port\":1,\"username\":\"u\"}");
    CHECK(loadConfigFromFile(missing.c_str(), cfg, err));
    CHECK(!hasLogin(cfg));
    CHECK(hasLogin(odd));

    // A saved token is enough without a password.
    UfinConfig tok;
    std::string tokPath = writeTemp("tok",
        "{\"host\":\"h\",\"port\":8096,\"username\":\"u\",\"user_id\":\"id1\",\"access_token\":\"t0k\","
        "\"device_id\":\"dev\",\"menu_music\":true,\"menu_music_item\":\"song\",\"menu_music_name\":\"Tune\","
        "\"crt\":true}");
    CHECK(loadConfigFromFile(tokPath.c_str(), tok, err));
    CHECK(hasLogin(tok));
    CHECK_STR(tok.accessToken, "t0k");
    CHECK_STR(tok.userId, "id1");
    CHECK_STR(tok.deviceId, "dev");
    CHECK(tok.menuMusicEnabled);
    CHECK_STR(tok.menuMusicItemId, "song");
    CHECK_STR(tok.menuMusicName, "Tune");
    CHECK(tok.crt);
    CHECK_EQ(tok.accent, 0); // look & feel defaults when absent
    CHECK(!tok.ambient);
    CHECK(!tok.snow);
    CHECK(!tok.clock);
    CHECK(!tok.rainbowUnlocked);
    CHECK(tok.autoplayNext);        // on unless turned off
    CHECK(!tok.gamepadOffInVideo);
    UfinConfig noHost = tok;
    noHost.host.clear();
    CHECK(!hasLogin(noHost));

    // Saving: round-trips, keeps unknown keys, never adds a password.
    {
        std::string path = writeTemp("save", "{\"host\":\"old\",\"my_note\":\"keep me\",\"port\":1}");
        UfinConfig c;
        c.host = "192.168.1.100";
        c.port = 8096;
        c.username = "alex";
        c.userId = "uid";
        c.accessToken = "abc";
        c.deviceId = "ufin-123";
        c.menuMusicEnabled = true;
        c.menuMusicItemId = "track9";
        c.menuMusicName = "Citt\xc3\xa0";
        c.crt = true;
        c.accent = 3;
        c.ambient = true;
        c.snow = true;
        c.clock = true;
        c.rainbowUnlocked = true;
        c.autoplayNext = false;
        c.gamepadOffInVideo = true;
        CHECK(saveConfigToFile(path.c_str(), c, err));
        UfinConfig back;
        CHECK(loadConfigFromFile(path.c_str(), back, err));
        CHECK_STR(back.host, "192.168.1.100");
        CHECK_EQ(back.port, 8096);
        CHECK_STR(back.accessToken, "abc");
        CHECK_STR(back.menuMusicName, "Citt\xc3\xa0");
        CHECK(back.crt);
        CHECK_EQ(back.accent, 3);
        CHECK(back.ambient);
        CHECK(back.snow);
        CHECK(back.clock);
        CHECK(back.rainbowUnlocked);
        CHECK(!back.autoplayNext);
        CHECK(back.gamepadOffInVideo);
        CHECK_STR(back.password, "");
        FILE* f = fopen(path.c_str(), "rb");
        char text[4096] = {0};
        fread(text, 1, sizeof(text) - 1, f);
        fclose(f);
        std::string t = text;
        CHECK(t.find("keep me") != std::string::npos);
        CHECK(t.find("password") == std::string::npos);

        // A password the user put in the file stays there.
        std::string withPw = writeTemp("savepw", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"pw\"}");
        UfinConfig p2;
        CHECK(loadConfigFromFile(withPw.c_str(), p2, err));
        p2.accessToken = "t";
        CHECK(saveConfigToFile(withPw.c_str(), p2, err));
        UfinConfig p3;
        CHECK(loadConfigFromFile(withPw.c_str(), p3, err));
        CHECK_STR(p3.password, "pw");
        CHECK_STR(p3.accessToken, "t");

        // Unwritable location: an error, not a crash.
        CHECK(!saveConfigToFile("/nonexistent-dir/x/config.json", c, err));
        CHECK(!err.empty());
    }

    // Invalid JSON and empty file.
    std::string invalid = writeTemp("invalid", "{host: nope");
    CHECK(!loadConfigFromFile(invalid.c_str(), cfg, err));
    CHECK_STR(err, "config.json is not valid JSON");
    std::string empty = writeTemp("empty", "");
    CHECK(!loadConfigFromFile(empty.c_str(), cfg, err));
    CHECK_STR(err, "config.json is empty");

    return check::finish("test_config");
}
