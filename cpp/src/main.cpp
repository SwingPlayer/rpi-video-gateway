#include "capture/v4l2_capture.hpp"
#include "codec/video_decoder.hpp"
#include "codec/video_encoder.hpp"
#include "ipc/metrics_server.hpp"
#include "pipeline/frame_queue.hpp"
#include "pipeline/h264_packet_queue.hpp"
#include "publish/rtsp_publisher.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

volatile std::sig_atomic_t g_stop = 0;

void signalHandler(int) {
    g_stop = 1;
}

std::pair<std::size_t, std::size_t> findStartCode(
    const uint8_t* data,
    std::size_t size,
    std::size_t from
) {
    for (std::size_t i = from; i + 2 < size; ++i) {
        if (data[i] != 0 || data[i + 1] != 0)
            continue;

        if (data[i + 2] == 1)
            return {i, 3};

        if (i + 3 < size &&
            data[i + 2] == 0 &&
            data[i + 3] == 1) {
            return {i, 4};
        }
    }

    return {size, 0};
}

class H264ParameterCache {
public:
    void update(const AVPacket* packet) {
        if (!packet || !packet->data ||
            packet->size <= 0) {
            return;
        }

        const uint8_t* data = packet->data;

        std::size_t size =
            static_cast<std::size_t>(packet->size);

        auto current = findStartCode(data, size, 0);

        while (current.first < size) {
            std::size_t begin =
                current.first + current.second;

            auto next = findStartCode(
                data, size, begin
            );

            if (begin < next.first) {
                int type = data[begin] & 0x1F;

                if (type == 7 || type == 8) {
                    std::vector<uint8_t> nal = {
                        0, 0, 0, 1
                    };

                    nal.insert(
                        nal.end(),
                        data + begin,
                        data + next.first
                    );

                    if (type == 7) {
                        sps_ = std::move(nal);
                    } else {
                        pps_ = std::move(nal);
                    }
                }
            }

            if (next.first >= size)
                break;

            current = next;
        }
    }

    bool ready() const {
        return !sps_.empty() && !pps_.empty();
    }

    bool isKeyFrame(const AVPacket* packet) const {
        if (!packet || !packet->data)
            return false;

        if (packet->flags & AV_PKT_FLAG_KEY)
            return true;

        const uint8_t* data = packet->data;
        std::size_t size =
            static_cast<std::size_t>(packet->size);

        auto current = findStartCode(data, size, 0);

        while (current.first < size) {
            std::size_t begin =
                current.first + current.second;

            if (begin < size &&
                (data[begin] & 0x1F) == 5) {
                return true;
            }

            current = findStartCode(
                data, size, begin
            );
        }

        return false;
    }

    H264PacketPtr makeStartPacket(
        const AVPacket* original
    ) const {
        if (!ready() || !original) {
            throw std::runtime_error(
                "H264 parameter sets unavailable"
            );
        }

        std::size_t totalSize =
            sps_.size() +
            pps_.size() +
            static_cast<std::size_t>(original->size);

        if (totalSize > INT_MAX) {
            throw std::runtime_error(
                "H264 start packet too large"
            );
        }

        AVPacket* raw = av_packet_alloc();

        if (!raw) {
            throw std::runtime_error(
                "Allocating H264 packet failed"
            );
        }

        H264PacketPtr packet(
            raw,
            [](AVPacket* p) {
                av_packet_free(&p);
            }
        );

        if (av_new_packet(
                raw,
                static_cast<int>(totalSize)
            ) < 0) {
            throw std::runtime_error(
                "Allocating start packet data failed"
            );
        }

        if (av_packet_copy_props(raw, original) < 0) {
            throw std::runtime_error(
                "Copying packet properties failed"
            );
        }

        uint8_t* dest = raw->data;

        std::memcpy(
            dest,
            sps_.data(),
            sps_.size()
        );

        dest += sps_.size();

        std::memcpy(
            dest,
            pps_.data(),
            pps_.size()
        );

        dest += pps_.size();

        std::memcpy(
            dest,
            original->data,
            original->size
        );

        raw->flags |= AV_PKT_FLAG_KEY;

        return packet;
    }

private:
    std::vector<uint8_t> sps_;
    std::vector<uint8_t> pps_;
};

} // namespace

