#include "audio_visualizer.h"
#include <cmath>
#include <algorithm>

namespace media {

namespace {

// The UI has twelve bars, so analyse twelve useful logarithmic regions rather
// than twelve individual frequencies. A real song has energy spread across
// many nearby FFT bins; summing each region makes the bars respond to actual
// music instead of only moving when a signal happens to land on one exact bin.
const double LOW_HZ = 35.0;
const double HIGH_HZ = 10000.0;
const int FFT_SIZE = 1024;
const double TWO_PI = 2.0 * M_PI;

// Convert the FFT bin magnitude into an approximate full-scale sine amplitude.
// With a Hann window the coherent gain is ~0.5, while the positive-frequency
// FFT amplitude needs the usual 2/N normalization.
inline double binAmplitude(double re, double im, int count) {
    return (2.0 * std::sqrt(re * re + im * im)) / (double)count / 0.5;
}

// In-place radix-2 FFT. This is intentionally small/fixed-cost: 1024 points
// is enough resolution for a music visualizer, and avoids bringing another
// dependency into the Wii U build.
void fft(float* re, float* im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }

    for (int len = 2; len <= n; len <<= 1) {
        const double angle = -TWO_PI / (double)len;
        const float wLenRe = (float)std::cos(angle);
        const float wLenIm = (float)std::sin(angle);
        for (int i = 0; i < n; i += len) {
            float wRe = 1.0f;
            float wIm = 0.0f;
            const int half = len >> 1;
            for (int j = 0; j < half; j++) {
                const int u = i + j;
                const int v = u + half;
                const float vRe = re[v] * wRe - im[v] * wIm;
                const float vIm = re[v] * wIm + im[v] * wRe;
                const float uRe = re[u];
                const float uIm = im[u];
                re[u] = uRe + vRe;
                im[u] = uIm + vIm;
                re[v] = uRe - vRe;
                im[v] = uIm - vIm;

                const float nextWRe = wRe * wLenRe - wIm * wLenIm;
                wIm = wRe * wLenIm + wIm * wLenRe;
                wRe = nextWRe;
            }
        }
    }
}

} // namespace

