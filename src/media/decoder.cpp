#include "decoder.h"
#include "../h264_profile.h"
#include "../h264_diag.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <coreinit/debug.h>
#include "../ufin_log.h" // OSReport -> also the SD-card log file (see ufin_log.h)

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

    // Safety gate for the Wii U hardware decoder specifically (see the
    // long comment on Width=1280&Height=720 in
    // JellyfinClient::buildVideoStreamUrl): h264_wiiu's framebuffer
    // allocation is width*height*1.5 with NO internal rounding, while the
    // hardware itself writes rows at a 256-pixel-aligned pitch and a
    // 16-row-aligned height. Ufin always *asks* Jellyfin for exactly
    // 1280x720 -- both already aligned -- but Jellyfin does not always
    // honor that: depending on server-side scaling/policy behavior, the
    // stream that actually arrives can have a different, non-aligned
    // geometry (e.g. 1280x692 observed from a source that Jellyfin scaled
    // by preserving aspect ratio instead of squeezing to the exact
    // request). If that ever reaches avcodec_open2() here, the hardware
    // decoder's under-sized allocation vs. the pitch/height it actually
    // writes is exactly the kind of mismatch that can write past the end
    // of its buffer -- which on real hardware doesn't raise a catchable
    // error, it corrupts memory or crashes the console outright.
    //
    // So: refuse before ever touching the hardware decoder if the stream
    // isn't aligned the way it assumes, and fail cleanly through the
    // existing Decoder::open() -> Player::play() -> PlayResult::Error
    // path instead. This is deliberately specific to h264_wiiu (by name)
    // -- a software H.264 fallback has no such alignment requirement, and
    // this must never turn into a blanket "reject anything but 1280x720"
    // rule (see repository investigation notes): only the alignment the
    // hardware actually needs is enforced here.
    int hwHeight = 0; // see h264diag::hardwareHeightOverride; 0 for every stream that works today
    if (strcmp(codec->name, "h264_wiiu") == 0) {
        hwHeight = h264diag::hardwareHeightOverride(
            h264diag::spsGeometry(params->extradata, params->extradata_size > 0 ? (size_t)params->extradata_size : 0),
            params->width, params->height);
        if (hwHeight > 0) {
            // The container says %dx%d, but the SPS codes the picture taller and crops it
            // (a burned-in-subtitle transcode did this: 832 rows, 28 cropped). The hardware
            // writes the coded picture and h264_wiiu places chroma after align16(height)
            // rows, so it has to be told the coded height -- the extra rows are cropped
            // off again when each frame comes out (decodeFrom).
            OSReport("Ufin: stream is coded %dx%d for a visible %dx%d -- giving h264_wiiu the coded height\n",
                     params->width, hwHeight, params->width, params->height);
        }
        const int kPitchAlign = 256;
        const int kHeightAlign = 16;
        bool unsafe = (params->width <= 0 || params->height <= 0 ||
                       (params->width % kPitchAlign) != 0 || ((hwHeight > 0 ? hwHeight : params->height) % kHeightAlign) != 0);
        if (unsafe && !allow_unaligned_geometry_) {
            snprintf(last_error_, sizeof(last_error_),
                     "unsafe geometry for h264_wiiu: %dx%d (needs width a multiple of %d, "
                     "height a multiple of %d) -- refusing to open the hardware decoder",
                     params->width, params->height, kPitchAlign, kHeightAlign);
            OSReport("Ufin: %s\n", last_error_);
            return false;
        }
        if (unsafe) {
            // allow_unaligned_geometry_ set (UfinConfig::allowUnalignedVideoGeometry):
            // proceeding on the assumption this FFmpeg-wiiu build already has the
            // matching aligned-framebuffer fix in h264_wiiu.c. If it doesn't, this
            // is exactly the crash the check above exists to prevent -- only
            // enable that config setting after confirming that fix is actually
            // in the FFmpeg-wiiu build this binary links against.
            OSReport("Ufin: opening h264_wiiu with unaligned geometry %dx%d "
                      "(allow_unaligned_video_geometry is set)\n",
                      params->width, params->height);
        }
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

    if (hwHeight > 0) {
        ctx->height = hwHeight;
        ctx->coded_height = hwHeight;
        video_hw_height_ = hwHeight;
        video_visible_height_ = params->height;
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

    if (audio_only_) {
        // A bare audio file (MP3) has no moov box: ask the demuxer/decoder for the stream
        // parameters. Safe here -- no H.264 involved (see the note just below).
        int probe = avformat_find_stream_info(fmt_ctx_, nullptr);
        if (probe < 0) OSReport("Ufin: avformat_find_stream_info failed (%d)\n", probe);
    }

    // NOT calling avformat_find_stream_info() otherwise: for H.264 it opens its own
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

    video_stream_index_ = audio_only_ ? -1 : av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audio_stream_index_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    OSReport("Ufin: stream detection -- video_index=%d audio_index=%d (nb_streams=%d)\n",
             video_stream_index_, audio_stream_index_, fmt_ctx_->nb_streams);

    if (video_stream_index_ < 0 && audio_stream_index_ < 0) {
        snprintf(last_error_, sizeof(last_error_), "no video or audio stream found");
        return false;
    }

    if (video_stream_index_ >= 0) {
        // What the server really produced, read from the avcC box (byte 1 = profile_idc, byte 3 =
        // level_idc). codecpar->profile is not filled by the mov demuxer without find_stream_info.
        const AVCodecParameters* vp = fmt_ctx_->streams[video_stream_index_]->codecpar;
        video_profile_idc_ = (vp->extradata_size >= 4 && vp->extradata[0] == 1) ? vp->extradata[1] : 0;
        OSReport("Ufin: stream h264 profile=%s (idc %d) level_idc=%d -- %s\n",
                 h264::nameForProfileIdc(video_profile_idc_),
                 video_profile_idc_, (vp->extradata_size >= 4 && vp->extradata[0] == 1) ? vp->extradata[3] : 0,
                 videoNeedsReorder() ? "display-order reorder ON" : "baseline, no reorder");
        // Diagnostics: the parameter sets as the server sent them, logged before the
        // decoder sees anything (see h264_diag.h).
        if (vp->extradata_size >= 7 && vp->extradata[0] == 1) {
            video_nal_length_size_ = (vp->extradata[4] & 3) + 1;
            OSReport("Ufin: video %s\n", h264diag::describeAvcC(vp->extradata, (size_t)vp->extradata_size).c_str());
            OSReport("Ufin: video avcC hex: %s\n", h264diag::hex(vp->extradata, (size_t)vp->extradata_size, 96).c_str());
        }
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

double Decoder::spanSeconds(const QueueSpan& span, int streamIndex) const {
    if (streamIndex < 0 || !fmt_ctx_ || span.front == AV_NOPTS_VALUE || span.back == AV_NOPTS_VALUE ||
        span.back < span.front) {
        return 0.0;
    }
    return (double)(span.back - span.front + (span.backDuration > 0 ? span.backDuration : 0)) *
           av_q2d(fmt_ctx_->streams[streamIndex]->time_base);
}

void Decoder::publishQueueStats() {
    auto ms = [](double seconds) -> int32_t {
        const double v = seconds * 1000.0 + 0.5;
        return v <= 0.0 ? 0 : (v >= 2147483647.0 ? 2147483647 : (int32_t)v);
    };
    // The larger of the two estimates: identical to the plain duration sum
    // whenever packets carry durations, and still a real timeline figure when
    // they do not.
    pub_video_ms_.store(ms(std::max(queued_video_seconds_, spanSeconds(video_span_, video_stream_index_))),
                        std::memory_order_relaxed);
    pub_audio_ms_.store(ms(std::max(queued_audio_seconds_, spanSeconds(audio_span_, audio_stream_index_))),
                        std::memory_order_relaxed);
    pub_packet_bytes_.store(queued_packet_bytes_ > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)queued_packet_bytes_,
                            std::memory_order_relaxed);
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
            reachedEof_.store(true, std::memory_order_release);
            return false;
        }
        std::deque<AVPacket*>* queue = nullptr;
        AVRational timeBase{1, 1};
        double* queuedSeconds = nullptr;
        if (packet_->stream_index == video_stream_index_ && video_ctx_) {
            queue = &video_packets_;
            timeBase = fmt_ctx_->streams[video_stream_index_]->time_base;
            queuedSeconds = &queued_video_seconds_;
        } else if (packet_->stream_index == audio_stream_index_ && audio_ctx_) {
            queue = &audio_packets_;
            timeBase = fmt_ctx_->streams[audio_stream_index_]->time_base;
            queuedSeconds = &queued_audio_seconds_;
        }
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
        // Packet duration in stream time -- what fragmented-mp4 sample
        // deltas give us. A missing/zero duration (some containers don't
        // always set it) just means this one packet doesn't count toward
        // the buffered-ahead estimate; it's still queued and decoded
        // normally, so nothing is lost, the estimate is just briefly
        // conservative.
        if (copy->duration > 0) {
            *queuedSeconds += (double)copy->duration * av_q2d(timeBase);
        }
        {
            const bool isVideo = (queuedSeconds == &queued_video_seconds_);
            QueueSpan& span = isVideo ? video_span_ : audio_span_;
            const int64_t ts = packetTs(copy);
            if (ts != AV_NOPTS_VALUE) {
                if (queue->empty() || span.front == AV_NOPTS_VALUE) span.front = ts;
                span.back = ts;
                span.backDuration = copy->duration > 0 ? copy->duration : 0;
            }
            if (isVideo) {
                if (copy->duration > 0 || ts != AV_NOPTS_VALUE) video_timed_packets_.fetch_add(1, std::memory_order_relaxed);
                else video_untimed_packets_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        queue->push_back(copy);
        publishQueueStats();
        return true;
    }
}

bool Decoder::decodeFrom(AVCodecContext* ctx, std::deque<AVPacket*>& queue, bool& flushed,
                         bool& drained, bool isVideo, AVFrame** outFrame) {
    if (!ctx || drained) return false;
    if (isVideo && fatal_decode_error_.load(std::memory_order_acquire)) {
        // Already tripped: Player's own thread will notice
        // fatalDecodeError() and tear playback down shortly. Don't feed
        // this decoder any more packets in the meantime -- that's the
        // whole point of the breaker (see the comment on
        // consecutive_video_decode_errors_ in decoder.h).
        return false;
    }
    for (int guard = 0; guard < 64; guard++) {
        int ret = avcodec_receive_frame(ctx, frame_);
        if (ret == 0) {
            if (isVideo) consecutive_video_decode_errors_ = 0;
            if (isVideo && video_hw_height_ > 0 && frame_->height == video_hw_height_) {
                // Bottom rows of the coded picture only: the planes keep their own pointers and
                // strides, so shortening the frame is all the crop there is.
                frame_->height = video_visible_height_;
            }
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
            // consumed, so log it and keep feeding -- unless this is now
            // an unbroken streak long enough to call the decoder wedged.
            video_decode_errors_++;
            consecutive_video_decode_errors_++;
            if (video_decode_errors_ <= 5 || video_decode_errors_ % 100 == 0) {
                OSReport("Ufin: video decode error %d (count=%d, consecutive=%d)\n", ret,
                         video_decode_errors_, consecutive_video_decode_errors_);
            }
            if (consecutive_video_decode_errors_ >= MAX_CONSECUTIVE_VIDEO_DECODE_ERRORS) {
                OSReport("Ufin: %d consecutive video decode errors -- hardware decoder "
                         "appears wedged, tripping the circuit breaker\n",
                         consecutive_video_decode_errors_);
                fatal_decode_error_.store(true, std::memory_order_release);
                return false;
            }
        }

        if (!queue.empty()) {
            AVPacket* pkt = queue.front();
            if (isVideo && video_diag_packets_ < 6) {
                // Logged *before* the send: if the process dies in the decoder, this is the last line.
                video_diag_packets_++;
                OSReport("Ufin: video packet #%d about to be decoded: %d B key=%d pts=%lld dts=%lld nals=[%s]\n",
                         video_diag_packets_, pkt->size, (pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0, (long long)pkt->pts,
                         (long long)pkt->dts,
                         h264diag::describePacketNals(pkt->data, (size_t)pkt->size, video_nal_length_size_).c_str());
            }
            int sendRet = avcodec_send_packet(ctx, pkt);
            if (sendRet == AVERROR(EAGAIN)) {
                // Wants output drained first, but just said it has none:
                // try again on the next call rather than spin.
                return false;
            }
            // Accepted -- or rejected outright, in which case the packet
            // is unusable and dropping it is the only option.
            queued_packet_bytes_ -= (size_t)pkt->size;
            if (pkt->duration > 0) {
                AVRational timeBase = isVideo ? fmt_ctx_->streams[video_stream_index_]->time_base
                                               : fmt_ctx_->streams[audio_stream_index_]->time_base;
                double& queuedSeconds = isVideo ? queued_video_seconds_ : queued_audio_seconds_;
                queuedSeconds -= (double)pkt->duration * av_q2d(timeBase);
                if (queuedSeconds < 0.0) queuedSeconds = 0.0; // clamp against fp drift
            }
            av_packet_free(&pkt);
            queue.pop_front();
            {
                QueueSpan& span = isVideo ? video_span_ : audio_span_;
                if (queue.empty()) span = QueueSpan();
                else span.front = packetTs(queue.front());
            }
            publishQueueStats();
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
    //
    // Not for B-frame-capable video: pictures arrive in decode order, so pts is not monotonic and
    // best_effort_timestamp then falls back to dts -- right order, wrong display times. Each
    // picture's own pts is the true one; FrameReorder sorts them (Baseline is unchanged).
    int64_t ts = (type == DecodedFrameType::VIDEO && videoNeedsReorder() && frame->pts != AV_NOPTS_VALUE)
                     ? frame->pts : frame->best_effort_timestamp;
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
    queued_video_seconds_ = 0.0;
    queued_audio_seconds_ = 0.0;
    video_span_ = QueueSpan();
    audio_span_ = QueueSpan();
    publishQueueStats();
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
    reachedEof_.store(false, std::memory_order_release);
    video_timed_packets_.store(0, std::memory_order_relaxed);
    video_untimed_packets_.store(0, std::memory_order_relaxed);
    video_diag_packets_ = 0;
    video_hw_height_ = 0;
    video_visible_height_ = 0;
    video_decode_errors_ = 0;
    consecutive_video_decode_errors_ = 0;
    fatal_decode_error_.store(false, std::memory_order_release);
}
