#include "jellyfin_client.h"
#include "http_client.h"
#include "vendor/cJSON.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cctype>

JellyfinClient::JellyfinClient(std::string host, int port)
    : host_(std::move(host)), port_(port) {}

std::string JellyfinClient::authHeader() const {
    // Jellyfin expects this identifying header on basically every request.
    // DeviceId should stay stable across launches so Jellyfin treats it as
    // the same device (matters for resume points / "continue watching").
    //
    // "Authorization: MediaBrowser ..." rather than the old
    // X-Emby-Authorization header: Jellyfin 12 disables the legacy
    // authorization methods by default (EnableLegacyAuthorization=false),
    // ignores X-Emby-Authorization, and then rejects the login with a 400
    // because no client/device info arrived. The Authorization form has
    // been accepted since Jellyfin 10.8.
    std::string h = "Authorization: MediaBrowser Client=\"Ufin\", Device=\"Wii U\", DeviceId=\"" +
                    (device_id_.empty() ? std::string("wiiu-ufin-001") : device_id_) + "\", Version=\"0.1.0\"";
    if (!token_.empty()) {
        h += ", Token=\"" + token_ + "\"";
    }
    h += "\r\n";
    return h;
}

// Query-string escaping for ids we didn't generate ourselves (live
// stream ids, play session ids). Jellyfin item ids are plain hex, but
// there's no promise about the others.
static std::string urlEncode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : in) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

static std::string jsonString(cJSON* obj, const char* key) {
    cJSON* v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(v) ? std::string(v->valuestring) : std::string();
}

static int jsonInt(cJSON* obj, const char* key, int fallback) {
    cJSON* v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(v) ? v->valueint : fallback;
}

bool JellyfinClient::authenticate(const std::string& username, const std::string& password) {
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "Username", username.c_str());
    cJSON_AddStringToObject(body, "Pw", password.c_str());
    char* body_str = cJSON_PrintUnformatted(body);

    HttpResponse resp = http_post(host_, port_, "/Users/AuthenticateByName",
                                   body_str, "application/json", authHeader());
    free(body_str);
    cJSON_Delete(body);

    if (!resp.success) {
        last_error_ = "AuthenticateByName failed (status " +
                       std::to_string(resp.status_code) + "): " + resp.body;
        return false;
    }

    return takeAuthResult(resp.body);
}

bool JellyfinClient::takeAuthResult(const std::string& body) {
    cJSON* json = cJSON_Parse(body.c_str());
    if (!json) {
        last_error_ = "could not parse auth response JSON";
        return false;
    }

    cJSON* access_token = cJSON_GetObjectItem(json, "AccessToken");
    cJSON* user = cJSON_GetObjectItem(json, "User");
    cJSON* user_id = user ? cJSON_GetObjectItem(user, "Id") : nullptr;

    if (!cJSON_IsString(access_token) || !cJSON_IsString(user_id)) {
        last_error_ = "auth response missing AccessToken or User.Id";
        cJSON_Delete(json);
        return false;
    }

    token_ = access_token->valuestring;
    user_id_ = user_id->valuestring;
    user_name_ = jsonString(user, "Name");
    cJSON_Delete(json);
    return true;
}

void JellyfinClient::useSavedLogin(const std::string& userId, const std::string& token) {
    user_id_ = userId;
    token_ = token;
}

bool JellyfinClient::validateLogin() {
    if (token_.empty()) {
        last_error_ = "not signed in";
        return false;
    }
    HttpResponse resp = http_get(host_, port_, "/Users/Me", authHeader());
    if (!resp.success) {
        last_error_ = resp.status_code == 401 ? "the saved sign-in has expired"
                                              : "could not check the saved sign-in (status " +
                                                    std::to_string(resp.status_code) + ")";
        return false;
    }
    cJSON* json = cJSON_Parse(resp.body.c_str());
    if (json) {
        std::string id = jsonString(json, "Id");
        if (!id.empty()) user_id_ = id;
        user_name_ = jsonString(json, "Name");
        cJSON_Delete(json);
    }
    return true;
}

void JellyfinClient::signOut() {
    if (!token_.empty()) http_post(host_, port_, "/Sessions/Logout", "", "application/json", authHeader());
    token_.clear();
    user_id_.clear();
    user_name_.clear();
}

