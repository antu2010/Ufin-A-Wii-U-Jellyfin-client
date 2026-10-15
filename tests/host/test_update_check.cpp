// The startup update check's decisions, over fake GitHub / App Store answers:
// version parsing and ordering, the two JSON formats, and the rules from the
// feature request (newer -> prompt, equal or older -> silent, any failure ->
// silent). No network, no TLS.
#include "check.h"
#include "update_check.h"
#include "version.h"

#include <string>
#include <vector>

using namespace updatecheck;

static bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

static int cmp(const char* a, const char* b) { return compareVersions(parseVersion(a), parseVersion(b)); }

static const char* kRelease130 = R"({"tag_name":"v1.3.0","html_url":"https://github.com/antu2010/Ufin-A-Wii-U-Jellyfin-client/releases/tag/v1.3.0","prerelease":false,"draft":false})";
static const char* kRelease131 = R"({"tag_name":"v1.3.1","html_url":"https://github.com/antu2010/Ufin-A-Wii-U-Jellyfin-client/releases/tag/v1.3.1","prerelease":false,"draft":false,"assets":[]})";

static std::string storeJson(const char* version) {
    return std::string(R"({"packages":[{"name":"other","version":"9.9.9"},{"name":"Ufin","title":"Ufin","version":")") + version +
           R"(","category":"media"}]})";
}

// A fake fetcher: answers by host, records what was asked.
struct Fake {
    FetchResult github, store;
    std::vector<std::string> hosts, headers;
    Fetcher fetcher() {
        return [this](const std::string& host, const std::string& path, const std::string& extra) {
            hosts.push_back(host);
            headers.push_back(extra);
            (void)path;
            return host == "api.github.com" ? github : store;
        };
    }
};
static FetchResult answer(int status, const std::string& body) {
    FetchResult r;
    r.ok = true;
    r.status = status;
    r.body = body;
    return r;
}
static FetchResult down(const char* why) {
    FetchResult r;
    r.error = why;
    return r;
}

