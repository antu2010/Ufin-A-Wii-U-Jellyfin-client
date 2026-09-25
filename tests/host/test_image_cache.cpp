// ImageCache with fake fetch/upload/release: background loading, one
// fetch per image, uploads paced per frame, LRU eviction that never
// touches this frame's textures, failures not retried, stale requests
// dropped, and real JPEG/PNG decoding + shrinking through stb_image.
#include "check.h"
#include "image_cache.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "vendor/stb_image_write.h"

#include <atomic>
#include <chrono>
#include <set>
#include <thread>

static void appendBytes(void* ctx, void* data, int size) {
    ((std::string*)ctx)->append((const char*)data, (size_t)size);
}

// A solid-colour image encoded as JPEG or PNG.
static std::string makeImage(int w, int h, uint8_t r, uint8_t g, uint8_t b, bool png) {
    std::vector<uint8_t> px((size_t)w * h * 3);
    for (size_t i = 0; i < px.size(); i += 3) { px[i] = r; px[i + 1] = g; px[i + 2] = b; }
    std::string out;
    if (png) stbi_write_png_to_func(appendBytes, &out, w, h, 3, px.data(), w * 3);
    else stbi_write_jpg_to_func(appendBytes, &out, w, h, 3, px.data(), 90);
    return out;
}

struct Fake {
    std::mutex m;
    std::map<std::string, std::string> files;
    std::map<std::string, int> fetches;
    std::atomic<int> fetchDelayMs{0};
    std::map<ImageCache::Handle, std::pair<int, int>> textures; // live
    std::vector<ImageCache::Handle> released;
    ImageCache::Handle next = 100;
    uint8_t firstPixel[4] = {0, 0, 0, 0};

    ImageCache::Backend backend() {
        ImageCache::Backend b;
        b.fetch = [this](const std::string& path, std::string& bytes) {
            if (fetchDelayMs) std::this_thread::sleep_for(std::chrono::milliseconds(fetchDelayMs.load()));
            std::lock_guard<std::mutex> lock(m);
            fetches[path]++;
            auto it = files.find(path);
            if (it == files.end()) return false;
            bytes = it->second;
            return true;
        };
        b.upload = [this](const uint8_t* rgba, int w, int h) {
            for (int i = 0; i < 4; i++) firstPixel[i] = rgba[i];
            ImageCache::Handle hnd = next++;
            textures[hnd] = {w, h};
            return hnd;
        };
        b.release = [this](ImageCache::Handle h) {
            textures.erase(h);
            released.push_back(h);
        };
        return b;
    }
    int fetchCount(const std::string& p) { std::lock_guard<std::mutex> lock(m); return fetches[p]; }
};

