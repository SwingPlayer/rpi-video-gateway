#include "codec/video_encoder.hpp"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>
}

#include <cerrno>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

std::string ffmpegError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return std::string(buffer);
}

void checkResult(int result, const std::string& action) {
    if (result < 0) {
        throw std::runtime_error(
            action + ": " + ffmpegError(result)
        );
    }
}

}

VideoEncoder::VideoEncoder(
    const std::string& outputPath,
    int width,
    int height,
    int fps,
    int bitrate
)
    : width_(width),
      height_(height),
      fps_(fps) {

    if (width <= 0 || height <= 0 ||
        fps <= 0 || bitrate <= 0) {
        throw std::invalid_argument(
            "Invalid encoder parameters"
        );
    }

    try {
        output_.open(
            outputPath,
            std::ios::binary | std::ios::trunc
        );

        if (!output_.is_open()) {
            throw std::runtime_error(
                "Cannot open output: " + outputPath
            );
        }

        const AVCodec* codec =
            avcodec_find_encoder_by_name("h264_v4l2m2m");

        if (!codec) {
            throw std::runtime_error(
                "H.264 V4L2 hardware encoder not found"
            );
        }

        context_ = avcodec_alloc_context3(codec);

        if (!context_) {
            throw std::runtime_error(
                "Encoder context allocation failed"
            );
        }

        context_->width = width_;
        context_->height = height_;
        context_->time_base = {1, fps_};
        context_->framerate = {fps_, 1};
        context_->pix_fmt = AV_PIX_FMT_YUV420P;

        context_->bit_rate = bitrate;
        context_->gop_size = fps_;
        context_->max_b_frames = 0;

        context_->color_range = AVCOL_RANGE_MPEG;
        context_->colorspace = AVCOL_SPC_BT470BG;

        checkResult(
            avcodec_open2(context_, codec, nullptr),
            "Opening H.264 hardware encoder"
        );

        packet_ = av_packet_alloc();
        converted_frame_ = av_frame_alloc();

        if (!packet_ || !converted_frame_) {
            throw std::runtime_error(
                "Encoder buffer allocation failed"
            );
        }

        converted_frame_->format = AV_PIX_FMT_YUV420P;
        converted_frame_->width = width_;
        converted_frame_->height = height_;
        converted_frame_->color_range = AVCOL_RANGE_MPEG;
        converted_frame_->colorspace = AVCOL_SPC_BT470BG;

        checkResult(
            av_frame_get_buffer(converted_frame_, 32),
            "Allocating YUV420P frame"
        );

    } catch (...) {
        cleanup();
        throw;
    }

    std::cout
        << "[OK] H.264 hardware encoder initialized\n"
        << "[INFO] Encoder: h264_v4l2m2m\n"
        << "[INFO] Resolution: "
        << width_ << "x" << height_ << "\n"
        << "[INFO] Bitrate: "
        << bitrate / 1000000.0 << " Mbps\n"
        << "[INFO] FPS: " << fps_ << "\n";
}

VideoEncoder::~VideoEncoder() {
    cleanup();
}

void VideoEncoder::setPacketCallback(
    PacketCallback callback
) {
    packet_callback_ = std::move(callback);
}

void VideoEncoder::drainPackets() {
    while (true) {
        int ret = avcodec_receive_packet(
            context_, packet_
        );

        if (ret == AVERROR(EAGAIN) ||
            ret == AVERROR_EOF) {
            break;
        }

        checkResult(ret, "Receiving H.264 packet");

        try {
            // 1. 保留本地 H.264 文件
            output_.write(
                reinterpret_cast<const char*>(
                    packet_->data
                ),
                packet_->size
            );

            if (!output_) {
                throw std::runtime_error(
                    "Writing H.264 file failed"
                );
            }

            // 2. 把编码后的 AVPacket 交给 RTSP 发布器
            if (packet_callback_) {
                packet_callback_(packet_);
            }

            ++written_packets_;
            written_bytes_ += packet_->size;

            if (written_packets_ % 100 == 0) {
                std::cout
                    << "[ENCODE] "
                    << written_packets_
                    << " H.264 packets processed\n";
            }

        } catch (...) {
            av_packet_unref(packet_);
            throw;
        }

        av_packet_unref(packet_);
    }
}

