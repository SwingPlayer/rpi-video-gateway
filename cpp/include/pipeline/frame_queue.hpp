
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

struct MjpegFrame {
    std::vector<uint8_t> data;

    std::chrono::steady_clock::time_point capturedAt;
};

struct FrameQueueStats {
    uint64_t pushed = 0;
    uint64_t dropped = 0;
    std::size_t peakDepth = 0;
};

class MjpegFrameQueue {
public:
    explicit MjpegFrameQueue(std::size_t capacity)
        : capacity_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument(
                "Frame queue capacity must be positive"
            );
        }
    }

    MjpegFrameQueue(const MjpegFrameQueue&) = delete;
    MjpegFrameQueue& operator=(const MjpegFrameQueue&) = delete;

    // 采集线程调用：复制并提交一帧 MJPEG
    bool push(const uint8_t* data, std::size_t size) {
        if (!data || size == 0)
            return false;

        MjpegFrame frame;

        // 必须复制数据，因为 V4L2 缓冲区会被复用
        frame.data.assign(data, data + size);
        frame.capturedAt =
            std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (closed_)
                return false;

            // 队列满时丢弃最旧的待处理帧
            if (queue_.size() >= capacity_) {
                queue_.pop_front();
                ++stats_.dropped;
            }

            queue_.push_back(std::move(frame));
            ++stats_.pushed;

            if (queue_.size() > stats_.peakDepth) {
                stats_.peakDepth = queue_.size();
            }
        }

        condition_.notify_one();
        return true;
    }

    // 解码线程调用：等待并取出 MJPEG 帧
    bool pop(MjpegFrame& frame) {
        std::unique_lock<std::mutex> lock(mutex_);

        condition_.wait(lock, [this]() {
            return closed_ || !queue_.empty();
        });

        // 队列关闭且已经没有剩余帧
        if (queue_.empty())
            return false;

        frame = std::move(queue_.front());
        queue_.pop_front();

        return true;
    }

    // 通知消费者：采集已经结束
    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }

        condition_.notify_all();
    }

    FrameQueueStats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

private:
    const std::size_t capacity_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;

    std::deque<MjpegFrame> queue_;

    bool closed_ = false;
    FrameQueueStats stats_;
};