// Calls get() + pump() like a frame loop until the image is ready.
static ImageCache::Handle waitFor(ImageCache& cache, const std::string& path, int w, int h,
                                  float* aspect = nullptr, int frames = 400) {
    for (int i = 0; i < frames; i++) {
        ImageCache::Handle t = cache.get(path, w, h, aspect);
        if (t) return t;
        cache.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return 0;
}

int main() {
    // decode(): JPEG and PNG, colours kept, shrunk to fit keeping aspect.
    {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        CHECK(ImageCache::decode(makeImage(200, 300, 200, 40, 40, false), 100, 100, rgba, w, h));
        CHECK_EQ(w, 67);
        CHECK_EQ(h, 100);
        CHECK(rgba[0] > 170 && rgba[1] < 80 && rgba[3] == 255); // red-ish, opaque (JPEG is lossy)
        CHECK(ImageCache::decode(makeImage(40, 20, 10, 200, 30, true), 400, 400, rgba, w, h));
        CHECK_EQ(w, 40); CHECK_EQ(h, 20); // never enlarged
        CHECK_EQ(rgba[1], 200);
        CHECK(ImageCache::decode(makeImage(2000, 1000, 1, 2, 3, true), 0, 0, rgba, w, h));
        CHECK_EQ(w, 512); CHECK_EQ(h, 256); // capped
        CHECK(!ImageCache::decode("not an image at all", 64, 64, rgba, w, h));
        CHECK(!ImageCache::decode("", 64, 64, rgba, w, h));
    }

    // Loads in the background; one fetch per path however often it's asked.
    {
        Fake fake;
        fake.files["/a"] = makeImage(90, 135, 0, 0, 255, true);
        ImageCache cache(fake.backend(), 8);
        CHECK_EQ(cache.get("/a", 60, 90), (ImageCache::Handle)0);
        float aspect = 0;
        ImageCache::Handle t = waitFor(cache, "/a", 60, 90, &aspect);
        CHECK(t != 0);
        CHECK_NEAR(aspect, 60.0 / 90.0, 0.02);
        CHECK_EQ(fake.textures[t].first, 60);
        CHECK_EQ(fake.firstPixel[2], 255);
        for (int i = 0; i < 20; i++) { CHECK_EQ(cache.get("/a", 60, 90), t); cache.pump(); }
        CHECK_EQ(fake.fetchCount("/a"), 1);
        CHECK_EQ(cache.textureCount(), (size_t)1);
        CHECK(cache.get("", 60, 90) == 0);
    }

    // Uploads are paced: at most maxUploads per pump.
    {
        Fake fake;
        for (int i = 0; i < 6; i++) fake.files["/p" + std::to_string(i)] = makeImage(8, 8, 1, 1, 1, true);
        ImageCache cache(fake.backend(), 32);
        for (int i = 0; i < 6; i++) cache.get("/p" + std::to_string(i), 8, 8);
        // Let the worker fetch + decode all six without pumping.
        for (int i = 0; i < 500; i++) {
            bool all = true;
            for (int k = 0; k < 6; k++) all = all && fake.fetchCount("/p" + std::to_string(k)) == 1;
            if (all) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20)); // last decode lands
        cache.pump(2);
        CHECK_EQ(fake.textures.size(), (size_t)2);
        cache.pump(2);
        CHECK_EQ(fake.textures.size(), (size_t)4);
        cache.pump(10);
        CHECK_EQ(fake.textures.size(), (size_t)6);
    }

    // LRU eviction: over the limit, the oldest unused go; this frame's stay.
    {
        Fake fake;
        for (int i = 0; i < 5; i++) fake.files["/e" + std::to_string(i)] = makeImage(8, 8, 9, 9, 9, true);
        ImageCache cache(fake.backend(), 3);
        std::vector<ImageCache::Handle> h;
        for (int i = 0; i < 5; i++) h.push_back(waitFor(cache, "/e" + std::to_string(i), 8, 8));
        for (ImageCache::Handle x : h) CHECK(x != 0);
        cache.pump();
        CHECK(cache.textureCount() <= 3);
        CHECK(fake.textures.size() <= 3);
        // The newest ones survived, the oldest were released.
        CHECK(fake.textures.count(h[4]) == 1);
        CHECK(std::find(fake.released.begin(), fake.released.end(), h[0]) != fake.released.end());

        // All used in the same frame: nothing that frame's draws use is freed.
        for (int i = 0; i < 3; i++) cache.get("/e" + std::to_string(i + 2), 8, 8);
        size_t releasedBefore = fake.released.size();
        cache.pump();
        CHECK_EQ(fake.released.size(), releasedBefore);
    }

    // Failures are remembered: no refetch, stays 0.
    {
        Fake fake;
        fake.files["/bad"] = "garbage";
        ImageCache cache(fake.backend(), 8);
        CHECK(waitFor(cache, "/missing", 8, 8, nullptr, 60) == 0);
        CHECK(waitFor(cache, "/bad", 8, 8, nullptr, 60) == 0);
        CHECK_EQ(fake.fetchCount("/missing"), 1);
        CHECK_EQ(fake.fetchCount("/bad"), 1);
        CHECK(fake.textures.empty());
    }

    // Stale requests (scrolled away before loading) are dropped, not fetched.
    {
        Fake fake;
        fake.fetchDelayMs = 50; // worker is busy on the first request
        for (int i = 0; i < 4; i++) fake.files["/s" + std::to_string(i)] = makeImage(8, 8, 5, 5, 5, true);
        ImageCache cache(fake.backend(), 8);
        cache.get("/s0", 8, 8); // worker takes this one
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        cache.get("/s1", 8, 8);
        cache.get("/s2", 8, 8);
        for (int f = 0; f < 40; f++) { cache.get("/s3", 8, 8); cache.pump(); } // only s3 stays wanted
        fake.fetchDelayMs = 0;
        CHECK(waitFor(cache, "/s3", 8, 8) != 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        CHECK_EQ(fake.fetchCount("/s1"), 0);
        CHECK_EQ(fake.fetchCount("/s2"), 0);
    }

    // Destruction releases every texture.
    {
        Fake fake;
        fake.files["/d"] = makeImage(8, 8, 1, 2, 3, true);
        std::set<ImageCache::Handle> made;
        {
            ImageCache cache(fake.backend(), 8);
            made.insert(waitFor(cache, "/d", 8, 8));
        }
        CHECK(fake.textures.empty());
    }

    return check::finish("test_image_cache");
}
