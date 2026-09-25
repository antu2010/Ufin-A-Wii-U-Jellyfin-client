// Loads server/login config from sd:/wiiu/apps/ufin/config.json at
// startup, so credentials don't need to be hardcoded and rebuilt every
// time. Falls back to config.h's UFIN_SERVER_HOST/etc constants if the
// file is missing, unreadable, or malformed -- callers should treat a
// false return as "use the hardcoded defaults", not a fatal error.
//
// Cemu maps sd:/ to its own sdcard folder (e.g. ~/.local/share/Cemu/sdcard
// on Linux), so the same config.json works in the emulator too.

#pragma once
#include <string>

struct UfinConfig {
    std::string host;
    int port = 0;
    std::string username;
    std::string password;    // only if the user wrote one into config.json; the app never saves it

    // Saved sign-in (written by the login screen): used instead of the
    // password when present.
    std::string userId;
    std::string accessToken;
    std::string deviceId;    // generated once, so Jellyfin sees one device

    // Menu music: a song from the user's own library, looped quietly in
    // the menus.
    bool menuMusicEnabled = false;
    std::string menuMusicItemId;
    std::string menuMusicName;

    // Look & feel (Settings)
    bool crt = false;              // scanlines + vignette over everything
    int accent = 0;                // ui::Accent index
    bool ambient = false;          // drifting glow behind the menus
    bool snow = false;             // falling snow over the menus
    bool clock = false;            // time in the header
    bool rainbowUnlocked = false;  // hidden accent (tap About seven times)

    // Playback
    bool autoplayNext = true;      // next episode after a countdown
    bool gamepadOffInVideo = false; // turn the GamePad screen off during TV playback

    // Optional video transcode settings (see JellyfinClient::
    // buildVideoStreamUrl for what they mean and why the defaults are
    // what they are). Missing from config.json -> these defaults.
    int videoBitrate = 2500000;
    std::string videoProfile = "baseline";
};

// True if there is enough to connect: a server plus a token or a password.
bool hasLogin(const UfinConfig& cfg);

// Tries to load and parse the config file. On success, fills outConfig
// and returns true. On failure, returns false and sets outError to a
// short human-readable reason (missing file, invalid JSON, missing
// field) -- meant to be shown on screen, not just logged.
bool loadConfigFromSD(UfinConfig& outConfig, std::string& outError);

// Same, from an arbitrary path (what loadConfigFromSD calls with the SD
// card location; also what the host tests use).
bool loadConfigFromFile(const char* path, UfinConfig& outConfig, std::string& outError);

// Writes the config back (creating the folder if needed). Keys Ufin
// doesn't know are kept as they were. The password is only written if
// it came from the file in the first place.
bool saveConfigToSD(const UfinConfig& config, std::string& outError);
bool saveConfigToFile(const char* path, const UfinConfig& config, std::string& outError);
