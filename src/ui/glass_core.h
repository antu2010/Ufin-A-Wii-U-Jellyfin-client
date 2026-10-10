// Liquid Glass core -- everything about the shared blurred backdrop that does
// not need ImGui or GX2, so it is host-testable: the low-resolution (1/4) blur
// dimensions, the Dual-Kawase chain, virtual-space -> texture coordinate
// mapping, update scheduling, and BackdropManager, which owns the three small
// textures the glass panels sample and talks to the GPU only through an
// injected TextureSink (gfx.cpp wires that to gpu_image.cpp's CPU-written
// textures -- the same lock/memcpy/invalidate path artwork already uses).
//
// One shared blurred texture serves every panel on both the TV and the
// GamePad: it lives in the UI's virtual 1280x720 space (layout.h), which both
// displays are laid out in, so a panel maps to it with the same UVs whatever
// the real framebuffer is. Its size is derived from the real targets (1/4 of
// the smaller axis-wise of TV and GamePad, e.g. 214x120 for the 854x480 pad).
//
// Deliberately NOT here: any playback code, and any GPU render-to-texture.
// The backdrop is a synthesized soft light field (see BackdropParams), so it
// costs a tiny CPU blur a few times a second in menus and nothing at all
// while video is on screen.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace ui {
namespace glass {

struct Dims {
    int w = 0, h = 0;
    bool valid() const { return w > 0 && h > 0; }
    bool operator==(const Dims& o) const { return w == o.w && h == o.h; }
};

static const int BLUR_DIVISOR = 4; // ~1/4 of the display
static const int MIN_DIM = 8;      // never smaller than this per axis
static const int MAX_LEVELS = 3;   // Dual-Kawase levels (down passes) ever allocated

// ~1/4 of a render target, rounded up, never below MIN_DIM. {0,0} for a
// nonsensical target.
Dims blurDims(int targetW, int targetH, int divisor = BLUR_DIVISOR);
// One texture for both displays: the smaller of the two 1/4 sizes per axis.
Dims sharedDims(int tvW, int tvH, int drcW, int drcH, int divisor = BLUR_DIVISOR);
// Dimensions of Dual-Kawase level `level` (0 = the base image; each level
// halves, rounding up, never below 1).
Dims levelDims(Dims base, int level);
// How many down passes to actually run: at most `requested` and MAX_LEVELS,
// and few enough that the smallest level keeps at least 4 px per axis.
int kawaseLevels(Dims base, int requested);

struct Uv {
    float u0, v0, u1, v1;
};
// Texture coordinates of a rectangle given in the UI's virtual space
// (screenW x screenH, y down). Clamped to 0..1; v = 0 is the top row, exactly
// like the artwork textures the same renderer already draws.
Uv uvForRect(float x, float y, float w, float h, float screenW, float screenH);
// The blur texel under a virtual-space point (clamped inside the texture).
void texelForPoint(float x, float y, float screenW, float screenH, Dims d, int& tx, int& ty);
// Physical pixels per virtual unit for a display of pw x ph. The TV and the
// GamePad differ; each is handled with its own scale.
struct Scale {
    float x, y;
};
Scale displayScale(int pw, int ph, float screenW, float screenH);

struct Rgb {
    float r = 0, g = 0, b = 0; // 0..1
};

// What the backdrop looks like: a soft gradient with a few slowly drifting
// colour blobs (accent colours, plus the selected artwork's colour). A
// stand-in for "the scene behind the glass" for the UI layer only -- video
// is never blurred (see BackdropManager::update).
struct BackdropParams {
    double time = 0.0;
    Rgb base{0.078f, 0.086f, 0.102f};
    Rgb accent{0.0f, 0.64f, 0.86f};
    Rgb accent2{0.67f, 0.36f, 0.76f};
    Rgb art{0, 0, 0};
    float artStrength = 0.0f; // 0..1, how much the artwork colour shows
    bool light = false;       // light palette
    bool animated = true;     // false: no drift, so it never needs regenerating
};

// Dual-Kawase blur over a synthesized backdrop, in preallocated float
// buffers (no allocation after init()). Steps: synthesize at the base size,
// `levels` down passes (5 taps), `levels` up passes (8 taps), convert to RGBA8.
// It can run in one go (render) or time-sliced (begin / step / finish): the
// work is split into row-sized units so a frame only ever pays for a small,
// bounded slice -- a whole regeneration would be a visible hitch on the
// console's CPU.
class BlurChain {
public:
    bool init(Dims base, int maxLevels); // false on invalid dims / allocation failure
    bool ready() const { return ready_; }
    Dims dims() const { return base_; }
    int allocatedLevels() const { return maxLevels_; }
    int levels() const { return levels_; }
    void setLevels(int n); // 1..allocatedLevels
    // Writes base.w * base.h * 4 bytes (alpha = 255). All at once.
    void render(const BackdropParams& p, uint8_t* rgbaOut);

