// Startup update check: is there a newer Ufin release on GitHub, and is it
// already in the Homebrew App Store?
//
// Everything here is pure logic over strings plus one injected "fetch this
// HTTPS URL" function, so it is fully host-testable; the real transport (the
// Wii U's system TLS) lives in https_client.h and the thread / on-screen prompt
// in main.cpp.
//
// The rules (from the feature request):
//   * GitHub's latest release newer than this build  -> tell the user to update;
//   * equal, or this build is newer (a dev build)     -> say nothing;
//   * anything that goes wrong (offline, TLS refused, rate limited, unexpected
//     JSON)                                           -> say nothing, log why.
// A failed check must never get in the user's way, so nothing here throws, and
// every outcome other than "update available" is silent.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace updatecheck {

// ---- versions --------------------------------------------------------------

struct Version {
    bool valid = false;
    int major = 0, minor = 0, patch = 0;
    bool prerelease = false; // "1.3.0-beta2": sorts *before* 1.3.0
    int prereleaseRank = 0;  // digits found in the suffix, only to order two prereleases
};

// "1.3.0", "v1.3.0", "V1.3", "1.3.0-beta2", "1.3.0+build5". Anything that does not
// start with a number (after an optional v) is invalid.
Version parseVersion(const std::string& text);
// <0 if a is older than b, 0 if equal, >0 if newer. Invalid versions compare as
// equal to everything (callers check .valid first).
int compareVersions(const Version& a, const Version& b);

// ---- what the two sources said --------------------------------------------

struct GithubRelease {
    std::string tag;       // "v1.3.1"
    std::string url;       // the release page
    bool prerelease = false;
    bool draft = false;
    std::string notes;     // the release description ("body", markdown)
};
// GET /repos/{owner}/{repo}/releases/latest
bool parseGithubRelease(const std::string& json, GithubRelease& out);
// repo.json of the Homebrew App Store: the version listed for `packageName`, if any.
bool parseStoreVersion(const std::string& json, const std::string& packageName, std::string& versionOut);

// A GitHub release description (markdown) as plain lines for the update window: headings and emphasis
// marks removed, links reduced to their text, images / HTML comments / rules / bare "Full Changelog" links
// dropped, emoji (which the UI font cannot draw) removed, bullets as "- ", blank runs collapsed, long lines
// cut. At most maxLines lines; when the description is longer the last line says to read the rest on GitHub.
std::vector<std::string> changelogLines(const std::string& markdown, size_t maxLines = 12, size_t maxLineChars = 140);

// ---- the verdict -----------------------------------------------------------

struct UpdateInfo {
    bool available = false;      // GitHub has a release newer than this build
    std::string latestVersion;   // "1.3.1" (no leading v)
    std::string releaseUrl;      // GitHub release page
    bool inStore = false;        // the App Store already lists that version (or a newer one)
    std::vector<std::string> changelog; // the release description, tidied for the screen (see changelogLines)
};

// `store` may be empty (not listed, or the store could not be reached).
UpdateInfo decide(const std::string& currentVersion, const GithubRelease& github, const std::string& storeVersion);

// The lines of the on-screen prompt (the UI wraps long ones).
std::vector<std::string> promptLines(const UpdateInfo& info, const std::string& currentVersion);

// ---- orchestration ---------------------------------------------------------

struct FetchResult {
    bool ok = false;     // got an HTTP response at all
    int status = 0;
    std::string body;
    std::string error;   // when !ok: why (for the log)
};
// GET https://host + path with the given extra header lines ("Name: value\r\n" each).
using Fetcher = std::function<FetchResult(const std::string& host, const std::string& path,
                                          const std::string& extraHeaders)>;

struct Outcome {
    enum class Kind { Unknown, UpToDate, Available } kind = Kind::Unknown; // Unknown = the check itself failed
    UpdateInfo info;        // meaningful when kind == Available
    std::string detail;     // one log line saying what happened (also shown when the check failed)
    // What GitHub said, whenever it said something usable (empty when the check failed early).
    std::string githubVersion;   // "1.3.1", no leading v
    bool githubPrerelease = false; // that release is a pre-release / draft, so it never counts as an update
    bool buildIsNewer = false;   // this build is newer than GitHub's latest stable release ("a dev build")
};

// Asks GitHub for the latest release and, only when it is newer than
// currentVersion, the App Store for its listing. `cancelled` (optional) is
// polled between requests.
Outcome checkForUpdate(const Fetcher& fetch, const std::string& currentVersion, const std::string& owner,
              const std::string& repo, const std::string& storePackage,
              const std::function<bool()>& cancelled = {});

// The card shown when the user asks for a check themselves (Settings > Check for updates): always
// says what this build is and what GitHub has, and which of the things that means (update
// available / up to date / "you're in the future" / check failed).
struct Card {
    std::string title;
    std::vector<std::string> lines;
    bool isError = false;
};
Card resultCard(const Outcome& outcome, const std::string& currentVersion);

// Where the App Store's package list lives.
constexpr const char* kStoreHost = "wiiu.cdn.fortheusers.org";
constexpr const char* kStorePath = "/repo.json";

} // namespace updatecheck
