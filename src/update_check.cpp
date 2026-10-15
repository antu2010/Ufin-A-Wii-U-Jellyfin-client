#include "update_check.h"

#include "vendor/cJSON.h"

#include <cctype>
#include <cstdlib>

namespace updatecheck {

// ---- versions --------------------------------------------------------------

static bool readNumber(const std::string& s, size_t& pos, int& out) {
    size_t start = pos;
    long v = 0;
    while (pos < s.size() && isdigit((unsigned char)s[pos])) {
        v = v * 10 + (s[pos] - '0');
        if (pos - start > 6) return false; // absurdly long: not a version
        pos++;
    }
    if (pos == start) return false;
    out = (int)v;
    return true;
}

Version parseVersion(const std::string& text) {
    Version v;
    size_t i = 0;
    while (i < text.size() && isspace((unsigned char)text[i])) i++;
    if (i < text.size() && (text[i] == 'v' || text[i] == 'V')) i++;
    int parts[3] = {0, 0, 0};
    int count = 0;
    while (count < 3) {
        if (!readNumber(text, i, parts[count])) return Version();
        count++;
        if (i < text.size() && text[i] == '.' && count < 3) {
            i++;
            continue;
        }
        break;
    }
    // What may follow: nothing, "-suffix" (prerelease), "+build", or trailing space.
    if (i < text.size() && text[i] == '-') {
        v.prerelease = true;
        size_t j = i + 1;
        while (j < text.size() && !isdigit((unsigned char)text[j]) && text[j] != '+') j++;
        int rank = 0;
        if (j < text.size() && isdigit((unsigned char)text[j])) readNumber(text, j, rank);
        v.prereleaseRank = rank;
    } else if (i < text.size() && text[i] != '+') {
        while (i < text.size() && isspace((unsigned char)text[i])) i++;
        if (i < text.size()) return Version(); // "1.3.0.1", "1.3.0abc": not understood, so never compared
    }
    v.valid = true;
    v.major = parts[0];
    v.minor = parts[1];
    v.patch = parts[2];
    return v;
}

int compareVersions(const Version& a, const Version& b) {
    if (!a.valid || !b.valid) return 0;
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.prerelease != b.prerelease) return a.prerelease ? -1 : 1; // 1.3.0-beta < 1.3.0
    if (a.prerelease && a.prereleaseRank != b.prereleaseRank) return a.prereleaseRank < b.prereleaseRank ? -1 : 1;
    return 0;
}

// ---- parsing the two sources ----------------------------------------------

static std::string stringField(const cJSON* obj, const char* name) {
    const cJSON* f = cJSON_GetObjectItemCaseSensitive(obj, name);
    return (cJSON_IsString(f) && f->valuestring) ? std::string(f->valuestring) : std::string();
}

bool parseGithubRelease(const std::string& json, GithubRelease& out) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) return false;
    bool ok = false;
    if (cJSON_IsObject(root)) {
        GithubRelease r;
        r.tag = stringField(root, "tag_name");
        r.url = stringField(root, "html_url");
        const cJSON* pre = cJSON_GetObjectItemCaseSensitive(root, "prerelease");
        const cJSON* draft = cJSON_GetObjectItemCaseSensitive(root, "draft");
        r.prerelease = cJSON_IsTrue(pre);
        r.draft = cJSON_IsTrue(draft);
        r.notes = stringField(root, "body");
        if (!r.tag.empty()) {
            out = r;
            ok = true;
        }
    }
    cJSON_Delete(root);
    return ok;
}

static std::string lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool parseStoreVersion(const std::string& json, const std::string& packageName, std::string& versionOut) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) return false;
    bool found = false;
    const cJSON* packages = cJSON_GetObjectItemCaseSensitive(root, "packages");
    if (cJSON_IsArray(packages)) {
        const std::string want = lower(packageName);
        const cJSON* p = nullptr;
        cJSON_ArrayForEach(p, packages) {
            if (!cJSON_IsObject(p)) continue;
            if (lower(stringField(p, "name")) != want) continue;
            const std::string version = stringField(p, "version");
            if (!version.empty()) {
                versionOut = version;
                found = true;
            }
            break;
        }
    }
    cJSON_Delete(root);
    return found;
}

// ---- verdict ---------------------------------------------------------------

static std::string withoutV(const std::string& tag) {
    size_t i = 0;
    while (i < tag.size() && isspace((unsigned char)tag[i])) i++;
    if (i < tag.size() && (tag[i] == 'v' || tag[i] == 'V')) i++;
    return tag.substr(i);
}


// ---- the changelog ---------------------------------------------------------

static std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
    return s.substr(a, b - a);
}

static void removeAll(std::string& s, const std::string& what) {
    size_t p;
    while ((p = s.find(what)) != std::string::npos) s.erase(p, what.size());
}

