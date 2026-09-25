// MenuMusic over a real AAC stream from the fake HTTP server: plays,
// loops when the song ends (asking for a fresh path each time), plays
// at the reduced volume, backs off when the song is missing, and stops
// promptly either way.
#include "check.h"
#include "fake_http_server.h"
#include "media/menu_music.h"

#include <SDL2/SDL.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <set>
#include <thread>

static double since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: test_menu_music <song.m4a>\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();

    FakeHttpServer server;
    CHECK(server.start());
    FakeRoute song;
    song.contentType = "audio/mp4";
    song.body = ss.str();
    song.chunked = true;
    song.chunkSize = 4000;
    server.addRoute("/song", song);

    // Plays and loops.
    {
        MenuMusic music;
        int pathCalls = 0;
        const std::thread::id mainThread = std::this_thread::get_id();
        CHECK_EQ(fake_sdl_open_devices(), 0);
        CHECK(music.start("127.0.0.1", server.port(), [&] {
            pathCalls++;
            return "/song?PlaySessionId=" + std::to_string(pathCalls);
        }, 0.25f));
        // The device is opened by the thread that called start() -- on the
        // Wii U, AX must not be torn down from another core.
        CHECK(fake_sdl_open_thread() == mainThread);
        CHECK_EQ(fake_sdl_open_devices(), 1);
        CHECK(music.running());
        auto t0 = std::chrono::steady_clock::now();
        Uint32 queuedAtStart = fake_sdl_total_queued();
        int maxSample = 0;
        // "Play" the audio (the fake device never drains by itself) until
        // the song has looped at least twice.
        while (music.loopsStarted() < 3 && since(t0) < 20.0) {
            Uint32 n = 0;
            const unsigned char* d = fake_sdl_last_queued(&n);
            for (Uint32 i = 0; i + 1 < n; i += 2) maxSample = std::max(maxSample, std::abs((int)((const int16_t*)d)[i / 2]));
            fake_sdl_consume_audio(48000 * 4 / 10);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(music.loopsStarted() >= 3);
        CHECK(fake_sdl_total_queued() - queuedAtStart > 48000 * 4); // more than a second of audio
        CHECK(maxSample > 1000);   // not silent...
        CHECK(maxSample < 13000);  // ...but quieter than the full-scale tone
        {
            auto reqs = server.requests();
            int songRequests = 0;
            std::set<std::string> paths;
            for (auto& r : reqs) if (r.path.rfind("/song", 0) == 0) { songRequests++; paths.insert(r.path); }
            CHECK(songRequests >= 3);
            CHECK((int)paths.size() == songRequests); // fresh path (session) every loop
        }
        CHECK_EQ(fake_sdl_open_devices(), 1); // one device across all loops
        auto t1 = std::chrono::steady_clock::now();
        music.stop();
        CHECK(since(t1) < 2.0);
        CHECK(!music.running());
        CHECK(fake_sdl_close_thread() == mainThread); // closed on the main thread too
        CHECK_EQ(fake_sdl_open_devices(), 0);
        music.stop(); // twice is fine
        CHECK_EQ(fake_sdl_open_devices(), 0);
    }

    // Missing song: retries with a back-off, and stop() doesn't wait it out.
    {
        MenuMusic music;
        music.start("127.0.0.1", server.port(), [] { return std::string("/nope"); });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK_EQ(music.loopsStarted(), 1); // waiting 5 s before the next try
        auto t1 = std::chrono::steady_clock::now();
        music.stop();
        CHECK(since(t1) < 1.0);
        CHECK_EQ(fake_sdl_open_devices(), 0);
    }

    // Restarting replaces the old thread.
    {
        MenuMusic music;
        music.start("127.0.0.1", server.port(), [] { return std::string("/song"); });
        music.start("127.0.0.1", server.port(), [] { return std::string("/song"); });
        CHECK(music.running());
        CHECK_EQ(fake_sdl_open_devices(), 1); // the first device was closed before the second opened
        music.stop();
        CHECK_EQ(fake_sdl_open_devices(), 0);
    }

    server.stop();
    return check::finish("test_menu_music");
}
