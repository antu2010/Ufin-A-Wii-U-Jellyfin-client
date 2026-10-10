// Who's watching? -- the profile rules (profiles.h), the bounce physics (ui/profile_anim.h) and how
// profiles survive in config.json (config_loader). No UI, no ImGui.
#include "check.h"
#include "config_loader.h"
#include "profiles.h"
#include "ui/profile_anim.h"

#include <cstdio>
#include <string>
#include <sys/stat.h>

static Profile make(const std::string& name, const std::string& id, bool keep = false, const std::string& token = "tok") {
    Profile p;
    p.name = name; p.host = "192.168.1.43"; p.port = 8097; p.userId = id; p.accessToken = token; p.keepLoggedIn = keep;
    return p;
}

static void testRules() {
    std::vector<Profile> list;
    // upsert: new profiles get distinct colours and start with "keep logged in" off.
    CHECK_EQ(profiles::upsert(list, make("antu", "u1", true)), 0);
    CHECK_EQ(profiles::upsert(list, make("mum", "u2")), 1);
    CHECK_EQ(profiles::upsert(list, make("dad", "u3")), 2);
    CHECK(!list[0].keepLoggedIn);
    CHECK(list[0].color != list[1].color && list[1].color != list[2].color && list[0].color != list[2].color);
    // the same user again (new token, new address, new name): updates in place, keeps choice + colour
    list[1].keepLoggedIn = true;
    const int colour = list[1].color;
    Profile again = make("Mum", "u2", false, "newtok");
    again.host = "10.0.0.5";
    CHECK_EQ(profiles::upsert(list, again), 1);
    CHECK_EQ(list.size(), (size_t)3);
    CHECK_STR(list[1].accessToken, "newtok");
    CHECK_STR(list[1].host, "10.0.0.5");
    CHECK_STR(list[1].name, "Mum");
    CHECK(list[1].keepLoggedIn);
    CHECK_EQ(list[1].color, colour);
    // a profile without an id is matched by name + server
    std::vector<Profile> legacy = {make("old", "")};
    CHECK_EQ(profiles::find(legacy, "192.168.1.43", 8097, "", "old"), 0);
    CHECK_EQ(profiles::find(legacy, "192.168.1.43", 8097, "", "other"), -1);
    // full: the sixth is refused
    CHECK_EQ(profiles::upsert(list, make("a", "u4")), 3);
    CHECK_EQ(profiles::upsert(list, make("b", "u5")), 4);
    CHECK_EQ(profiles::upsert(list, make("c", "u6")), -1);
    CHECK_EQ(list.size(), profiles::kMax);

    // remove keeps "last used" pointing at the same profile
    CHECK_EQ(profiles::remove(list, 0, 3), 2);
    CHECK_EQ(profiles::remove(list, 3, 2), 2);
    CHECK_EQ(profiles::remove(list, 2, 2), -1);
    CHECK_EQ(profiles::remove(list, 9, 1), 1);

    // auto-login: only a ticked profile with a saved sign-in; the last used wins among several
    std::vector<Profile> a = {make("x", "1"), make("y", "2"), make("z", "3")};
    CHECK_EQ(profiles::autoLoginIndex(a, 0), -1);
    a[1].keepLoggedIn = true;
    CHECK_EQ(profiles::autoLoginIndex(a, 0), 1);
    a[2].keepLoggedIn = true;
    CHECK_EQ(profiles::autoLoginIndex(a, 2), 2);
    CHECK_EQ(profiles::autoLoginIndex(a, 0), 1);
    CHECK_EQ(profiles::autoLoginIndex(a, -1), 1);
    a[1].accessToken.clear();                       // its sign-in expired: not auto-signed-in
    CHECK_EQ(profiles::autoLoginIndex(a, 1), 2);
    a[2].accessToken.clear();
    CHECK_EQ(profiles::autoLoginIndex(a, 1), -1);   // -> Who's watching shows
    CHECK_EQ(profiles::autoLoginIndex({}, -1), -1);

    // auto log in is exclusive: ticking one unticks the others
    std::vector<Profile> e = {make("x", "1"), make("y", "2"), make("z", "3")};
    profiles::setAutoLogin(e, 0, true);
    profiles::setAutoLogin(e, 2, true);
    CHECK(!e[0].keepLoggedIn && !e[1].keepLoggedIn && e[2].keepLoggedIn);
    profiles::setAutoLogin(e, 2, false);
    CHECK(!e[2].keepLoggedIn);
    profiles::setAutoLogin(e, 9, true);             // out of range: nothing happens
    CHECK(!e[0].keepLoggedIn && !e[1].keepLoggedIn && !e[2].keepLoggedIn);
    // a list with several ticked (hand-edited): the last used one stays, else the first
    e[0].keepLoggedIn = e[1].keepLoggedIn = e[2].keepLoggedIn = true;
    profiles::normalizeAutoLogin(e, 1);
    CHECK(!e[0].keepLoggedIn && e[1].keepLoggedIn && !e[2].keepLoggedIn);
    e[0].keepLoggedIn = e[2].keepLoggedIn = true;
    profiles::normalizeAutoLogin(e, -1);
    CHECK(e[0].keepLoggedIn && !e[1].keepLoggedIn && !e[2].keepLoggedIn);
    // log out: forgets the sign-in, keeps the profile, drops auto log in
    CHECK(profiles::logOut(e, 0));
    CHECK(e[0].accessToken.empty() && !e[0].keepLoggedIn && e[0].name == "x");
    CHECK(!profiles::logOut(e, 0));                 // already logged out
    CHECK(!profiles::logOut(e, 7));
    CHECK_EQ(profiles::autoLoginIndex(e, 0), -1);

    // the letter on a picture-less avatar
    CHECK_STR(profiles::initial("antu"), "A");
    CHECK_STR(profiles::initial("  mum"), "M");
    CHECK_STR(profiles::initial("Élodie"), "\xC3\x89");
    CHECK_STR(profiles::initial("\xE6\x97\xA5\xE6\x9C\xAC"), "\xE6\x97\xA5");
    CHECK_STR(profiles::initial(""), "?");
}

