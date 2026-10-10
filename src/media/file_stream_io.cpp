#include "file_stream_io.h"

bool fileReadable(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

std::string findLocalMenuMusic() {
    for (const char* p : kMenuMusicLocalPaths)
        if (fileReadable(p)) return p;
    return std::string();
}

FileStreamIO::FileStreamIO(std::string path) : path_(std::move(path)) {}

FileStreamIO::~FileStreamIO() {
    if (avio_ctx_) {
        // FFmpeg may have reallocated the buffer: free the context's own pointer.
        av_free(avio_ctx_->buffer);
        avio_context_free(&avio_ctx_);
    }
    if (file_) fclose(file_);
}

bool FileStreamIO::open() {
    file_ = fopen(path_.c_str(), "rb");
    if (!file_) {
        last_error_ = "cannot open " + path_;
        return false;
    }
    if (fseek(file_, 0, SEEK_END) == 0) {
        long end = ftell(file_);
        if (end >= 0) size_ = (int64_t)end;
        fseek(file_, 0, SEEK_SET);
    }

    const int avioBufferSize = 32 * 1024;
    uint8_t* buffer = (uint8_t*)av_malloc(avioBufferSize);
    if (!buffer) {
        last_error_ = "av_malloc failed";
        return false;
    }
    avio_ctx_ = avio_alloc_context(buffer, avioBufferSize, 0, this, &FileStreamIO::readPacketTrampoline, nullptr,
                                   &FileStreamIO::seekTrampoline);
    if (!avio_ctx_) {
        av_free(buffer);
        last_error_ = "avio_alloc_context failed";
        return false;
    }
    avio_ctx_->seekable = AVIO_SEEKABLE_NORMAL;
    return true;
}

int FileStreamIO::readPacket(uint8_t* buf, int bufSize) {
    size_t n = fread(buf, 1, (size_t)bufSize, file_);
    if (n == 0) return ferror(file_) ? AVERROR(EIO) : AVERROR_EOF;
    return (int)n;
}

int64_t FileStreamIO::seek(int64_t offset, int whence) {
    if (whence & AVSEEK_SIZE) return size_; // -1 if unknown
    whence &= ~AVSEEK_FORCE;
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) return -1;
    if (fseek(file_, (long)offset, whence) != 0) return -1;
    return (int64_t)ftell(file_);
}

int FileStreamIO::readPacketTrampoline(void* opaque, uint8_t* buf, int bufSize) {
    return static_cast<FileStreamIO*>(opaque)->readPacket(buf, bufSize);
}

int64_t FileStreamIO::seekTrampoline(void* opaque, int64_t offset, int whence) {
    return static_cast<FileStreamIO*>(opaque)->seek(offset, whence);
}
