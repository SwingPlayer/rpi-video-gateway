#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

class RtspPublisher {
public:
    RtspPublisher(
        const std::string& url,
        const AVCodecContext* encoderContext,
        std::atomic<bool>* stopRequested = nullptr
    );

    ~RtspPublisher();

    RtspPublisher(const RtspPublisher&) = delete;
    RtspPublisher& operator=(const RtspPublisher&) = delete;

    void publish(const AVPacket* packet);
    void close();

    uint64_t publishedPackets() const {
        return published_packets_;
    }

private:
    std::string url_;

    AVFormatContext* format_ = nullptr;
    AVStream* stream_ = nullptr;

    AVRational source_time_base_{1, 30};

    std::atomic<bool>* stop_requested_ = nullptr;

    std::chrono::steady_clock::time_point deadline_;

    bool started_ = false;
    uint64_t published_packets_ = 0;

    static int interruptCallback(void* opaque);

    void setDeadline();
    void start(const AVPacket* firstPacket);

    static std::vector<uint8_t> extractParameterSets(
        const uint8_t* data,
        std::size_t size
    );

    void cleanup() noexcept;
};