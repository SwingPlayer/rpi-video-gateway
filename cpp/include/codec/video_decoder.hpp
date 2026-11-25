
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

class VideoDecoder {
public:
    using FrameCallback =
        std::function<void(const AVFrame*)>;

    VideoDecoder();
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    bool decode(
        const uint8_t* data,
        std::size_t size,
        const FrameCallback& onFrame = {}
    );

    uint64_t decodedFrames() const {
        return decoded_frames_;
    }

    uint64_t failedFrames() const {
        return failed_frames_;
    }

private:
    AVCodecContext* context_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;

    uint64_t decoded_frames_ = 0;
    uint64_t failed_frames_ = 0;
    bool first_frame_ = true;

    void cleanup() noexcept;
};