bool JellyfinClient::quickConnectStart(QuickConnectRequest& out) {
    token_.clear(); // Initiate must come from a not-yet-signed-in device
    HttpResponse resp = http_post(host_, port_, "/QuickConnect/Initiate", "", "application/json", authHeader());
    if (!resp.success) {
        last_error_ = resp.status_code == 401 ? "Quick Connect is turned off on this server "
                                                "(Dashboard -> General -> Enable Quick Connect)"
                                              : "could not start Quick Connect (status " +
                                                    std::to_string(resp.status_code) + ")";
        return false;
    }
    cJSON* json = cJSON_Parse(resp.body.c_str());
    if (!json) {
        last_error_ = "could not parse Quick Connect response";
        return false;
    }
    out.secret = jsonString(json, "Secret");
    out.code = jsonString(json, "Code");
    cJSON_Delete(json);
    if (out.secret.empty() || out.code.empty()) {
        last_error_ = "Quick Connect response had no code";
        return false;
    }
    return true;
}

bool JellyfinClient::quickConnectApproved(const std::string& secret) {
    last_error_.clear();
    HttpResponse resp = http_get(host_, port_, "/QuickConnect/Connect?secret=" + urlEncode(secret), authHeader());
    if (!resp.success) {
        last_error_ = resp.status_code == 404 ? "the Quick Connect code expired -- start again"
                                              : "Quick Connect check failed (status " +
                                                    std::to_string(resp.status_code) + ")";
        return false;
    }
    cJSON* json = cJSON_Parse(resp.body.c_str());
    if (!json) return false;
    cJSON* authed = cJSON_GetObjectItem(json, "Authenticated");
    bool ok = cJSON_IsTrue(authed);
    cJSON_Delete(json);
    return ok;
}

bool JellyfinClient::quickConnectFinish(const std::string& secret) {
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "Secret", secret.c_str());
    char* bodyStr = cJSON_PrintUnformatted(body);
    HttpResponse resp = http_post(host_, port_, "/Users/AuthenticateWithQuickConnect", bodyStr, "application/json",
                                  authHeader());
    free(bodyStr);
    cJSON_Delete(body);
    if (!resp.success) {
        last_error_ = "Quick Connect sign-in failed (status " + std::to_string(resp.status_code) + ")";
        return false;
    }
    return takeAuthResult(resp.body);
}

static void parseItemsArray(cJSON* items, std::vector<JellyfinItem>& out) {
    out.clear(); // callers reuse the same vector across fetches
    if (!items) return;
    cJSON* item;
    cJSON_ArrayForEach(item, items) {
        JellyfinItem ji;
        ji.id = jsonString(item, "Id");
        ji.name = jsonString(item, "Name");
        ji.type = jsonString(item, "Type");
        ji.collectionType = jsonString(item, "CollectionType");
        cJSON* runTime = cJSON_GetObjectItem(item, "RunTimeTicks");
        if (cJSON_IsNumber(runTime)) ji.runTimeTicks = (int64_t)runTime->valuedouble;
        ji.indexNumber = jsonInt(item, "IndexNumber", -1);
        ji.parentIndexNumber = jsonInt(item, "ParentIndexNumber", -1);
        ji.productionYear = jsonInt(item, "ProductionYear", 0);
        ji.channelNumber = jsonString(item, "ChannelNumber");
        cJSON* program = cJSON_GetObjectItem(item, "CurrentProgram");
        if (cJSON_IsObject(program)) ji.currentProgram = jsonString(program, "Name");
        ji.seriesName = jsonString(item, "SeriesName");
        ji.seriesId = jsonString(item, "SeriesId");
        ji.childCount = jsonInt(item, "ChildCount", -1);
        cJSON* userData = cJSON_GetObjectItem(item, "UserData");
        if (cJSON_IsObject(userData)) {
            cJSON* pos = cJSON_GetObjectItem(userData, "PlaybackPositionTicks");
            if (cJSON_IsNumber(pos)) ji.positionTicks = (int64_t)pos->valuedouble;
            ji.played = cJSON_IsTrue(cJSON_GetObjectItem(userData, "Played"));
            ji.favorite = cJSON_IsTrue(cJSON_GetObjectItem(userData, "IsFavorite"));
            cJSON* pct = cJSON_GetObjectItem(userData, "PlayedPercentage");
            if (cJSON_IsNumber(pct)) ji.playedPercentage = pct->valuedouble;
            ji.unplayedCount = jsonInt(userData, "UnplayedItemCount", -1);
        }

        // Artwork: own Primary image first, then the album's (songs),
        // then the series' (episodes / seasons).
        cJSON* imageTags = cJSON_GetObjectItem(item, "ImageTags");
        std::string ownTag = cJSON_IsObject(imageTags) ? jsonString(imageTags, "Primary") : std::string();
        if (!ownTag.empty()) {
            ji.imageItemId = ji.id;
            ji.imageTag = ownTag;
        } else if (!jsonString(item, "AlbumPrimaryImageTag").empty() && !jsonString(item, "AlbumId").empty()) {
            ji.imageItemId = jsonString(item, "AlbumId");
            ji.imageTag = jsonString(item, "AlbumPrimaryImageTag");
        } else if (!jsonString(item, "SeriesPrimaryImageTag").empty() && !jsonString(item, "SeriesId").empty()) {
            ji.imageItemId = jsonString(item, "SeriesId");
            ji.imageTag = jsonString(item, "SeriesPrimaryImageTag");
        }
        out.push_back(ji);
    }
}

