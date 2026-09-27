// A small, fixed-cost spectrum analyser for the music visualizer. It uses a
// 1024-point Hann-windowed FFT and twelve logarithmic frequency bands. The
// analyser runs on the UI/render thread over the PCM snapshot supplied by
// AudioOutput, then applies a smoothed level plus a short-lived transient
// envelope so ordinary FFT noise does not make the bars chatter while kicks
// and beat drops can still punch the bars upward.
#pragma once
#include <cstdint>

namespace media {

class AudioVisualizer {
public:
    static const int BANDS = 12;

    // samples: mono PCM (see AudioOutput::fetchVisualizerSamples).
    // count: how many of `samples` are valid; 0 is fine (e.g. nothing
    // new since the last frame) and just lets levels decay toward zero.
    // sampleRate: the output sample rate (AudioOutput always resamples
    // to 48000, but don't hardcode that here).
    // dt: seconds since the last call, for frame-rate-independent
    // attack/decay smoothing.
    void update(const int16_t* samples, int count, int sampleRate, double dt);

    // Display levels, normalized to roughly 0..1, one per band from low to
    // high frequency. The renderer clamps these before converting them to
    // pixel heights.
    const float* levels() const { return levels_; }

    // Back to silence -- call when playback stops or the visualizer is
    // turned off, so it doesn't show stale bars if turned back on later.
    void reset();

private:
    // Target smoothing keeps ordinary spectral fluctuations from making the
    // bars chatter, while the separate peak envelope lets transients (kick
    // drums, snares, drops) punch upward immediately and then fall away
    // naturally like a classic music visualizer.
    float levels_[BANDS] = {};
    float smoothedTargets_[BANDS] = {};
    float peakEnvelope_[BANDS] = {};
};

} // namespace media
