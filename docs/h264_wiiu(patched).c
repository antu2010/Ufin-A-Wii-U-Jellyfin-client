/**
 * Decoder using Wii U's h264.rpl
 * Created 2020 by GaryOderNichts
 */

#include "h264_wiiu.h"

#include "libavutil/imgutils.h"
#include "internal.h"

#include <malloc.h>

static void frame_callback(H264DecodeOutput *output) { }

static int h264_wiiu_decode_init(AVCodecContext *avctx)
{
    WIIUContext* ctx = avctx->priv_data;
    uint32_t memRequirement;
    int res;
    void* decoder;
    int framebuf_size;
    void* framebuffer;

    // memory requirement for the maximum supported level (level 42)
    memRequirement = 0x2200000 + 0x3ff + 0x480000;

    decoder = malloc(memRequirement);
    res = H264DECInitParam(memRequirement, decoder);
    if (res != 0)
    {
        av_log(avctx, AV_LOG_ERROR, "h264_wiiu: Error initializing decoder 0x%07X", res);
        return -1;
    }

    res = H264DECSetParam_FPTR_OUTPUT(decoder, frame_callback);
    if (res != 0)
    {
        av_log(avctx, AV_LOG_ERROR, "h264_wiiu: Error setting callback 0x%07X", res);
        return -1;
    }

    res = H264DECSetParam_OUTPUT_PER_FRAME(decoder, 1);
    if (res != 0)
    {
        av_log(avctx, AV_LOG_ERROR, "h264_wiiu: Error setting OUTPUT_PER_FRAME 0x%07X", res);
        return -1;
    }

    res = H264DECOpen(decoder);
    if (res != 0)
    {
        av_log(avctx, AV_LOG_ERROR, "h264_wiiu: Error opening decoder 0x%07X", res);
        return -1;
    }

    res = H264DECBegin(decoder);
    if (res != 0)
    {
        av_log(avctx, AV_LOG_ERROR, "h264_wiiu: Error preparing decoder 0x%07X", res);
        return -1;
    }

    ctx->decoder = decoder;

    // avctx->width/height here are the *display* (cropped) dimensions from
    // the SPS -- e.g. 1280x692. Every H.264 stream is coded on a 16x16
    // macroblock grid internally (this is exactly why mainstream 1080p
    // H.264 is almost always coded at 1088 and cropped down to 1080), and
    // the Wii U's own h264.rpl hardware decoder is no exception: it writes
    // a full macroblock-aligned frame (height rounded up to a multiple of
    // 16) into `framebuffer` at a 256-pixel-aligned pitch, regardless of
    // what the cropped display height says. Sizing this allocation from
    // the raw, un-aligned width/height under-allocates by exactly the
    // padding the hardware still writes -- e.g. for 1280x692 the hardware
    // writes as if height were 704, needing pitch*704*1.5 = 1,351,680
    // bytes, while (width*height*3)>>1 here only reserves 1,327,680 --
    // a 24,000 byte heap overflow on every single decoded frame.
    //
    // Match the same alignment the pitch calculation in
    // h264_wiiu_decode_frame() below already uses for width, so both ends
    // agree on the geometry the hardware actually writes.
    int aligned_width = (avctx->width + (256 - 1)) & ~(256 - 1);
    int aligned_height = (avctx->height + (16 - 1)) & ~(16 - 1);

    framebuf_size = (aligned_width * aligned_height * 3) >> 1;
    framebuffer = memalign(1024, framebuf_size);

    ctx->framebuffer = framebuffer;
    ctx->framebuffer_size = framebuf_size;

    // Wii U decoder only outputs NV12
    avctx->pix_fmt = AV_PIX_FMT_NV12;

    return 0;
}

static int h264_wiiu_decode_close(AVCodecContext *avctx)
{
    WIIUContext* ctx = avctx->priv_data;
    void* decoder = ctx->decoder;
    void* framebuffer = ctx->framebuffer;

    free(framebuffer);

    H264DECFlush(decoder);
    H264DECEnd(decoder);
    H264DECClose(decoder);

    free(decoder);

    return 0;
}

static int h264_wiiu_decode_frame(AVCodecContext *avctx, void *data, int *got_frame, AVPacket *avpkt)
{
    WIIUContext* ctx = avctx->priv_data;
    uint8_t* buf = avpkt->buf->data;
    int buf_size = avpkt->buf->size;
    void* decoder = ctx->decoder;
    void* framebuffer = ctx->framebuffer;
    uint8_t* pointers[2];
    AVFrame *avframe = data;
    int pitch;
    int linesize[2];
    int res;
    BOOL skippable = FALSE;

    // H264DECCheckSkipableFrame() reports non-reference pictures (nal_ref_idc == 0) as
    // skippable. In a stream with B-frames (Main/High) that is every non-reference B-frame:
    // honouring it unconditionally dropped them all, so such streams played back at half the
    // frame rate. Only skip when the caller explicitly asked for it (-skip_frame nonref or
    // higher); by default every picture is decoded and returned.
    if (avctx->skip_frame >= AVDISCARD_NONREF)
    {
        res = H264DECCheckSkipableFrame(buf, buf_size, &skippable);
        if (res != 0)
        {
            av_log(avctx, AV_LOG_ERROR, "h264_wiiu: Error checking for skippable frame");
            return -1;
        }

        if (skippable)
        {
            // skip the frame
            return 0;
        }
    }

    res = H264DECSetBitstream(decoder, buf, buf_size, 0);
    if (res != 0)
    {
        av_log(avctx, AV_LOG_ERROR, "h264_wiiu: Error setting bitstream 0x%07X", res);
        return -1;
    }

    res = H264DECExecute(decoder, framebuffer);
    if (res != 0xE4)
    {
        av_log(avctx, AV_LOG_ERROR, "h264_wiiu: Error decoding frame 0x%07X", res);
        return -1;
    }

    if (ff_get_buffer(avctx, avframe, 0) < 0)
    {
        av_log(avctx, AV_LOG_ERROR, "h264_wiiu: Unable to allocate buffer");
        return AVERROR(ENOMEM);
    }

    pitch = (avctx->width + (256 - 1)) & ~(256 - 1);
    linesize[0] = linesize[1] = pitch;

    // Same macroblock-alignment reasoning as h264_wiiu_decode_init() above:
    // the hardware's Y plane occupies a 16-row-aligned height, so the UV
    // plane starts after that many rows of pitch, not after the raw
    // (possibly unaligned) display height. Using avctx->height directly
    // here both under-allocated the buffer above AND read chroma from the
    // wrong offset (into what is actually still padding/Y data) whenever
    // height isn't already a multiple of 16.
    int aligned_height_for_uv = (avctx->height + (16 - 1)) & ~(16 - 1);

    // Y
    pointers[0] = (uint8_t*) framebuffer;
    // U/V
    pointers[1] = ((uint8_t*) framebuffer) + (aligned_height_for_uv * pitch);

    av_image_copy(avframe->data, avframe->linesize, (const uint8_t **) pointers, linesize, AV_PIX_FMT_NV12, avctx->width, avctx->height);

    *got_frame = 1;
    return avpkt->size;
}

AVCodec ff_h264_wiiu_decoder =
{
    .name           = "h264_wiiu",
    .long_name      = NULL_IF_CONFIG_SMALL("H.264 Decoder using h264.rpl"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_H264,
    .priv_data_size = sizeof(WIIUContext),
    .init           = h264_wiiu_decode_init,
    .close          = h264_wiiu_decode_close,
    .decode         = h264_wiiu_decode_frame,
    .bsfs           = "h264_mp4toannexb",
};