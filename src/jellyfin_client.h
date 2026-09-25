#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct JellyfinItem {
    std::string id;
    std::string name;
    std::string type;           // "CollectionFolder", "Folder", "Movie", "Series", "Episode", "TvChannel", ...
    std::string collectionType; // views only: "movies", "tvshows", "music", "livetv", "boxsets", ...
    int64_t runTimeTicks = 0;   // duration in Jellyfin ticks (100 ns), 0 if not provided
    int indexNumber = -1;       // episode / track / season number, -1 if not provided
    int parentIndexNumber = -1; // season number of an episode, -1 if not provided
    int productionYear = 0;     // 0 if not provided
    std::string channelNumber;  // TvChannel only, e.g. "5" or "5.1"
    std::string currentProgram; // TvChannel only: what's on right now, if the guide knows
    std::string seriesName;     // Episode / Season: the show it belongs to

    // Artwork: whose Primary image to show, and its tag (the tag changes
    // when the image does, so it's part of the cache key). Resolved when
    // parsing: the item's own image, else its album's (songs) or its
    // series' (episodes, seasons). Empty = no artwork.
    std::string imageItemId;
    std::string imageTag;

    std::string seriesId;       // Episode / Season: its show (for "up next")

    // Per-user state (UserData)
    int64_t positionTicks = 0;  // where the user stopped, 0 = not started
    bool played = false;        // watched / listened
    bool favorite = false;
    double playedPercentage = 0.0;
    int unplayedCount = -1;     // Series / Season: unwatched episodes, -1 = unknown
    int childCount = -1;        // folders: items inside, -1 = unknown
};

// One audio or subtitle track of a video.
struct MediaTrack {
    int index = -1;             // Jellyfin's stream index (AudioStreamIndex / SubtitleStreamIndex)
    std::string title;          // "Italian - AAC - 5.1 - Default"
    std::string language;       // "ita"
    bool isDefault = false;
};

// Everything needed to make HTTP requests against a media stream:
// http_client (and http_stream_io, for playback) work with host/port/path
// separately rather than a single URL string, so this is what
// buildVideoStreamUrl() below returns instead of a plain std::string.
struct StreamTarget {
    std::string host;
    int port = 0;
    std::string path; // includes leading "/" and the full query string
};

// Knobs for the video transcode request (see buildVideoStreamUrl).
// Defaults are what's known to be safe for the Wii U's hardware decoder;
// config.json can override them (video_bitrate / video_profile).
struct VideoStreamOptions {
    int videoBitrate = 2500000;       // bits/s. Wii U Wi-Fi is 2.4 GHz 802.11n only.
    std::string profile = "baseline"; // H.264 profile: baseline | main | high

    // Optional, added to the URL when set. mediaSourceId picks one
    // version of a multi-version item; liveStreamId/playSessionId come
    // from openLiveStream() and are required for Live TV.
    std::string mediaSourceId;
    std::string liveStreamId;
    std::string playSessionId;

    // Where to start, in Jellyfin ticks (100 ns). Seeking restarts the
    // transcode here -- the stream itself can't be seeked.
    int64_t startTimeTicks = 0;

    // Track choice. audio -1 = the server's default; subtitles -1 = none,
    // otherwise burned into the picture (the Wii U decoder shows no
    // separate subtitle stream). -2 = leave both to the server.
    int audioStreamIndex = -2;
    int subtitleStreamIndex = -2;
};

// What we need to know about a video before playing it -- fetched from
// the item's metadata rather than the stream, because the stream we ask
// Jellyfin for is deliberately not at the original aspect ratio.
struct VideoInfo {
    int width = 0;                // source video dimensions, 0 if unknown
    int height = 0;
    double displayAspect = 0.0;   // width/height the picture should be shown at, 0 if unknown
    int64_t runTimeTicks = 0;     // duration in Jellyfin ticks (100 ns), 0 if unknown
    std::string mediaSourceId;    // id of the first media source, empty if unknown

    std::vector<MediaTrack> audioTracks;
    std::vector<MediaTrack> subtitleTracks;
    int defaultAudioIndex = -1;    // stream index, -1 = unknown
    int defaultSubtitleIndex = -1; // -1 = none
};

// An opened Live TV stream (see openLiveStream). Pass the ids into
// VideoStreamOptions and the playback reports, and hand liveStreamId to
// closeLiveStream() when playback ends so the tuner is released.
struct LiveStreamSession {
    std::string mediaSourceId;
    std::string liveStreamId;
    std::string playSessionId;
    VideoInfo info;
};

// Optional ids attached to the Sessions/Playing* reports.
struct PlaybackIds {
    std::string mediaSourceId;
    std::string liveStreamId;
    std::string playSessionId;
};

class JellyfinClient {
public:
    JellyfinClient(std::string host, int port);

    // Logs in with a username/password and stashes the access token +
    // user id for subsequent calls. Returns false on failure -- check
    // lastError() for details.
    bool authenticate(const std::string& username, const std::string& password);

    // Top-level libraries ("Movies", "TV Shows", "Music", "Live TV", ...).
    bool getViews(std::vector<JellyfinItem>& out);

    // Contents of a given library/folder.
    bool getItems(const std::string& parentId, std::vector<JellyfinItem>& out);

