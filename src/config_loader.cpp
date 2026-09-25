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
    outConfig.menuMusicEnabled = boolean("menu_music", false);
    outConfig.menuMusicItemId = str("menu_music_item");
    outConfig.menuMusicName = str("menu_music_name");
    outConfig.crt = boolean("crt", false);
    {
        cJSON* accent = cJSON_GetObjectItem(json, "accent");
        outConfig.accent = cJSON_IsNumber(accent) ? accent->valueint : 0;
        if (outConfig.accent < 0) outConfig.accent = 0;
    }
    outConfig.ambient = boolean("ambient_background", false);
    outConfig.snow = boolean("snow", false);
    outConfig.clock = boolean("clock", false);
    outConfig.rainbowUnlocked = boolean("rainbow_unlocked", false);
    outConfig.autoplayNext = boolean("autoplay_next", true);
    outConfig.gamepadOffInVideo = boolean("gamepad_off_in_video", false);

    cJSON* videoBitrate = cJSON_GetObjectItem(json, "video_bitrate");
    if (cJSON_IsNumber(videoBitrate) && videoBitrate->valueint > 0) {
        outConfig.videoBitrate = videoBitrate->valueint;
    }
    cJSON* videoProfile = cJSON_GetObjectItem(json, "video_profile");
    if (cJSON_IsString(videoProfile) && videoProfile->valuestring[0] != '\0') {
        outConfig.videoProfile = videoProfile->valuestring;
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
    setNum("video_bitrate", config.videoBitrate);
    setStr("video_profile", config.videoProfile);
    setBool("menu_music", config.menuMusicEnabled);
    setStr("menu_music_item", config.menuMusicItemId);
    setStr("menu_music_name", config.menuMusicName);
    setBool("crt", config.crt);
    setNum("accent", config.accent);
    setBool("ambient_background", config.ambient);
    setBool("snow", config.snow);
    setBool("clock", config.clock);
    setBool("rainbow_unlocked", config.rainbowUnlocked);
    setBool("autoplay_next", config.autoplayNext);
    setBool("gamepad_off_in_video", config.gamepadOffInVideo);

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