bool JellyfinClient::getViews(std::vector<JellyfinItem>& out) {
    std::string path = "/Users/" + user_id_ + "/Views";
    HttpResponse resp = http_get(host_, port_, path, authHeader());
    if (!resp.success) {
        last_error_ = "getViews failed (status " + std::to_string(resp.status_code) + ")";
        return false;
    }
    cJSON* json = cJSON_Parse(resp.body.c_str());
    if (!json) {
        last_error_ = "could not parse views JSON";
        return false;
    }
    parseItemsArray(cJSON_GetObjectItem(json, "Items"), out);
    cJSON_Delete(json);
    return true;
}

bool JellyfinClient::getItems(const std::string& parentId, std::vector<JellyfinItem>& out) {
    std::string path = "/Users/" + user_id_ + "/Items?ParentId=" + parentId +
                        "&SortBy=ParentIndexNumber,IndexNumber,SortName&SortOrder=Ascending";
    // Disc, then track/episode number, then name: albums play in track
    // order and seasons in episode order; items without numbers (movies,
    // folders) fall through to plain name order.
    HttpResponse resp = http_get(host_, port_, path, authHeader());
    if (!resp.success) {
        last_error_ = "getItems failed (status " + std::to_string(resp.status_code) + ")";
        return false;
    }
    cJSON* json = cJSON_Parse(resp.body.c_str());
    if (!json) {
        last_error_ = "could not parse items JSON";
        return false;
    }
    parseItemsArray(cJSON_GetObjectItem(json, "Items"), out);
    cJSON_Delete(json);
    return true;
}

bool JellyfinClient::getLiveTvChannels(std::vector<JellyfinItem>& out) {
    std::string path = "/LiveTv/Channels?UserId=" + user_id_ +
                       "&AddCurrentProgram=true&EnableImageTypes=Primary&ImageTypeLimit=1&EnableUserData=false";
    HttpResponse resp = http_get(host_, port_, path, authHeader());
    if (!resp.success) {
        last_error_ = "getLiveTvChannels failed (status " + std::to_string(resp.status_code) + ")";
        return false;
    }
    cJSON* json = cJSON_Parse(resp.body.c_str());
    if (!json) {
        last_error_ = "could not parse channels JSON";
        return false;
    }
    parseItemsArray(cJSON_GetObjectItem(json, "Items"), out);
    cJSON_Delete(json);
    return true;
}

bool JellyfinClient::search(const std::string& term, std::vector<JellyfinItem>& out) {
    std::string path = "/Users/" + user_id_ + "/Items?SearchTerm=" + urlEncode(term) +
                       "&Recursive=true&Limit=100&EnableImageTypes=Primary&ImageTypeLimit=1&EnableUserData=true"
                       "&IncludeItemTypes=Movie,Series,Episode,MusicAlbum,MusicArtist,Audio,BoxSet,Video";
    HttpResponse resp = http_get(host_, port_, path, authHeader());
    if (!resp.success) {
        last_error_ = "search failed (status " + std::to_string(resp.status_code) + ")";
        return false;
    }
    cJSON* json = cJSON_Parse(resp.body.c_str());
    if (!json) {
        last_error_ = "could not parse search JSON";
        return false;
    }
    parseItemsArray(cJSON_GetObjectItem(json, "Items"), out);
    cJSON_Delete(json);
    return true;
}