int main() {
    // --- this build -----------------------------------------------------------
    CHECK_EQ(std::string(ufin::kVersion), std::string("1.3.0"));
    CHECK(parseVersion(ufin::kVersion).valid);

    // --- versions -------------------------------------------------------------
    CHECK(parseVersion("1.3.0").valid);
    CHECK(parseVersion("v1.3.0").valid);
    CHECK(parseVersion("V1.3").valid && parseVersion("V1.3").patch == 0);
    CHECK(parseVersion("2").valid && parseVersion("2").major == 2);
    CHECK(parseVersion(" v1.3.0 ").valid);
    CHECK(parseVersion("1.3.0+build5").valid && !parseVersion("1.3.0+build5").prerelease);
    CHECK(parseVersion("1.3.0-beta2").prerelease);
    for (const char* bad : {"", "v", "abc", "latest", "1.x.0", "1..0", "1.3.0.1", "1.3.0abc", "-1.3.0", "99999999.0.0"})
        CHECK(!parseVersion(bad).valid);

    CHECK(cmp("1.3.0", "1.3.0") == 0);
    CHECK(cmp("1.3.0", "v1.3.0") == 0);
    CHECK(cmp("1.3", "1.3.0") == 0);
    CHECK(cmp("1.2.0", "1.3.0") < 0);
    CHECK(cmp("1.3.1", "1.3.0") > 0);
    CHECK(cmp("1.10.0", "1.9.0") > 0);        // numeric, not alphabetical
    CHECK(cmp("2.0.0", "1.99.99") > 0);
    CHECK(cmp("1.3.0-beta1", "1.3.0") < 0);   // a pre-release is older than its release
    CHECK(cmp("1.3.0-beta1", "1.3.0-beta2") < 0);
    CHECK(cmp("1.3.1-beta1", "1.3.0") > 0);
    CHECK_EQ(compareVersions(Version(), parseVersion("1.0.0")), 0); // invalid: never "newer"

    // --- the two JSON formats --------------------------------------------------
    GithubRelease r;
    CHECK(parseGithubRelease(kRelease131, r));
    CHECK_EQ(r.tag, std::string("v1.3.1"));
    CHECK(has(r.url, "/releases/tag/v1.3.1"));
    CHECK(!r.prerelease && !r.draft);
    CHECK(parseGithubRelease(R"({"tag_name":"v2","prerelease":true,"draft":true})", r) && r.prerelease && r.draft);
    CHECK(!parseGithubRelease("", r));
    CHECK(!parseGithubRelease("not json", r));
    CHECK(!parseGithubRelease(R"({"message":"Not Found"})", r)); // GitHub's error body: no tag
    CHECK(!parseGithubRelease("[1,2]", r));

    std::string v;
    CHECK(parseStoreVersion(storeJson("1.3.1"), "ufin", v) && v == "1.3.1"); // name matched case-insensitively
    CHECK(!parseStoreVersion(storeJson("1.3.1"), "missing", v));
    CHECK(!parseStoreVersion(R"({"packages":[{"name":"ufin"}]})", "ufin", v)); // no version listed
    CHECK(!parseStoreVersion(R"({"nope":1})", "ufin", v));
    CHECK(!parseStoreVersion("garbage", "ufin", v));

    // --- the verdict -----------------------------------------------------------
    GithubRelease g131; g131.tag = "v1.3.1"; g131.url = "https://example/r";
    UpdateInfo u = decide("1.3.0", g131, "");
    CHECK(u.available && !u.inStore && u.latestVersion == "1.3.1" && u.releaseUrl == "https://example/r");
    CHECK(decide("1.3.0", g131, "1.3.1").inStore);
    CHECK(decide("1.3.0", g131, "1.4.0").inStore);       // the store is ahead: it has at least that
    CHECK(!decide("1.3.0", g131, "1.3.0").inStore);      // store still on the old version
    CHECK(!decide("1.3.0", g131, "garbage").inStore);
    GithubRelease same; same.tag = "v1.3.0";
    CHECK(!decide("1.3.0", same, "").available);          // equal: ignored
    GithubRelease old; old.tag = "v1.2.0";
    CHECK(!decide("1.3.0", old, "").available);           // this build newer than GitHub: ignored
    GithubRelease pre = g131; pre.prerelease = true;
    CHECK(!decide("1.3.0", pre, "").available);           // never push a pre-release
    GithubRelease dr = g131; dr.draft = true;
    CHECK(!decide("1.3.0", dr, "").available);
    GithubRelease bad; bad.tag = "nightly";
    CHECK(!decide("1.3.0", bad, "").available);
    CHECK(!decide("dev", g131, "").available);            // unparsable own version: never prompt

    // --- the prompt text -------------------------------------------------------
    {
        u.releaseUrl = "https://github.com/antu2010/Ufin-A-Wii-U-Jellyfin-client/releases/tag/v1.3.1";
        u.inStore = true;
        auto lines = promptLines(u, "1.3.0");
        std::string all;
        for (auto& l : lines) all += l + "\n";
        CHECK(has(all, "Ufin 1.3.1 is available (you have 1.3.0)."));
        CHECK(has(all, "Homebrew App Store"));
        CHECK(has(all, "GitHub"));
        CHECK(has(all, u.releaseUrl.c_str()));
        CHECK(!has(all, "not in the Homebrew"));
        u.inStore = false;
        lines = promptLines(u, "1.3.0");
        all.clear();
        for (auto& l : lines) all += l + "\n";
        CHECK(has(all, "Download it from GitHub"));
        CHECK(has(all, "not in the Homebrew App Store yet"));
        CHECK(!has(all, "Update it from the Homebrew"));
    }

    // --- the whole check, scenario by scenario ---------------------------------
    // Newer release, already in the App Store.
    {
        Fake f; f.github = answer(200, kRelease131); f.store = answer(200, storeJson("1.3.1"));
        Outcome o = checkForUpdate(f.fetcher(), "1.3.0", "antu2010", "Ufin-A-Wii-U-Jellyfin-client", "ufin");
        CHECK(o.kind == Outcome::Kind::Available);
        CHECK(o.info.available && o.info.inStore && o.info.latestVersion == "1.3.1");
        CHECK(f.hosts.size() == 2 && f.hosts[0] == "api.github.com" && f.hosts[1] == kStoreHost);
        CHECK(has(f.headers[0], "User-Agent: Ufin/1.3.0\r\n"));   // GitHub refuses requests without one
        CHECK(has(f.headers[0], "Accept: application/vnd.github+json\r\n"));
        for (auto& h : f.headers) { CHECK(!has(h, "Authorization")); CHECK(!has(h, "ApiKey")); }
    }
    // Newer release, the store has not caught up.
    {
        Fake f; f.github = answer(200, kRelease131); f.store = answer(200, storeJson("1.3.0"));
        Outcome o = checkForUpdate(f.fetcher(), "1.3.0", "antu2010", "r", "ufin");
        CHECK(o.kind == Outcome::Kind::Available && !o.info.inStore);
    }
    // Newer release, store unreachable / broken: still prompts, GitHub only.
    {
        Fake f; f.github = answer(200, kRelease131); f.store = down("timed out");
        Outcome o = checkForUpdate(f.fetcher(), "1.3.0", "o", "r", "ufin");
        CHECK(o.kind == Outcome::Kind::Available && !o.info.inStore);
        Fake f2; f2.github = answer(200, kRelease131); f2.store = answer(500, "oops");
        CHECK(checkForUpdate(f2.fetcher(), "1.3.0", "o", "r", "ufin").kind == Outcome::Kind::Available);
        Fake f3; f3.github = answer(200, kRelease131); f3.store = answer(200, "<html>");
        CHECK(!checkForUpdate(f3.fetcher(), "1.3.0", "o", "r", "ufin").info.inStore);
    }
    // Cancelled between the two requests: no store request, still reports the update.
    {
        Fake f; f.github = answer(200, kRelease131); f.store = answer(200, storeJson("1.3.1"));
        Outcome o = checkForUpdate(f.fetcher(), "1.3.0", "o", "r", "ufin", [] { return true; });
        CHECK(o.kind == Outcome::Kind::Available && !o.info.inStore && f.hosts.size() == 1);
    }
    // Same version: silent, and the store is not even asked.
    {
        Fake f; f.github = answer(200, kRelease130); f.store = answer(200, storeJson("1.3.0"));
        Outcome o = checkForUpdate(f.fetcher(), "1.3.0", "o", "r", "ufin");
        CHECK(o.kind == Outcome::Kind::UpToDate && f.hosts.size() == 1);
        CHECK(has(o.detail, "up to date"));
    }
    // This build newer than GitHub's latest (the "current is 1.3.0, GitHub is 1.2.0" case): silent.
    {
        Fake f; f.github = answer(200, R"({"tag_name":"v1.2.0","html_url":"u"})");
        Outcome o = checkForUpdate(f.fetcher(), "1.3.0", "o", "r", "ufin");
        CHECK(o.kind == Outcome::Kind::UpToDate && f.hosts.size() == 1);
        CHECK(has(o.detail, "newer than the latest release"));
    }
    // Pre-release only: silent.
    {
        Fake f; f.github = answer(200, R"({"tag_name":"v2.0.0-beta1","prerelease":true,"html_url":"u"})");
        CHECK(checkForUpdate(f.fetcher(), "1.3.0", "o", "r", "ufin").kind == Outcome::Kind::UpToDate);
    }
    // Everything that can go wrong is "Unknown": no prompt, a log line.
    {
        Fake a; a.github = down("TLS handshake failed");
        Outcome o = checkForUpdate(a.fetcher(), "1.3.0", "o", "r", "ufin");
        CHECK(o.kind == Outcome::Kind::Unknown && has(o.detail, "TLS handshake failed"));
        Fake b; b.github = answer(403, R"({"message":"API rate limit exceeded"})");
        CHECK(checkForUpdate(b.fetcher(), "1.3.0", "o", "r", "ufin").kind == Outcome::Kind::Unknown);
        Fake c; c.github = answer(404, R"({"message":"Not Found"})");
        CHECK(checkForUpdate(c.fetcher(), "1.3.0", "o", "r", "ufin").kind == Outcome::Kind::Unknown);
        Fake d; d.github = answer(200, "<html>captive portal</html>");
        CHECK(checkForUpdate(d.fetcher(), "1.3.0", "o", "r", "ufin").kind == Outcome::Kind::Unknown);
        Fake e; e.github = answer(200, R"({"tag_name":"nightly"})");
        CHECK(checkForUpdate(e.fetcher(), "1.3.0", "o", "r", "ufin").kind == Outcome::Kind::Unknown);
        Fake g; g.github = answer(200, kRelease131);
        CHECK(checkForUpdate(g.fetcher(), "dev-build", "o", "r", "ufin").kind == Outcome::Kind::Unknown);
        CHECK(checkForUpdate(Fetcher(), "1.3.0", "o", "r", "ufin").kind == Outcome::Kind::Unknown);
    }
    // The request goes to the repo it was given.
    {
        std::string path;
        Fetcher spy = [&](const std::string& host, const std::string& p, const std::string&) {
            if (host == "api.github.com") path = p;
            return down("x");
        };
        checkForUpdate(spy, "1.3.0", "antu2010", "Ufin-A-Wii-U-Jellyfin-client", "ufin");
        CHECK_EQ(path, std::string("/repos/antu2010/Ufin-A-Wii-U-Jellyfin-client/releases/latest"));
    }

    // --- Settings > Check for updates: the card always names both versions ---------------
    {
        auto joined = [](const Card& c) { std::string j = c.title; for (auto& l : c.lines) j += "\n" + l; return j; };
        // GitHub newer, in the store.
        Fake f; f.github = answer(200, kRelease131); f.store = answer(200, storeJson("1.3.1"));
        Card c = resultCard(checkForUpdate(f.fetcher(), "1.3.0", "o", "r", "ufin"), "1.3.0");
        std::string j = joined(c);
        CHECK(!c.isError && c.title == "Update available");
        CHECK(has(j, "Your version: 1.3.0") && has(j, "Latest on GitHub: 1.3.1"));
        CHECK(has(j, "Please update") && has(j, "Homebrew App Store") && has(j, "github.com/antu2010"));
        // GitHub newer, not in the store.
        Fake g; g.github = answer(200, kRelease131); g.store = answer(200, storeJson("1.3.0"));
        std::string jg = joined(resultCard(checkForUpdate(g.fetcher(), "1.3.0", "o", "r", "ufin"), "1.3.0"));
        CHECK(has(jg, "Please update") && has(jg, "not in the Homebrew App Store yet"));
        // This build is ahead of GitHub: the "future" message.
        Fake h; h.github = answer(200, R"({"tag_name":"v1.2.0","html_url":"u"})");
        Outcome oh = checkForUpdate(h.fetcher(), "1.3.0", "o", "r", "ufin");
        CHECK(oh.buildIsNewer && oh.githubVersion == "1.2.0");
        Card ch = resultCard(oh, "1.3.0");
        std::string jh = joined(ch);
        CHECK(!ch.isError && has(jh, "Your version: 1.3.0") && has(jh, "Latest on GitHub: 1.2.0"));
        CHECK(has(jh, "in the future") && !has(jh, "Please update"));
        // Same version.
        Fake i; i.github = answer(200, kRelease130);
        Outcome oi = checkForUpdate(i.fetcher(), "1.3.0", "o", "r", "ufin");
        CHECK(!oi.buildIsNewer && oi.githubVersion == "1.3.0");
        std::string ji = joined(resultCard(oi, "1.3.0"));
        CHECK(has(ji, "up to date") && has(ji, "Latest on GitHub: 1.3.0") && !has(ji, "future") && !has(ji, "Please update"));
        // Pre-release only: never "update", never "future".
        Fake k; k.github = answer(200, R"({"tag_name":"v2.0.0-beta1","prerelease":true,"html_url":"u"})");
        Outcome ok = checkForUpdate(k.fetcher(), "1.3.0", "o", "r", "ufin");
        CHECK(ok.githubPrerelease && !ok.buildIsNewer);
        std::string jk = joined(resultCard(ok, "1.3.0"));
        CHECK(has(jk, "pre-release") && !has(jk, "future") && !has(jk, "Please update"));
        // A failed check: an error card with the reason and the current version.
        Fake e; e.github = down("TLS handshake failed");
        Card ce = resultCard(checkForUpdate(e.fetcher(), "1.3.0", "o", "r", "ufin"), "1.3.0");
        CHECK(ce.isError && has(joined(ce), "TLS handshake failed") && has(joined(ce), "Your version: 1.3.0"));
        // This build's own version string is what ships.
        CHECK(parseVersion(ufin::kVersion).valid);
    }

    // --- the changelog from the release description ----------------------------------------
    {
        const std::string md = "<!-- template -->\r\n## What's new\r\n\r\n* **Subtitles** now work at 1080p\r\n- Fixed the [Cemu crash](https://x.y/z) \xF0\x9F\x8E\x89\r\n![shot](https://i/s.png)\r\n\r\n\r\n---\r\n```\r\ncode here\r\n```\r\n**Full Changelog**: https://github.com/a/b/compare/v1...v2\r\n";
        auto lines = changelogLines(md);
        CHECK_EQ(lines.size(), (size_t)6);
        CHECK_EQ(lines[0], std::string("What's new"));
        CHECK_EQ(lines[1], std::string(""));
        CHECK_EQ(lines[2], std::string("- Subtitles now work at 1080p"));
        CHECK_EQ(lines[3], std::string("- Fixed the Cemu crash"));          // link kept as text, emoji gone
        CHECK_EQ(lines[4], std::string(""));                                  // the image line vanished, blank runs collapsed
        CHECK_EQ(lines[5], std::string("code here"));                        // fenced text kept as it is
        CHECK(changelogLines("").empty());
        CHECK(changelogLines("   \n\n").empty());
        // Length caps.
        std::string many;
        for (int i = 0; i < 30; i++) many += "- item " + std::to_string(i) + "\n";
        auto cut = changelogLines(many, 12);
        CHECK_EQ(cut.size(), (size_t)13);
        CHECK(has(cut.back(), "release page") && cut[0] == "- item 0" && cut[11] == "- item 11");
        auto longLine = changelogLines(std::string(500, 'a'), 12, 50);
        CHECK(longLine.size() == 1 && longLine[0].size() == 50 && longLine[0].compare(47, 3, "...") == 0);
        // Wired through: GitHub's "body" reaches the update window and the Settings card.
        Fake f;
        f.github = answer(200, R"({"tag_name":"v1.3.1","html_url":"https://g/r","body":"## Changes\r\n- Fixed a crash\r\n- Faster loading"})");
        f.store = answer(200, storeJson("1.3.0"));
        Outcome o = checkForUpdate(f.fetcher(), "1.3.0", "o", "r", "ufin");
        CHECK(o.kind == Outcome::Kind::Available && o.info.changelog.size() == 3);
        std::string pj, cj;
        for (auto& l : promptLines(o.info, "1.3.0")) pj += l + "\n";
        for (auto& l : resultCard(o, "1.3.0").lines) cj += l + "\n";
        CHECK(has(pj, "What's new:") && has(pj, "- Fixed a crash") && has(pj, "- Faster loading"));
        CHECK(has(cj, "What's new:") && has(cj, "- Fixed a crash"));
        CHECK(pj.find("What's new:") > pj.find("Download it from GitHub"));   // the changes come last: the card clips its bottom
        // No description: no "What's new" heading.
        Fake g; g.github = answer(200, kRelease131); g.store = answer(200, storeJson("1.3.1"));
        Outcome og = checkForUpdate(g.fetcher(), "1.3.0", "o", "r", "ufin");
        std::string pg;
        for (auto& l : promptLines(og.info, "1.3.0")) pg += l + "\n";
        CHECK(!has(pg, "What's new"));
    }

    return check::finish("test_update_check");
}
