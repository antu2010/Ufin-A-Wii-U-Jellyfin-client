#include "config_loader.h"
#include "vendor/cJSON.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <vector>

// NOTE: this SD mount path (/vol/external01/...) is the standard
// convention for Wii U homebrew SD card access, but hasn't been
// compile/run-tested against this specific wut version -- if the file
// genuinely exists on the SD card but this still reports "not found",
// this path is the first thing to double check.
static const char* CONFIG_PATH = "/vol/external01/wiiu/apps/ufin/config.json";

bool loadConfigFromSD(UfinConfig& outConfig, std::string& outError) {
    return loadConfigFromFile(CONFIG_PATH, outConfig, outError);
}

bool loadConfigFromFile(const char* path, UfinConfig& outConfig, std::string& outError) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        outError = "config.json not found";
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        outError = "config.json is empty";
        return false;
    }

    std::vector<char> buf((size_t)size + 1);
    size_t readBytes = fread(buf.data(), 1, (size_t)size, f);
    fclose(f);
    buf[readBytes] = '\0';

    cJSON* json = cJSON_Parse(buf.data());
    if (!json) {
        outError = "config.json is not valid JSON";
        return false;
    }

    auto str = [json](const char* key) {
        cJSON* v = cJSON_GetObjectItem(json, key);
        return cJSON_IsString(v) ? std::string(v->valuestring) : std::string();
    };
    auto boolean = [json](const char* key, bool fallback) {
        cJSON* v = cJSON_GetObjectItem(json, key);
        return cJSON_IsBool(v) ? (bool)cJSON_IsTrue(v) : fallback;
    };

    outConfig.host = str("host");
    cJSON* port = cJSON_GetObjectItem(json, "port");
    outConfig.port = cJSON_IsNumber(port) ? port->valueint : 0;
    outConfig.username = str("username");
    outConfig.password = str("password");
    outConfig.userId = str("user_id");
    outConfig.accessToken = str("access_token");
    outConfig.deviceId = str("device_id");
    cJSON* usage = cJSON_GetObjectItem(json, "usage_seconds");
    outConfig.usageSeconds = (cJSON_IsNumber(usage) && usage->valuedouble >= 0.0 && usage->valuedouble < 1e15)
                                 ? (long long)usage->valuedouble : 0;
    outConfig.profiles.clear();
    outConfig.lastProfile = -1;
    if (cJSON* list = cJSON_GetObjectItem(json, "profiles"); cJSON_IsArray(list)) {
        cJSON* entry = nullptr;
        cJSON_ArrayForEach(entry, list) {
            if (!cJSON_IsObject(entry) || outConfig.profiles.size() >= profiles::kMax) continue;
            auto field = [entry](const char* key) {
                cJSON* v = cJSON_GetObjectItem(entry, key);
                return cJSON_IsString(v) ? std::string(v->valuestring) : std::string();
            };
            Profile p;
            p.name = field("name");
            p.host = field("host");
            p.userId = field("user_id");
            p.accessToken = field("access_token");
            cJSON* pp = cJSON_GetObjectItem(entry, "port");
            p.port = cJSON_IsNumber(pp) ? pp->valueint : 0;
            cJSON* keep = cJSON_GetObjectItem(entry, "keep_logged_in");
            p.keepLoggedIn = cJSON_IsTrue(keep);
            cJSON* color = cJSON_GetObjectItem(entry, "color");
            p.color = cJSON_IsNumber(color) && color->valueint >= 0 ? color->valueint % profiles::kColors : -1;
            if (p.name.empty() && p.userId.empty()) continue; // nothing to show or sign in with
            if (p.name.empty()) p.name = "User";
            if (p.color < 0) p.color = profiles::nextColor(outConfig.profiles);
            outConfig.profiles.push_back(p);
        }
        cJSON* last = cJSON_GetObjectItem(json, "last_profile");
        if (cJSON_IsNumber(last) && last->valueint >= 0 && (size_t)last->valueint < outConfig.profiles.size())
            outConfig.lastProfile = last->valueint;
        profiles::normalizeAutoLogin(outConfig.profiles, outConfig.lastProfile); // only one auto log in
    }
    outConfig.logging = boolean("logging", false);
    outConfig.jellyfinBackdrops = boolean("jellyfin_backdrops", false);
    outConfig.menuMusicEnabled = boolean("menu_music", true);
    outConfig.menuMusicItemId = str("menu_music_item");
    outConfig.menuMusicName = str("menu_music_name");
    outConfig.crt = boolean("crt", false);
    {
        cJSON* accent = cJSON_GetObjectItem(json, "accent");
        outConfig.accent = cJSON_IsNumber(accent) ? accent->valueint : 0;
        if (outConfig.accent < 0) outConfig.accent = 0;
    }
    {
        cJSON* bg = cJSON_GetObjectItem(json, "background_effect");
        if (cJSON_IsNumber(bg)) {
            outConfig.backgroundEffect = bg->valueint;
        } else {
            // Back-compat with the old on/off "ambient_background" key:
            // true becomes Circles (1), the effect it used to mean.
            outConfig.backgroundEffect = boolean("ambient_background", false) ? 1 : 0;
        }
        if (outConfig.backgroundEffect < 0 || outConfig.backgroundEffect > 5) outConfig.backgroundEffect = 0;
    }
    outConfig.lightMode = boolean("light_mode", false);
    outConfig.snow = boolean("snow", false);
    outConfig.liquidGlass = boolean("liquid_glass", false);
    outConfig.glassRefraction = boolean("glass_refraction", true);
    {
        cJSON* stage = cJSON_GetObjectItem(json, "glass_refraction_stage");
        int v = cJSON_IsNumber(stage) ? stage->valueint : 6;
        outConfig.glassRefractionStage = v < 1 ? 1 : (v > 7 ? 7 : v);
    }
    outConfig.glassDebug = boolean("glass_debug", false);
    outConfig.clock = boolean("clock", false);
    outConfig.rainbowEasterEgg = boolean("rainbow_easter_egg", false);
    outConfig.rainbowUnlocked = boolean("rainbow_unlocked", false);
    outConfig.autoplayNext = boolean("autoplay_next", true);
    outConfig.gamepadOffInVideo = boolean("gamepad_off_in_video", false);
    outConfig.allowUnalignedVideoGeometry = boolean("allow_unaligned_video_geometry", true);
    outConfig.checkForUpdates = boolean("check_for_updates", true);
    outConfig.skippedUpdateVersion = str("skipped_update_version");

    cJSON* videoHeight = cJSON_GetObjectItem(json, "video_height");
    if (cJSON_IsNumber(videoHeight) && videosize::supportedHeight(videoHeight->valueint)) {
        outConfig.videoHeight = videoHeight->valueint; // 480 low-bandwidth, 720 stable, 1080 experimental
    } else {
        outConfig.videoHeight = 720;  // absent or anything unsupported
    }
    cJSON* bufMb = cJSON_GetObjectItem(json, "video_buffer_mb");
    outConfig.videoBufferMiB = (cJSON_IsNumber(bufMb) && bufMb->valueint >= 16 && bufMb->valueint <= 512)
                                   ? bufMb->valueint : 0;
    cJSON* qFrames = cJSON_GetObjectItem(json, "video_queue_frames");
    outConfig.videoQueueFrames = (cJSON_IsNumber(qFrames) && qFrames->valueint >= 4 && qFrames->valueint <= 48)
                                     ? qFrames->valueint : 0;
    cJSON* videoBitrate = cJSON_GetObjectItem(json, "video_bitrate");
    if (cJSON_IsNumber(videoBitrate) && videoBitrate->valueint > 0) {
        outConfig.videoBitrate = videoBitrate->valueint;
    } else if (outConfig.videoHeight != 720) {
        outConfig.videoBitrate = videosize::defaultBitrateFor(outConfig.videoHeight); // 720 keeps the struct default
    }
    cJSON* videoProfile = cJSON_GetObjectItem(json, "video_profile");
    if (cJSON_IsString(videoProfile) && videoProfile->valuestring[0] != '\0') {
        outConfig.videoProfile = h264::wireName(h264::parse(videoProfile->valuestring)); // unknown -> default
    }

    cJSON_Delete(json);
    return true;
}