// GET a list endpoint and parse its Items. `legacyPath`, if given, is
// tried when the server doesn't know `path` (Jellyfin before 10.9).
static bool getItemList(const std::string& host, int port, const std::string& auth, const std::string& path,
                        const std::string& legacyPath, std::vector<JellyfinItem>& out, std::string& err,
                        const char* what) {
    HttpResponse resp = http_get(host, port, path, auth);
    if (!resp.success && resp.status_code == 404 && !legacyPath.empty()) resp = http_get(host, port, legacyPath, auth);
    if (!resp.success) {
        err = std::string(what) + " failed (status " + std::to_string(resp.status_code) + ")";
        return false;
    }
    cJSON* json = cJSON_Parse(resp.body.c_str());
    if (!json) {
        err = std::string("could not parse ") + what + " JSON";
        return false;
    }
    // Most endpoints wrap the list in {"Items": [...]}; a few return a bare array.
    cJSON* items = cJSON_IsArray(json) ? json : cJSON_GetObjectItem(json, "Items");
    parseItemsArray(items, out);
    cJSON_Delete(json);
    return true;
}

static const char* LIST_FIELDS = "&EnableImageTypes=Primary&ImageTypeLimit=1&EnableUserData=true";

bool JellyfinClient::getResume(std::vector<JellyfinItem>& out) {
    return getItemList(host_, port_, authHeader(),
                       "/UserItems/Resume?userId=" + user_id_ + "&Limit=24&MediaTypes=Video" + LIST_FIELDS,
                       "/Users/" + user_id_ + "/Items/Resume?Limit=24&MediaTypes=Video" + LIST_FIELDS, out,
                       last_error_, "Continue watching");
}

bool JellyfinClient::getNextUp(std::vector<JellyfinItem>& out) {
    return getItemList(host_, port_, authHeader(), "/Shows/NextUp?userId=" + user_id_ + "&Limit=24" + LIST_FIELDS, "",
                       out, last_error_, "Next up");
}

bool JellyfinClient::getFavorites(std::vector<JellyfinItem>& out) {
    return getItemList(host_, port_, authHeader(),
                       "/Users/" + user_id_ + "/Items?Filters=IsFavorite&Recursive=true&SortBy=SortName"
                       "&IncludeItemTypes=Movie,Series,Episode,MusicAlbum,Audio,BoxSet,Video,TvChannel" + LIST_FIELDS,
                       "", out, last_error_, "Favourites");
}

bool JellyfinClient::getEpisodes(const std::string& seriesId, std::vector<JellyfinItem>& out) {
    return getItemList(host_, port_, authHeader(),
                       "/Shows/" + seriesId + "/Episodes?userId=" + user_id_ + LIST_FIELDS, "", out, last_error_,
                       "Episodes");
}

// POST (on) or DELETE (off) a per-user flag, new route first, then the
// pre-10.9 one.
static bool setUserFlag(const std::string& host, int port, const std::string& auth, bool on,
                        const std::string& path, const std::string& legacyPath, std::string& err, const char* what) {
    HttpResponse resp = on ? http_post(host, port, path, "", "application/json", auth) : http_delete(host, port, path, auth);
    if (!resp.success && resp.status_code == 404) {
        resp = on ? http_post(host, port, legacyPath, "", "application/json", auth) : http_delete(host, port, legacyPath, auth);
    }
    if (!resp.success) err = std::string(what) + " failed (status " + std::to_string(resp.status_code) + ")";
    return resp.success;
}

bool JellyfinClient::setFavorite(const std::string& itemId, bool favorite) {
    return setUserFlag(host_, port_, authHeader(), favorite, "/UserFavoriteItems/" + itemId + "?userId=" + user_id_,
                       "/Users/" + user_id_ + "/FavoriteItems/" + itemId, last_error_, "Favourite");
}

bool JellyfinClient::setPlayed(const std::string& itemId, bool played) {
    return setUserFlag(host_, port_, authHeader(), played, "/UserPlayedItems/" + itemId + "?userId=" + user_id_,
                       "/Users/" + user_id_ + "/PlayedItems/" + itemId, last_error_, "Watched");
}

