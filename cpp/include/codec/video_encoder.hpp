#pragma once

#include <cstdint>
#include <fstream>
#include <functional>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

class VideoEncoder {
public:
    using PacketCallback =
        std::function<void(const AVPacket*)>;

    VideoEncoder(
        const std::string& outputPath,
        int width = 1280,
        int height = 720,
        int fps = 30,
        int bitrate = 3000000
    );

    ~VideoEncoder();

    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    void setPacketCallback(PacketCallback callback);

    void encode(const AVFrame* source, int64_t pts);
    void finish();

    const AVCodecContext* codecContext() const {
        return context_;
    }

    uint64_t submittedFrames() const {
        return submitted_frames_;
    }

    uint64_t writtenPackets() const {
        return written_packets_;
    }

    uint64_t writtenBytes() const {
        return written_bytes_;
    }

private:
    AVCodecContext* context_ = nullptr;
    AVFrame* converted_frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* sws_ = nullptr;

    std::ofstream output_;
    PacketCallback packet_callback_;

    int width_;
    int height_;
    int fps_;

    uint64_t submitted_frames_ = 0;
    uint64_t written_packets_ = 0;
    uint64_t written_bytes_ = 0;

    bool finished_ = false;

    void drainPackets();
    void cleanup() noexcept;
};