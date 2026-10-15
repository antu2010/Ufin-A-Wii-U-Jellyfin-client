// The one place Ufin's version and home are written down.
//
// kVersion is what this build calls itself: the About text, the Jellyfin
// client string and the startup update check (update_check.h) all read it, so a
// release only has to change this line.
#pragma once

namespace ufin {

constexpr const char* kVersion = "1.1.0";

// Where releases are published (the startup update check asks GitHub for the
// latest one) and where people can read about / download them.
constexpr const char* kGithubOwner = "antu2010";
constexpr const char* kGithubRepo = "Ufin-A-Wii-U-Jellyfin-client";

// The package name Ufin has in the Homebrew App Store's repo.json
// (https://wiiu.cdn.fortheusers.org/repo.json). Compared case-insensitively.
constexpr const char* kStorePackageName = "ufin";

} // namespace ufin
