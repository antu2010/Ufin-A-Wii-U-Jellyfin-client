// Decoder -- wraps FFmpeg's demuxer + decoders behind a simple
// "give me the next decoded frame" interface. Knows nothing about SDL2,
// the network, or Jellyfin -- it just turns an already-open AVIOContext
// into a stream of decoded AVFrames. video_output.cpp / audio_output.cpp
// are the ones that know what to do with those frames.

#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <atomic>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

enum class DecodedFrameType {
    NONE,   // end of stream, nothing more to decode
    VIDEO,
    AUDIO,
};

class Decoder {
public:
    Decoder();
    ~Decoder();

    // avioCtx must already be open (see HttpStreamIO::open()) and stay
    // alive for as long as this Decoder is in use -- Decoder does not
    // own or free it.
    bool open(AVIOContext* avioCtx);

    // Diagnostic escape hatch for the h264_wiiu macroblock-alignment check
    // in openCodecForStream() -- see UfinConfig::allowUnalignedVideoGeometry
    // for the full explanation. Must be called before open(); has no effect
    // once open() has already run. Off by default.
    void setAllowUnalignedGeometry(bool allow) { allow_unaligned_geometry_ = allow; }

    // Audio-only mode, for a standalone audio file such as the .m4a/.mp3 menu music. Must be called
    // before open(). Ignores any video stream (embedded cover art shows up as one) and runs
    // avformat_find_stream_info(), which a bare MP3 needs to learn its sample rate and channels
    // (an .m4a has them in its moov box already, like the fragmented MP4 from Jellyfin). Never
    // used for real playback: find_stream_info is exactly what crashes h264_wiiu (see open()).
    void setAudioOnly(bool on) { audio_only_ = on; }

    // Decodes and returns the next available frame, whichever stream
    // (video or audio) it comes from first. The returned AVFrame* stays
    // owned by Decoder and is only valid until the next call to
    // decodeNextFrame() -- clone it (av_frame_clone) if it needs to
    // outlive that.
    DecodedFrameType decodeNextFrame(AVFrame** outFrame);

    // --- per-stream decoding (what Player uses) ---
    //
    // A live fragmented MP4 read front to back (it can't be seeked)
    // hands out packets in whole-fragment runs: every video packet of a
    // fragment (Jellyfin's are several seconds long), THEN every audio
    // packet for the same seconds. Decoding strictly in that order means
    // either buffering seconds of decoded 720p frames (far too much
    // memory) or starving audio while video waits for it -- which is
    // what made playback alternate between picture and sound.
    //
    // So demuxing and decoding are split: readPacket() pulls the next
    // packet off the network into a per-stream queue of *compressed*
    // packets (a few MB covers many seconds), and the two decode calls
    // consume only their own stream's queue. The caller decides which
    // stream needs a frame next.

    // Reads one packet into its stream's queue. Returns false at end of
    // stream (after which the decoders get flushed as they drain).
    bool readPacket();

    // Next decoded frame of that stream, or NONE if its packet queue is
    // empty (read more) or the stream is fully drained. Same ownership
    // rules as decodeNextFrame().
    DecodedFrameType decodeVideoFrame(AVFrame** outFrame);
    DecodedFrameType decodeAudioFrame(AVFrame** outFrame);

    // hasQueued*Packets() touch the packet deques directly, so they are for
    // the decode thread only (the one that also calls readPacket()).
    bool hasQueuedVideoPackets() const { return !video_packets_.empty(); }
    bool hasQueuedAudioPackets() const { return !audio_packets_.empty(); }
    // The four figures below (queuedPacketBytes, queuedVideoSeconds,
    // queuedAudioSeconds, demuxFinished) are safe to read from any thread:
    // the decode thread keeps its own exact accumulators and publishes them
    // after every change as 32-bit atomics (millisecond resolution for the
    // seconds). 32-bit on purpose -- the Wii U's Espresso has no lock-free
    // 8-byte atomics, and a plain double shared between threads is a data race
    // (Player's rebuffering decision reads these from the render thread).
    size_t queuedPacketBytes() const { return pub_packet_bytes_.load(std::memory_order_relaxed); }
    // Seconds of stream time currently sitting in each stream's compressed
    // packet queue, i.e. how far ahead of the decoder the network read
    // has gotten -- NOT how much has been decoded. This is what a
    // look-ahead/prebuffer target should be measured against: decoded
    // frames are far too large to buffer minutes of (see the comment on
    // per-stream decoding above), but compressed packets are cheap, so
    // "buffered ahead" means packets sitting here, waiting to be decoded
    // closer to when they're actually needed.
    double queuedVideoSeconds() const { return pub_video_ms_.load(std::memory_order_relaxed) / 1000.0; }
    double queuedAudioSeconds() const { return pub_audio_ms_.load(std::memory_order_relaxed) / 1000.0; }
    bool demuxFinished() const { return reachedEof_.load(std::memory_order_acquire); }
    // False only when plenty of video packets have been read and none of them
    // carried any usable timing (neither a duration nor a decode timestamp),
    // i.e. queuedVideoSeconds() can never mean anything for this stream.
    // Player then leaves rebuffering off instead of reading a permanent
    // "0 seconds buffered". True when there is not enough evidence yet (too
    // few packets read) or timing is present.
    bool videoTimelineUsable() const {
        return video_timed_packets_.load(std::memory_order_relaxed) > 0 ||
               video_untimed_packets_.load(std::memory_order_relaxed) < 30;
    }
    // True once the demuxer hit the end and every opened decoder has
    // given back its last frame.
    bool finished() const;

    // Presentation time of a frame just returned by decodeNextFrame(),
    // in seconds of stream time (i.e. comparable between the audio and
    // video streams). NAN if the container gave us no usable timestamp.
    double frameTimeSeconds(DecodedFrameType type, const AVFrame* frame) const;