// Jellyfin reports aspect ratios as strings like "16:9" or "2.35:1".
static double parseAspectRatio(const char* s) {
    if (!s) return 0.0;
    double a = 0.0, b = 0.0;
    if (sscanf(s, "%lf:%lf", &a, &b) == 2 && a > 0.0 && b > 0.0) {
        return a / b;
    }
    return 0.0;
}

// Pulls Width/Height/AspectRatio out of the first video-type entry of a
// MediaStreams array, if there is one.
static void parseVideoStream(cJSON* mediaStreams, VideoInfo& out) {
    if (!cJSON_IsArray(mediaStreams)) return;
    cJSON* stream;
    cJSON_ArrayForEach(stream, mediaStreams) {
        cJSON* type = cJSON_GetObjectItem(stream, "Type");
        if (!cJSON_IsString(type) || strcmp(type->valuestring, "Video") != 0) continue;

        cJSON* w = cJSON_GetObjectItem(stream, "Width");
        cJSON* h = cJSON_GetObjectItem(stream, "Height");
        cJSON* ar = cJSON_GetObjectItem(stream, "AspectRatio");
        if (cJSON_IsNumber(w) && w->valueint > 0) out.width = w->valueint;
        if (cJSON_IsNumber(h) && h->valueint > 0) out.height = h->valueint;
        if (cJSON_IsString(ar) && out.displayAspect <= 0.0) {
            out.displayAspect = parseAspectRatio(ar->valuestring);
        }
        return;
    }
}

// Audio and subtitle tracks from a MediaStreams array.
static void parseTracks(cJSON* mediaStreams, VideoInfo& out) {
    out.audioTracks.clear();
    out.subtitleTracks.clear();
    if (!cJSON_IsArray(mediaStreams)) return;
    cJSON* s;
    cJSON_ArrayForEach(s, mediaStreams) {
        std::string type = jsonString(s, "Type");
        if (type != "Audio" && type != "Subtitle") continue;
        MediaTrack t;
        t.index = jsonInt(s, "Index", -1);
        t.language = jsonString(s, "Language");
        t.title = jsonString(s, "DisplayTitle");
        if (t.title.empty()) t.title = jsonString(s, "Title");
        if (t.title.empty()) t.title = t.language.empty() ? type + " " + std::to_string(t.index) : t.language;
        t.isDefault = cJSON_IsTrue(cJSON_GetObjectItem(s, "IsDefault"));
        if (t.index < 0) continue;
        (type == "Audio" ? out.audioTracks : out.subtitleTracks).push_back(t);
    }
}

bool JellyfinClient::getVideoInfo(const std::string& itemId, VideoInfo& out) {
    out = VideoInfo{};

    std::string path = "/Users/" + user_id_ + "/Items/" + itemId;
    HttpResponse resp = http_get(host_, port_, path, authHeader());
    if (!resp.success) {
        last_error_ = "getVideoInfo failed (status " + std::to_string(resp.status_code) + ")";
        return false;
    }
    cJSON* json = cJSON_Parse(resp.body.c_str());
    if (!json) {
        last_error_ = "could not parse item JSON";
        return false;
    }

    cJSON* runTime = cJSON_GetObjectItem(json, "RunTimeTicks");
    if (cJSON_IsNumber(runTime)) out.runTimeTicks = (int64_t)runTime->valuedouble;

    // Item-level Width/Height are present for video items; the
    // MediaStreams entry carries the display aspect ratio (which for
    // anamorphic sources isn't just width/height).
    cJSON* w = cJSON_GetObjectItem(json, "Width");
    cJSON* h = cJSON_GetObjectItem(json, "Height");
    if (cJSON_IsNumber(w) && w->valueint > 0) out.width = w->valueint;
    if (cJSON_IsNumber(h) && h->valueint > 0) out.height = h->valueint;

    parseVideoStream(cJSON_GetObjectItem(json, "MediaStreams"), out);
    parseTracks(cJSON_GetObjectItem(json, "MediaStreams"), out);
    cJSON* sources = cJSON_GetObjectItem(json, "MediaSources");
    if (cJSON_IsArray(sources) && cJSON_GetArraySize(sources) > 0) {
        cJSON* first = cJSON_GetArrayItem(sources, 0);
        out.mediaSourceId = jsonString(first, "Id");
        if (out.displayAspect <= 0.0) {
            parseVideoStream(cJSON_GetObjectItem(first, "MediaStreams"), out);
        }
        // The media source's own list is authoritative when present.
        cJSON* ms = cJSON_GetObjectItem(first, "MediaStreams");
        if (cJSON_IsArray(ms) && cJSON_GetArraySize(ms) > 0) parseTracks(ms, out);
        out.defaultAudioIndex = jsonInt(first, "DefaultAudioStreamIndex", -1);
        out.defaultSubtitleIndex = jsonInt(first, "DefaultSubtitleStreamIndex", -1);
    }
    if (out.defaultAudioIndex < 0) {
        for (const MediaTrack& t : out.audioTracks) if (t.isDefault) { out.defaultAudioIndex = t.index; break; }
        if (out.defaultAudioIndex < 0 && !out.audioTracks.empty()) out.defaultAudioIndex = out.audioTracks[0].index;
    }

    if (out.displayAspect <= 0.0 && out.width > 0 && out.height > 0) {
        out.displayAspect = (double)out.width / (double)out.height;
    }

    cJSON_Delete(json);
    return true;
}