bool hasLogin(const UfinConfig& cfg) {
    if (cfg.host.empty() || cfg.port <= 0) return false;
    if (!cfg.accessToken.empty() && !cfg.userId.empty()) return true;
    return !cfg.username.empty() && !cfg.password.empty();
}

bool saveConfigToSD(const UfinConfig& config, std::string& outError) {
    // The app folder may not exist when Ufin runs as a .wuhb.
    mkdir("/vol/external01/wiiu", 0777);
    mkdir("/vol/external01/wiiu/apps", 0777);
    mkdir("/vol/external01/wiiu/apps/ufin", 0777);
    return saveConfigToFile(CONFIG_PATH, config, outError);
}

bool saveConfigToFile(const char* path, const UfinConfig& config, std::string& outError) {
    // Start from what's there, so keys we don't manage survive.
    cJSON* json = nullptr;
    if (FILE* in = fopen(path, "rb")) {
        std::string text;
        char chunk[1024];
        size_t n;
        while ((n = fread(chunk, 1, sizeof(chunk), in)) > 0) text.append(chunk, n);
        fclose(in);
        json = cJSON_Parse(text.c_str());
    }
    if (!json || !cJSON_IsObject(json)) {
        if (json) cJSON_Delete(json);
        json = cJSON_CreateObject();
    }

    auto setStr = [json](const char* key, const std::string& v) {
        cJSON_DeleteItemFromObject(json, key);
        cJSON_AddStringToObject(json, key, v.c_str());
    };
    auto setNum = [json](const char* key, double v) {
        cJSON_DeleteItemFromObject(json, key);
        cJSON_AddNumberToObject(json, key, v);
    };
    auto setBool = [json](const char* key, bool v) {
        cJSON_DeleteItemFromObject(json, key);
        cJSON_AddBoolToObject(json, key, v);
    };

    setStr("host", config.host);
    setNum("port", config.port);
    setStr("username", config.username);
    if (!config.password.empty()) setStr("password", config.password);
    else cJSON_DeleteItemFromObject(json, "password");
    setStr("user_id", config.userId);
    setStr("access_token", config.accessToken);
    setStr("device_id", config.deviceId);
    cJSON_DeleteItemFromObject(json, "profiles");
    if (!config.profiles.empty()) {
        cJSON* list = cJSON_AddArrayToObject(json, "profiles");
        for (const Profile& p : config.profiles) {
            cJSON* e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "name", p.name.c_str());
            cJSON_AddStringToObject(e, "host", p.host.c_str());
            cJSON_AddNumberToObject(e, "port", p.port);
            cJSON_AddStringToObject(e, "user_id", p.userId.c_str());
            cJSON_AddStringToObject(e, "access_token", p.accessToken.c_str());
            cJSON_AddBoolToObject(e, "keep_logged_in", p.keepLoggedIn);
            cJSON_AddNumberToObject(e, "color", p.color);
            cJSON_AddItemToArray(list, e);
        }
    }
    setNum("last_profile", config.lastProfile);
    setNum("video_bitrate", config.videoBitrate);
    setNum("video_height", config.videoHeight);
    setStr("video_profile", config.videoProfile);
    setNum("usage_seconds", (double)config.usageSeconds);
    setBool("logging", config.logging);
    setBool("jellyfin_backdrops", config.jellyfinBackdrops);
    setBool("menu_music", config.menuMusicEnabled);
    setStr("menu_music_item", config.menuMusicItemId);
    setStr("menu_music_name", config.menuMusicName);
    setBool("crt", config.crt);
    setNum("accent", config.accent);
    cJSON_DeleteItemFromObject(json, "ambient_background"); // superseded by background_effect
    setNum("background_effect", config.backgroundEffect);
    setBool("light_mode", config.lightMode);
    setBool("snow", config.snow);
    setBool("liquid_glass", config.liquidGlass);
    setBool("glass_refraction", config.glassRefraction);
    setNum("glass_refraction_stage", config.glassRefractionStage);
    setBool("glass_debug", config.glassDebug);
    setBool("clock", config.clock);
    setBool("rainbow_easter_egg", config.rainbowEasterEgg);
    setBool("rainbow_unlocked", config.rainbowUnlocked);
    setBool("autoplay_next", config.autoplayNext);
    setBool("gamepad_off_in_video", config.gamepadOffInVideo);
    setBool("allow_unaligned_video_geometry", config.allowUnalignedVideoGeometry);
    setBool("check_for_updates", config.checkForUpdates);
    setStr("skipped_update_version", config.skippedUpdateVersion);

    char* text = cJSON_Print(json);
    cJSON_Delete(json);
    if (!text) {
        outError = "could not build config.json";
        return false;
    }
    FILE* out = fopen(path, "wb");
    if (!out) {
        cJSON_free(text);
        outError = "could not write config.json (is the SD card read-only?)";
        return false;
    }
    size_t len = strlen(text);
    bool ok = fwrite(text, 1, len, out) == len;
    ok = (fclose(out) == 0) && ok;
    cJSON_free(text);
    if (!ok) outError = "writing config.json failed";
    return ok;
}
