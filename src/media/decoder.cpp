#include "decoder.h"
#include "../ufin_log.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <coreinit/debug.h>

Decoder::Decoder() {
    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
}

Decoder::~Decoder() {
    close();
    av_packet_free(&packet_);
    av_frame_free(&frame_);
}

bool Decoder::openCodecForStream(int streamIndex, AVCodecContext** outCtx) {
    AVStream* stream = fmt_ctx_->streams[streamIndex];
    AVCodecParameters* params = stream->codecpar;

    // For H.264, ask for the Wii U hardware decoder (h264_wiiu, from the
    // FFmpeg-wiiu fork) by name rather than trusting whatever
    // avcodec_find_decoder() happens to return first -- if the FFmpeg
    // build ever also has the generic software h264 decoder enabled, we
    // still want the hardware one (this matches how CafeMP picks it).
    // Falls back to the generic lookup for everything else (AAC, or an
    // H.264 build without the hardware decoder).
    const AVCodec* codec = nullptr;
    if (params->codec_id == AV_CODEC_ID_H264) {
        codec = avcodec_find_decoder_by_name("h264_wiiu");
    }
    if (!codec) {
        codec = avcodec_find_decoder(params->codec_id);
    }
    if (!codec) {
        snprintf(last_error_, sizeof(last_error_),
                 "no decoder registered for codec id %d", params->codec_id);
        return false;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        snprintf(last_error_, sizeof(last_error_), "avcodec_alloc_context3 failed");
        return false;
    }

    if (avcodec_parameters_to_context(ctx, params) < 0) {
        snprintf(last_error_, sizeof(last_error_), "avcodec_parameters_to_context failed");
        avcodec_free_context(&ctx);
        return false;
    }

    // Lets the decoder stamp output frames with timestamps in the
    // stream's own time base, which is what frameTimeSeconds() assumes.
    ctx->pkt_timebase = stream->time_base;

    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        snprintf(last_error_, sizeof(last_error_), "avcodec_open2 failed for %s (codec id %d)",
                 codec->name, params->codec_id);
        avcodec_free_context(&ctx);
        return false;
    }

    OSReport("Ufin: opened decoder %s for stream %d (%dx%d, tb=%d/%d)\n", codec->name,
             streamIndex, params->width, params->height, stream->time_base.num,
             stream->time_base.den);

    *outCtx = ctx;
    return true;
}

bool Decoder::open(AVIOContext* avioCtx) {
    fmt_ctx_ = avformat_alloc_context();
    if (!fmt_ctx_) {
        snprintf(last_error_, sizeof(last_error_), "avformat_alloc_context failed");
        return false;
    }

    fmt_ctx_->pb = avioCtx;
    fmt_ctx_->flags |= AVFMT_FLAG_CUSTOM_IO;

    // Bound how much of the live stream avformat_open_input() may read
    // while probing the container format. A 2.5 Mbit/s transcode only
    // produces bytes in real time, so a small limit keeps the black
    // screen at start short.
    fmt_ctx_->probesize = 2 * 1024 * 1024;
    fmt_ctx_->max_analyze_duration = AV_TIME_BASE;

    // The filename argument is only used by FFmpeg for format-guessing
    // hints when no custom IO is attached -- since pb is already set,
    // a non-null placeholder is enough; format detection here happens
    // by probing the actual bytes via avioCtx.
    int ret = avformat_open_input(&fmt_ctx_, "stream", nullptr, nullptr);
    if (ret < 0) {
        snprintf(last_error_, sizeof(last_error_), "avformat_open_input failed (%d)", ret);
        OSReport("Ufin: avformat_open_input failed (%d)\n", ret);
        return false;
    }
    OSReport("Ufin: avformat_open_input ok (format=%s)\n", fmt_ctx_->iformat->name);

    // NOT calling avformat_find_stream_info(): for H.264 it opens its own
    // throwaway decoder (h264_wiiu, the only H.264 decoder in our build)
    // and feeds it packets to guess the reorder delay -- and h264_wiiu
    // crashes in that half-initialised probe context. The mov demuxer
    // already fills in codec id, size, sample rate, channels and the
    // avcC/esds extradata from the moov box during avformat_open_input,
    // which is everything our own decoders need.
    for (unsigned i = 0; i < fmt_ctx_->nb_streams; i++) {
        AVCodecParameters* p = fmt_ctx_->streams[i]->codecpar;
        OSReport("Ufin: stream %u codec=%d %dx%d rate=%d extradata=%d\n", i, p->codec_id,
                 p->width, p->height, p->sample_rate, p->extradata_size);
    }

    video_stream_index_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audio_stream_index_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    OSReport("Ufin: stream detection -- video_index=%d audio_index=%d (nb_streams=%d)\n",
             video_stream_index_, audio_stream_index_, fmt_ctx_->nb_streams);

    if (video_stream_index_ < 0 && audio_stream_index_ < 0) {
        snprintf(last_error_, sizeof(last_error_), "no video or audio stream found");
        return false;
    }

    if (video_stream_index_ >= 0) {
        if (!openCodecForStream(video_stream_index_, &video_ctx_)) {
            return false; // last_error_ already set by openCodecForStream
        }
    }
    if (audio_stream_index_ >= 0) {
        if (!openCodecForStream(audio_stream_index_, &audio_ctx_)) {
            return false;
        }
    }

    return true;
}