// "[text](url)" -> "text"; "![alt](url)" -> "". Plain text otherwise.
static std::string stripLinks(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size();) {
        const bool image = in[i] == '!' && i + 1 < in.size() && in[i + 1] == '[';
        if (in[i] == '[' || image) {
            const size_t open = image ? i + 1 : i;
            const size_t close = in.find(']', open);
            if (close != std::string::npos && close + 1 < in.size() && in[close + 1] == '(') {
                const size_t end = in.find(')', close + 2);
                if (end != std::string::npos) {
                    if (!image) out += in.substr(open + 1, close - open - 1);
                    i = end + 1;
                    continue;
                }
            }
        }
        out += in[i++];
    }
    return out;
}

// Drops 4-byte UTF-8 sequences (emoji and friends) and control characters.
static std::string plainText(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size();) {
        const unsigned char c = (unsigned char)in[i];
        if (c >= 0xF0) { i += 4; continue; }
        if (c < 0x20 && c != '\t') { i++; continue; }
        out += (char)c;
        i++;
    }
    return out;
}

std::vector<std::string> changelogLines(const std::string& markdown, size_t maxLines, size_t maxLineChars) {
    std::string text = markdown;
    // HTML comments (release templates are full of them).
    for (size_t a; (a = text.find("<!--")) != std::string::npos;) {
        const size_t b = text.find("-->", a);
        text.erase(a, b == std::string::npos ? std::string::npos : b + 3 - a);
    }
    std::vector<std::string> lines;
    bool inFence = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = trimmed(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.compare(0, 3, "```") == 0) { inFence = !inFence; continue; }
        if (!inFence) {
            line = plainText(stripLinks(line));
            while (!line.empty() && line[0] == '#') line.erase(0, 1);
            for (const char* mark : {"**", "__", "`"}) removeAll(line, mark);
            line = trimmed(line);
            if (line.size() >= 3 && line.find_first_not_of("-*_= ") == std::string::npos && line.find_first_not_of(' ') != std::string::npos &&
                line.find_first_not_of("-*_=") == std::string::npos) continue; // a horizontal rule
            if (line.size() > 2 && (line[0] == '-' || line[0] == '*' || line[0] == '+') && line[1] == ' ') line = "- " + trimmed(line.substr(2));
            if (line.rfind("Full Changelog", 0) == 0 || line.rfind("**Full Changelog", 0) == 0) continue;
        }
        if (line.empty()) {
            if (!lines.empty() && !lines.back().empty()) lines.push_back("");
            continue;
        }
        if (line.size() > maxLineChars) line = line.substr(0, maxLineChars - 3) + "...";
        lines.push_back(line);
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    if (lines.size() > maxLines) {
        lines.resize(maxLines);
        while (!lines.empty() && lines.back().empty()) lines.pop_back();
        lines.push_back("... (the rest is on the release page)");
    }
    return lines;
}

UpdateInfo decide(const std::string& currentVersion, const GithubRelease& github, const std::string& storeVersion) {
    UpdateInfo info;
    if (github.draft || github.prerelease) return info; // never push a pre-release on people
    const Version latest = parseVersion(github.tag);
    const Version current = parseVersion(currentVersion);
    if (!latest.valid || !current.valid) return info;
    if (compareVersions(latest, current) <= 0) return info; // same, or this build is newer
    info.available = true;
    info.latestVersion = withoutV(github.tag);
    info.releaseUrl = github.url;
    info.changelog = changelogLines(github.notes, 8);
    const Version store = parseVersion(storeVersion);
    info.inStore = store.valid && compareVersions(store, latest) >= 0;
    return info;
}

std::vector<std::string> promptLines(const UpdateInfo& info, const std::string& currentVersion) {
    std::vector<std::string> lines;
    lines.push_back("Ufin " + info.latestVersion + " is available (you have " + currentVersion + ").");
    lines.push_back("");
    if (info.inStore) {
        lines.push_back("Update it from the Homebrew App Store, or download it from GitHub:");
    } else {
        lines.push_back("Download it from GitHub:");
    }
    lines.push_back(info.releaseUrl);
    if (!info.inStore) {
        lines.push_back("");
        lines.push_back("It is not in the Homebrew App Store yet.");
    }
    // Last, because the card has a fixed height and clips what does not fit: the download details above
    // must never be the part that gets cut.
    if (!info.changelog.empty()) {
        lines.push_back("");
        lines.push_back("What's new:");
        for (const auto& l : info.changelog) lines.push_back(l);
    }
    return lines;
}


// ---- the manual-check card -------------------------------------------------