int main() {
    constexpr int WIDTH = 1280;
    constexpr int HEIGHT = 720;
    constexpr int FPS = 30;
    constexpr int BITRATE = 3000000;

    constexpr std::size_t MJPEG_QUEUE_CAPACITY = 8;
    constexpr std::size_t H264_QUEUE_CAPACITY = 60;

    constexpr unsigned int CAPTURE_BATCH = 30;

    const std::string rtspUrl =
        "rtsp://127.0.0.1:8554/cam_cpp";

    const std::string metricsSocket =
        "runtime/data/gateway_metrics.sock";

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGPIPE, SIG_IGN);

    try {
        std::cout
            << "========================================\n"
            << " Raspberry Pi Video Gateway v0.7\n"
            << " Continuous Streaming + Live Metrics\n"
            << "========================================\n";

        const auto startedAt = Clock::now();

        VideoDecoder decoder;

        // 不保存连续 H264 录像，避免占满磁盘。
        VideoEncoder encoder(
            "/dev/null",
            WIDTH,
            HEIGHT,
            FPS,
            BITRATE
        );

        V4L2Capture camera(
            "/dev/video0",
            WIDTH,
            HEIGHT,
            FPS
        );

        camera.initialize();

        MjpegFrameQueue mjpegQueue(
            MJPEG_QUEUE_CAPACITY
        );

        H264PacketQueue h264Queue(
            H264_QUEUE_CAPACITY
        );

        std::atomic<bool> stopping{false};

        // 线程间共享的运行指标
        std::atomic<uint64_t> totalCaptured{0};
        std::atomic<uint64_t> totalValid{0};
        std::atomic<uint64_t> totalInvalid{0};
        std::atomic<uint64_t> totalTimeouts{0};

        std::atomic<uint64_t> decodeAttempts{0};
        std::atomic<uint64_t> decodedFrames{0};
        std::atomic<uint64_t> encodedPackets{0};
        std::atomic<uint64_t> publishedPackets{0};

        std::atomic<uint64_t> networkErrors{0};
        std::atomic<uint64_t> connections{0};

        std::atomic<uint64_t> syncDiscarded{0};
        std::atomic<uint64_t> shutdownDiscarded{0};

        // FPS 使用百分之一 FPS 为单位存储，
        // 避免多个线程直接读写普通浮点变量。
        std::atomic<int64_t> captureFps100{0};
        std::atomic<int64_t> encodeFps100{0};

        std::atomic<uint64_t> lastSampleMs{0};
        std::atomic<bool> sampleReady{false};

        std::exception_ptr workerError;
        std::exception_ptr captureError;
        std::exception_ptr networkFatal;

        // =====================================
        // Unix Socket：读取指标快照
        // =====================================

        MetricsServer metricsServer(
            metricsSocket,
            [&]() -> std::string {
                auto mjpegStats = mjpegQueue.stats();
                auto h264Stats = h264Queue.stats();

                auto uptime =
                    std::chrono::duration_cast<
                        std::chrono::seconds
                    >(
                        Clock::now() - startedAt
                    ).count();

                auto timestamp =
                    static_cast<long long>(
                        std::time(nullptr)
                    );

                std::ostringstream out;

                out << "{"
                    << "\"version\":1,"
                    << "\"running\":"
                    << (stopping.load() ? "false" : "true")
                    << ","
                    << "\"timestamp\":" << timestamp << ","
                    << "\"uptime_seconds\":" << uptime << ","
                    << "\"sample_ready\":"
                    << (sampleReady.load() ? "true" : "false")
                    << ","
                    << "\"sample_window_ms\":"
                    << lastSampleMs.load() << ","
                    << "\"capture_fps\":"
                    << std::fixed << std::setprecision(2)
                    << captureFps100.load() / 100.0 << ","
                    << "\"encode_fps\":"
                    << encodeFps100.load() / 100.0 << ","
                    << "\"captured_frames\":"
                    << totalCaptured.load() << ","
                    << "\"valid_mjpeg\":"
                    << totalValid.load() << ","
                    << "\"invalid_mjpeg\":"
                    << totalInvalid.load() << ","
                    << "\"capture_timeouts\":"
                    << totalTimeouts.load() << ","
                    << "\"decode_attempts\":"
                    << decodeAttempts.load() << ","
                    << "\"decoded_frames\":"
                    << decodedFrames.load() << ","
                    << "\"encoded_packets\":"
                    << encodedPackets.load() << ","
                    << "\"published_packets\":"
                    << publishedPackets.load() << ","
                    << "\"mjpeg_dropped\":"
                    << mjpegStats.dropped << ","
                    << "\"h264_dropped\":"
                    << h264Stats.dropped << ","
                    << "\"mjpeg_peak_depth\":"
                    << mjpegStats.peakDepth << ","
                    << "\"h264_peak_depth\":"
                    << h264Stats.peakDepth << ","
                    << "\"network_errors\":"
                    << networkErrors.load() << ","
                    << "\"rtsp_sessions\":"
                    << connections.load() << ","
                    << "\"rtsp_reconnections\":"
                    << (
                        connections.load() > 0
                        ? connections.load() - 1
                        : 0
                    ) << ","
                    << "\"sync_discards\":"
                    << syncDiscarded.load() << ","
                    << "\"shutdown_discards\":"
                    << shutdownDiscarded.load()
                    << "}";

                return out.str();
            }
        );

        metricsServer.start();

        encoder.setPacketCallback(
            [&](const AVPacket* packet) {
                if (!h264Queue.push(packet)) {
                    throw std::runtime_error(
                        "H264 packet queue closed"
                    );
                }

                ++encodedPackets;
            }
        );

        // =====================================
        // 线程 3：RTSP 网络发布
        // =====================================

        std::thread networkThread([&]() {
            try {
                std::unique_ptr<RtspPublisher> publisher;

                H264ParameterCache parameters;
                H264PacketPtr packet;

                auto nextRetry =
                    Clock::time_point::min();

                std::cout
                    << "[THREAD] RTSP worker started\n";

                while (h264Queue.pop(packet)) {
                    if (stopping.load()) {
                        ++shutdownDiscarded;
                        packet.reset();
                        continue;
                    }

                    parameters.update(packet.get());

                    if (!publisher) {
                        if (Clock::now() < nextRetry ||
                            !parameters.ready() ||
                            !parameters.isKeyFrame(
                                packet.get()
                            )) {
                            ++syncDiscarded;
                            packet.reset();
                            continue;
                        }

                        try {
                            publisher =
                                std::make_unique<RtspPublisher>(
                                    rtspUrl,
                                    encoder.codecContext(),
                                    &stopping
                                );

                            auto firstPacket =
                                parameters.makeStartPacket(
                                    packet.get()
                                );

                            publisher->publish(
                                firstPacket.get()
                            );

                            uint64_t session =
                                ++connections;

                            ++publishedPackets;

                            std::cout
                                << "[RTSP] Session "
                                << session
                                << " established\n";

                            if (session > 1) {
                                std::cout
                                    << "[RTSP] Reconnected\n";
                            }

                        } catch (const std::exception& e) {
                            ++networkErrors;

                            std::cerr
                                << "[RTSP] Connection failed: "
                                << e.what() << "\n";

                            publisher.reset();

                            nextRetry =
                                Clock::now() +
                                std::chrono::seconds(2);
                        }

                        packet.reset();
                        continue;
                    }

                    try {
                        publisher->publish(packet.get());
                        ++publishedPackets;

                    } catch (const std::exception& e) {
                        ++networkErrors;

                        std::cerr
                            << "[RTSP] Publish failed: "
                            << e.what() << "\n";

                        publisher.reset();

                        nextRetry =
                            Clock::now() +
                            std::chrono::seconds(2);
                    }

                    packet.reset();
                }

                if (publisher) {
                    try {
                        publisher->close();
                    } catch (const std::exception& e) {
                        std::cerr
                            << "[RTSP] Close warning: "
                            << e.what() << "\n";
                    }
                }

                std::cout
                    << "[THREAD] RTSP worker stopped\n";

            } catch (...) {
                networkFatal = std::current_exception();

                stopping.store(true);
                mjpegQueue.close();
                h264Queue.close();
            }
        });

        // =====================================
        // 线程 2：MJPEG 解码 + H264 编码
        // =====================================

        std::thread videoThread([&]() {
            try {
                std::cout
                    << "[THREAD] Video worker started\n";

                MjpegFrame frame;

                bool firstFrame = true;
                Clock::time_point startTime;

                int64_t previousPts = -1;

                while (mjpegQueue.pop(frame)) {
                    if (firstFrame) {
                        startTime = frame.capturedAt;
                        firstFrame = false;
                    }

                    double elapsed =
                        std::chrono::duration<double>(
                            frame.capturedAt - startTime
                        ).count();

                    int64_t pts =
                        static_cast<int64_t>(
                            std::llround(elapsed * FPS)
                        );

                    pts = std::max(
                        pts,
                        previousPts + 1
                    );

                    previousPts = pts;

                    decoder.decode(
                        frame.data.data(),
                        frame.data.size(),
                        [&](const AVFrame* decodedFrame) {
                            ++decodedFrames;

                            encoder.encode(
                                decodedFrame,
                                pts
                            );
                        }
                    );

                    ++decodeAttempts;
                    frame.data.clear();
                }

                encoder.finish();

                std::cout
                    << "[THREAD] Video worker stopped\n";

            } catch (...) {
                workerError = std::current_exception();

                stopping.store(true);
                mjpegQueue.close();
            }

            h264Queue.close();
        });

        // =====================================
        // 线程 1：V4L2 持续采集
        // =====================================

        auto lastReport = Clock::now();

        uint64_t lastReportCaptured = 0;
        uint64_t lastReportEncoded = 0;

        std::cout
            << "[INFO] Continuous capture started\n"
            << "[INFO] RTSP URL: " << rtspUrl << "\n"
            << "[INFO] Metrics socket: "
            << metricsSocket << "\n"
            << "[INFO] Press Ctrl+C to stop\n";

        try {
            while (!g_stop && !stopping.load()) {
                CaptureStats batch =
                    camera.captureFrames(
                        CAPTURE_BATCH,
                        [&](const uint8_t* data,
                            std::size_t size) {
                            if (!mjpegQueue.push(
                                    data, size
                                )) {
                                throw std::runtime_error(
                                    "MJPEG queue closed"
                                );
                            }
                        }
                    );

                totalCaptured.fetch_add(batch.frames);
                totalValid.fetch_add(batch.valid);
                totalInvalid.fetch_add(batch.invalid);
                totalTimeouts.fetch_add(batch.timeouts);

                auto now = Clock::now();

                double elapsed =
                    std::chrono::duration<double>(
                        now - lastReport
                    ).count();

                if (elapsed >= 10.0) {
                    uint64_t capturedNow =
                        totalCaptured.load();

                    uint64_t encodedNow =
                        encodedPackets.load();

                    double captureFps =
                        (capturedNow - lastReportCaptured)
                        / elapsed;

                    double encodeFps =
                        (encodedNow - lastReportEncoded)
                        / elapsed;

                    captureFps100.store(
                        static_cast<int64_t>(
                            std::llround(
                                captureFps * 100.0
                            )
                        )
                    );

                    encodeFps100.store(
                        static_cast<int64_t>(
                            std::llround(
                                encodeFps * 100.0
                            )
                        )
                    );

                    lastSampleMs.store(
                        static_cast<uint64_t>(
                            std::llround(elapsed * 1000.0)
                        )
                    );

                    sampleReady.store(true);

                    auto mjpegStats = mjpegQueue.stats();
                    auto h264Stats = h264Queue.stats();

                    std::cout
                        << "\n========== 10s STATUS ==========\n"
                        << "Capture FPS       : "
                        << std::fixed
                        << std::setprecision(2)
                        << captureFps << "\n"
                        << "Encode FPS        : "
                        << encodeFps << "\n"
                        << "Captured          : "
                        << capturedNow << "\n"
                        << "Encoded packets   : "
                        << encodedNow << "\n"
                        << "Published packets : "
                        << publishedPackets.load() << "\n"
                        << "MJPEG dropped     : "
                        << mjpegStats.dropped << "\n"
                        << "H264 dropped      : "
                        << h264Stats.dropped << "\n"
                        << "Network errors    : "
                        << networkErrors.load() << "\n"
                        << "RTSP sessions     : "
                        << connections.load() << "\n"
                        << "===============================\n";

                    lastReport = now;

                    lastReportCaptured = capturedNow;
                    lastReportEncoded = encodedNow;
                }
            }

        } catch (...) {
            captureError = std::current_exception();
        }

        // =====================================
        // 正常退出与资源清理
        // =====================================

        std::cout
            << "\n[STOP] Stopping video gateway...\n";

        stopping.store(true);
        mjpegQueue.close();

        if (videoThread.joinable()) {
            videoThread.join();
        }

        h264Queue.close();

        if (networkThread.joinable()) {
            networkThread.join();
        }

        metricsServer.stop();

        if (workerError) {
            std::rethrow_exception(workerError);
        }

        if (networkFatal) {
            std::rethrow_exception(networkFatal);
        }

        if (captureError) {
            std::rethrow_exception(captureError);
        }

        auto mjpegStats = mjpegQueue.stats();
        auto h264Stats = h264Queue.stats();

        std::cout
            << "\n========================================\n"
            << "      Final Gateway Statistics\n"
            << "========================================\n"
            << "Captured frames   : "
            << totalCaptured.load() << "\n"
            << "Valid MJPEG       : "
            << totalValid.load() << "\n"
            << "Invalid MJPEG     : "
            << totalInvalid.load() << "\n"
            << "Capture timeouts  : "
            << totalTimeouts.load() << "\n"
            << "Decoded frames    : "
            << decodedFrames.load() << "\n"
            << "Encoded packets   : "
            << encodedPackets.load() << "\n"
            << "RTSP packets      : "
            << publishedPackets.load() << "\n"
            << "MJPEG queue drops : "
            << mjpegStats.dropped << "\n"
            << "H264 queue drops  : "
            << h264Stats.dropped << "\n"
            << "Sync discards     : "
            << syncDiscarded.load() << "\n"
            << "Shutdown discards : "
            << shutdownDiscarded.load() << "\n"
            << "Network errors    : "
            << networkErrors.load() << "\n"
            << "RTSP sessions     : "
            << connections.load() << "\n"
            << "========================================\n";

        std::cout
            << "[EXIT] Video gateway stopped cleanly\n";

        return 0;

    } catch (const std::exception& e) {
        std::cerr
            << "[ERROR] "
            << e.what() << "\n";

        return 1;
    }
}