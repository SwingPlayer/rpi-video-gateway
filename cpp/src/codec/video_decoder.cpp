
#include "codec/video_decoder.hpp"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
}

#include <cerrno>
#include <climits>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

std::string ffmpegError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return std::string(buffer);
}

}

VideoDecoder::VideoDecoder() {
    try {
        const AVCodec* codec =
            avcodec_find_decoder(AV_CODEC_ID_MJPEG);

        if (!codec) {
            throw std::runtime_error(
                "MJPEG decoder not found"
            );
        }

        context_ = avcodec_alloc_context3(codec);
        packet_ = av_packet_alloc();
        frame_ = av_frame_alloc();

        if (!context_ || !packet_ || !frame_) {
            throw std::runtime_error(
                "Decoder allocation failed"
            );
        }

        int ret = avcodec_open2(
            context_, codec, nullptr
        );

        if (ret < 0) {
            throw std::runtime_error(
                "Open decoder failed: " + ffmpegError(ret)
            );
        }
    } catch (...) {
        cleanup();
        throw;
    }

    std::cout
        << "[OK] FFmpeg MJPEG decoder initialized\n";
}

VideoDecoder::~VideoDecoder() {
    cleanup();
}

bool VideoDecoder::decode(
    const uint8_t* data,
    std::size_t size,
    const FrameCallback& onFrame
) {
    if (!data || size == 0 || size > INT_MAX) {
        ++failed_frames_;
        return false;
    }

    av_packet_unref(packet_);

    int ret = av_new_packet(
        packet_, static_cast<int>(size)
    );

    if (ret < 0) {
        ++failed_frames_;
        return false;
    }

    std::memcpy(packet_->data, data, size);

    ret = avcodec_send_packet(context_, packet_);
    av_packet_unref(packet_);

    if (ret < 0) {
        ++failed_frames_;
        std::cerr
            << "[DECODE] Send failed: "
            << ffmpegError(ret) << "\n";
        return false;
    }

    bool produced = false;

    while (true) {
        ret = avcodec_receive_frame(context_, frame_);

        if (ret == AVERROR(EAGAIN) ||
            ret == AVERROR_EOF) {
            break;
        }

        if (ret < 0) {
            ++failed_frames_;
            std::cerr
                << "[DECODE] Receive failed: "
                << ffmpegError(ret) << "\n";
            return false;
        }

        if (frame_->flags & AV_FRAME_FLAG_CORRUPT) {
            ++failed_frames_;
            av_frame_unref(frame_);
            return false;
        }

        produced = true;
        ++decoded_frames_;

        if (first_frame_) {
            const char* format = av_get_pix_fmt_name(
                static_cast<AVPixelFormat>(frame_->format)
            );

            std::cout
                << "[INFO] Decoded resolution: "
                << frame_->width << "x"
                << frame_->height << "\n"
                << "[INFO] Pixel format: "
                << (format ? format : "unknown")
                << "\n";

            first_frame_ = false;
        }

        // 传递解码后的 AVFrame 给 H.264 编码器
        // 回调只能在当前 AVFrame 有效期间使用它
        try {
            if (onFrame) {
                onFrame(frame_);
            }
        } catch (...) {
            av_frame_unref(frame_);
            throw;
        }

        if (decoded_frames_ % 100 == 0) {
            std::cout
                << "[DECODE] "
                << decoded_frames_
                << " frames decoded\n";
        }

        av_frame_unref(frame_);
    }

    if (!produced) {
        ++failed_frames_;
    }

    return produced;
}

void VideoDecoder::cleanup() noexcept {
    av_frame_free(&frame_);
    av_packet_free(&packet_);
    avcodec_free_context(&context_);
}
