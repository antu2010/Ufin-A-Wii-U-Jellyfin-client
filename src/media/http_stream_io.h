// HttpStreamIO -- bridges FFmpeg's AVIOContext (how libavformat reads
// input data) to HttpStreamReader, making a live, non-seekable HTTP
// stream look like a file to FFmpeg.
//
// Non-seekable is a real, deliberate constraint here, not a limitation
// we're working around: Jellyfin's transcode endpoint returns
// "Accept-Ranges: none" and streams the response as an indefinite
// "Transfer-Encoding: chunked" body (confirmed via curl) since the video
// is being generated live as it's watched -- there is no fixed byte
// range to seek within. FFmpeg handles non-seekable inputs fine (same
// code path as e.g. a live broadcast), so this is surfaced honestly via
// avio_ctx_->seekable = 0 rather than pretending to support seeking.

#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "../http_stream_reader.h"

extern "C" {
#include <libavformat/avformat.h>
}

class HttpStreamIO {
public:
    HttpStreamIO(std::string host, int port, std::string path);
    ~HttpStreamIO();

    // Both forwarded to the HttpStreamReader (see its header); call before
    // open(). Defaults keep the plain 15 s, non-cancellable wait.
    void setStartupTimeoutSeconds(int seconds) { reader_.setStartupTimeoutSeconds(seconds); }
    void setAbortCheck(std::function<bool()> abortCheck) { reader_.setAbortCheck(std::move(abortCheck)); }
    // True when open() failed because the abort check ended the wait (the user
    // backed out) rather than because anything went wrong.
    bool cancelled() const { return reader_.cancelled(); }

    // Opens the underlying HttpStreamReader and sets up the AVIOContext.
    // Must succeed before avioContext() is usable.
    bool open();

    // The AVIOContext to assign to an AVFormatContext's `pb` field
    // (along with setting AVFMT_FLAG_CUSTOM_IO). Owned by this class --
    // do not free it yourself, it's cleaned up in the destructor.
    AVIOContext* avioContext() const { return avio_ctx_; }

    const std::string& lastError() const { return last_error_; }

private:
    HttpStreamReader reader_;
    std::string last_error_;

    AVIOContext* avio_ctx_ = nullptr;
    uint8_t* avio_internal_buffer_ = nullptr; // FFmpeg's own working buffer

    int readPacket(uint8_t* buf, int bufSize);
    int64_t seek(int64_t offset, int whence);

    static int readPacketTrampoline(void* opaque, uint8_t* buf, int bufSize);
    static int64_t seekTrampoline(void* opaque, int64_t offset, int whence);
};
