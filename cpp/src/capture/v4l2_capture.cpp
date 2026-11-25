
#include "capture/v4l2_capture.hpp"

#include <linux/videodev2.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace {

void fail(const std::string& message) {
    throw std::runtime_error(
        message + ": " + std::strerror(errno)
    );
}

} // namespace

V4L2Capture::V4L2Capture(
    const std::string& device,
    unsigned int width,
    unsigned int height,
    unsigned int fps
)
    : device_(device),
      width_(width),
      height_(height),
      fps_(fps) {}

V4L2Capture::~V4L2Capture() {
    cleanup();
}

int V4L2Capture::xioctl(
    int fd,
    unsigned long request,
    void* arg
) {
    int result;

    do {
        result = ioctl(fd, request, arg);
    } while (result == -1 && errno == EINTR);

    return result;
}

bool V4L2Capture::checkJpeg(
    const uint8_t* data,
    std::size_t size
) {
    if (!data || size < 4)
        return false;

    // 检查 JPEG SOI 帧头
    if (data[0] != 0xFF || data[1] != 0xD8)
        return false;

    // 检查 JPEG EOI 帧尾
    // 允许末尾存在少量填充字节
    std::size_t begin =
        size > 64 ? size - 64 : 2;

    for (std::size_t i = size - 1; i > begin; --i) {
        if (data[i - 1] == 0xFF &&
            data[i] == 0xD9) {
            return true;
        }
    }

    return false;
}

void V4L2Capture::initialize() {
    if (fd_ >= 0) {
        throw std::runtime_error(
            "Camera already initialized"
        );
    }

    if (fps_ == 0) {
        throw std::runtime_error(
            "FPS must be greater than zero"
        );
    }

    fd_ = open(
        device_.c_str(),
        O_RDWR | O_NONBLOCK
    );

    if (fd_ < 0)
        fail("Failed to open camera");

    // 1. 查询摄像头能力
    v4l2_capability cap{};

    if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0)
        fail("VIDIOC_QUERYCAP failed");

    uint32_t capabilities =
        (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
        ? cap.device_caps
        : cap.capabilities;

    if (!(capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(capabilities & V4L2_CAP_STREAMING)) {
        throw std::runtime_error(
            "Device does not support V4L2 capture streaming"
        );
    }

    // 2. 配置 MJPEG 采集格式
    v4l2_format fmt{};

    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width_;
    fmt.fmt.pix.height = height_;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0)
        fail("VIDIOC_S_FMT failed");

    if (fmt.fmt.pix.width != width_ ||
        fmt.fmt.pix.height != height_ ||
        fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
        throw std::runtime_error(
            "Camera did not accept requested MJPEG format"
        );
    }

    // 3. 设置采集帧率
    v4l2_streamparm parm{};

    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = fps_;

    if (xioctl(fd_, VIDIOC_S_PARM, &parm) < 0)
        fail("VIDIOC_S_PARM failed");

    // 4. 申请 MMAP 缓冲区
    v4l2_requestbuffers req{};

    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0)
        fail("VIDIOC_REQBUFS failed");

    if (req.count < 2) {
        throw std::runtime_error(
            "Insufficient MMAP buffers"
        );
    }

    // 5. 查询并映射缓冲区
    for (unsigned int i = 0; i < req.count; ++i) {
        v4l2_buffer buf{};

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0)
            fail("VIDIOC_QUERYBUF failed");

        void* data = mmap(
            nullptr,
            buf.length,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            fd_,
            buf.m.offset
        );

        if (data == MAP_FAILED)
            fail("mmap failed");

        buffers_.push_back({
            data,
            static_cast<std::size_t>(buf.length)
        });
    }

    // 6. 所有缓冲区入队
    for (unsigned int i = 0;
         i < buffers_.size();
         ++i) {

        v4l2_buffer buf{};

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0)
            fail("VIDIOC_QBUF failed");
    }

    // 7. 启动视频采集
    v4l2_buf_type type =
        V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0)
        fail("VIDIOC_STREAMON failed");

    streaming_ = true;

    std::cout
        << "[OK] V4L2 camera initialized\n"
        << "[INFO] Device: " << device_ << "\n"
        << "[INFO] Resolution: "
        << width_ << "x" << height_ << "\n"
        << "[INFO] Requested FPS: "
        << fps_ << "\n"
        << "[INFO] MMAP buffers: "
        << buffers_.size() << "\n";
}

