// AudioVisualizer: silence stays at zero, a pure tone lights up the band
// nearest its frequency more than distant bands, levels never go
// negative, and reset() actually clears state -- the things a bars
// visualizer needs to not look broken, without needing real audio
// hardware or ears to check.
#include "check.h"
#include "../../src/media/audio_visualizer.h"

#include <cmath>
#include <vector>

using media::AudioVisualizer;

static std::vector<int16_t> makeTone(double freqHz, int sampleRate, int count, double amplitude = 0.8) {
    std::vector<int16_t> out(count);
    for (int i = 0; i < count; i++) {
        double s = std::sin(2.0 * M_PI * freqHz * i / sampleRate);
        out[i] = (int16_t)(s * amplitude * 32767.0);
    }
    return out;
}

int main() {
    const int sampleRate = 48000;

    // Silence (or no data at all) settles to all-zero levels.
    {
        AudioVisualizer viz;
        std::vector<int16_t> silence(1024, 0);
        for (int i = 0; i < 30; i++) viz.update(silence.data(), (int)silence.size(), sampleRate, 1.0 / 60.0);
        bool allZero = true;
        for (int i = 0; i < AudioVisualizer::BANDS; i++) if (viz.levels()[i] > 0.01f) allZero = false;
        CHECK(allZero);

        // No samples at all (nothing new this frame) behaves the same as
        // silence -- decays, never crashes on null/zero-count input.
        viz.update(nullptr, 0, sampleRate, 1.0 / 60.0);
        CHECK(viz.levels()[0] >= 0.0f);
    }

    // A low-frequency tone (~110 Hz) should, after settling, peak in the
    // lower half of the bands (its nearest band is #0, but Goertzel
    // response spreads across neighbors -- what matters for a bars
    // visualizer is that energy shows up on the correct side).
    {
        AudioVisualizer viz;
        auto tone = makeTone(110.0, sampleRate, 1024);
        for (int i = 0; i < 60; i++) viz.update(tone.data(), (int)tone.size(), sampleRate, 1.0 / 60.0);
        int argmax = 0;
        for (int i = 1; i < AudioVisualizer::BANDS; i++) if (viz.levels()[i] > viz.levels()[argmax]) argmax = i;
        CHECK(argmax < AudioVisualizer::BANDS / 2);
        CHECK(viz.levels()[argmax] > 0.05f); // actually lit up, not just "less small"
    }

    // A high-frequency tone (~6000 Hz, near the top band's ~7500 Hz)
    // should peak in the upper half of the bands.
    {
        AudioVisualizer viz;
        auto tone = makeTone(6000.0, sampleRate, 1024);
        for (int i = 0; i < 60; i++) viz.update(tone.data(), (int)tone.size(), sampleRate, 1.0 / 60.0);
        int argmax = 0;
        for (int i = 1; i < AudioVisualizer::BANDS; i++) if (viz.levels()[i] > viz.levels()[argmax]) argmax = i;
        CHECK(argmax >= AudioVisualizer::BANDS / 2);
        CHECK(viz.levels()[argmax] > 0.05f);
    }

    // A music-like signal made from several frequencies should light
    // multiple separated regions, not collapse everything into one bar.
    {
        AudioVisualizer viz;
        std::vector<int16_t> mix(1024);
        for (int i = 0; i < (int)mix.size(); i++) {
            double t = (double)i / sampleRate;
            double s = 0.50 * std::sin(2.0 * M_PI * 120.0 * t)
                     + 0.35 * std::sin(2.0 * M_PI * 1000.0 * t)
                     + 0.25 * std::sin(2.0 * M_PI * 6000.0 * t);
            mix[i] = (int16_t)(std::max(-1.0, std::min(1.0, s)) * 32767.0);
        }
        for (int i = 0; i < 60; i++) viz.update(mix.data(), (int)mix.size(), sampleRate, 1.0 / 60.0);
        int lit = 0;
        for (int i = 0; i < AudioVisualizer::BANDS; i++) if (viz.levels()[i] > 0.10f) lit++;
        CHECK(lit >= 3);
    }

    // Levels never go negative, however long silence continues (decay
    // shouldn't overshoot below zero).
    {
        AudioVisualizer viz;
        auto tone = makeTone(1000.0, sampleRate, 1024);
        viz.update(tone.data(), (int)tone.size(), sampleRate, 1.0 / 60.0);
        std::vector<int16_t> silence(1024, 0);
        for (int i = 0; i < 500; i++) {
            viz.update(silence.data(), (int)silence.size(), sampleRate, 1.0 / 60.0);
            for (int b = 0; b < AudioVisualizer::BANDS; b++) CHECK(viz.levels()[b] >= 0.0f);
        }
    }

    // A steady signal with alternating amplitude should not make the
    // displayed bar chatter wildly: the baseline smoothing damps the
    // frame-to-frame spectral wobble.
    {
        AudioVisualizer viz;
        std::vector<int16_t> tone(1024);
        float minLevel = 1.0f;
        float maxLevel = 0.0f;
        for (int frame = 0; frame < 60; frame++) {
            const double amp = (frame % 2 == 0) ? 0.42 : 0.58;
            for (int i = 0; i < (int)tone.size(); i++) {
                const double t = (double)i / sampleRate;
                tone[i] = (int16_t)(std::sin(2.0 * M_PI * 1000.0 * t) * amp * 32767.0);
            }
            viz.update(tone.data(), (int)tone.size(), sampleRate, 1.0 / 60.0);
            if (frame >= 30) {
                minLevel = std::min(minLevel, viz.levels()[7]);
                maxLevel = std::max(maxLevel, viz.levels()[7]);
            }
        }
        CHECK(maxLevel - minLevel < 0.20f);
    }

    // A one-frame loud transient should punch the relevant band above the
    // established baseline and then release gradually rather than dropping
    // immediately on the next frame.
    {
        AudioVisualizer viz;
        std::vector<int16_t> quiet(1024);
        std::vector<int16_t> loud(1024);
        for (int i = 0; i < 1024; i++) {
            double t = (double)i / sampleRate;
            quiet[i] = (int16_t)(std::sin(2.0 * M_PI * 1000.0 * t) * 0.02 * 32767.0);
            loud[i] = (int16_t)(std::sin(2.0 * M_PI * 1000.0 * t) * 0.95 * 32767.0);
        }
        for (int i = 0; i < 30; i++) viz.update(quiet.data(), 1024, sampleRate, 1.0 / 60.0);
        const float baseline = viz.levels()[7];
        viz.update(loud.data(), 1024, sampleRate, 1.0 / 60.0);
        const float punch = viz.levels()[7];
        viz.update(quiet.data(), 1024, sampleRate, 1.0 / 60.0);
        const float after = viz.levels()[7];
        CHECK(punch > baseline + 0.15f);
        CHECK(after > baseline);
    }

    // reset() clears everything back to zero immediately, not just over
    // time -- important so turning the visualizer off and back on later
    // doesn't show a stale frame from the previous track.
    {
        AudioVisualizer viz;
        auto tone = makeTone(1000.0, sampleRate, 1024);
        for (int i = 0; i < 30; i++) viz.update(tone.data(), (int)tone.size(), sampleRate, 1.0 / 60.0);
        bool anyNonZero = false;
        for (int i = 0; i < AudioVisualizer::BANDS; i++) if (viz.levels()[i] > 0.0f) anyNonZero = true;
        CHECK(anyNonZero);
        viz.reset();
        bool allZero = true;
        for (int i = 0; i < AudioVisualizer::BANDS; i++) if (viz.levels()[i] != 0.0f) allZero = false;
        CHECK(allZero);
    }

    // A wildly out-of-range dt (e.g. a stall) is clamped internally
    // rather than producing NaN/inf levels.
    {
        AudioVisualizer viz;
        auto tone = makeTone(1000.0, sampleRate, 1024);
        viz.update(tone.data(), (int)tone.size(), sampleRate, 9999.0);
        for (int i = 0; i < AudioVisualizer::BANDS; i++) {
            CHECK(std::isfinite(viz.levels()[i]));
        }
    }

    return check::finish("test_audio_visualizer");
}
