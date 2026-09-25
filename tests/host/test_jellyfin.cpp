// Exercises JellyfinClient (and the real http_client underneath it)
// against a fake Jellyfin server: authentication, browsing, item
// metadata, stream URL construction, and playback reporting.
#include "check.h"
#include "fake_http_server.h"
#include "jellyfin_client.h"

#include <chrono>
#include <thread>

static bool contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

int main() {
    FakeHttpServer server;
    CHECK(server.start());

    FakeRoute auth;
    auth.body = "{\"AccessToken\":\"tok123\",\"User\":{\"Id\":\"user1\",\"Name\":\"wiiu\"}}";
    server.addRoute("/Users/AuthenticateByName", auth);

    FakeRoute views;
    views.body = "{\"Items\":[{\"Id\":\"lib1\",\"Name\":\"Movies\",\"Type\":\"CollectionFolder\"},"
                 "{\"Id\":\"lib2\",\"Name\":\"Music\",\"Type\":\"CollectionFolder\"}],\"TotalRecordCount\":2}";
    server.addRoute("/Users/user1/Views", views);

    FakeRoute items;
    items.body = "{\"Items\":[{\"Id\":\"m1\",\"Name\":\"Some Movie\",\"Type\":\"Movie\",\"RunTimeTicks\":72000000000},"
                 "{\"Id\":\"a1\",\"Name\":\"Caf\xc3\xa9 Song\",\"Type\":\"Audio\",\"RunTimeTicks\":1850000000}]}";
    server.addRoute("/Users/user1/Items", items);

    FakeRoute movie;
    movie.body = "{\"Id\":\"m1\",\"Name\":\"Some Movie\",\"Width\":1920,\"Height\":804,\"RunTimeTicks\":72000000000,"
                 "\"MediaStreams\":[{\"Type\":\"Audio\",\"Codec\":\"ac3\"},"
                 "{\"Type\":\"Video\",\"Width\":1920,\"Height\":804,\"AspectRatio\":\"2.40:1\"}]}";
    server.addRoute("/Users/user1/Items/m1", movie);

    FakeRoute oldMovie; // no item-level aspect, only inside MediaSources
    oldMovie.body = "{\"Id\":\"m2\",\"Width\":640,\"Height\":480,"
                    "\"MediaSources\":[{\"MediaStreams\":[{\"Type\":\"Video\",\"Width\":640,\"Height\":480}]}]}";
    server.addRoute("/Users/user1/Items/m2", oldMovie);

    FakeRoute anamorphic; // AspectRatio disagrees with Width/Height -> AspectRatio wins
    anamorphic.body = "{\"Id\":\"m3\",\"Width\":720,\"Height\":576,"
                      "\"MediaStreams\":[{\"Type\":\"Video\",\"Width\":720,\"Height\":576,\"AspectRatio\":\"16:9\"}]}";
    server.addRoute("/Users/user1/Items/m3", anamorphic);

    FakeRoute versioned; // MediaSources[0].Id is picked up
    versioned.body = "{\"Id\":\"m4\",\"MediaSources\":[{\"Id\":\"src-4k\",\"MediaStreams\":[{\"Type\":\"Video\",\"Width\":3840,\"Height\":2160}]}]}";
    server.addRoute("/Users/user1/Items/m4", versioned);

    FakeRoute viewsLive;
    viewsLive.body = "{\"Items\":[{\"Id\":\"tv\",\"Name\":\"Live TV\",\"Type\":\"UserView\",\"CollectionType\":\"livetv\"}]}";

    FakeRoute channels;
    channels.body = "{\"Items\":[{\"Id\":\"ch1\",\"Name\":\"Rai 1\",\"Type\":\"TvChannel\",\"ChannelNumber\":\"1\","
                    "\"CurrentProgram\":{\"Name\":\"Telegiornale\"}},"
                    "{\"Id\":\"ch2\",\"Name\":\"Rai 2\",\"Type\":\"TvChannel\",\"ChannelNumber\":\"2\"}]}";
    server.addRoute("/LiveTv/Channels", channels);

    FakeRoute playbackInfo;
    playbackInfo.body = "{\"MediaSources\":[{\"Id\":\"ms1\",\"LiveStreamId\":\"ls 1\","
                        "\"MediaStreams\":[{\"Type\":\"Video\",\"Width\":720,\"Height\":576,\"AspectRatio\":\"16:9\"}]}],"
                        "\"PlaySessionId\":\"ps1\"}";
    server.addRoute("/Items/ch1/PlaybackInfo", playbackInfo);

    FakeRoute refused;
    refused.body = "{\"MediaSources\":[],\"ErrorCode\":\"NoCompatibleStream\"}";
    server.addRoute("/Items/ch2/PlaybackInfo", refused);

    FakeRoute ok;
    ok.body = "";
    ok.status = 204;
    server.addRoute("/Sessions/Playing", ok);
    server.addRoute("/Sessions/Playing/Progress", ok);
    server.addRoute("/Sessions/Playing/Stopped", ok);
    server.addRoute("/LiveStreams/Close", ok);

    JellyfinClient client("127.0.0.1", server.port());

    // Authentication failure is reported, not swallowed.
    {
        JellyfinClient bad("127.0.0.1", server.port());
        FakeRoute denied;
        denied.status = 401;
        denied.body = "{\"error\":\"nope\"}";
        FakeHttpServer badServer;
        CHECK(badServer.start());
        badServer.addRoute("/Users/AuthenticateByName", denied);
        JellyfinClient bad2("127.0.0.1", badServer.port());
        CHECK(!bad2.authenticate("u", "p"));
        CHECK(contains(bad2.lastError(), "401"));
    }

    CHECK(client.authenticate("wiiu", "secret"));
    {
        auto reqs = server.requests();
        CHECK(!reqs.empty());
        CHECK_STR(reqs.back().method, "POST");
        CHECK(contains(reqs.back().body, "\"Username\":\"wiiu\""));
        CHECK(contains(reqs.back().body, "\"Pw\":\"secret\""));
        CHECK(contains(reqs.back().headers, "Authorization: MediaBrowser Client=\"Ufin\""));
        CHECK(!contains(reqs.back().headers, "X-Emby-Authorization"));
    }

    std::vector<JellyfinItem> list;
    CHECK(client.getViews(list));
    CHECK_EQ((int)list.size(), 2);
    CHECK_STR(list[0].name, "Movies");
    CHECK_STR(list[1].type, "CollectionFolder");
    {
        auto reqs = server.requests();
        CHECK(contains(reqs.back().headers, "Token=\"tok123\""));
    }

    // getItems replaces the vector contents rather than appending.
    CHECK(client.getItems("lib1", list));
    CHECK_EQ((int)list.size(), 2);
    CHECK_STR(list[0].id, "m1");
    CHECK_EQ(list[0].runTimeTicks, (int64_t)72000000000LL);
    CHECK_STR(list[1].type, "Audio");
    CHECK_EQ(list[1].runTimeTicks, (int64_t)1850000000LL);
    {
        auto reqs = server.requests();
        CHECK(contains(reqs.back().path, "ParentId=lib1"));
    }

    VideoInfo info;
    CHECK(client.getVideoInfo("m1", info));
    CHECK_EQ(info.width, 1920);
    CHECK_EQ(info.height, 804);
    CHECK_NEAR(info.displayAspect, 2.40, 0.001);
    CHECK_EQ(info.runTimeTicks, (int64_t)72000000000LL);

    CHECK(client.getVideoInfo("m2", info));
    CHECK_NEAR(info.displayAspect, 4.0 / 3.0, 0.001);
    CHECK_EQ(info.runTimeTicks, (int64_t)0);

    CHECK(client.getVideoInfo("m3", info));
    CHECK_NEAR(info.displayAspect, 16.0 / 9.0, 0.001);

    CHECK(!client.getVideoInfo("missing", info));
    CHECK(contains(client.lastError(), "404"));

    // Stream URLs.
    StreamTarget audio = client.buildAudioStreamUrl("a1");
    CHECK_STR(audio.host, "127.0.0.1");
    CHECK_EQ(audio.port, server.port());
    CHECK(contains(audio.path, "/Audio/a1/stream.mp4?"));
    CHECK(contains(audio.path, "AudioCodec=aac"));
    CHECK(contains(audio.path, "ApiKey=tok123"));
    CHECK(!contains(audio.path, "api_key"));

    StreamTarget video = client.buildVideoStreamUrl("m1");
    CHECK(contains(video.path, "/Videos/m1/stream.mp4?"));
    CHECK(contains(video.path, "&Width=1280&Height=720&"));
    CHECK(!contains(video.path, "MaxWidth"));
    CHECK(contains(video.path, "VideoBitrate=2500000"));
    CHECK(contains(video.path, "Profile=baseline"));
    CHECK(contains(video.path, "Level=41"));
    CHECK(contains(video.path, "MaxFramerate=30"));
    CHECK(contains(video.path, "MaxAudioChannels=2"));
    CHECK(contains(video.path, "AllowVideoStreamCopy=false"));
    CHECK(contains(video.path, "AllowAudioStreamCopy=false"));
    CHECK(contains(video.path, "ApiKey=tok123"));
    CHECK(!contains(video.path, "api_key"));
    CHECK(!contains(video.path, "MediaSourceId"));

    VideoStreamOptions opts;
    opts.videoBitrate = 4000000;
    opts.profile = "High";
    video = client.buildVideoStreamUrl("m1", opts);
    CHECK(contains(video.path, "VideoBitrate=4000000"));
    CHECK(contains(video.path, "Profile=high"));

    opts.videoBitrate = 10;                    // clamped up
    opts.profile = "ultra&evil=1";             // unknown -> baseline, nothing injected
    video = client.buildVideoStreamUrl("m1", opts);
    CHECK(contains(video.path, "VideoBitrate=300000"));
    CHECK(contains(video.path, "Profile=baseline"));
    CHECK(!contains(video.path, "evil"));

    opts.videoBitrate = 999999999;             // clamped down
    video = client.buildVideoStreamUrl("m1", opts);
    CHECK(contains(video.path, "VideoBitrate=20000000"));

    // Playback reporting posts JSON with the position in ticks.
    CHECK(client.reportPlaybackStart("m1"));
    CHECK(client.reportPlaybackProgress("m1", 123450000));
    CHECK(client.reportPlaybackStopped("m1", 987650000));
    {
        auto reqs = server.requests();
        CHECK((int)reqs.size() >= 3);
        const FakeRequest& start = reqs[reqs.size() - 3];
        const FakeRequest& progress = reqs[reqs.size() - 2];
        const FakeRequest& stopped = reqs[reqs.size() - 1];
        CHECK_STR(start.path, "/Sessions/Playing");
        CHECK(contains(start.body, "\"ItemId\":\"m1\""));
        CHECK(!contains(start.body, "PositionTicks"));
        CHECK_STR(progress.path, "/Sessions/Playing/Progress");
        CHECK(contains(progress.body, "\"PositionTicks\":123450000"));
        CHECK(contains(progress.body, "\"PlayMethod\":\"Transcode\""));
        CHECK_STR(stopped.path, "/Sessions/Playing/Stopped");
        CHECK(contains(stopped.body, "\"PositionTicks\":987650000"));
    }

    // Items carry the extra fields the UI uses.
    {
        FakeRoute episodes;
        episodes.body = "{\"Items\":[{\"Id\":\"e1\",\"Name\":\"Pilot\",\"Type\":\"Episode\","
                        "\"IndexNumber\":1,\"ParentIndexNumber\":2,\"ProductionYear\":2008}]}";
        server.addRoute("/Users/user1/Items", episodes);
        CHECK(client.getItems("season2", list));
        CHECK_EQ((int)list.size(), 1);
        CHECK_EQ(list[0].indexNumber, 1);
        CHECK_EQ(list[0].parentIndexNumber, 2);
        CHECK_EQ(list[0].productionYear, 2008);
    }

    // Views keep their CollectionType (how main.cpp spots Live TV).
    server.addRoute("/Users/user1/Views", viewsLive);
    CHECK(client.getViews(list));
    CHECK_EQ((int)list.size(), 1);
    CHECK_STR(list[0].collectionType, "livetv");

    // Channel list with the current programme.
    CHECK(client.getLiveTvChannels(list));
    CHECK_EQ((int)list.size(), 2);
    CHECK_STR(list[0].type, "TvChannel");
    CHECK_STR(list[0].channelNumber, "1");
    CHECK_STR(list[0].currentProgram, "Telegiornale");
    CHECK_STR(list[1].currentProgram, "");
    {
        auto reqs = server.requests();
        CHECK(contains(reqs.back().path, "UserId=user1"));
        CHECK(contains(reqs.back().path, "AddCurrentProgram=true"));
    }

    // Media source id from item metadata.
    CHECK(client.getVideoInfo("m4", info));
    CHECK_STR(info.mediaSourceId, "src-4k");

    // Opening a channel: ids come back and go into the stream URL.
    LiveStreamSession live;
    CHECK(client.openLiveStream("ch1", 2500000, live));
    CHECK_STR(live.mediaSourceId, "ms1");
    CHECK_STR(live.liveStreamId, "ls 1");
    CHECK_STR(live.playSessionId, "ps1");
    CHECK_NEAR(live.info.displayAspect, 16.0 / 9.0, 0.001);
    {
        auto reqs = server.requests();
        CHECK_STR(reqs.back().method, "POST");
        CHECK(contains(reqs.back().path, "AutoOpenLiveStream=true"));
        CHECK(contains(reqs.back().body, "\"AutoOpenLiveStream\":true"));
        CHECK(contains(reqs.back().body, "\"UserId\":\"user1\""));
    }
    {
        VideoStreamOptions o;
        o.mediaSourceId = live.mediaSourceId;
        o.liveStreamId = live.liveStreamId;
        o.playSessionId = live.playSessionId;
        StreamTarget t = client.buildVideoStreamUrl("ch1", o);
        CHECK(contains(t.path, "/Videos/ch1/stream.mp4?"));
        CHECK(contains(t.path, "&MediaSourceId=ms1"));
        CHECK(contains(t.path, "&LiveStreamId=ls%201"));
        CHECK(contains(t.path, "&PlaySessionId=ps1"));
    }
    CHECK(!client.openLiveStream("ch2", 2500000, live));
    CHECK(contains(client.lastError(), "NoCompatibleStream"));

    // Reports carry the ids; closing releases the tuner.
    {
        PlaybackIds ids;
        ids.mediaSourceId = "ms1";
        ids.liveStreamId = "ls 1";
        ids.playSessionId = "ps1";
        CHECK(client.reportPlaybackStart("ch1", ids));
        auto reqs = server.requests();
        CHECK(contains(reqs.back().body, "\"LiveStreamId\":\"ls 1\""));
        CHECK(contains(reqs.back().body, "\"PlaySessionId\":\"ps1\""));
    }
    CHECK(client.closeLiveStream("ls 1"));
    {
        auto reqs = server.requests();
        CHECK(contains(reqs.back().path, "/LiveStreams/Close?liveStreamId=ls%201"));
    }
    CHECK(client.closeLiveStream(""));

    // Search: recursive, URL-encoded term, series name kept.
    {
        FakeRoute results;
        results.body = "{\"Items\":[{\"Id\":\"e9\",\"Name\":\"Pilot\",\"Type\":\"Episode\",\"SeriesName\":\"Lost\"}]}";
        server.addRoute("/Users/user1/Items", results);
        CHECK(client.search("citt\xc3\xa0 & co", list));
        CHECK_EQ((int)list.size(), 1);
        CHECK_STR(list[0].seriesName, "Lost");
        auto reqs = server.requests();
        CHECK(contains(reqs.back().path, "SearchTerm=citt%C3%A0%20%26%20co"));
        CHECK(contains(reqs.back().path, "Recursive=true"));
    }

    // Seeking restarts the transcode at StartTimeTicks.
    {
        VideoStreamOptions o;
        o.startTimeTicks = 6000000000LL; // 10 min
        CHECK(contains(client.buildVideoStreamUrl("m1", o).path, "&StartTimeTicks=6000000000"));
        CHECK(!contains(client.buildVideoStreamUrl("m1").path, "StartTimeTicks"));
        CHECK(contains(client.buildAudioStreamUrl("a1", 1234).path, "&StartTimeTicks=1234"));
        CHECK(!contains(client.buildAudioStreamUrl("a1").path, "StartTimeTicks"));
        CHECK(contains(client.buildAudioStreamUrl("a1", 5, "ps9").path, "&PlaySessionId=ps9"));
        CHECK(!contains(client.buildAudioStreamUrl("a1").path, "PlaySessionId"));
        // Each seek's request carries its own session, so Jellyfin starts a
        // new transcode instead of replaying the old one from 0:00.
        VideoStreamOptions s1, s2;
        s1.startTimeTicks = 0;
        s1.playSessionId = "aaa";
        s2.startTimeTicks = 6000000000LL;
        s2.playSessionId = "bbb";
        std::string p1 = client.buildVideoStreamUrl("m1", s1).path, p2 = client.buildVideoStreamUrl("m1", s2).path;
        CHECK(contains(p1, "&PlaySessionId=aaa"));
        CHECK(contains(p2, "&PlaySessionId=bbb"));
        CHECK(contains(p2, "&StartTimeTicks=6000000000"));
    }

    // Progress carries IsPaused; CanSeek is true except for Live TV.
    {
        CHECK(client.reportPlaybackProgress("m1", 10, PlaybackIds(), true));
        auto reqs = server.requests();
        CHECK(contains(reqs.back().body, "\"IsPaused\":true"));
        CHECK(contains(reqs.back().body, "\"CanSeek\":true"));
        PlaybackIds liveIds;
        liveIds.liveStreamId = "ls";
        CHECK(client.reportPlaybackProgress("ch1", 10, liveIds));
        reqs = server.requests();
        CHECK(contains(reqs.back().body, "\"IsPaused\":false"));
        CHECK(contains(reqs.back().body, "\"CanSeek\":false"));
    }

    // Artwork: own Primary image, else the album's, else the series'.
    {
        FakeRoute art;
        art.body = "{\"Items\":["
            "{\"Id\":\"m1\",\"Type\":\"Movie\",\"ImageTags\":{\"Primary\":\"t1\"}},"
            "{\"Id\":\"s1\",\"Type\":\"Audio\",\"ImageTags\":{},\"AlbumId\":\"al9\",\"AlbumPrimaryImageTag\":\"t2\"},"
            "{\"Id\":\"e1\",\"Type\":\"Episode\",\"SeriesId\":\"se4\",\"SeriesPrimaryImageTag\":\"t3\"},"
            "{\"Id\":\"f1\",\"Type\":\"Folder\"}]}";
        server.addRoute("/Users/user1/Items", art);
        CHECK(client.getItems("x", list));
        CHECK_EQ((int)list.size(), 4);
        CHECK_STR(list[0].imageItemId, "m1"); CHECK_STR(list[0].imageTag, "t1");
        CHECK_STR(list[1].imageItemId, "al9"); CHECK_STR(list[1].imageTag, "t2");
        CHECK_STR(list[2].imageItemId, "se4"); CHECK_STR(list[2].imageTag, "t3");
        CHECK_STR(list[3].imageItemId, ""); CHECK_STR(list[3].imageTag, "");

        std::string p = client.buildImagePath("m1", "t 1", 96, 90);
        CHECK_STR(p, "/Items/m1/Images/Primary?maxWidth=96&maxHeight=90&quality=85&format=Jpg&tag=t%201");

        // Binary-safe fetch (JPEG bytes contain NULs).
        FakeRoute jpg;
        jpg.contentType = "image/jpeg";
        jpg.body = std::string("\xff\xd8\x00\x01\x02\x00\xff\xd9", 8);
        server.addRoute("/Items/m1/Images/Primary", jpg);
        std::string bytes;
        CHECK(client.fetchBinary(p, bytes));
        CHECK_EQ((int)bytes.size(), 8);
        CHECK(bytes == jpg.body);
        CHECK(!client.fetchBinary("/Items/none/Images/Primary", bytes));
    }

    // --- watched / favourites / resume / tracks ---
    {
        FakeRoute items;
        items.body = "{\"Items\":["
            "{\"Id\":\"m1\",\"Type\":\"Movie\",\"UserData\":{\"PlaybackPositionTicks\":6000000000,"
            "\"Played\":false,\"IsFavorite\":true,\"PlayedPercentage\":12.5}},"
            "{\"Id\":\"s1\",\"Type\":\"Series\",\"UserData\":{\"Played\":false,\"UnplayedItemCount\":4}},"
            "{\"Id\":\"e1\",\"Type\":\"Episode\",\"SeriesId\":\"s1\",\"UserData\":{\"Played\":true}},"
            "{\"Id\":\"x\",\"Type\":\"Folder\"}]}";
        server.addRoute("/Users/user1/Items", items);
        CHECK(client.getItems("p", list));
        CHECK_EQ((int)list.size(), 4);
        CHECK_EQ((long long)list[0].positionTicks, 6000000000LL);
        CHECK(list[0].favorite);
        CHECK(!list[0].played);
        CHECK_NEAR(list[0].playedPercentage, 12.5, 1e-9);
        CHECK_EQ(list[1].unplayedCount, 4);
        CHECK(list[2].played);
        CHECK_STR(list[2].seriesId, "s1");
        CHECK_EQ(list[3].unplayedCount, -1);
        CHECK(!list[3].favorite);

        // Home rows.
        FakeRoute resume;
        resume.body = "{\"Items\":[{\"Id\":\"m1\",\"Type\":\"Movie\"}]}";
        server.addRoute("/UserItems/Resume", resume);
        CHECK(client.getResume(list));
        CHECK_EQ((int)list.size(), 1);
        CHECK(contains(server.requests().back().path, "userId=user1"));
        CHECK(contains(server.requests().back().path, "MediaTypes=Video"));
        server.addRoute("/Shows/NextUp", resume);
        CHECK(client.getNextUp(list));
        CHECK(contains(server.requests().back().path, "/Shows/NextUp?userId=user1"));
        CHECK(!client.getEpisodes("s1", list)); // route not registered yet: 404
        CHECK(contains(server.requests().back().path, "/Shows/s1/Episodes?userId=user1"));
        server.addRoute("/Shows/s1/Episodes", resume);
        CHECK(client.getEpisodes("s1", list));
        server.addRoute("/Users/user1/Items", resume);
        CHECK(client.getFavorites(list));
        CHECK(contains(server.requests().back().path, "Filters=IsFavorite"));

        // Old servers: the pre-10.9 resume route is tried after a 404.
        JellyfinClient old("127.0.0.1", server.port());
        old.useSavedLogin("user1", "tok");
        FakeRoute gone;
        gone.status = 404;
        server.addRoute("/UserItems/Resume", gone);
        server.addRoute("/Users/user1/Items/Resume", resume);
        CHECK(old.getResume(list));
        CHECK(contains(server.requests().back().path, "/Users/user1/Items/Resume"));

        // Favourite / watched: POST to set, DELETE to clear.
        FakeRoute ok;
        ok.status = 200;
        server.addRoute("/UserFavoriteItems/m1", ok);
        CHECK(client.setFavorite("m1", true));
        CHECK_STR(server.requests().back().method, "POST");
        CHECK(contains(server.requests().back().path, "/UserFavoriteItems/m1?userId=user1"));
        CHECK(client.setFavorite("m1", false));
        CHECK_STR(server.requests().back().method, "DELETE");
        server.addRoute("/UserPlayedItems/m1", ok);
        CHECK(client.setPlayed("m1", true));
        CHECK_STR(server.requests().back().method, "POST");
        // ...and the legacy route when the new one doesn't exist.
        server.addRoute("/Users/user1/PlayedItems/m2", ok);
        CHECK(client.setPlayed("m2", false));
        CHECK_STR(server.requests().back().method, "DELETE");
        CHECK(contains(server.requests().back().path, "/Users/user1/PlayedItems/m2"));

        // Tracks from the media source, with defaults.
        FakeRoute info;
        info.body = "{\"Id\":\"m5\",\"MediaSources\":[{\"Id\":\"src\",\"DefaultAudioStreamIndex\":2,"
            "\"DefaultSubtitleStreamIndex\":-1,\"MediaStreams\":["
            "{\"Type\":\"Video\",\"Index\":0,\"Width\":1920,\"Height\":1080},"
            "{\"Type\":\"Audio\",\"Index\":1,\"Language\":\"eng\",\"DisplayTitle\":\"English - AAC - Stereo\"},"
            "{\"Type\":\"Audio\",\"Index\":2,\"Language\":\"ita\",\"DisplayTitle\":\"Italiano - AC3 - 5.1\",\"IsDefault\":true},"
            "{\"Type\":\"Subtitle\",\"Index\":3,\"Language\":\"ita\",\"DisplayTitle\":\"Italiano - SRT\"}]}]}";
        server.addRoute("/Users/user1/Items/m5", info);
        VideoInfo vi;
        CHECK(client.getVideoInfo("m5", vi));
        CHECK_EQ((int)vi.audioTracks.size(), 2);
        CHECK_EQ((int)vi.subtitleTracks.size(), 1);
        CHECK_STR(vi.audioTracks[1].title, "Italiano - AC3 - 5.1");
        CHECK_EQ(vi.defaultAudioIndex, 2);
        CHECK_EQ(vi.defaultSubtitleIndex, -1);
        CHECK_STR(vi.subtitleTracks[0].language, "ita");

        // Track indices in the stream URL; subtitles burned in.
        VideoStreamOptions o;
        CHECK(!contains(client.buildVideoStreamUrl("m5", o).path, "StreamIndex")); // server default
        o.audioStreamIndex = 2;
        o.subtitleStreamIndex = 3;
        std::string p = client.buildVideoStreamUrl("m5", o).path;
        CHECK(contains(p, "&AudioStreamIndex=2"));
        CHECK(contains(p, "&SubtitleStreamIndex=3&SubtitleMethod=Encode"));
        o.subtitleStreamIndex = -1;
        p = client.buildVideoStreamUrl("m5", o).path;
        CHECK(contains(p, "&SubtitleStreamIndex=-1"));
        CHECK(!contains(p, "SubtitleMethod"));
    }

    // --- signing in without config.json ---
    {
        JellyfinClient c2("127.0.0.1", server.port());
        c2.setDeviceId("ufin-abc");

        // Saved token: accepted when /Users/Me says so.
        FakeRoute me;
        me.body = "{\"Id\":\"user7\",\"Name\":\"alex\"}";
        server.addRoute("/Users/Me", me);
        c2.useSavedLogin("user7", "savedtok");
        CHECK(c2.validateLogin());
        CHECK_STR(c2.userName(), "alex");
        {
            auto reqs = server.requests();
            CHECK(contains(reqs.back().headers, "DeviceId=\"ufin-abc\""));
            CHECK(contains(reqs.back().headers, "Token=\"savedtok\""));
        }
        FakeRoute expired;
        expired.status = 401;
        server.addRoute("/Users/Me", expired);
        CHECK(!c2.validateLogin());
        CHECK(contains(c2.lastError(), "expired"));

        // Quick Connect: code shown, approved elsewhere, then signed in.
        FakeRoute init;
        init.body = "{\"Secret\":\"s3cr3t\",\"Code\":\"123456\",\"Authenticated\":false}";
        server.addRoute("/QuickConnect/Initiate", init);
        JellyfinClient::QuickConnectRequest qc;
        CHECK(c2.quickConnectStart(qc));
        CHECK_STR(qc.code, "123456");
        CHECK_STR(qc.secret, "s3cr3t");
        {
            auto reqs = server.requests();
            CHECK_STR(reqs.back().method, "POST");
            CHECK(!contains(reqs.back().headers, "Token=")); // not signed in yet
        }
        FakeRoute waiting;
        waiting.body = "{\"Authenticated\":false}";
        server.addRoute("/QuickConnect/Connect", waiting);
        CHECK(!c2.quickConnectApproved(qc.secret));
        CHECK_STR(c2.lastError(), "");
        CHECK(contains(server.requests().back().path, "secret=s3cr3t"));
        FakeRoute approved;
        approved.body = "{\"Authenticated\":true}";
        server.addRoute("/QuickConnect/Connect", approved);
        CHECK(c2.quickConnectApproved(qc.secret));
        FakeRoute qcAuth;
        qcAuth.body = "{\"AccessToken\":\"newtok\",\"User\":{\"Id\":\"user7\",\"Name\":\"alex\"}}";
        server.addRoute("/Users/AuthenticateWithQuickConnect", qcAuth);
        CHECK(c2.quickConnectFinish(qc.secret));
        CHECK_STR(c2.accessToken(), "newtok");
        CHECK_STR(c2.userId(), "user7");
        CHECK(contains(server.requests().back().body, "\"Secret\":\"s3cr3t\""));

        // Expired code, and Quick Connect turned off.
        FakeRoute gone;
        gone.status = 404;
        server.addRoute("/QuickConnect/Connect", gone);
        CHECK(!c2.quickConnectApproved(qc.secret));
        CHECK(contains(c2.lastError(), "expired"));
        FakeRoute off;
        off.status = 401;
        server.addRoute("/QuickConnect/Initiate", off);
        CHECK(!c2.quickConnectStart(qc));
        CHECK(contains(c2.lastError(), "turned off"));

        // Sign out forgets the token and tells the server.
        FakeRoute ok;
        ok.status = 204;
        server.addRoute("/Sessions/Logout", ok);
        c2.useSavedLogin("user7", "tok");
        c2.signOut();
        CHECK_STR(c2.accessToken(), "");
        CHECK(contains(server.requests().back().path, "/Sessions/Logout"));
        CHECK(!c2.validateLogin());

        // Password login also reports the user's name.
        FakeRoute byName;
        byName.body = "{\"AccessToken\":\"pwtok\",\"User\":{\"Id\":\"u1\",\"Name\":\"Mario\"}}";
        server.addRoute("/Users/AuthenticateByName", byName);
        CHECK(c2.authenticate("Mario", "pw"));
        CHECK_STR(c2.userName(), "Mario");
    }

    server.stop();
    return check::finish("test_jellyfin");
}