// ---- the bounce ----
struct Trace { float minSy = 9, maxSy = 0, maxLift = 0, minSx = 9, maxSx = 0; int jumps = 0; };

static Trace run(ui::Bounce& b, float seconds, bool hovered) {
    Trace t;
    bool wasAir = false;
    for (float s = 0; s < seconds; s += 1.0f / 60.0f) {
        b.step(1.0f / 60.0f, hovered);
        const ui::BounceShape sh = b.shape();
        t.minSy = std::min(t.minSy, sh.sy); t.maxSy = std::max(t.maxSy, sh.sy);
        t.minSx = std::min(t.minSx, sh.sx); t.maxSx = std::max(t.maxSx, sh.sx);
        t.maxLift = std::max(t.maxLift, sh.lift);
        const bool air = b.phase() == ui::Bounce::Air;
        if (air && !wasAir) t.jumps++;
        wasAir = air;
        CHECK(sh.lift >= 0.0f);
    }
    return t;
}

static void testBounce() {
    // A picture nobody hovers never moves, not even a pixel.
    {
        ui::Bounce b;
        const Trace t = run(b, 5.0f, false);
        CHECK(b.shape().still());
        CHECK(t.maxLift == 0.0f && t.minSy == 1.0f && t.maxSy == 1.0f && t.minSx == 1.0f && t.maxSx == 1.0f);
        CHECK(b.resting());
    }
    // Hovered: it keeps jumping (about once a second), and it really distorts: squashed flat and wide
    // when it lands or winds up, stretched tall and thin on the way up.
    {
        ui::Bounce b;
        const Trace t = run(b, 5.0f, true);
        CHECK(t.jumps >= 4 && t.jumps <= 6);
        CHECK(t.maxLift > 0.25f && t.maxLift < 0.36f);        // ~0.30 picture diameters
        CHECK(t.minSy < 0.80f);                                // squash
        CHECK(t.maxSx > 1.12f);                                // ...which is wider
        CHECK(t.maxSy > 1.10f);                                // stretch
        CHECK(t.minSx < 0.92f);                                // ...which is thinner
        CHECK(t.minSy > 0.55f && t.maxSy < 1.5f);              // never absurd
    }
    // Hover leaves mid-jump: it drops at once, settles, and then stays perfectly still.
    {
        ui::Bounce b;
        run(b, 0.5f, true);                                    // somewhere in the first jump
        run(b, 1.2f, false);
        CHECK(b.resting());
        CHECK(b.shape().still());
        const Trace later = run(b, 3.0f, false);
        CHECK(later.maxLift == 0.0f && later.jumps == 0);
    }
    // ...and a long frame (a stall) does not blow the spring up.
    {
        ui::Bounce b;
        for (int i = 0; i < 200; i++) b.step(0.25f, true);
        const ui::BounceShape s = b.shape();
        CHECK(s.sy > 0.5f && s.sy < 1.6f && s.sx > 0.6f && s.sx < 1.6f && s.lift < 0.5f);
        // frame-rate independence: the same second at 30 and at 120 fps jumps about equally high
        ui::Bounce a30, a120;
        float h30 = 0, h120 = 0;
        for (int i = 0; i < 90; i++) { a30.step(1.0f / 30.0f, true); h30 = std::max(h30, a30.shape().lift); }
        for (int i = 0; i < 360; i++) { a120.step(1.0f / 120.0f, true); h120 = std::max(h120, a120.shape().lift); }
        CHECK_NEAR(h30, h120, 0.03);
    }
}

