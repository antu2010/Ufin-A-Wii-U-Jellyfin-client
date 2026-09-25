// Artwork cache: loads images in the background and hands out GPU
// textures once they're ready.
//
// The UI calls get() every frame for what's on screen. A miss queues a
// request; a worker thread fetches the bytes (HTTP) and decodes them
// (stb_image) -- the slow parts -- off the render loop. pump(), once per
// frame on the main thread, turns a couple of decoded images into
// textures and evicts the least-recently-used ones beyond the limit.
// Requests for rows that scrolled away before loading are dropped.
//
// The fetch / upload / release steps are injected, so the same logic
// runs in the host tests with fakes and on the Wii U with HTTP + GX2.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class ImageCache {
public:
    using Handle = uint64_t; // an ImTextureID; 0 = none

    struct Backend {
        // Worker thread: bytes for `path`, false on failure.
        std::function<bool(const std::string& path, std::string& bytes)> fetch;
        // Main thread: RGBA8 pixels -> texture handle (0 = failed).
        std::function<Handle(const uint8_t* rgba, int width, int height)> upload;
        // Main thread, only between frames (the GPU is done with it).
        std::function<void(Handle)> release;
    };

    explicit ImageCache(Backend backend, size_t maxTextures = 96);
    ~ImageCache(); // stops the worker; releases every texture

    // Main thread. Texture for `path` if loaded, else 0 (and it's queued).
    // Decoded images are shrunk to fit maxW x maxH. `aspect` (w/h) is
    // set when a texture is returned.
    Handle get(const std::string& path, int maxW, int maxH, float* aspect = nullptr);

    // Main thread, once per frame after drawing: uploads up to maxUploads
    // finished images and evicts beyond the limit.
    void pump(int maxUploads = 2);

    size_t textureCount() const;
    size_t pendingCount() const;

    // Decodes JPEG/PNG bytes to RGBA8 and box-shrinks to fit. Exposed for
    // tests. Returns false for data stb_image can't read.
    static bool decode(const std::string& bytes, int maxW, int maxH, std::vector<uint8_t>& rgba,
                       int& width, int& height);

private:
    enum class State { Queued, Loading, Decoded, Ready, Failed };
    struct Entry {
        State state = State::Queued;
        Handle tex = 0;
        int maxW = 0, maxH = 0;
        int width = 0, height = 0;
        std::vector<uint8_t> pixels; // Decoded, waiting for upload
        uint64_t lastUsed = 0;       // frame number
        uint64_t queuedAt = 0;
    };

    void workerLoop();

    Backend backend_;
    size_t maxTextures_;
    std::map<std::string, Entry> entries_;
    uint64_t frame_ = 1;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::thread worker_;
};