bool JellyfinClient::openLiveStream(const std::string& channelId, int maxBitrate,
                                    LiveStreamSession& out) {
    out = LiveStreamSession{};

    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "UserId", user_id_.c_str());
    cJSON_AddBoolToObject(body, "IsPlayback", true);
    cJSON_AddBoolToObject(body, "AutoOpenLiveStream", true);
    cJSON_AddNumberToObject(body, "MaxStreamingBitrate", (double)maxBitrate);
    cJSON_AddBoolToObject(body, "EnableDirectPlay", false);
    cJSON_AddBoolToObject(body, "EnableDirectStream", false);
    cJSON_AddBoolToObject(body, "EnableTranscoding", true);
    cJSON_AddBoolToObject(body, "AllowVideoStreamCopy", false);
    cJSON_AddBoolToObject(body, "AllowAudioStreamCopy", false);
    char* bodyStr = cJSON_PrintUnformatted(body);

    std::string path = "/Items/" + channelId + "/PlaybackInfo?UserId=" + user_id_ +
                       "&IsPlayback=true&AutoOpenLiveStream=true&StartTimeTicks=0";
    HttpResponse resp = http_post(host_, port_, path, bodyStr, "application/json", authHeader());
    free(bodyStr);
    cJSON_Delete(body);

    if (!resp.success) {
        last_error_ = "opening the channel failed (status " + std::to_string(resp.status_code) + ")";
        return false;
    }
    cJSON* json = cJSON_Parse(resp.body.c_str());
    if (!json) {
        last_error_ = "could not parse PlaybackInfo JSON";
        return false;
    }

    std::string errorCode = jsonString(json, "ErrorCode");
    cJSON* sources = cJSON_GetObjectItem(json, "MediaSources");
    if (!errorCode.empty() || !cJSON_IsArray(sources) || cJSON_GetArraySize(sources) == 0) {
        last_error_ = errorCode.empty() ? "server returned no media source for this channel"
                                        : "server refused the channel: " + errorCode;
        cJSON_Delete(json);
        return false;
    }

    cJSON* first = cJSON_GetArrayItem(sources, 0);
    out.mediaSourceId = jsonString(first, "Id");
    out.liveStreamId = jsonString(first, "LiveStreamId");
    out.playSessionId = jsonString(json, "PlaySessionId");
    out.info.mediaSourceId = out.mediaSourceId;
    parseVideoStream(cJSON_GetObjectItem(first, "MediaStreams"), out.info);
    if (out.info.displayAspect <= 0.0 && out.info.width > 0 && out.info.height > 0) {
        out.info.displayAspect = (double)out.info.width / (double)out.info.height;
    }
    cJSON_Delete(json);

    if (out.mediaSourceId.empty()) {
        last_error_ = "channel media source has no id";
        return false;
    }
    return true;
}

bool JellyfinClient::closeLiveStream(const std::string& liveStreamId) {
    if (liveStreamId.empty()) return true;
    HttpResponse resp = http_post(host_, port_, "/LiveStreams/Close?liveStreamId=" + urlEncode(liveStreamId),
                                  "", "application/json", authHeader());
    return resp.success;
}