    // Time-sliced form of render(): begin() captures the parameters, step()
    // does up to `rowBudget` rows of work and returns true once everything is
    // done, finish() converts the result. cancel() abandons a job in progress.
    void begin(const BackdropParams& p);
    bool step(int rowBudget);
    void finish(uint8_t* rgbaOut);
    void cancel() { stage_ = nStages_; }
    bool busy() const { return ready_ && stage_ < nStages_; }
    int totalRows() const; // work units in one full regeneration at the current level count
    size_t memoryBytes() const;

    struct Img {
        Dims d;
        std::vector<float> px; // RGB
    };

private:
    bool ready_ = false;
    Dims base_;
    int maxLevels_ = 0, levels_ = 0;
    BackdropParams params_;
    struct Stage {
        int kind; // 0 synth, 1 down, 2 up
        int level;
        int rows;
    };
    Stage stages_[2 * MAX_LEVELS + 1];
    int nStages_ = 0, stage_ = 0, row_ = 0;
    Img synth_;
    std::vector<Img> down_; // levels 1..maxLevels
    std::vector<Img> up_;   // levels 0..maxLevels-1 results (up_[0] is the output)
};

// How the GPU side is reached. gfx.cpp supplies real implementations;
// tests supply fakes. create returns 0 on failure.
struct TextureSink {
    std::function<uint64_t(const uint8_t* rgba, int w, int h)> create;
    std::function<bool(uint64_t handle, const uint8_t* rgba, int w, int h)> update;
    std::function<void(uint64_t handle)> release;
};

enum class Mode {
    Off,  // Liquid Glass disabled: no resources, nothing runs
    Full, // shared blur active
    Low,  // cost too high: fewer passes
    Flat, // no blur texture (init failed / too slow): panels use tint + gloss only
};
const char* modeName(Mode m);

struct Stats {
    uint32_t updates = 0;        // completed blur regenerations
    uint32_t slices = 0;         // update() calls that did blur work
    uint32_t skipped = 0;        // update() calls that reused the previous blur
    uint32_t texturesCreated = 0; // total create() calls (stays at 3 -- nothing per frame)
    uint32_t updateFailures = 0;
    double lastMs = 0.0, avgMs = 0.0; // per-frame slice cost
};

// Owns the blur chain and the two textures. All methods are called from the
// render thread between frames (never from the playback thread).
class BackdropManager {
public:
    // Blur regeneration interval while animated, in seconds (the light drifts
    // over tens of seconds; 1 Hz is invisible, and a 1/4-res pass takes most of a second anyway).
    static constexpr double ANIMATED_INTERVAL = 1.0;
    // Blur work per update() call, measured in pixels (rows x width) so a frame's
    // share stays ~1 ms whatever the blur resolution: ROWS_PER_UPDATE rows of a
    // REF_WIDTH-wide image (the old 1/8-resolution GamePad texture).
    static constexpr int ROWS_PER_UPDATE = 16;
    static constexpr int REF_WIDTH = 107;
    static int rowsPerUpdate(int width) {
        const int r = width > 0 ? (ROWS_PER_UPDATE * REF_WIDTH) / width : ROWS_PER_UPDATE;
        return r < 2 ? 2 : (r > 64 ? 64 : r);
    }
    // Averaged per-frame cost (ms) above which the passes drop / blur is
    // abandoned. Playback performance wins over the effect.
    static constexpr double LOW_MS = 3.0, FLAT_MS = 6.0;