// ---- profiles in config.json ----
static void testConfig() {
    const char* path = "/tmp/ufin_profiles_test_config.json";
    remove(path);
    UfinConfig c;
    c.host = "192.168.1.43"; c.port = 8097; c.username = "antu"; c.userId = "u1"; c.accessToken = "tok1";
    profiles::upsert(c.profiles, make("antu", "u1", false, "tok1"));
    profiles::upsert(c.profiles, make("mum", "u2", false, "tok2"));
    c.profiles[1].keepLoggedIn = true;
    c.lastProfile = 1;
    std::string err;
    CHECK(saveConfigToFile(path, c, err));

    UfinConfig d;
    CHECK(loadConfigFromFile(path, d, err));
    CHECK_EQ(d.profiles.size(), (size_t)2);
    CHECK_STR(d.profiles[0].name, "antu");
    CHECK_STR(d.profiles[1].name, "mum");
    CHECK_STR(d.profiles[1].userId, "u2");
    CHECK_STR(d.profiles[1].accessToken, "tok2");   // the saved sign-in survives (the password never is stored)
    CHECK_STR(d.profiles[1].host, "192.168.1.43");
    CHECK_EQ(d.profiles[1].port, 8097);
    CHECK(d.profiles[1].keepLoggedIn && !d.profiles[0].keepLoggedIn);
    CHECK_EQ(d.profiles[0].color, c.profiles[0].color);
    CHECK_EQ(d.profiles[1].color, c.profiles[1].color);
    CHECK_EQ(d.lastProfile, 1);
    CHECK_STR(d.accessToken, "tok1");               // the single sign-in keys stay for older versions

    // saved again after removing everything: the key is gone, nothing stale comes back
    UfinConfig e = d;
    e.profiles.clear();
    e.lastProfile = -1;
    CHECK(saveConfigToFile(path, e, err));
    UfinConfig f;
    CHECK(loadConfigFromFile(path, f, err));
    CHECK(f.profiles.empty());
    CHECK_EQ(f.lastProfile, -1);

    // an old config.json (no "profiles") loads with an empty list, and junk entries are skipped
    FILE* fp = fopen(path, "wb");
    fputs("{\"host\":\"h\",\"port\":8096,\"user_id\":\"u\",\"access_token\":\"t\",\"username\":\"old\"}", fp);
    fclose(fp);
    UfinConfig g;
    CHECK(loadConfigFromFile(path, g, err));
    CHECK(g.profiles.empty());
    CHECK_STR(g.accessToken, "t");
    fp = fopen(path, "wb");
    fputs("{\"host\":\"h\",\"port\":1,\"last_profile\":7,\"profiles\":[5,{\"name\":\"ok\",\"user_id\":\"1\",\"color\":99},"
          "{},{\"name\":\"\",\"user_id\":\"\"},{\"user_id\":\"only-id\"}]}", fp);
    fclose(fp);
    UfinConfig h;
    CHECK(loadConfigFromFile(path, h, err));
    CHECK_EQ(h.profiles.size(), (size_t)2);
    CHECK_STR(h.profiles[0].name, "ok");
    CHECK(h.profiles[0].color >= 0 && h.profiles[0].color < profiles::kColors);
    CHECK_STR(h.profiles[1].name, "User");          // no name saved: a placeholder
    CHECK_EQ(h.lastProfile, -1);                    // 7 is out of range
    // several profiles ticked "auto log in" in config.json: only one is kept
    fp = fopen(path, "wb");
    fputs("{\"host\":\"h\",\"port\":1,\"last_profile\":1,\"profiles\":[{\"name\":\"a\",\"user_id\":\"1\",\"keep_logged_in\":true},"
          "{\"name\":\"b\",\"user_id\":\"2\",\"keep_logged_in\":true},{\"name\":\"c\",\"user_id\":\"3\",\"keep_logged_in\":true}]}", fp);
    fclose(fp);
    UfinConfig two;
    CHECK(loadConfigFromFile(path, two, err));
    CHECK(!two.profiles[0].keepLoggedIn && two.profiles[1].keepLoggedIn && !two.profiles[2].keepLoggedIn);
    // at most kMax are read
    std::string many = "{\"host\":\"h\",\"port\":1,\"profiles\":[";
    for (int i = 0; i < 9; i++) many += std::string(i ? "," : "") + "{\"name\":\"p" + std::to_string(i) + "\",\"user_id\":\"" + std::to_string(i) + "\"}";
    many += "]}";
    fp = fopen(path, "wb");
    fputs(many.c_str(), fp);
    fclose(fp);
    UfinConfig k;
    CHECK(loadConfigFromFile(path, k, err));
    CHECK_EQ(k.profiles.size(), profiles::kMax);
    remove(path);
}

int main() {
    testRules();
    testBounce();
    testConfig();
    return check::finish("test_profiles");
}
