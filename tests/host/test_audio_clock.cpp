// AudioOutput against the fake SDL backend: resampling output size,
// paused-until-start behaviour, and the playback clock that video sync
// depends on.
#include "check.h"
#include "media/audio_output.h"

static AVFrame* makeAudioFrame(int samples, int rate) {
    AVFrame* f = av_frame_alloc();
    f->format = AV_SAMPLE_FMT_FLTP;
    f->channel_layout = AV_CH_LAYOUT_STEREO;
    f->channels = 2;
    f->sample_rate = rate;
    f->nb_samples = samples;
    av_frame_get_buffer(f, 0);
    av_samples_set_silence(f->data, 0, samples, 2, AV_SAMPLE_FMT_FLTP);
    return f;
}

int main() {
    const int outBytesPerSec = 48000 * 2 * 2;

    AudioOutput audio;
    CHECK(audio.init(48000, 2, AV_SAMPLE_FMT_FLTP));
    CHECK(fake_sdl_device_paused());
    CHECK(!audio.started());
    CHECK(!audio.hasClock());
    CHECK(std::isnan(audio.clockSeconds()));
    CHECK_EQ(audio.queuedBytes(), 0u);

    // Queue 1 second of audio starting at t=10.0 in 1024-sample frames.
    double pts = 10.0;
    int frames = 0;
    while (frames * 1024 < 48000) {
        AVFrame* f = makeAudioFrame(1024, 48000);
        audio.queueFrame(f, pts);
        av_frame_free(&f);
        pts += 1024.0 / 48000.0;
        frames++;
    }
    CHECK_EQ(audio.queuedBytes(), (uint32_t)(frames * 1024 * 4));
    CHECK_NEAR(audio.queuedSeconds(), frames * 1024.0 / 48000.0, 1e-6);

    // Not started: the clock sits at the first frame's time.
    CHECK(audio.hasClock());
    CHECK_NEAR(audio.clockSeconds(), 10.0, 1e-9);
    fake_sdl_advance_ticks(500);
    CHECK_NEAR(audio.clockSeconds(), 10.0, 1e-9);

    // Start: device unpaused, clock advances with wall time.
    audio.start();
    CHECK(audio.started());
    CHECK(!fake_sdl_device_paused());
    fake_sdl_advance_ticks(500);
    CHECK_NEAR(audio.clockSeconds(), 10.5, 0.02);

    // SDL has played 0.5 s; queueing more audio should agree with the
    // extrapolated clock (nudge, not snap) -- within a device buffer.
    fake_sdl_consume_audio(outBytesPerSec / 2);
    {
        AVFrame* f = makeAudioFrame(1024, 48000);
        audio.queueFrame(f, pts);
        av_frame_free(&f);
        pts += 1024.0 / 48000.0;
    }
    CHECK_NEAR(audio.clockSeconds(), 10.5, 0.1);

    // A stall: wall time runs 2 s but SDL played nothing more. The next
    // queued frame snaps the clock back to what is actually audible.
    fake_sdl_advance_ticks(2000);
    CHECK_NEAR(audio.clockSeconds(), 12.5, 0.1);
    {
        AVFrame* f = makeAudioFrame(1024, 48000);
        audio.queueFrame(f, pts);
        av_frame_free(&f);
        pts += 1024.0 / 48000.0;
    }
    CHECK_NEAR(audio.clockSeconds(), 10.5, 0.15);

    // Pause: device paused, clock frozen however long the pause lasts,
    // queueing more audio doesn't move it; resume carries on from there.
    {
        double before = audio.clockSeconds();
        audio.setPaused(true);
        CHECK(audio.paused());
        CHECK(fake_sdl_device_paused());
        fake_sdl_advance_ticks(3000);
        CHECK_NEAR(audio.clockSeconds(), before, 1e-6);
        AVFrame* f = makeAudioFrame(1024, 48000);
        audio.queueFrame(f, pts);
        av_frame_free(&f);
        pts += 1024.0 / 48000.0;
        CHECK_NEAR(audio.clockSeconds(), before, 1e-6);
        audio.setPaused(false);
        CHECK(!audio.paused());
        CHECK(!fake_sdl_device_paused());
        fake_sdl_advance_ticks(250);
        CHECK_NEAR(audio.clockSeconds(), before + 0.25, 0.01);
        audio.setPaused(false); // no-op
        CHECK_NEAR(audio.clockSeconds(), before + 0.25, 0.01);
    }

    // Volume scales the samples handed to SDL (menu music plays quieter).
    {
        AudioOutput loud;
        CHECK(loud.init(48000, 2, AV_SAMPLE_FMT_FLTP));
        auto tone = [](float value) {
            AVFrame* f = makeAudioFrame(256, 48000);
            for (int c = 0; c < 2; c++)
                for (int i = 0; i < 256; i++) ((float*)f->data[c])[i] = value;
            return f;
        };
        auto lastSample = []() {
            Uint32 n = 0;
            const unsigned char* d = fake_sdl_last_queued(&n);
            return n >= 4 ? (int)((const int16_t*)d)[(n / 2) - 1] : 0; // last sample (settled)
        };
        AVFrame* f = tone(0.5f);
        loud.queueFrame(f, 0.0);
        int full = lastSample();
        av_frame_free(&f);
        loud.setVolume(0.25f);
        f = tone(0.5f);
        loud.queueFrame(f, 0.1);
        int quiet = lastSample();
        av_frame_free(&f);
        CHECK(full > 15000);
        CHECK(quiet > full / 4 - 200 && quiet < full / 4 + 200);
        loud.setVolume(5.0f); // clamped to 1
        f = tone(0.5f);
        loud.queueFrame(f, 0.2);
        CHECK(std::abs(lastSample() - full) < 50);
        av_frame_free(&f);
        loud.shutdown();
    }

    // Resampling: a 44.1 kHz source produces ~48/44.1 as many output samples.
    {
        AudioOutput resampled;
        CHECK(resampled.init(44100, 1, AV_SAMPLE_FMT_S16));
        AVFrame* f = av_frame_alloc();
        f->format = AV_SAMPLE_FMT_S16;
        f->channel_layout = AV_CH_LAYOUT_MONO;
        f->channels = 1;
        f->sample_rate = 44100;
        f->nb_samples = 4410;
        av_frame_get_buffer(f, 0);
        av_samples_set_silence(f->data, 0, 4410, 1, AV_SAMPLE_FMT_S16);
        resampled.queueFrame(f, NAN);
        av_frame_free(&f);
        uint32_t expectedBytes = (uint32_t)(4410.0 * 48000.0 / 44100.0) * 4;
        CHECK(resampled.queuedBytes() > expectedBytes - 200 && resampled.queuedBytes() < expectedBytes + 200);
        CHECK(!resampled.hasClock()); // NAN pts never establishes a clock
        resampled.shutdown();
    }

    audio.shutdown();
    return check::finish("test_audio_clock");
}