    void setLog(std::function<void(const char*)> log) { log_ = std::move(log); }
    void setDebug(bool on) { debug_ = on; }

    // Allocates the chain and the three textures. On any failure everything is
    // released, the mode becomes Flat and false is returned (the caller keeps
    // going with the flat glass style).
    bool enable(Dims dims, const TextureSink& sink, int levels = 2);
    // Releases the textures and the chain; mode Off. Safe to call twice.
    void disable();

    // Once per frame, before the frame is built and never mid-frame (the GPU
    // is finished with last frame's textures). Regenerates the blur only when
    // it is due; while `videoActive` (a video frame is being presented) it does
    // nothing at all, so playback is never touched. `now` in seconds.
    void update(double now, const BackdropParams& params, bool videoActive);

    Mode mode() const { return mode_; }
    bool active() const { return mode_ == Mode::Full || mode_ == Mode::Low; }
    // Smooth motion without regenerating at frame rate: the blur is rebuilt about
    // once a second, and the GPU cross-fades from the previous result to the
    // newest one. Panels draw textureFrom() then texture() at blend(now).
    // Texture handle of the newest blur (the fade target), 0 when there is none (Off / Flat).
    uint64_t texture() const { return active() ? tex_[to_] : 0; }
    // The blur being faded away from (same as texture() when no fade is running).
    uint64_t textureFrom() const { return active() ? tex_[from_] : 0; }
    // 0 = all textureFrom(), 1 = all texture(); eased, time-based, no per-frame cost.
    float blend(double now) const;
    static constexpr int TEXTURES = 3; // from, to, and one free to be written
    Dims dims() const { return chain_.dims(); }
    int levels() const { return chain_.levels(); }
    const Stats& stats() const { return stats_; }
    size_t memoryBytes() const; // chain + the textures

private:
    void say(const char* msg) const {
        if (log_) log_(msg);
    }
    void toFlat(const char* why);
    static uint32_t signature(const BackdropParams& p);

