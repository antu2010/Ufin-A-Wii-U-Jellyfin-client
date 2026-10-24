// Loads server/login config from sd:/wiiu/apps/ufin/config.json at
// startup, so credentials don't need to be hardcoded and rebuilt every
// time. Falls back to config.h's UFIN_SERVER_HOST/etc constants if the
// file is missing, unreadable, or malformed -- callers should treat a
// false return as "use the hardcoded defaults", not a fatal error.
//
// Cemu maps sd:/ to its own sdcard folder (e.g. ~/.local/share/Cemu/sdcard
// on Linux), so the same config.json works in the emulator too.

#pragma once
#include "h264_profile.h"
#include "profiles.h"
#include "video_size.h"
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

// A Jellyfin server reached over https (see net_conn.h). `pin` = the SHA-256 fingerprint (64 hex digits) of the one
// certificate the user chose to trust for it (a self-signed certificate); "" = it must have a certificate a public
// authority signed.
struct TlsServer {
    std::string host;
    int port = 0;
    std::string pin;
};

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
    std::vector<TlsServer> tlsServers; // the servers (host + port) that speak https; any other is plain http

    // "Who's watching?" profiles: every saved sign-in (see profiles.h). host / user_id / access_token
    // above always mirror the profile used last, so an older Ufin still finds a sign-in.
    std::vector<Profile> profiles;
    int lastProfile = -1;    // index into `profiles` of the one used last, -1 = none

    // Menu music: a song from the user's own library, looped quietly in
    // the menus.
    long long usageSeconds = 0;     // total time Ufin has been used, in seconds (sidebar counter); saved every 30 min
    bool logging = false;           // write ufin_log.txt to the SD card (off: spares the card)
    bool jellyfinBackdrops = false; // show the selected title's Jellyfin backdrop behind the whole UI
    bool menuMusicEnabled = true; // default on: plays assets/bgm.m4a unless a library song is chosen
    std::string menuMusicItemId;
    std::string menuMusicName;

    // Look & feel (Settings)
    bool crt = false;              // scanlines + vignette over everything
    int accent = 0;                // ui::Accent index
    // Settings > Customization: "Custom Palette" replaces the premade accent with three colours of the user's
    // own (0xRRGGBB, defaults = Jellyfin blue) and an arrangement (0..5) saying which one is main / deep / second.
    bool customPalette = false;
    uint32_t customColor[3] = {0x00A4DC, 0x0B7FB0, 0xAA5CC3};
    int customArrange = 0;
    bool paletteEdited = false;    // (not saved) the colours were touched this session: do not re-seed them from the accent
    bool customShapes = false;     // Material 3 Expressive: "Customize shapes" is on
    int buttonSize[8] = {100, 100, 100, 100, 100, 100, 100, 100}; // width, height per button kind, in percent (60..130; dialog height 60..112)
    int buttonShape[4] = {0, 0, 0, 0}; // per button kind (play/pause, seek & skip, toggles, dialog); 0 = default look
    bool artworkAccent = false;    // Material 3 Expressive only: the accent follows the playing item's artwork
    bool lightMode = false;        // light palette instead of the dark default
    // Animated background effect. ui::Background index: 0=Off, 1=Circles
    // (the original drifting glow), 2=Wii U-style bubbles, 3=Animated
    // gradient, 4=Polygons, 5=Starfield, then the Liquid Glass showcases 6=Grid, 7=Aurora,
    // 8=Ripples, 9=Beams, 10=Orbs, 11=Waves. Out-of-range values are clamped
    // back to Off by App::applyLook().
    int backgroundEffect = 0;
    bool snow = false;             // falling snow over the menus
    bool liquidGlass = false;      // Liquid Glass UI: frosted, rounded, softly lit panels (default off)
    // Material 3 Expressive UI (default off). Mutually exclusive with liquidGlass (the UI style is exactly one
    // of Plain / Liquid Glass / M3E); if a hand-edited config.json sets both, loadConfig keeps Liquid Glass.
    bool m3Expressive = false;
    // Liquid Glass bends the real picture behind it (needs a GPU copy of the frame per glass
    // layer). On by default; "glass_refraction": false falls back to the soft blurred glass.
    bool glassRefraction = true;
    // Which steps of the frame-copy are performed (config.json only; ignored unless
    // glass_refraction is on). A bring-up ladder from real-hardware testing -- the default,
    // 6, is the lowest that works on a console (verified on a Wii U with a 1080p TV):
    //   1 allocate the snapshot textures only        2 + insert the draw-list callbacks (copies nothing)
    //   3 + copy the frame (bare GX2CopySurface)     4 + cache invalidation and render-target restore
    //   5 + re-assert shader mode, viewport and scissor after the copy
    //   6 + re-enter the display's libwhb render pass (context state) after the copy   (default)
    //   7 + snapshot textures made without the colour-buffer flag (also works)
    // 3, 4 and 5 blank the UI on real hardware (Cemu is fine with all of them); they are kept
    // so a console that misbehaves can be bisected. Each stage includes the ones before it,
    // except that 7 also changes how the textures are created.
    int glassRefractionStage = 6;
    // Liquid Glass while the music visualizer is on (the TV shows the visualizer, the GamePad Now Playing: two render
    // passes in one frame). config.json only. 0 = the GamePad refracts, the TV's glass is flat (default: a snapshot copy
    // in the TV pass made the visualizer vanish from the TV and show under the GamePad's UI in a Cemu test);
    // 1 = flat glass on both displays in those frames; 2 = both refract (the old behaviour, for comparison).
    int visualizerGlass = 0;
    int glassBlur = 1;             // Liquid Glass frost: 0 = sharp refraction, 1 light, 2 medium, 3 strong (a shrunk, smoothed copy of the picture is what the glass bends); config.json only
    bool glassDebug = false;       // log the glass blur's cost every few seconds (config.json only)
    bool clock = false;            // time in the header
    bool rainbowEasterEgg = false; // found the hidden About easter egg -- reveals the toggle below
    bool rainbowUnlocked = false;  // Rainbow accent toggle state (only shown/settable once the egg is found)

    // Playback
    bool autoplayNext = true;      // next episode after a countdown
    bool gamepadOffInVideo = false; // turn the GamePad screen off during TV playback
    bool alwaysDefaultTracks = false; // true: start videos with the default audio / subtitles instead of asking first

    // Diagnostic/testing escape hatch for the h264_wiiu macroblock-alignment
    // safety check in Decoder::open() (see decoder.cpp): that check refuses
    // to open the hardware decoder for a stream whose width/height aren't
    // 256/16-pixel aligned, since an unaligned stream previously meant an
    // under-allocated framebuffer and a real hardware crash. Surfaced in
    // Settings as "Strict video format check" (inverted there: On means
    // this is false). Defaults to true (protection off) as of the
    // FFmpeg-wiiu aligned-framebuffer fix in h264_wiiu.c -- real-hardware
    // testing after that fix still showed an occasional crash (~1 in 3 on
    // known-bad titles), so this is not a fully closed issue; a user who
    // hits it can turn the check back on from Settings.
    bool allowUnalignedVideoGeometry = true;
    // On start-up, ask GitHub (over HTTPS) whether a newer Ufin release exists and say so once if it
    // does (see update_check.h). On by default; "check_for_updates": false, Settings > Automatic update
    // check or Y on the prompt turn it off. Settings > Check for updates works either way.
    bool checkForUpdates = true;
    // The release the user chose "Don't remind me" for on the start-up prompt ("1.3.1"); only that
    // version is skipped -- a newer one prompts again. Settings > Check for updates still shows it.
    std::string skippedUpdateVersion;

    // Optional video transcode settings (see JellyfinClient::
    // buildVideoStreamUrl for what they mean and why the defaults are
    // what they are). Missing from config.json -> these defaults.
    int videoBitrate = 2500000;
    std::string videoProfile = h264::wireName(h264::kDefault); // Settings > H.264 profile
    // Transcode height: 480 (low bandwidth), 720 (stable, the default) or 1080
    // (experimental 1080p30). Opt in via Settings > Video quality or
    // "video_height" in config.json. The width follows (768 / 1280 / 1920; see
    // video_size.h for why 480p is 768x480). Any other value in config.json is
    // ignored and stays 720. When video_height is 480 or 1080 and video_bitrate
    // is not set, the bitrate defaults to 1.5 Mbps / 6 Mbps.
    int videoHeight = 720;
    int videoWidth() const { return videosize::forHeight(videoHeight).width; }
    static constexpr int kDefault480Bitrate = videosize::defaultBitrateFor(480);
    static constexpr int kDefault1080Bitrate = videosize::defaultBitrateFor(1080);
    // Optional buffering overrides for tuning on a real console, 0 = automatic
    // (see Player::play): compressed read-ahead cap in MiB, and decoded-frame
    // queue depth. Absent/invalid values stay 0.
    int videoBufferMiB = 0;
    int videoQueueFrames = 0;
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

// The https entry for host:port, or nullptr (host compared without regard to case).
inline const TlsServer* findTlsServer(const std::vector<TlsServer>& list, const std::string& host, int port) {
    auto lower = [](std::string t) { for (char& c : t) c = (char)tolower((unsigned char)c); return t; };
    const std::string h = lower(host);
    for (const TlsServer& s : list) if (s.port == port && lower(s.host) == h) return &s;
    return nullptr;
}
// Adds or updates the entry for host:port (https = false removes it).
inline void setTlsServer(std::vector<TlsServer>& list, const std::string& host, int port, bool https, const std::string& pin) {
    for (size_t i = 0; i < list.size(); i++) {
        if (list[i].port == port && findTlsServer({list[i]}, host, port)) {
            if (https) list[i].pin = pin; else list.erase(list.begin() + (long)i);
            return;
        }
    }
    if (https && list.size() < 16) list.push_back(TlsServer{host, port, pin});
}
