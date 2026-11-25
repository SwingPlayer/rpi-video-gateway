#pragma once

#include <libavcodec/avcodec.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>

using H264PacketPtr = std::shared_ptr<AVPacket>;

struct H264QueueStats {
    uint64_t pushed = 0;
    uint64_t dropped = 0;
    std::size_t peakDepth = 0;
};

class H264PacketQueue {
public:
    explicit H264PacketQueue(std::size_t capacity)
        : capacity_(capacity) {

        if (capacity == 0) {
            throw std::invalid_argument(
                "H264 queue capacity must be positive"
            );
        }
    }

    H264PacketQueue(const H264PacketQueue&) = delete;
    H264PacketQueue& operator=(const H264PacketQueue&) = delete;

    bool push(const AVPacket* source) {
        if (!source || !source->data || source->size <= 0) {
            return false;
        }

        AVPacket* cloned = av_packet_clone(source);

        if (!cloned) {
            throw std::runtime_error(
                "Failed to clone H264 packet"
            );
        }

        H264PacketPtr packet(
            cloned,
            [](AVPacket* p) {
                av_packet_free(&p);
            }
        );

        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (closed_) {
                return false;
            }

            // 网络端跟不上时丢弃最旧数据
            if (queue_.size() >= capacity_) {
                queue_.pop_front();
                ++stats_.dropped;
            }

            queue_.push_back(std::move(packet));
            ++stats_.pushed;

            if (queue_.size() > stats_.peakDepth) {
                stats_.peakDepth = queue_.size();
            }
        }

        condition_.notify_one();
        return true;
    }

    bool pop(H264PacketPtr& packet) {
        std::unique_lock<std::mutex> lock(mutex_);

        condition_.wait(lock, [this]() {
            return closed_ || !queue_.empty();
        });

        if (queue_.empty()) {
            return false;
        }

        packet = std::move(queue_.front());
        queue_.pop_front();

        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }

        condition_.notify_all();
    }

    H264QueueStats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

private:
    const std::size_t capacity_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;

    std::deque<H264PacketPtr> queue_;

    H264QueueStats stats_;
    bool closed_ = false;
};