    // Live TV channel list (with what's currently on, when the guide
    // has data). This is what opening the "Live TV" view shows --
    // browsing it via getItems() doesn't return channels.
    bool getLiveTvChannels(std::vector<JellyfinItem>& out);

    // Library-wide search by name (movies, shows, episodes, music).
    bool search(const std::string& term, std::vector<JellyfinItem>& out);

    // Home rows: partly watched videos, the next unwatched episode of each
    // show, and the user's favourites.
    bool getResume(std::vector<JellyfinItem>& out);
    bool getNextUp(std::vector<JellyfinItem>& out);
    bool getFavorites(std::vector<JellyfinItem>& out);

    // Every episode of a show, in order (for autoplaying the next one).
    bool getEpisodes(const std::string& seriesId, std::vector<JellyfinItem>& out);

    // Favourite / watched toggles.
    bool setFavorite(const std::string& itemId, bool favorite);
    bool setPlayed(const std::string& itemId, bool played);

    // Fetches the item's metadata to learn its real aspect ratio and
    // duration. Returns false (with lastError() set) if the request
    // fails; fields that couldn't be determined stay at their zero
    // defaults, so callers should fall back to 16:9.
    bool getVideoInfo(const std::string& itemId, VideoInfo& out);

    // Asks Jellyfin to tune a Live TV channel (PlaybackInfo with
    // AutoOpenLiveStream). On success the session's ids must be passed
    // to buildVideoStreamUrl (via VideoStreamOptions) -- the stream
    // endpoint refuses a channel without them.
    bool openLiveStream(const std::string& channelId, int maxBitrate, LiveStreamSession& out);

    // Releases a stream opened with openLiveStream(). Safe to call with
    // an empty id (does nothing).
    bool closeLiveStream(const std::string& liveStreamId);

    // Builds a request target for streaming an audio-only item
    // (Jellyfin's /Audio/ endpoint rather than /Videos/). Forces AAC in
    // MP4 so it matches the decoder + demuxer our FFmpeg build actually
    // has.
    StreamTarget buildAudioStreamUrl(const std::string& itemId, int64_t startTimeTicks = 0,
                                     const std::string& playSessionId = "") const;

    // Builds a request target for streaming a video item (or an opened
    // Live TV channel), forcing a server-side transcode to H.264 + AAC
    // in a (fragmented) progressive MP4 -- the only combination our
    // FFmpeg build (h264_wiiu + aac decoders, mov demuxer, no HLS) can
    // decode. The exact parameters are dictated by the Wii U hardware
    // decoder wrapper; see the implementation for the reasoning.
    StreamTarget buildVideoStreamUrl(const std::string& itemId,
                                     const VideoStreamOptions& options = VideoStreamOptions()) const;

    // Request path for an item's Primary image, scaled by the server to
    // fit inside width x height (keeping its shape), as JPEG -- what
    // stb_image decodes.
    std::string buildImagePath(const std::string& imageItemId, const std::string& imageTag,
                               int width, int height) const;

    // Fetches bytes from this server (for images). Safe to call from a
    // background thread: touches no client state besides reading the
    // login token.
    bool fetchBinary(const std::string& path, std::string& out) const;

    // Jellyfin's "Sessions" API -- reporting these is what makes the
    // server's own web UI show "Ufin is playing X" instead of nothing.
    // positionTicks is in Jellyfin ticks (100-nanosecond units, i.e.
    // 10,000,000 per second).
    bool reportPlaybackStart(const std::string& itemId, const PlaybackIds& ids = PlaybackIds());
    bool reportPlaybackProgress(const std::string& itemId, int64_t positionTicks,
                                const PlaybackIds& ids = PlaybackIds(), bool isPaused = false);
    bool reportPlaybackStopped(const std::string& itemId, int64_t positionTicks,
                               const PlaybackIds& ids = PlaybackIds());

    // --- signing in without config.json ---

    // Identifies this console to Jellyfin (its device list, Quick Connect).
    // Keep it stable across launches; empty = a fixed default.
    void setDeviceId(const std::string& id) { device_id_ = id; }

    // Server can change on the login screen.
    void setServer(const std::string& host, int port) { host_ = host; port_ = port; }

    // Reuses a token saved by an earlier sign-in; check it with
    // validateLogin() before trusting it.
    void useSavedLogin(const std::string& userId, const std::string& token);
    bool validateLogin();    // GET /Users/Me
    void signOut();          // forgets the token (and tells the server)

    // Quick Connect: show `code` on screen; the user approves it from
    // another signed-in device (Jellyfin -> Settings -> Quick Connect).
    struct QuickConnectRequest {
        std::string secret;
        std::string code;
    };
    bool quickConnectStart(QuickConnectRequest& out);
    // True once approved (then call quickConnectFinish). False otherwise;
    // lastError() is set only if something went wrong.
    bool quickConnectApproved(const std::string& secret);
    bool quickConnectFinish(const std::string& secret);

    const std::string& lastError() const { return last_error_; }
    const std::string& userId() const { return user_id_; }
    const std::string& userName() const { return user_name_; }
    const std::string& accessToken() const { return token_; }
    const std::string& host() const { return host_; }
    int port() const { return port_; }

private:
    std::string host_;
    int port_;
    std::string token_;
    std::string user_id_;
    std::string user_name_;
    std::string device_id_;
    std::string last_error_;

    bool takeAuthResult(const std::string& body); // AccessToken + User from an auth response

    std::string authHeader() const;
};
