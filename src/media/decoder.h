// Decoder -- wraps FFmpeg's demuxer + decoders behind a simple
// "give me the next decoded frame" interface. Knows nothing about SDL2,
// the network, or Jellyfin -- it just turns an already-open AVIOContext
// into a stream of decoded AVFrames. video_output.cpp / audio_output.cpp
// are the ones that know what to do with those frames.

#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>

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

    bool hasQueuedVideoPackets() const { return !video_packets_.empty(); }
    bool hasQueuedAudioPackets() const { return !audio_packets_.empty(); }
    size_t queuedPacketBytes() const { return queued_packet_bytes_; }
    // Seconds of stream time currently sitting in each stream's compressed
    // packet queue, i.e. how far ahead of the decoder the network read
    // has gotten -- NOT how much has been decoded. This is what a
    // look-ahead/prebuffer target should be measured against: decoded
    // frames are far too large to buffer minutes of (see the comment on
    // per-stream decoding above), but compressed packets are cheap, so
    // "buffered ahead" means packets sitting here, waiting to be decoded
    // closer to when they're actually needed.
    double queuedVideoSeconds() const { return queued_video_seconds_; }
    double queuedAudioSeconds() const { return queued_audio_seconds_; }
    bool demuxFinished() const { return reachedEof_; }
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

    // Audio stream info (only meaningful once open() succeeds and an
    // audio stream was found).
    bool hasAudio() const { return audio_stream_index_ >= 0; }
    int audioSampleRate() const { return audio_ctx_ ? audio_ctx_->sample_rate : 0; }
    int audioChannels() const;
    AVSampleFormat audioSampleFormat() const { return audio_ctx_ ? audio_ctx_->sample_fmt : AV_SAMPLE_FMT_NONE; }

    const char* lastError() const { return last_error_; }

private:
    AVFormatContext* fmt_ctx_ = nullptr;

    int video_stream_index_ = -1;
    int audio_stream_index_ = -1;
    AVCodecContext* video_ctx_ = nullptr;
    AVCodecContext* audio_ctx_ = nullptr;

    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;

    bool reachedEof_ = false;
    bool video_drained_ = false;
    bool audio_drained_ = false;
    bool video_flushed_ = false;
    bool audio_flushed_ = false;
    std::deque<AVPacket*> video_packets_;
    std::deque<AVPacket*> audio_packets_;
    size_t queued_packet_bytes_ = 0;
    double queued_video_seconds_ = 0.0;
    double queued_audio_seconds_ = 0.0;

    bool decodeFrom(AVCodecContext* ctx, std::deque<AVPacket*>& queue, bool& flushed,
                    bool& drained, bool isVideo, AVFrame** outFrame);
    int video_decode_errors_ = 0;
    char last_error_[256] = {0};

    bool openCodecForStream(int streamIndex, AVCodecContext** outCtx);
};
