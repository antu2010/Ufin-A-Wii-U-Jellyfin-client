#include "image_cache.h"

#define STB_IMAGE_IMPLEMENTATION
// No thread_local: the Wii U's RPX format has no thread-local storage, and
// elf2rpl rejects the TLS relocations stb_image otherwise generates
// ("Unsupported relocation type 70/72"). Only the image worker thread
// decodes, so stb's per-thread error state isn't needed.
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include <stb/stb_image.h>

#include <algorithm>

// Frames a queued request may wait without being asked for again before
// it's dropped (it scrolled off screen); it's re-queued if it comes back.
static const uint64_t STALE_FRAMES = 30;
// Longest side kept for any image, whatever the caller asks for.
static const int MAX_SIDE = 512;

ImageCache::ImageCache(Backend backend, size_t maxTextures)
    : backend_(std::move(backend)), maxTextures_(maxTextures), worker_([this] { workerLoop(); }) {}

ImageCache::~ImageCache() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    worker_.join();
    for (auto& kv : entries_) {
        if (kv.second.state == State::Ready && kv.second.tex && backend_.release) backend_.release(kv.second.tex);
    }
}

ImageCache::Handle ImageCache::get(const std::string& path, int maxW, int maxH, float* aspect) {
    if (path.empty()) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(path);
    if (it == entries_.end()) {
        Entry e;
        e.maxW = maxW;
        e.maxH = maxH;
        e.lastUsed = frame_;
        e.queuedAt = frame_;
        entries_.emplace(path, std::move(e));
        wake_.notify_one();
        return 0;
    }
    Entry& e = it->second;
    e.lastUsed = frame_;
    if (e.state == State::Ready) {
        if (aspect && e.height > 0) *aspect = (float)e.width / (float)e.height;
        return e.tex;
    }
    return 0;
}

void ImageCache::pump(int maxUploads) {
    std::unique_lock<std::mutex> lock(mutex_);

    // Upload finished decodes, most recently wanted first (most likely
    // on screen).
    std::vector<std::pair<uint64_t, std::string>> decoded;
    for (auto& kv : entries_) {
        if (kv.second.state == State::Decoded) decoded.emplace_back(kv.second.lastUsed, kv.first);
    }
    std::sort(decoded.begin(), decoded.end(),
              [](const std::pair<uint64_t, std::string>& a, const std::pair<uint64_t, std::string>& b) {
                  return a.first > b.first;
              });
    int uploads = 0;
    for (const auto& d : decoded) {
        if (uploads >= maxUploads) break;
        auto it = entries_.find(d.second);
        if (it == entries_.end() || it->second.state != State::Decoded) continue;
        std::vector<uint8_t> pixels;
        pixels.swap(it->second.pixels);
        const int w = it->second.width, h = it->second.height;
        lock.unlock();
        Handle tex = backend_.upload ? backend_.upload(pixels.data(), w, h) : 0;
        lock.lock();
        it = entries_.find(d.second); // only this thread erases, but be safe
        if (it == entries_.end()) {
            if (tex && backend_.release) backend_.release(tex);
            continue;
        }
        it->second.tex = tex;
        it->second.state = tex ? State::Ready : State::Failed;
        uploads++;
    }

    // Drop queued requests nobody has asked for lately.
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.state == State::Queued && frame_ - it->second.lastUsed > STALE_FRAMES) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }

    // Evict least-recently-used textures beyond the limit -- never one
    // drawn this frame.
    size_t ready = 0;
    for (auto& kv : entries_) if (kv.second.state == State::Ready) ready++;
    while (ready > maxTextures_) {
        auto victim = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->second.state != State::Ready || it->second.lastUsed >= frame_) continue;
            if (victim == entries_.end() || it->second.lastUsed < victim->second.lastUsed) victim = it;
        }
        if (victim == entries_.end()) break;
        Handle tex = victim->second.tex;
        entries_.erase(victim);
        ready--;
        if (tex && backend_.release) backend_.release(tex);
    }

    frame_++;
}

size_t ImageCache::textureCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for (auto& kv : entries_) if (kv.second.state == State::Ready) n++;
    return n;
}

size_t ImageCache::pendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for (auto& kv : entries_) {
        if (kv.second.state == State::Queued || kv.second.state == State::Loading ||
            kv.second.state == State::Decoded) n++;
    }
    return n;
}

void ImageCache::workerLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_) {
        // Most recently requested first: that's what's on screen now.
        auto pick = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->second.state != State::Queued) continue;
            if (pick == entries_.end() || it->second.lastUsed > pick->second.lastUsed ||
                (it->second.lastUsed == pick->second.lastUsed && it->second.queuedAt > pick->second.queuedAt)) {
                pick = it;
            }
        }
        if (pick == entries_.end()) {
            wake_.wait(lock);
            continue;
        }
        const std::string path = pick->first;
        const int maxW = pick->second.maxW, maxH = pick->second.maxH;
        pick->second.state = State::Loading;
        lock.unlock();

        std::string bytes;
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        bool ok = backend_.fetch && backend_.fetch(path, bytes) && decode(bytes, maxW, maxH, rgba, w, h);

        lock.lock();
        auto it = entries_.find(path);
        if (it == entries_.end()) continue;
        if (ok) {
            it->second.pixels.swap(rgba);
            it->second.width = w;
            it->second.height = h;
            it->second.state = State::Decoded;
        } else {
            it->second.state = State::Failed; // no retry this session
        }
    }
}

bool ImageCache::decode(const std::string& bytes, int maxW, int maxH, std::vector<uint8_t>& rgba,
                        int& width, int& height) {
    int w = 0, h = 0, comp = 0;
    unsigned char* src = stbi_load_from_memory((const stbi_uc*)bytes.data(), (int)bytes.size(), &w, &h, &comp, 4);
    if (!src || w <= 0 || h <= 0) {
        if (src) stbi_image_free(src);
        return false;
    }
    maxW = std::min(maxW > 0 ? maxW : MAX_SIDE, MAX_SIDE);
    maxH = std::min(maxH > 0 ? maxH : MAX_SIDE, MAX_SIDE);
    double scale = std::min(1.0, std::min((double)maxW / w, (double)maxH / h));
    int dw = std::max(1, (int)(w * scale + 0.5)), dh = std::max(1, (int)(h * scale + 0.5));
    rgba.assign((size_t)dw * dh * 4, 0);
    if (dw == w && dh == h) {
        std::copy(src, src + (size_t)w * h * 4, rgba.begin());
    } else {
        // Box filter: average every source pixel that lands in each target pixel.
        for (int y = 0; y < dh; y++) {
            int y0 = y * h / dh, y1 = std::max(y0 + 1, (y + 1) * h / dh);
            for (int x = 0; x < dw; x++) {
                int x0 = x * w / dw, x1 = std::max(x0 + 1, (x + 1) * w / dw);
                unsigned sum[4] = {0, 0, 0, 0};
                for (int sy = y0; sy < y1; sy++)
                    for (int sx = x0; sx < x1; sx++)
                        for (int c = 0; c < 4; c++) sum[c] += src[((size_t)sy * w + sx) * 4 + c];
                unsigned n = (unsigned)((y1 - y0) * (x1 - x0));
                for (int c = 0; c < 4; c++) rgba[((size_t)y * dw + x) * 4 + c] = (uint8_t)(sum[c] / n);
            }
        }
    }
    stbi_image_free(src);
    width = dw;
    height = dh;
    return true;
}