Card resultCard(const Outcome& o, const std::string& currentVersion) {
    Card card;
    const std::string yours = "Your version: " + currentVersion;
    switch (o.kind) {
        case Outcome::Kind::Available: {
            card.title = "Update available";
            card.lines = {yours, "Latest on GitHub: " + o.info.latestVersion, "", "Please update Ufin to the latest version.", ""};
            card.lines.push_back(o.info.inStore ? "Get it from the Homebrew App Store, or download it from GitHub:"
                                                : "Download it from GitHub:");
            card.lines.push_back(o.info.releaseUrl);
            if (!o.info.inStore) {
                card.lines.push_back("");
                card.lines.push_back("It is not in the Homebrew App Store yet.");
            }
            if (!o.info.changelog.empty()) { // last: the card clips at its bottom edge
                card.lines.push_back("");
                card.lines.push_back("What's new:");
                for (const auto& l : o.info.changelog) card.lines.push_back(l);
            }
            break;
        }
        case Outcome::Kind::UpToDate: {
            const std::string theirs = "Latest on GitHub: " + o.githubVersion;
            if (o.buildIsNewer) {
                card.title = "Welcome to the future!";
                card.lines = {yours, theirs, "", "Wow, you seem to be in the future! You have a newer version than the latest release."};
            } else if (o.githubPrerelease) {
                card.title = "You're up to date";
                card.lines = {yours, theirs + " (a pre-release, so not offered)", "", "There is no newer stable release."};
            } else {
                card.title = "You're up to date";
                card.lines = {yours, theirs, "", "You have the latest version."};
            }
            break;
        }
        case Outcome::Kind::Unknown:
        default:
            card.title = "Couldn't check for updates";
            card.isError = true;
            card.lines = {yours, "", o.detail.empty() ? std::string("The check did not finish.") : o.detail, "",
                          "Make sure the console is online and try again."};
            break;
    }
    return card;
}

// ---- orchestration ---------------------------------------------------------

static Outcome unknown(const std::string& why) {
    Outcome o;
    o.kind = Outcome::Kind::Unknown;
    o.detail = why;
    return o;
}

Outcome checkForUpdate(const Fetcher& fetch, const std::string& currentVersion, const std::string& owner,
              const std::string& repo, const std::string& storePackage, const std::function<bool()>& cancelled) {
    if (!fetch) return unknown("no transport");
    if (!parseVersion(currentVersion).valid) return unknown("this build's version \"" + currentVersion + "\" is not a version number");

    const std::string agent = "User-Agent: Ufin/" + currentVersion + "\r\n"; // GitHub rejects requests without one
    FetchResult gh = fetch("api.github.com", "/repos/" + owner + "/" + repo + "/releases/latest",
                           agent + "Accept: application/vnd.github+json\r\n");
    if (!gh.ok) return unknown("GitHub not reachable: " + gh.error);
    if (gh.status != 200) return unknown("GitHub answered HTTP " + std::to_string(gh.status));

    GithubRelease release;
    if (!parseGithubRelease(gh.body, release)) return unknown("GitHub's answer was not a release");
    if (release.url.empty()) release.url = "https://github.com/" + owner + "/" + repo + "/releases";

    const Version latest = parseVersion(release.tag);
    if (!latest.valid) return unknown("latest release tag \"" + release.tag + "\" is not a version number");
    const std::string githubVersion = withoutV(release.tag);

    UpdateInfo info = decide(currentVersion, release, "");
    if (!info.available) {
        Outcome o;
        o.kind = Outcome::Kind::UpToDate;
        o.githubVersion = githubVersion;
        o.githubPrerelease = release.prerelease || release.draft;
        const int c = compareVersions(latest, parseVersion(currentVersion));
        o.buildIsNewer = !o.githubPrerelease && c < 0;
        o.detail = std::string(c == 0 ? "up to date" : (release.draft || release.prerelease ? "latest release is a pre-release, ignored"
                                                                                              : "this build is newer than the latest release")) +
                   " (GitHub " + release.tag + ", this build " + currentVersion + ")";
        return o;
    }

    // Newer than this build: is it in the App Store too? Best effort -- a failure here only
    // means the prompt names GitHub alone.
    std::string storeVersion;
    if (!(cancelled && cancelled())) {
        FetchResult store = fetch(kStoreHost, kStorePath, agent);
        if (store.ok && store.status == 200) {
            parseStoreVersion(store.body, storePackage, storeVersion);
        }
    }
    info = decide(currentVersion, release, storeVersion);

    Outcome o;
    o.kind = Outcome::Kind::Available;
    o.info = info;
    o.githubVersion = githubVersion;
    o.detail = "update available: " + info.latestVersion + " (this build " + currentVersion + "), " +
               (info.inStore ? "also in the Homebrew App Store" : "not in the Homebrew App Store");
    return o;
}

} // namespace updatecheck