bool Decoder::readPacket() {
    if (reachedEof_ || !fmt_ctx_) return false;
    while (true) {
        int readRet = av_read_frame(fmt_ctx_, packet_);
        if (readRet < 0) {
            // End of stream (or a network error we can't distinguish
            // from EOF here). Decoders are flushed once their queues
            // drain, so buffered packets still get decoded.
            OSReport("Ufin: av_read_frame returned %d -- treating as end of stream\n", readRet);
            reachedEof_ = true;
            return false;
        }
        std::deque<AVPacket*>* queue = nullptr;
        if (packet_->stream_index == video_stream_index_ && video_ctx_) queue = &video_packets_;
        else if (packet_->stream_index == audio_stream_index_ && audio_ctx_) queue = &audio_packets_;
        if (!queue) {
            // e.g. a subtitle track we never opened a codec for
            av_packet_unref(packet_);
            continue;
        }
        AVPacket* copy = av_packet_alloc();
        if (!copy) {
            av_packet_unref(packet_);
            continue;
        }
        av_packet_move_ref(copy, packet_);
        queued_packet_bytes_ += (size_t)copy->size;
        queue->push_back(copy);
        return true;
    }
}

bool Decoder::decodeFrom(AVCodecContext* ctx, std::deque<AVPacket*>& queue, bool& flushed,
                         bool& drained, bool isVideo, AVFrame** outFrame) {
    if (!ctx || drained) return false;
    for (int guard = 0; guard < 64; guard++) {
        int ret = avcodec_receive_frame(ctx, frame_);
        if (ret == 0) {
            *outFrame = frame_;
            return true;
        }
        if (ret == AVERROR_EOF) {
            drained = true;
            return false;
        }
        if (ret != AVERROR(EAGAIN) && isVideo) {
            // A genuine decode error for one packet (h264_wiiu reports
            // hardware decoder failures this way); the packet is already
            // consumed, so log it and keep feeding.
            video_decode_errors_++;
            if (video_decode_errors_ <= 5 || video_decode_errors_ % 100 == 0) {
                OSReport("Ufin: video decode error %d (count=%d)\n", ret, video_decode_errors_);
            }
        }

        if (!queue.empty()) {
            AVPacket* pkt = queue.front();
            int sendRet = avcodec_send_packet(ctx, pkt);
            if (sendRet == AVERROR(EAGAIN)) {
                // Wants output drained first, but just said it has none:
                // try again on the next call rather than spin.
                return false;
            }
            // Accepted -- or rejected outright, in which case the packet
            // is unusable and dropping it is the only option.
            queued_packet_bytes_ -= (size_t)pkt->size;
            av_packet_free(&pkt);
            queue.pop_front();
            continue;
        }
        if (reachedEof_ && !flushed) {
            avcodec_send_packet(ctx, nullptr); // flush: push out held frames
            flushed = true;
            continue;
        }
        if (flushed) {
            // Flushed, nothing left to feed, and still no frame: treat as
            // drained even if the decoder never says AVERROR_EOF, so the
            // end of the stream is always reached.
            drained = true;
        }
        return false; // needs more packets (or done)
    }
    return false;
}

DecodedFrameType Decoder::decodeVideoFrame(AVFrame** outFrame) {
    return decodeFrom(video_ctx_, video_packets_, video_flushed_, video_drained_, true, outFrame)
        ? DecodedFrameType::VIDEO : DecodedFrameType::NONE;
}

DecodedFrameType Decoder::decodeAudioFrame(AVFrame** outFrame) {
    return decodeFrom(audio_ctx_, audio_packets_, audio_flushed_, audio_drained_, false, outFrame)
        ? DecodedFrameType::AUDIO : DecodedFrameType::NONE;
}