    void close();

    // Video stream info (only meaningful once open() succeeds and a
    // video stream was found).
    bool hasVideo() const { return video_stream_index_ >= 0; }
    int videoWidth() const { return video_ctx_ ? video_ctx_->width : 0; }
    int videoHeight() const { return video_ctx_ ? video_ctx_->height : 0; }
    AVRational videoTimeBase() const;
    // Nominal seconds per frame from the container's frame rate, or 0 if
    // unknown. Used as a fallback pacing interval when frames arrive
    // without timestamps.
    double videoFrameDuration() const;
    const char* videoCodecName() const;
    // H.264 profile_idc from the stream's avcC box (66 Baseline, 77 Main, 100 High...),
    // 0 if unknown. Only Baseline is known to be free of B-frames.
    int videoProfileIdc() const { return video_profile_idc_; }
    // True unless the stream is known to be Baseline: its pictures may come out of the
    // hardware decoder in decode order and need putting back in display order (FrameReorder).
    bool videoNeedsReorder() const { return video_profile_idc_ != 66; }

    // Audio stream info (only meaningful once open() succeeds and an
    // audio stream was found).
    bool hasAudio() const { return audio_stream_index_ >= 0; }
    int audioSampleRate() const { return audio_ctx_ ? audio_ctx_->sample_rate : 0; }
    int audioChannels() const;
    AVSampleFormat audioSampleFormat() const { return audio_ctx_ ? audio_ctx_->sample_fmt : AV_SAMPLE_FMT_NONE; }

    const char* lastError() const { return last_error_; }

    // Circuit breaker for a persistently failing hardware video decoder
    // (see the long comment on consecutive_video_decode_errors_ below).
    // Once this is true it never goes back to false for this Decoder
    // instance -- a fresh Decoder (i.e. a fresh Player::play() call,
    // such as after a seek) starts clean. Safe to read from another
    // thread: consecutive_video_decode_errors_ is decode-thread-only,
    // but this flag is the one piece of that state Player's own thread
    // needs to observe, so it alone is atomic.
    bool fatalDecodeError() const { return fatal_decode_error_.load(std::memory_order_acquire); }

private:
    AVFormatContext* fmt_ctx_ = nullptr;

    int video_profile_idc_ = 0; // see videoProfileIdc()
    int video_stream_index_ = -1;
    int audio_stream_index_ = -1;
    AVCodecContext* video_ctx_ = nullptr;
    AVCodecContext* audio_ctx_ = nullptr;

    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;

    std::atomic<bool> reachedEof_{false}; // set by the decode thread, read by Player's
    bool video_drained_ = false;
    bool audio_drained_ = false;
    bool video_flushed_ = false;
    bool audio_flushed_ = false;
    std::deque<AVPacket*> video_packets_;
    std::deque<AVPacket*> audio_packets_;
    // Decode-thread-owned accumulators (exact). Other threads read the
    // published copies below via the accessors above.
    size_t queued_packet_bytes_ = 0;
    double queued_video_seconds_ = 0.0;
    double queued_audio_seconds_ = 0.0;
    std::atomic<int32_t> pub_video_ms_{0};
    std::atomic<int32_t> pub_audio_ms_{0};
    std::atomic<uint32_t> pub_packet_bytes_{0};
    std::atomic<uint32_t> video_timed_packets_{0};   // video packets read with any usable timing
    std::atomic<uint32_t> video_untimed_packets_{0}; // ...and with none

    // Timeline span of each queue from packet timestamps: oldest and newest
    // queued packet's DTS (stream time base units) plus the newest packet's
    // duration when it has one. Needed because this FFmpeg's mov demuxer often
    // leaves AVPacket::duration at 0 on a live fragmented stream (it derives
    // it from the *next* index entry, which has not been read yet), which
    // would make the duration sum above read 0 forever. Timestamps are always
    // there. Decode-thread-owned like the accumulators.
    struct QueueSpan {
        int64_t front = AV_NOPTS_VALUE;
        int64_t back = AV_NOPTS_VALUE;
        int64_t backDuration = 0;
    };
    QueueSpan video_span_, audio_span_;
    static int64_t packetTs(const AVPacket* pkt) { return pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts; }
    double spanSeconds(const QueueSpan& span, int streamIndex) const;
    void publishQueueStats();

    bool decodeFrom(AVCodecContext* ctx, std::deque<AVPacket*>& queue, bool& flushed,
                    bool& drained, bool isVideo, AVFrame** outFrame);
    int video_decode_errors_ = 0; // total, for logging -- never reset except by close()

    // Consecutive-failure circuit breaker: unlike video_decode_errors_
    // above (a running total, purely informational), this counts only
    // an unbroken streak of hardware decode errors and is reset to 0 by
    // every successfully decoded video frame -- see decodeFrom(). A few
    // isolated errors in an otherwise-healthy stream are expected and
    // harmless (a dropped/corrupt packet, a brief hardware hiccup) and
    // must not end playback; what must end playback is the hardware
    // decoder never recovering. Only decodeFrom() (decode thread) ever
    // touches this counter, so it doesn't need to be atomic itself --
    // only the fatal_decode_error_ flag it sets does, for Player's
    // thread to read.
    static const int MAX_CONSECUTIVE_VIDEO_DECODE_ERRORS = 30;
    int consecutive_video_decode_errors_ = 0;
    std::atomic<bool> fatal_decode_error_{false};

    char last_error_[256] = {0};
    bool allow_unaligned_geometry_ = false;
    bool audio_only_ = false; // see setAudioOnly()

    bool openCodecForStream(int streamIndex, AVCodecContext** outCtx);
};
