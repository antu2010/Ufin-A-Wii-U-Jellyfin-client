// FileStreamIO -- bridges FFmpeg's AVIOContext to a plain file on the SD card, the local-file
// counterpart of HttpStreamIO. Used for the default menu music (bgm.m4a). Unlike the live HTTP
// stream it is seekable, which the MP3 demuxer likes (duration, probing).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
}

// Where the default menu music lives (Cemu maps /vol/external01 to its sdcard folder). Tried in
// order. .m4a (AAC in MP4) works with the stock FFmpeg build; .mp3 only with one that has the
// mp3 demuxer and mpegaudio parser (patches/ffmpeg-wiiu-mp3-demuxer.patch).
constexpr const char* kMenuMusicLocalPaths[] = {"/vol/external01/wiiu/apps/ufin/assets/bgm.m4a",
                                                "/vol/external01/wiiu/apps/ufin/assets/bgm.mp3"};

// True if `path` can be opened for reading. Cheap enough to call once per decision, not per frame.
bool fileReadable(const std::string& path);

// The first of kMenuMusicLocalPaths that exists, or "" if none does.
std::string findLocalMenuMusic();

class FileStreamIO {
public:
    explicit FileStreamIO(std::string path);
    ~FileStreamIO();

    // Opens the file and sets up the AVIOContext. Must succeed before avioContext() is usable.
    bool open();

    // Owned by this class (freed in the destructor); assign to AVFormatContext::pb together with
    // AVFMT_FLAG_CUSTOM_IO, as Decoder::open() does.
    AVIOContext* avioContext() const { return avio_ctx_; }

    const std::string& lastError() const { return last_error_; }

private:
    std::string path_;
    std::string last_error_;
    FILE* file_ = nullptr;
    int64_t size_ = -1;

    AVIOContext* avio_ctx_ = nullptr;

    int readPacket(uint8_t* buf, int bufSize);
    int64_t seek(int64_t offset, int whence);

    static int readPacketTrampoline(void* opaque, uint8_t* buf, int bufSize);
    static int64_t seekTrampoline(void* opaque, int64_t offset, int whence);
};