bool Decoder::finished() const {
    return reachedEof_ && (!video_ctx_ || video_drained_) && (!audio_ctx_ || audio_drained_);
}

DecodedFrameType Decoder::decodeNextFrame(AVFrame** outFrame) {
    // Stream-order convenience wrapper over the per-stream calls: returns
    // whatever frame is ready, reading packets as needed.
    while (true) {
        if (decodeVideoFrame(outFrame) == DecodedFrameType::VIDEO) return DecodedFrameType::VIDEO;
        if (decodeAudioFrame(outFrame) == DecodedFrameType::AUDIO) return DecodedFrameType::AUDIO;
        if (finished()) return DecodedFrameType::NONE;
        if (!reachedEof_) {
            readPacket();
            continue;
        }
        // At end of stream with nothing left to feed: both decoders have
        // been flushed by now, so if neither produced a frame we're done
        // (guards against a decoder that never reports EOF after flush).
        if (video_packets_.empty() && audio_packets_.empty() &&
            (!video_ctx_ || video_flushed_) && (!audio_ctx_ || audio_flushed_)) {
            return DecodedFrameType::NONE;
        }
    }
}

double Decoder::frameTimeSeconds(DecodedFrameType type, const AVFrame* frame) const {
    if (!frame || !fmt_ctx_) return NAN;

    int streamIndex = -1;
    if (type == DecodedFrameType::VIDEO) streamIndex = video_stream_index_;
    else if (type == DecodedFrameType::AUDIO) streamIndex = audio_stream_index_;
    if (streamIndex < 0) return NAN;

    // best_effort_timestamp is FFmpeg's reconciled pts/dts guess -- for a
    // clean transcode it simply equals pts, but it also copes with the
    // occasional missing pts without us having to.
    int64_t ts = frame->best_effort_timestamp;
    if (ts == AV_NOPTS_VALUE) ts = frame->pts;
    if (ts == AV_NOPTS_VALUE) return NAN;

    return (double)ts * av_q2d(fmt_ctx_->streams[streamIndex]->time_base);
}

AVRational Decoder::videoTimeBase() const {
    if (video_stream_index_ >= 0 && fmt_ctx_) {
        return fmt_ctx_->streams[video_stream_index_]->time_base;
    }
    return AVRational{1, 1};
}

double Decoder::videoFrameDuration() const {
    if (video_stream_index_ < 0 || !fmt_ctx_) return 0.0;
    AVRational fr = av_guess_frame_rate(fmt_ctx_, fmt_ctx_->streams[video_stream_index_], nullptr);
    if (fr.num <= 0 || fr.den <= 0) return 0.0;
    return (double)fr.den / (double)fr.num;
}

const char* Decoder::videoCodecName() const {
    return (video_ctx_ && video_ctx_->codec) ? video_ctx_->codec->name : "none";
}

int Decoder::audioChannels() const {
    // NOTE: using the classic `channels` field rather than the newer
    // AVChannelLayout (ch_layout) API -- the FFmpeg-wiiu fork is based
    // on FFmpeg 4.3 (libavcodec 58), where ch_layout doesn't exist yet.
    // If a future FFmpeg-wiiu update pulls in FFmpeg 5.1+ this is the
    // first thing to change (see also swr_alloc_set_opts in
    // audio_output.cpp).
    return audio_ctx_ ? audio_ctx_->channels : 0;
}

void Decoder::close() {
    if (packet_) av_packet_unref(packet_);
    for (AVPacket* p : video_packets_) av_packet_free(&p);
    for (AVPacket* p : audio_packets_) av_packet_free(&p);
    video_packets_.clear();
    audio_packets_.clear();
    queued_packet_bytes_ = 0;
    video_drained_ = audio_drained_ = false;
    video_flushed_ = audio_flushed_ = false;
    if (video_ctx_) avcodec_free_context(&video_ctx_);
    if (audio_ctx_) avcodec_free_context(&audio_ctx_);
    if (fmt_ctx_) {
        // Do NOT let this free our AVIOContext -- we don't own it
        // (HttpStreamIO does), and fmt_ctx_->pb was assigned rather
        // than created by avformat_open_input. Clear pb first so
        // avformat_close_input's internal cleanup leaves it alone.
        fmt_ctx_->pb = nullptr;
        avformat_close_input(&fmt_ctx_);
    }
    video_stream_index_ = -1;
    audio_stream_index_ = -1;
    reachedEof_ = false;
    video_decode_errors_ = 0;
}