void AudioVisualizer::update(const int16_t* samples, int count, int sampleRate, double dt) {
    if (dt < 0.0 || dt > 1.0) dt = 1.0 / 60.0;

    // The visualizer has two motion layers: a relatively stable spectral
    // baseline to suppress FFT chatter, and a transient envelope that only
    // reacts when a band jumps meaningfully above that baseline. This gives
    // the classic fast-up / slow-down motion without making every tiny
    // sample-to-sample fluctuation look like a beat.
    const double targetAttackTau = 0.070;
    const double targetDecayTau = 0.24;
    const double displayAttackTau = 0.020;
    const double displayDecayTau = 0.52;
    const double transientDecayTau = 0.26;
    const double transientThreshold = 0.08;

    // We need a complete power-of-two window for the FFT. AudioOutput hands
    // us 1024 samples already, but keep this defensive for callers/tests with
    // shorter buffers.
    if (!samples || count <= 0 || sampleRate <= 0) {
        for (int i = 0; i < BANDS; i++) {
            const double decayAlpha = 1.0 - std::exp(-dt / displayDecayTau);
            const double peakAlpha = 1.0 - std::exp(-dt / transientDecayTau);
            levels_[i] = (float)(levels_[i] * (1.0 - decayAlpha));
            smoothedTargets_[i] = (float)(smoothedTargets_[i] * (1.0 - decayAlpha));
            peakEnvelope_[i] = (float)(peakEnvelope_[i] * (1.0 - peakAlpha));
            if (levels_[i] < 0.0001f) levels_[i] = 0.0f;
            if (smoothedTargets_[i] < 0.0001f) smoothedTargets_[i] = 0.0f;
            if (peakEnvelope_[i] < 0.0001f) peakEnvelope_[i] = 0.0f;
        }
        return;
    }

    const int n = std::min(count, FFT_SIZE);
    float re[FFT_SIZE] = {};
    float im[FFT_SIZE] = {};

    // Center the latest samples in the FFT window when fewer than 1024 were
    // supplied. This also means a partial window is padded with zeros rather
    // than reading outside the caller's buffer.
    const int start = count - n;
    for (int i = 0; i < n; i++) {
        // Hann window: strongly reduces leakage between neighbouring bins,
        // which is particularly important when we sum broad music bands.
        const double w = 0.5 - 0.5 * std::cos(TWO_PI * i / (double)std::max(1, n - 1));
        re[i] = (float)(((double)samples[start + i] / 32768.0) * w);
    }

    // Zero pad any short input to the fixed FFT size. (For the normal player
    // path n == FFT_SIZE, so this costs nothing beyond the already-zeroed
    // stack arrays.)
    fft(re, im, FFT_SIZE);

    const double binHz = (double)sampleRate / (double)FFT_SIZE;
    const double logLow = std::log(LOW_HZ);
    const double logHigh = std::log(HIGH_HZ);

    for (int band = 0; band < BANDS; band++) {
        const double t0 = (double)band / (double)BANDS;
        const double t1 = (double)(band + 1) / (double)BANDS;
        const double f0 = std::exp(logLow + (logHigh - logLow) * t0);
        const double f1 = std::exp(logLow + (logHigh - logLow) * t1);

        int firstBin = (int)std::ceil(f0 / binHz);
        int lastBin = (int)std::floor(f1 / binHz);
        firstBin = std::max(1, firstBin);
        lastBin = std::min(FFT_SIZE / 2 - 1, lastBin);

        double energy = 0.0;
        int bins = 0;
        for (int k = firstBin; k <= lastBin; k++) {
            const double amp = binAmplitude(re[k], im[k], FFT_SIZE);
            energy += amp * amp;
            bins++;
        }

        double rms = bins > 0 ? std::sqrt(energy / (double)bins) : 0.0;

        // Express each band's energy in dB relative to digital full scale.
        // This gives useful movement over normal music's very large dynamic
        // range without needing the user to crank the source volume.
        double db = 20.0 * std::log10(std::max(rms, 1.0e-6));
        // Keep a wider dynamic range so ordinary music sits in the middle of
        // the bars instead of living near the ceiling all the time. The
        // exponent further reserves headroom for loud transients.
        const double floorDb = -60.0;
        const double ceilingDb = -4.0;
        double target = (db - floorDb) / (ceilingDb - floorDb);
        target = std::max(0.0, std::min(1.0, target));
        target = std::pow(target, 1.35);

        // Give the very lowest bands a little extra punch: kick/bass energy is
        // otherwise spread over fewer FFT bins than the wider upper bands.
        const double bandT = (double)band / (double)(BANDS - 1);
        target *= 1.12 - 0.12 * bandT;
        target = std::min(1.0, target);

        // Smooth the normal spectral level. It follows genuine movement, but
        // filters out frame-to-frame FFT noise so steady notes look steady.
        {
            const double tau = (target > smoothedTargets_[band])
                ? targetAttackTau : targetDecayTau;
            const double alpha = 1.0 - std::exp(-dt / tau);
            smoothedTargets_[band] = (float)(smoothedTargets_[band] +
                (target - smoothedTargets_[band]) * alpha);
        }

        // Capture only genuine onset energy above the local baseline. A
        // noise gate here is important: without it, every small FFT wobble
        // becomes a new peak and the bars look jittery. When a real transient
        // arrives, the excess is held briefly so a kick/drop can punch high.
        {
            const double excess = std::max(
                0.0, target - (double)smoothedTargets_[band] - transientThreshold
            );
            const double alpha = 1.0 - std::exp(-dt / transientDecayTau);
            if (excess > (double)peakEnvelope_[band]) {
                peakEnvelope_[band] = (float)excess;
            } else {
                peakEnvelope_[band] = (float)(peakEnvelope_[band] * (1.0 - alpha));
            }
        }

        // Add the transient excess on top of the stable spectral baseline.
        // The multiplier makes beat drops visibly punchy without allowing a
        // single noisy FFT bin to peg the bar at full height.
        double displayTarget = (double)smoothedTargets_[band] +
                               (double)peakEnvelope_[band] * 1.70;
        displayTarget = std::max(0.0, std::min(1.0, displayTarget));

        const double tau = (displayTarget > levels_[band])
            ? displayAttackTau : displayDecayTau;
        const double alpha = 1.0 - std::exp(-dt / tau);
        levels_[band] = (float)(levels_[band] +
            (displayTarget - levels_[band]) * alpha);
        if (levels_[band] < 0.0f) levels_[band] = 0.0f;
        if (levels_[band] > 1.0f) levels_[band] = 1.0f;
    }
}

void AudioVisualizer::reset() {
    for (int i = 0; i < BANDS; i++) {
        levels_[i] = 0.0f;
        smoothedTargets_[i] = 0.0f;
        peakEnvelope_[i] = 0.0f;
    }
}

} // namespace media