StreamTarget JellyfinClient::buildAudioStreamUrl(const std::string& itemId, int64_t startTimeTicks,
                                                 const std::string& playSessionId) const {
    StreamTarget target;
    target.host = host_;
    target.port = port_;

    // .mp4 container, not .mp3 -- our FFmpeg build (see configure-wiiu)
    // only has the mov demuxer enabled, no MP3/ADTS demuxer, so an
    // actual .mp3-formatted stream would be undecodable by us even
    // though the URL would "work" against Jellyfin. AAC-in-MP4 is
    // exactly what the video path uses for its audio track, just
    // without a video stream here.
    char pathBuf[900];
    snprintf(pathBuf, sizeof(pathBuf),
        "/Audio/%s/stream.mp4?static=false&AudioCodec=aac&AudioBitrate=192000"
        "&ApiKey=%s",
        itemId.c_str(), token_.c_str());
    target.path = pathBuf;
    if (startTimeTicks > 0) target.path += "&StartTimeTicks=" + std::to_string((long long)startTimeTicks);
    if (!playSessionId.empty()) target.path += "&PlaySessionId=" + urlEncode(playSessionId);
    return target;
}

// Only letters end up in the URL, whatever the config file says.
static std::string sanitizeProfile(const std::string& profile) {
    std::string out;
    for (char c : profile) {
        if (isalpha((unsigned char)c)) out += (char)tolower((unsigned char)c);
    }
    if (out != "baseline" && out != "main" && out != "high") out = "baseline";
    return out;
}

StreamTarget JellyfinClient::buildVideoStreamUrl(const std::string& itemId,
                                                 const VideoStreamOptions& options) const {
    StreamTarget target;
    target.host = host_;
    target.port = port_;

    int bitrate = options.videoBitrate;
    if (bitrate < 300000) bitrate = 300000;
    if (bitrate > 20000000) bitrate = 20000000;
    std::string profile = sanitizeProfile(options.profile);

    // Every parameter here is load-bearing for the Wii U hardware
    // decoder wrapper (h264_wiiu in the FFmpeg-wiiu fork):
    //
    // Width=1280&Height=720 (exact, not MaxWidth/MaxHeight):
    //   h264_wiiu sizes its decode framebuffer as width*height*1.5, but
    //   the hardware writes rows at a 256-pixel-aligned pitch and a
    //   16-row-aligned height. The two only agree when width is a
    //   multiple of 256 and height a multiple of 16 -- 1280x720 is, but
    //   the 1280x536 that MaxWidth/MaxHeight would produce for a 2.39:1
    //   movie, or 960x720 for 4:3 content, are not, and overflow the
    //   heap. So we ask Jellyfin to scale *everything* to exactly
    //   1280x720 (its scale filter is "scale=1280:720" when both are
    //   given), which squeezes non-16:9 pictures anamorphically, and
    //   VideoOutput un-squeezes them at draw time using the aspect
    //   ratio from the item's metadata (getVideoInfo).
    //
    // Profile=baseline (default):
    //   With the "output per frame" setting h264_wiiu uses, the hardware
    //   hands back exactly one picture per H264DECExecute call, in
    //   decode order. B-frames (Main/High profile) would come out in the
    //   wrong order with the wrong timestamps. Baseline has no B-frames,
    //   so decode order == display order and every frame lines up with
    //   its packet's timestamp. Costs some compression efficiency; can
    //   be overridden via config.json (video_profile) for experiments.
    //
    // MaxFramerate=30: 60 fps sources get decimated to 30 -- the CPU
    //   side (frame copy + upload) is sized for 720p30, per the README.
    //
    // MaxAudioChannels=2: downmix on the server so we don't pay for 5.1
    //   over Wi-Fi only to downmix it ourselves in AudioOutput.
    //
    // AllowVideoStreamCopy=false / AllowAudioStreamCopy=false:
    //   Without these Jellyfin remuxes an already-H.264 source unchanged,
    //   ignoring every constraint above.
    char pathBuf[1024];
    snprintf(pathBuf, sizeof(pathBuf),
        "/Videos/%s/stream.mp4?static=false&VideoCodec=h264&AudioCodec=aac"
        "&Width=1280&Height=720&VideoBitrate=%d&AudioBitrate=192000"
        "&Profile=%s&Level=41&MaxFramerate=30&MaxAudioChannels=2"
        "&AllowVideoStreamCopy=false&AllowAudioStreamCopy=false"
        "&ApiKey=%s",
        itemId.c_str(), bitrate, profile.c_str(), token_.c_str());
    target.path = pathBuf;

    // ApiKey (not api_key): Jellyfin 12 only accepts the new spelling
    // once legacy authorization is off; 10.8+ read it too.
    if (!options.mediaSourceId.empty()) target.path += "&MediaSourceId=" + urlEncode(options.mediaSourceId);
    if (!options.liveStreamId.empty()) target.path += "&LiveStreamId=" + urlEncode(options.liveStreamId);
    if (!options.playSessionId.empty()) target.path += "&PlaySessionId=" + urlEncode(options.playSessionId);
    if (options.startTimeTicks > 0) {
        target.path += "&StartTimeTicks=" + std::to_string((long long)options.startTimeTicks);
    }
    if (options.audioStreamIndex >= 0) {
        target.path += "&AudioStreamIndex=" + std::to_string(options.audioStreamIndex);
    }
    if (options.subtitleStreamIndex >= 0) {
        // Burned into the picture: the Wii U decoder has no subtitle track.
        target.path += "&SubtitleStreamIndex=" + std::to_string(options.subtitleStreamIndex) + "&SubtitleMethod=Encode";
    } else if (options.subtitleStreamIndex == -1) {
        target.path += "&SubtitleStreamIndex=-1"; // explicitly none
    }
    return target;
}