void VideoEncoder::encode(
    const AVFrame* source,
    int64_t pts
) {
    if (finished_) {
        throw std::runtime_error(
            "Encoder already finished"
        );
    }

    if (!source) {
        throw std::runtime_error(
            "Null source frame"
        );
    }

    if (source->width != width_ ||
        source->height != height_) {
        throw std::runtime_error(
            "Unexpected input resolution"
        );
    }

    checkResult(
        av_frame_make_writable(converted_frame_),
        "Making frame writable"
    );

    AVPixelFormat inputFormat =
        static_cast<AVPixelFormat>(source->format);

    bool fullRange =
        source->color_range == AVCOL_RANGE_JPEG ||
        inputFormat == AV_PIX_FMT_YUVJ422P ||
        inputFormat == AV_PIX_FMT_YUVJ420P;

    if (inputFormat == AV_PIX_FMT_YUVJ422P) {
        inputFormat = AV_PIX_FMT_YUV422P;
    } else if (inputFormat == AV_PIX_FMT_YUVJ420P) {
        inputFormat = AV_PIX_FMT_YUV420P;
    }

    sws_ = sws_getCachedContext(
        sws_,
        source->width,
        source->height,
        inputFormat,
        width_,
        height_,
        AV_PIX_FMT_YUV420P,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr
    );

    if (!sws_) {
        throw std::runtime_error(
            "Failed to initialize swscale"
        );
    }

    int colorSpace =
        source->colorspace == AVCOL_SPC_BT709
        ? SWS_CS_ITU709
        : SWS_CS_ITU601;

    const int* coefficients =
        sws_getCoefficients(colorSpace);

    checkResult(
        sws_setColorspaceDetails(
            sws_,
            coefficients,
            fullRange ? 1 : 0,
            coefficients,
            0,
            0,
            1 << 16,
            1 << 16
        ),
        "Configuring color range"
    );

    int convertedHeight = sws_scale(
        sws_,
        source->data,
        source->linesize,
        0,
        source->height,
        converted_frame_->data,
        converted_frame_->linesize
    );

    if (convertedHeight != height_) {
        throw std::runtime_error(
            "Pixel format conversion failed"
        );
    }

    converted_frame_->pts = pts;

    int ret = avcodec_send_frame(
        context_, converted_frame_
    );

    if (ret == AVERROR(EAGAIN)) {
        drainPackets();

        ret = avcodec_send_frame(
            context_, converted_frame_
        );
    }

    checkResult(
        ret,
        "Sending frame to H.264 encoder"
    );

    ++submitted_frames_;

    drainPackets();
}

void VideoEncoder::finish() {
    if (finished_)
        return;

    int ret = avcodec_send_frame(
        context_, nullptr
    );

    if (ret == AVERROR(EAGAIN)) {
        drainPackets();
        ret = avcodec_send_frame(context_, nullptr);
    }

    if (ret != AVERROR_EOF) {
        checkResult(
            ret,
            "Flushing H.264 encoder"
        );
    }

    drainPackets();

    output_.flush();

    if (!output_) {
        throw std::runtime_error(
            "Flushing output file failed"
        );
    }

    finished_ = true;

    std::cout
        << "[OK] H.264 encoder finished\n";
}

void VideoEncoder::cleanup() noexcept {
    sws_freeContext(sws_);
    sws_ = nullptr;

    av_frame_free(&converted_frame_);
    av_packet_free(&packet_);
    avcodec_free_context(&context_);

    if (output_.is_open()) {
        output_.close();
    }
}