CaptureStats V4L2Capture::captureFrames(
    unsigned int count,
    const FrameCallback& onFrame
) {
    if (!streaming_) {
        throw std::runtime_error(
            "Camera is not streaming"
        );
    }

    CaptureStats stats;

    using Clock = std::chrono::steady_clock;

    Clock::time_point first{};
    Clock::time_point last{};

    unsigned int consecutiveTimeouts = 0;

    while (stats.frames < count) {
        // 等待摄像头数据就绪
        pollfd pfd{};

        pfd.fd = fd_;
        pfd.events = POLLIN;

        int result = poll(&pfd, 1, 2000);

        if (result < 0) {
            if (errno == EINTR)
                continue;

            fail("poll failed");
        }

        if (result == 0) {
            ++stats.timeouts;
            ++consecutiveTimeouts;

            std::cerr
                << "[WARN] Camera timeout\n";

            if (consecutiveTimeouts >= 3) {
                throw std::runtime_error(
                    "Camera timed out three times"
                );
            }

            continue;
        }

        if (pfd.revents &
            (POLLERR | POLLHUP | POLLNVAL)) {
            throw std::runtime_error(
                "Camera poll error or disconnect"
            );
        }

        if (!(pfd.revents & POLLIN))
            continue;

        // 从驱动取出一帧
        v4l2_buffer buf{};

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN)
                continue;

            fail("VIDIOC_DQBUF failed");
        }

        consecutiveTimeouts = 0;

        if (buf.index >= buffers_.size()) {
            throw std::runtime_error(
                "Invalid V4L2 buffer index"
            );
        }

        auto now = Clock::now();

        if (stats.frames == 0)
            first = now;

        last = now;
        ++stats.frames;

        // 检查驱动错误、缓冲区长度和 JPEG 标记
        bool valid =
            !(buf.flags & V4L2_BUF_FLAG_ERROR) &&
            buf.bytesused <= buffers_[buf.index].length &&
            checkJpeg(
                static_cast<const uint8_t*>(
                    buffers_[buf.index].data
                ),
                buf.bytesused
            );

        if (valid)
            ++stats.valid;
        else
            ++stats.invalid;

        // 将有效 MJPEG 帧传递给解码器
        // 回调必须在归还 MMAP 缓冲区前完成
        try {
            if (valid && onFrame) {
                onFrame(
                    static_cast<const uint8_t*>(
                        buffers_[buf.index].data
                    ),
                    buf.bytesused
                );
            }
        } catch (...) {
            // 发生异常也尽量归还缓冲区
            xioctl(fd_, VIDIOC_QBUF, &buf);
            throw;
        }

        // 将缓冲区归还给 V4L2 驱动
        if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0)
            fail("Requeue buffer failed");

        if (stats.frames % 100 == 0) {
            std::cout
                << "[CAPTURE] "
                << stats.frames
                << " frames, valid="
                << stats.valid
                << ", invalid="
                << stats.invalid
                << "\n";
        }
    }

    // 按采集帧时间计算平均 FPS
    double seconds =
        std::chrono::duration<double>(
            last - first
        ).count();

    if (stats.frames > 1 && seconds > 0.0) {
        stats.fps =
            static_cast<double>(stats.frames - 1)
            / seconds;
    }

    return stats;
}

void V4L2Capture::cleanup() noexcept {
    if (fd_ < 0)
        return;

    // 停止采集
    if (streaming_) {
        v4l2_buf_type type =
            V4L2_BUF_TYPE_VIDEO_CAPTURE;

        xioctl(fd_, VIDIOC_STREAMOFF, &type);
        streaming_ = false;
    }

    // 解除内存映射
    for (auto& buffer : buffers_) {
        if (buffer.data != nullptr) {
            munmap(buffer.data, buffer.length);
        }
    }

    buffers_.clear();

    // 关闭摄像头
    close(fd_);
    fd_ = -1;
}