std::string JellyfinClient::buildImagePath(const std::string& imageItemId, const std::string& imageTag,
                                           int width, int height) const {
    char buf[512];
    snprintf(buf, sizeof(buf), "/Items/%s/Images/Primary?maxWidth=%d&maxHeight=%d&quality=85&format=Jpg&tag=%s",
             imageItemId.c_str(), width, height, urlEncode(imageTag).c_str());
    return buf;
}

bool JellyfinClient::fetchBinary(const std::string& path, std::string& out) const {
    HttpResponse resp = http_get(host_, port_, path, authHeader());
    if (!resp.success) return false;
    out.swap(resp.body);
    return true;
}

static bool postSessionEvent(const std::string& host, int port, const std::string& authHeader,
                              const std::string& endpoint, const std::string& itemId,
                              int64_t positionTicks, bool includePosition, const PlaybackIds& ids,
                              bool isPaused) {
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "ItemId", itemId.c_str());
    // Seeking = restarting the transcode at a new StartTimeTicks, which
    // works for everything except Live TV.
    cJSON_AddBoolToObject(body, "CanSeek", ids.liveStreamId.empty());
    if (includePosition) cJSON_AddBoolToObject(body, "IsPaused", isPaused);
    cJSON_AddStringToObject(body, "PlayMethod", "Transcode");
    if (!ids.mediaSourceId.empty()) cJSON_AddStringToObject(body, "MediaSourceId", ids.mediaSourceId.c_str());
    if (!ids.liveStreamId.empty()) cJSON_AddStringToObject(body, "LiveStreamId", ids.liveStreamId.c_str());
    if (!ids.playSessionId.empty()) cJSON_AddStringToObject(body, "PlaySessionId", ids.playSessionId.c_str());
    if (includePosition) {
        // cJSON numbers are doubles -- exact for any realistic tick count.
        cJSON_AddNumberToObject(body, "PositionTicks", (double)positionTicks);
    }
    char* bodyStr = cJSON_PrintUnformatted(body);

    HttpResponse resp = http_post(host, port, endpoint, bodyStr, "application/json", authHeader);
    free(bodyStr);
    cJSON_Delete(body);
    return resp.success;
}

bool JellyfinClient::reportPlaybackStart(const std::string& itemId, const PlaybackIds& ids) {
    return postSessionEvent(host_, port_, authHeader(), "/Sessions/Playing", itemId, 0, false, ids, false);
}

bool JellyfinClient::reportPlaybackProgress(const std::string& itemId, int64_t positionTicks,
                                            const PlaybackIds& ids, bool isPaused) {
    return postSessionEvent(host_, port_, authHeader(), "/Sessions/Playing/Progress", itemId,
                             positionTicks, true, ids, isPaused);
}

bool JellyfinClient::reportPlaybackStopped(const std::string& itemId, int64_t positionTicks,
                                           const PlaybackIds& ids) {
    return postSessionEvent(host_, port_, authHeader(), "/Sessions/Playing/Stopped", itemId,
                             positionTicks, true, ids, false);
}