    std::function<void(const char*)> log_;
    bool debug_ = false;
    TextureSink sink_;
    BlurChain chain_;
    std::vector<uint8_t> rgba_;
    uint64_t tex_[TEXTURES] = {0, 0, 0};
    int from_ = 0, to_ = 0; // cross-fade source / target; the third texture is written next
    double swapTime_ = 0.0, fadeDur_ = 1.0;
    Mode mode_ = Mode::Off;
    bool haveFrame_ = false;
    double lastUpdate_ = 0.0;
    uint32_t lastSig_ = 0, jobSig_ = 0;
    Stats stats_;
    double lastLog_ = 0.0;
};

BackdropManager& backdrop();

// ---------------------------------------------------------------------------
// Real refraction.
//
// The blurred backdrop above is a soft colour field; it cannot show what is
// actually *behind* a panel. Real refraction works from a snapshot of the frame
// as drawn so far (the platform copies the colour buffer into a texture when a
// draw-list callback runs) and draws each glass shape from that snapshot with a
// bent lookup -- near the rim the content is pulled in from further inside, so
// letters and pictures behind the glass visibly warp, the way Apple's Liquid
// Glass does. Everything here is pure maths, so it is unit-tested on the host;
// the capture itself lives in gfx.cpp / gpu_image.cpp.
// ---------------------------------------------------------------------------

// How a shape bends what is behind it. Distances are in the virtual 1280x720 space.
struct Lens {
    float edge;    // width of the bent band along the rim (0 = flat)
    float bend;    // how far the lookup is pulled inwards right at the rim
    float magnify; // <= 1: the (unbent) interior samples slightly closer to the centre -- a faint lens
};

// The lens for a w x h shape with corner radius `rad`. The band never exceeds
// what the corner arc can hold (so the mesh rings keep one vertex count) and
// never more than ~45% of the short side, so a thin pill is still bent along its
// whole height.
inline Lens lensFor(float w, float h, float rad) {
    const float shortSide = w < h ? w : h, longSide = w < h ? h : w;
    Lens l;
    float edge = shortSide * 0.45f;
    if (edge > 30.0f) edge = 30.0f;
    if (edge > rad - 0.6f) edge = rad - 0.6f;
    l.edge = edge > 0.0f ? edge : 0.0f;
    l.bend = l.edge * 0.75f; // > 0.5 folds the picture back right at the rim (a flipped sliver, like Apple's); 0.5 would only stretch it
    // Interior magnification, limited so even a screen-wide panel shifts content by
    // at most ~7 px at its far corners.
    float m = 7.0f / (0.5f * (longSide > 1.0f ? longSide : 1.0f));
    if (m > 0.035f) m = 0.035f;
    l.magnify = 1.0f - m;
    return l;
}

// How far (virtual px) the lookup is pulled inwards at depth t into the band:
// t = 0 at the rim (full bend), t = 1 where the band ends (none). Smooth and
// monotonic, so there are no creases in the picture.
inline float lensShift(const Lens& l, float t) {
    if (t <= 0.0f) return l.bend;
    if (t >= 1.0f) return 0.0f;
    const float k = 1.0f - t;
    return l.bend * k * k;
}

// Mesh ring depths, as fractions of the band: dense at the rim where the bending
// is steep, sparse in the flat interior.
static constexpr int LENS_RINGS = 6;
static constexpr float LENS_RING_DEPTH[LENS_RINGS] = {0.0f, 0.05f, 0.13f, 0.25f, 0.45f, 1.0f};

// Decides when a glass shape needs a fresh snapshot of the frame.
//
// A snapshot holds everything drawn *before* it, so one is enough for every glass
// shape that does not overlap another glass shape drawn since (a list of rows
// does not overlap itself). A shape that sits on top of earlier glass -- the rows
// over the sidebar panel -- needs a new snapshot so it refracts the panel too.
// Typically 2 snapshots per frame; a hard cap keeps the worst case bounded (past
// it, shapes just reuse the latest snapshot, which is slightly off but safe).
// Call onSurface() for every glass shape, in draw order.
class SnapshotPlanner {
public:
    static constexpr int MAX_PER_FRAME = 4;
    static constexpr int MAX_RECTS = 96;
    static constexpr float MARGIN = 6.0f; // covers the soft shadow around a shape

    // `frame` changes every UI frame; `list` identifies the draw list (lists render
    // in a fixed order that is not the call order, so a list change needs a
    // snapshot of its own).
    bool onSurface(uint64_t frame, const void* list, float x0, float y0, float x1, float y1) {
        if (frame != frame_) {
            frame_ = frame;
            list_ = list;
            count_ = 0;
            rects_.clear();
            needFirst_ = true;
        } else if (list != list_) {
            list_ = list;
            rects_.clear();
            needFirst_ = true;
        }
        bool need = needFirst_ || (int)rects_.size() >= MAX_RECTS;
        for (size_t i = 0; !need && i < rects_.size(); i++) {
            const Rect& r = rects_[i];
            need = x0 - MARGIN < r.x1 && x1 + MARGIN > r.x0 && y0 - MARGIN < r.y1 && y1 + MARGIN > r.y0;
        }
        if (need && count_ >= MAX_PER_FRAME) {
            need = false;
            if ((int)rects_.size() >= MAX_RECTS) rects_.clear();
        }
        if (need) {
            count_++;
            rects_.clear();
            needFirst_ = false;
        }
        rects_.push_back({x0, y0, x1, y1});
        return need;
    }
    int snapshotsThisFrame() const { return count_; }

private:
    struct Rect { float x0, y0, x1, y1; };
    uint64_t frame_ = ~0ull;
    const void* list_ = nullptr;
    int count_ = 0;
    bool needFirst_ = true;
    std::vector<Rect> rects_;
};

} // namespace glass
} // namespace ui
