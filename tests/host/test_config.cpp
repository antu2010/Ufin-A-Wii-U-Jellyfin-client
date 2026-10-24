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
    CHECK_STR(minimal.videoProfile, h264::wireName(h264::kDefault)); // fresh install -> the single default
    CHECK_EQ((int)minimal.usageSeconds, 0);        // the sidebar's use-time counter starts at zero
    CHECK(!minimal.logging);                       // logging wears the SD card: off by default
    CHECK(!minimal.jellyfinBackdrops);             // Jellyfin backdrops are opt-in
    CHECK(minimal.menuMusicEnabled);               // menu music is on by default (plays assets/bgm.m4a)
    CHECK(minimal.menuMusicItemId.empty());        // ...from the local file, until a library song is picked

    // Total use time loads, survives a save/load round trip, and bad values fall back to 0.
    {
        UfinConfig u;
        std::string up = writeTemp("usage", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"usage_seconds\":45245}");
        CHECK(loadConfigFromFile(up.c_str(), u, err));
        CHECK_EQ((int)u.usageSeconds, 45245);
        std::string rt = writeTemp("usagert", "{}");
        std::string serr;
        u.usageSeconds = 8000000000LL; // > 2^31: must not truncate
        CHECK(saveConfigToFile(rt.c_str(), u, serr));
        UfinConfig back;
        CHECK(loadConfigFromFile(rt.c_str(), back, err));
        CHECK(back.usageSeconds == 8000000000LL);
        UfinConfig bad;
        std::string bp = writeTemp("usagebad", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"usage_seconds\":-5}");
        CHECK(loadConfigFromFile(bp.c_str(), bad, err));
        CHECK_EQ((int)bad.usageSeconds, 0);
    }

    // Both new switches load and survive a save/load round trip.
    {
        UfinConfig on;
        std::string onPath = writeTemp("logon", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"logging\":true,\"jellyfin_backdrops\":true}");
        CHECK(loadConfigFromFile(onPath.c_str(), on, err));
        CHECK(on.logging);
        CHECK(on.jellyfinBackdrops);
        std::string rt = writeTemp("logrt", "{}");
        std::string serr;
        CHECK(saveConfigToFile(rt.c_str(), on, serr));
        UfinConfig back;
        CHECK(loadConfigFromFile(rt.c_str(), back, err));
        CHECK(back.logging);
        CHECK(back.jellyfinBackdrops);
    }

    // An explicit "menu_music": false is still honoured.
    {
        UfinConfig off;
        std::string offPath = writeTemp("musicoff", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"menu_music\":false}");
        CHECK(loadConfigFromFile(offPath.c_str(), off, err));
        CHECK(!off.menuMusicEnabled);
    }

    // Bad optional values are ignored, not fatal.
    UfinConfig odd;
    std::string oddPath = writeTemp("odd",
        "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"video_bitrate\":\"fast\",\"video_profile\":\"\"}");
    CHECK(loadConfigFromFile(oddPath.c_str(), odd, err));
    CHECK_EQ(odd.videoBitrate, 2500000);
    CHECK_STR(odd.videoProfile, h264::wireName(h264::kDefault));

    // Profile names are normalised; unknown values fall back to the default.
    {
        UfinConfig hp;
        std::string hi = writeTemp("hprof", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"video_profile\":\"HIGH\"}");
        CHECK(loadConfigFromFile(hi.c_str(), hp, err));
        CHECK_STR(hp.videoProfile, "high");
        UfinConfig hq;
        std::string bad = writeTemp("hprofbad", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"video_profile\":\"ultra\"}");
        CHECK(loadConfigFromFile(bad.c_str(), hq, err));
        CHECK_STR(hq.videoProfile, h264::wireName(h264::kDefault));

        // Persistence round trip for all three choices (what Settings saves and a restart reloads).
        const char* names[] = {"main", "baseline", "high"};
        for (const char* n : names) {
            UfinConfig out; out.host = "h"; out.port = 1; out.username = "u"; out.videoProfile = n;
            std::string path = writeTemp((std::string("hrt_") + n).c_str(), "{}");
            std::string serr;
            CHECK(saveConfigToFile(path.c_str(), out, serr));
            UfinConfig back;
            CHECK(loadConfigFromFile(path.c_str(), back, err));
            CHECK_STR(back.videoProfile, n);
        }
    }

    // Video resolution: default is the stable 720p; 1080p is explicit opt-in.
    {
        CHECK_EQ(minimal.videoHeight, 720);
        CHECK_EQ(minimal.videoWidth(), 1280);
        UfinConfig d;
        CHECK_EQ(d.videoHeight, 720);
        CHECK_EQ(d.videoBitrate, 2500000);

        UfinConfig h;
        std::string p1080 = writeTemp("h1080",
            "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"video_height\":1080}");
        CHECK(loadConfigFromFile(p1080.c_str(), h, err));
        CHECK_EQ(h.videoHeight, 1080);
        CHECK_EQ(h.videoWidth(), 1920);
        CHECK_EQ(h.videoBitrate, 6000000);        // 1080p default, no video_bitrate given
        CHECK_STR(h.videoProfile, h264::wireName(h264::kDefault));

        UfinConfig hb;
        std::string p1080b = writeTemp("h1080b",
            "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"video_height\":1080,\"video_bitrate\":8000000}");
        CHECK(loadConfigFromFile(p1080b.c_str(), hb, err));
        CHECK_EQ(hb.videoBitrate, 8000000);       // explicit bitrate still wins

        UfinConfig hs;
        std::string p720 = writeTemp("h720",
            "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"video_height\":720}");
        CHECK(loadConfigFromFile(p720.c_str(), hs, err));
        CHECK_EQ(hs.videoHeight, 720);
        CHECK_EQ(hs.videoBitrate, 2500000);       // 720p default unchanged

        // 480p: low-bandwidth opt-in, 768x480 (aligned), 1.5 Mbps unless a bitrate is given.
        UfinConfig l;
        std::string p480 = writeTemp("h480",
            "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"video_height\":480}");
        CHECK(loadConfigFromFile(p480.c_str(), l, err));
        CHECK_EQ(l.videoHeight, 480);
        CHECK_EQ(l.videoWidth(), 768);
        CHECK_EQ(l.videoBitrate, 1500000);
        CHECK_STR(l.videoProfile, h264::wireName(h264::kDefault));
        UfinConfig lb;
        std::string p480b = writeTemp("h480b",
            "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"video_height\":480,\"video_bitrate\":750000}");
        CHECK(loadConfigFromFile(p480b.c_str(), lb, err));
        CHECK_EQ(lb.videoBitrate, 750000);        // explicit bitrate still wins
        UfinConfig l480rt = lb;
        std::string l480Path = "/tmp/ufin_test_h480_rt.json";
        CHECK(saveConfigToFile(l480Path.c_str(), l480rt, err));
        UfinConfig l480back;
        CHECK(loadConfigFromFile(l480Path.c_str(), l480back, err));
        CHECK_EQ(l480back.videoHeight, 480);
        CHECK_EQ(l480back.videoBitrate, 750000);
        CHECK_EQ(l480back.videoWidth(), 768);

        UfinConfig hx;
        std::string pOdd = writeTemp("hodd",
            "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"video_height\":2160}");
        CHECK(loadConfigFromFile(pOdd.c_str(), hx, err));
        CHECK_EQ(hx.videoHeight, 720);            // unsupported -> stable
        CHECK_EQ(hx.videoBitrate, 2500000);
        for (int odd : {0, 360, 540, 576, 854, 1440}) { // near misses are not silently accepted
            UfinConfig ho;
            std::string pn = writeTemp("hodd2", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"video_height\":" + std::to_string(odd) + "}");
            CHECK(loadConfigFromFile(pn.c_str(), ho, err));
            CHECK_EQ(ho.videoHeight, 720);
            CHECK_EQ(ho.videoBitrate, 2500000);
        }

        // Round trip through save/load keeps the selection.
        UfinConfig rt = h;
        std::string rtPath = "/tmp/ufin_test_h1080_rt.json";
        CHECK(saveConfigToFile(rtPath.c_str(), rt, err));
        UfinConfig back;
        CHECK(loadConfigFromFile(rtPath.c_str(), back, err));
        CHECK_EQ(back.videoHeight, 1080);
        CHECK_EQ(back.videoBitrate, 6000000);
    }

    // Optional buffering overrides: absent/out-of-range -> 0 (automatic).
    {
        CHECK_EQ(minimal.videoBufferMiB, 0);
        CHECK_EQ(minimal.videoQueueFrames, 0);
        UfinConfig b;
        std::string pb = writeTemp("buf",
            "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"video_buffer_mb\":200,\"video_queue_frames\":20}");
        CHECK(loadConfigFromFile(pb.c_str(), b, err));
        CHECK_EQ(b.videoBufferMiB, 200);
        CHECK_EQ(b.videoQueueFrames, 20);
        UfinConfig bad;
        std::string pbad = writeTemp("bufbad",
            "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"video_buffer_mb\":5000,\"video_queue_frames\":1}");
        CHECK(loadConfigFromFile(pbad.c_str(), bad, err));
        CHECK_EQ(bad.videoBufferMiB, 0);
        CHECK_EQ(bad.videoQueueFrames, 0);
    }

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
    {
        // Settings > Restore default menu music: only the song choice goes (what the action does to the config),
        // the rest survives a save + load, and the default stays selected after a restart.
        UfinConfig r = tok, back;
        r.accent = 3; r.menuMusicEnabled = true;
        r.menuMusicItemId.clear();
        r.menuMusicName.clear();
        std::string pR = writeTemp("bgmrestore", "{}");
        CHECK(saveConfigToFile(pR.c_str(), r, err));
        CHECK(loadConfigFromFile(pR.c_str(), back, err));
        CHECK(back.menuMusicItemId.empty() && back.menuMusicName.empty());
        CHECK(back.menuMusicEnabled);
        CHECK(back.crt && back.accent == 3);
        CHECK_STR(back.accessToken, "t0k");
    }
    CHECK_EQ(tok.accent, 0); // look & feel defaults when absent
    CHECK_EQ(tok.backgroundEffect, 0);
    CHECK(!tok.lightMode);
    CHECK(!tok.snow);
    CHECK(!tok.liquidGlass); // Liquid Glass UI is opt-in: off unless the setting says otherwise
    CHECK(!tok.m3Expressive); // so is Material 3 Expressive: Plain is the default style
    {
        UfinConfig m3;
        std::string pM = writeTemp("m3on", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"m3_expressive\":true}");
        CHECK(loadConfigFromFile(pM.c_str(), m3, err));
        CHECK(m3.m3Expressive && !m3.liquidGlass);
        // Both set by hand: exactly one style survives -- Liquid Glass, the older one.
        UfinConfig both;
        std::string pB = writeTemp("m3both", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"liquid_glass\":true,\"m3_expressive\":true}");
        CHECK(loadConfigFromFile(pB.c_str(), both, err));
        CHECK(both.liquidGlass && !both.m3Expressive);
        // M3E alone round-trips through save + load.
        UfinConfig save = m3, back;
        std::string pS = writeTemp("m3save", "{}");
        CHECK(saveConfigToFile(pS.c_str(), save, err));
        CHECK(loadConfigFromFile(pS.c_str(), back, err));
        CHECK(back.m3Expressive && !back.liquidGlass);
    }
    CHECK(!tok.glassDebug);
    CHECK(tok.glassRefraction);   // real refraction is on by default (it only matters once Liquid Glass is on)...
    CHECK_EQ(tok.glassRefractionStage, 6); // ...at stage 6, the lowest that works on real hardware (3-5 blank the UI)
    CHECK(!tok.clock);
    {
        // Real refraction is opted into from config.json, with an optional bring-up stage.
        UfinConfig on;
        std::string pOn = writeTemp("glasson",
            "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"liquid_glass\":true,\"glass_refraction\":true,\"glass_refraction_stage\":3}");
        CHECK(loadConfigFromFile(pOn.c_str(), on, err));
        CHECK(on.liquidGlass);
        CHECK(on.glassRefraction);
        CHECK_EQ(on.glassRefractionStage, 3);
        for (int v : {-4, 0}) { // out of range clamps to the first step (textures only), never to "off by accident"
            UfinConfig lo;
            std::string pLo = writeTemp("stagelo", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"glass_refraction_stage\":" + std::to_string(v) + "}");
            CHECK(loadConfigFromFile(pLo.c_str(), lo, err));
            CHECK_EQ(lo.glassRefractionStage, 1);
        }
        UfinConfig hi;
        std::string pHi = writeTemp("stagehi", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"glass_refraction_stage\":99}");
        CHECK(loadConfigFromFile(pHi.c_str(), hi, err));
        CHECK_EQ(hi.glassRefractionStage, 7);   // 7 is the highest rung of the bring-up ladder
        CHECK(hi.glassRefraction);  // (a stage alone does not turn it off)
        {
            UfinConfig vg; CHECK_EQ(vg.visualizerGlass, 0);
            for (int v : {0, 1, 2}) {
                UfinConfig c2;
                std::string pv = writeTemp("visglass", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"visualizer_glass\":" + std::to_string(v) + "}");
                CHECK(loadConfigFromFile(pv.c_str(), c2, err));
                CHECK_EQ(c2.visualizerGlass, v);
            }
            {
                UfinConfig g0; CHECK_EQ(g0.glassBlur, 1);
                for (int v : {-3, 0, 4, 99}) {
                    UfinConfig c3;
                    std::string pg = writeTemp("glassblur", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"glass_blur\":" + std::to_string(v) + "}");
                    CHECK(loadConfigFromFile(pg.c_str(), c3, err));
                    CHECK_EQ(c3.glassBlur, v < 0 ? 0 : (v > 3 ? 3 : v));
                }
            }
            UfinConfig bad;
            std::string pb = writeTemp("visglassbad", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"visualizer_glass\":9}");
            CHECK(loadConfigFromFile(pb.c_str(), bad, err));
            CHECK_EQ(bad.visualizerGlass, 0); // unknown value: the default
        }
        UfinConfig offc;
        std::string pOffc = writeTemp("refroff", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"glass_refraction\":false}");
        CHECK(loadConfigFromFile(pOffc.c_str(), offc, err));
        CHECK(!offc.glassRefraction);          // explicit opt-out is honoured
        CHECK_EQ(offc.glassRefractionStage, 6); // and does not disturb the stage
        for (int v : {3, 4, 5, 6, 7}) { // every rung of the bring-up ladder is accepted as-is
            UfinConfig mid;
            std::string pm = writeTemp("stagemid", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"glass_refraction_stage\":" + std::to_string(v) + "}");
            CHECK(loadConfigFromFile(pm.c_str(), mid, err));
            CHECK_EQ(mid.glassRefractionStage, v);
        }
    }
    CHECK(!tok.rainbowUnlocked);
    CHECK(tok.autoplayNext);        // on unless turned off
    CHECK(tok.skippedUpdateVersion.empty()); // nothing skipped by default
    CHECK(tok.checkForUpdates);     // the startup update check is on by default
    CHECK(!tok.gamepadOffInVideo);
    // An older config.json (none of the customisation keys) loads with safe defaults.
    CHECK(!tok.customPalette);
    CHECK_EQ(tok.customColor[0], 0x00A4DCu); CHECK_EQ(tok.customColor[1], 0x0B7FB0u); CHECK_EQ(tok.customColor[2], 0xAA5CC3u);
    CHECK_EQ(tok.customArrange, 0);
    CHECK(!tok.artworkAccent);
    CHECK(!tok.customShapes);
    for (int i = 0; i < 4; i++) CHECK_EQ(tok.buttonShape[i], 0);
    for (int i = 0; i < 8; i++) CHECK_EQ(tok.buttonSize[i], 100);
    CHECK(tok.tlsServers.empty());
    {   // https servers: loaded with their pin, a damaged pin / entry is dropped to "no pin" / skipped
        UfinConfig ts;
        const std::string fp(64, 'A');
        std::string ps = writeTemp("tls", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"tls_servers\":["
            "{\"host\":\"192.168.1.5\",\"port\":8920,\"pin\":\"" + fp + "\"},"
            "{\"host\":\"jelly.example.com\",\"port\":443},"
            "{\"host\":\"bad.pin\",\"port\":443,\"pin\":\"not a fingerprint\"},"
            "{\"host\":\"\",\"port\":443},{\"host\":\"x\",\"port\":0},{\"port\":443},7]}");
        CHECK(loadConfigFromFile(ps.c_str(), ts, err));
        CHECK_EQ(ts.tlsServers.size(), (size_t)3);
        CHECK_STR(ts.tlsServers[0].host, "192.168.1.5"); CHECK_EQ(ts.tlsServers[0].port, 8920); CHECK_STR(ts.tlsServers[0].pin, std::string(64, 'a'));
        CHECK_STR(ts.tlsServers[1].host, "jelly.example.com"); CHECK(ts.tlsServers[1].pin.empty());
        CHECK(ts.tlsServers[2].pin.empty());
        CHECK(findTlsServer(ts.tlsServers, "JELLY.example.com", 443) != nullptr);
        CHECK(findTlsServer(ts.tlsServers, "jelly.example.com", 8920) == nullptr);
        // round trip
        std::string rt = writeTemp("tls_rt", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\"}");
        UfinConfig a;
        CHECK(loadConfigFromFile(rt.c_str(), a, err));
        setTlsServer(a.tlsServers, "srv", 8920, true, std::string(64, 'b'));
        setTlsServer(a.tlsServers, "SRV", 8920, true, std::string(64, 'c')); // same server: updated, not added
        setTlsServer(a.tlsServers, "plain", 8096, false, "");                  // not https: nothing to remove / add
        CHECK_EQ(a.tlsServers.size(), (size_t)1);
        CHECK(saveConfigToFile(rt.c_str(), a, err));
        UfinConfig b;
        CHECK(loadConfigFromFile(rt.c_str(), b, err));
        CHECK_EQ(b.tlsServers.size(), (size_t)1); CHECK_STR(b.tlsServers[0].pin, std::string(64, 'c'));
        setTlsServer(b.tlsServers, "srv", 8920, false, "");                    // back to http: the entry goes
        CHECK(b.tlsServers.empty());
        CHECK(saveConfigToFile(rt.c_str(), b, err));
        UfinConfig c2;
        CHECK(loadConfigFromFile(rt.c_str(), c2, err));
        CHECK(c2.tlsServers.empty());
    }
    {   // sizes: 60..130 load, anything else (out of range, wrong type, short array) is the normal size
        UfinConfig sz;
        std::string ps = writeTemp("sizes", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"button_sizes\":[60,130,75,59,131,\"x\",90]}");
        CHECK(loadConfigFromFile(ps.c_str(), sz, err));
        CHECK_EQ(sz.buttonSize[0], 60); CHECK_EQ(sz.buttonSize[1], 130); CHECK_EQ(sz.buttonSize[2], 75);
        CHECK_EQ(sz.buttonSize[3], 100); CHECK_EQ(sz.buttonSize[4], 100); CHECK_EQ(sz.buttonSize[5], 100);
        CHECK_EQ(sz.buttonSize[6], 90); CHECK_EQ(sz.buttonSize[7], 100);
        std::string pt = writeTemp("sizes2", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"button_sizes\":5}");
        CHECK(loadConfigFromFile(pt.c_str(), sz, err));
        for (int i = 0; i < 8; i++) CHECK_EQ(sz.buttonSize[i], 100);
    }
    {   // shapes: valid values load, nonsense (out of range, wrong type, scalloped on dialog buttons) falls back to 0
        UfinConfig sh;
        std::string ps = writeTemp("shapes", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"custom_shapes\":true,\"button_shapes\":[6,2,99,5]}");
        CHECK(loadConfigFromFile(ps.c_str(), sh, err));
        CHECK(sh.customShapes);
        CHECK_EQ(sh.buttonShape[0], 6); CHECK_EQ(sh.buttonShape[1], 2); CHECK_EQ(sh.buttonShape[2], 0); CHECK_EQ(sh.buttonShape[3], 0);
        std::string pt = writeTemp("shapes2", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"button_shapes\":\"x\"}");
        CHECK(loadConfigFromFile(pt.c_str(), sh, err));
        for (int i = 0; i < 4; i++) CHECK_EQ(sh.buttonShape[i], 0);
    }
    {   // Hand-edited / damaged values never break the load: bad colours keep their default, a bad arrangement is 0.
        UfinConfig bad;
        std::string pb = writeTemp("custombad", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\","
                                   "\"custom_palette\":true,\"custom_colors\":[\"#FF0000\",\"nope\",42],\"custom_arrange\":99,\"artwork_accent\":true}");
        CHECK(loadConfigFromFile(pb.c_str(), bad, err));
        CHECK(bad.customPalette);
        CHECK_EQ(bad.customColor[0], 0xFF0000u);
        CHECK_EQ(bad.customColor[1], 0x0B7FB0u);
        CHECK_EQ(bad.customColor[2], 0xAA5CC3u);
        CHECK_EQ(bad.customArrange, 0);
        CHECK(bad.artworkAccent);
        UfinConfig notArray;
        std::string pn = writeTemp("customnotarray", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"custom_colors\":\"#FF0000\",\"custom_arrange\":-1}");
        CHECK(loadConfigFromFile(pn.c_str(), notArray, err));
        CHECK_EQ(notArray.customColor[0], 0x00A4DCu);
        CHECK_EQ(notArray.customArrange, 0);
        // The newer background effects (Aurora..Waves) are no longer reset to Off on load.
        UfinConfig bg;
        std::string pg = writeTemp("bgeffect", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"background_effect\":11}");
        CHECK(loadConfigFromFile(pg.c_str(), bg, err));
        CHECK_EQ(bg.backgroundEffect, 11);
        std::string pg2 = writeTemp("bgeffect2", "{\"host\":\"h\",\"port\":1,\"username\":\"u\",\"password\":\"p\",\"background_effect\":12}");
        CHECK(loadConfigFromFile(pg2.c_str(), bg, err));
        CHECK_EQ(bg.backgroundEffect, 0);
    }
    CHECK(!tok.alwaysDefaultTracks); // the first-play audio & subtitle prompt is on by default
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
        c.backgroundEffect = 3;
        c.lightMode = true;
        c.snow = true;
        c.liquidGlass = true;
        c.m3Expressive = true; // (both set by hand: saving keeps Liquid Glass only -- one style at a time)
        c.glassDebug = true;
        c.glassRefraction = true;
        c.glassRefractionStage = 2;
        c.clock = true;
        c.rainbowUnlocked = true;
        c.autoplayNext = false;
        c.gamepadOffInVideo = true;
        c.alwaysDefaultTracks = true;
        c.checkForUpdates = false;
        c.skippedUpdateVersion = "1.3.1";
        c.customPalette = true;
        c.customColor[0] = 0x123456; c.customColor[1] = 0xABCDEF; c.customColor[2] = 0x000001;
        c.customArrange = 4;
        c.artworkAccent = true;
        c.customShapes = true;
        c.buttonShape[0] = 4; c.buttonShape[1] = 6; c.buttonShape[2] = 1; c.buttonShape[3] = 3;
        for (int i = 0; i < 8; i++) c.buttonSize[i] = i == 7 ? 112 : 60 + i * 10;
        CHECK(saveConfigToFile(path.c_str(), c, err));
        UfinConfig back;
        CHECK(loadConfigFromFile(path.c_str(), back, err));
        CHECK_STR(back.host, "192.168.1.100");
        CHECK_EQ(back.port, 8096);
        CHECK_STR(back.accessToken, "abc");
        CHECK_STR(back.menuMusicName, "Citt\xc3\xa0");
        CHECK(back.crt);
        CHECK_EQ(back.accent, 3);
        CHECK_EQ(back.backgroundEffect, 3);
        CHECK(back.lightMode);
        CHECK(back.snow);
        CHECK(back.liquidGlass);
        CHECK(!back.m3Expressive);
        CHECK(back.glassDebug);
        CHECK(back.glassRefraction);
        CHECK_EQ(back.glassRefractionStage, 2);
        CHECK(back.clock);
        CHECK(back.rainbowUnlocked);
        CHECK(!back.autoplayNext);
        CHECK(back.gamepadOffInVideo);
        CHECK(back.alwaysDefaultTracks);
        CHECK(back.customPalette);
        CHECK_EQ(back.customColor[0], 0x123456u); CHECK_EQ(back.customColor[1], 0xABCDEFu); CHECK_EQ(back.customColor[2], 0x000001u);
        CHECK_EQ(back.customArrange, 4);
        CHECK(back.artworkAccent);
        CHECK(back.customShapes);
        CHECK_EQ(back.buttonShape[0], 4); CHECK_EQ(back.buttonShape[1], 6); CHECK_EQ(back.buttonShape[2], 1); CHECK_EQ(back.buttonShape[3], 3);
        for (int i = 0; i < 8; i++) CHECK_EQ(back.buttonSize[i], i == 7 ? 112 : 60 + i * 10);
        CHECK(back.paletteEdited);  // (non-default colours were chosen by the user)
        CHECK(!tok.paletteEdited);
        CHECK(back.skippedUpdateVersion == "1.3.1"); // "Don't remind me for this release" survives
        CHECK(!back.checkForUpdates);  // turning the update check off survives a save and a load
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
