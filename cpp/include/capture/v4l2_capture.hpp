
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct CaptureStats {
    unsigned int frames = 0;
    unsigned int valid = 0;
    unsigned int invalid = 0;
    unsigned int timeouts = 0;
    double fps = 0.0;
};

class V4L2Capture {
public:
    using FrameCallback =
        std::function<void(const uint8_t*, std::size_t)>;

    explicit V4L2Capture(
        const std::string& device = "/dev/video0",
        unsigned int width = 1280,
        unsigned int height = 720,
        unsigned int fps = 30
    );

    ~V4L2Capture();

    V4L2Capture(const V4L2Capture&) = delete;
    V4L2Capture& operator=(const V4L2Capture&) = delete;

    void initialize();

    CaptureStats captureFrames(
        unsigned int count,
        const FrameCallback& onFrame = {}
    );

private:
    struct Buffer {
        void* data = nullptr;
        std::size_t length = 0;
    };

    std::string device_;
    unsigned int width_;
    unsigned int height_;
    unsigned int fps_;

    int fd_ = -1;
    bool streaming_ = false;

    std::vector<Buffer> buffers_;

    static int xioctl(
        int fd,
        unsigned long request,
        void* arg
    );

    static bool checkJpeg(
        const uint8_t* data,
        std::size_t size
    );

    void cleanup() noexcept;
};
