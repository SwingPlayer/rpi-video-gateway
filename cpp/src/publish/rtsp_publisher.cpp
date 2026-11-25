#include "publish/rtsp_publisher.hpp"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

} // namespace

std::vector<uint8_t> RtspPublisher::extractParameterSets(
    const uint8_t* data,
    std::size_t size
) {
    std::vector<uint8_t> parameters;

    if (!data || size < 5)
        return parameters;

    bool hasSps = false;
    bool hasPps = false;

    auto current = findStartCode(data, size, 0);

    while (current.first < size) {
        std::size_t nalBegin =
            current.first + current.second;

        auto next = findStartCode(
            data, size, nalBegin
        );

        std::size_t nalEnd = next.first;

        if (nalBegin < nalEnd) {
            uint8_t nalType = data[nalBegin] & 0x1F;

            if (nalType == 7 || nalType == 8) {
                const uint8_t startCode[4] = {
                    0, 0, 0, 1
                };

                parameters.insert(
                    parameters.end(),
                    startCode,
                    startCode + 4
                );

                parameters.insert(
                    parameters.end(),
                    data + nalBegin,
                    data + nalEnd
                );

                if (nalType == 7)
                    hasSps = true;

                if (nalType == 8)
                    hasPps = true;
            }
        }

        if (next.first >= size)
            break;

        current = next;
    }

    if (!hasSps || !hasPps) {
        parameters.clear();
    }

    return parameters;
}

int RtspPublisher::interruptCallback(void* opaque) {
    auto* self = static_cast<RtspPublisher*>(opaque);

    if (!self)
        return 0;

    if (self->stop_requested_ &&
        self->stop_requested_->load()) {
        return 1;
    }

    if (std::chrono::steady_clock::now() >
        self->deadline_) {
        return 1;
    }

    return 0;
}

void RtspPublisher::setDeadline() {
    deadline_ =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(3);
}

RtspPublisher::RtspPublisher(
    const std::string& url,
    const AVCodecContext* encoderContext,
    std::atomic<bool>* stopRequested
)
    : url_(url),
      stop_requested_(stopRequested) {

    if (!encoderContext) {
        throw std::invalid_argument(
            "Null encoder context"
        );
    }

    if (encoderContext->codec_id != AV_CODEC_ID_H264) {
        throw std::invalid_argument(
            "RTSP publisher requires H264"
        );
    }

    try {
        source_time_base_ = encoderContext->time_base;

        checkResult(
            avformat_alloc_output_context2(
                &format_,
                nullptr,
                "rtsp",
                url_.c_str()
            ),
            "Creating RTSP output context"
        );

        if (!format_) {
            throw std::runtime_error(
                "RTSP output context unavailable"
            );
        }

        stream_ = avformat_new_stream(
            format_, nullptr
        );

        if (!stream_) {
            throw std::runtime_error(
                "Creating RTSP stream failed"
            );
        }

        checkResult(
            avcodec_parameters_from_context(
                stream_->codecpar,
                encoderContext
            ),
            "Copying H264 codec parameters"
        );

        stream_->codecpar->codec_tag = 0;
        stream_->time_base = source_time_base_;

        // FFmpeg 网络操作中断回调
        format_->interrupt_callback.callback =
            &RtspPublisher::interruptCallback;

        format_->interrupt_callback.opaque = this;

        setDeadline();

    } catch (...) {
        cleanup();
        throw;
    }
}

RtspPublisher::~RtspPublisher() {
    cleanup();
}

void RtspPublisher::start(
    const AVPacket* firstPacket
) {
    if (started_)
        return;

    if (stream_->codecpar->extradata_size == 0) {
        auto parameters = extractParameterSets(
            firstPacket->data,
            firstPacket->size
        );

        if (parameters.empty()) {
            throw std::runtime_error(
                "Missing H264 SPS/PPS in first packet"
            );
        }

        std::size_t size = parameters.size();

        stream_->codecpar->extradata =
            static_cast<uint8_t*>(
                av_mallocz(
                    size + AV_INPUT_BUFFER_PADDING_SIZE
                )
            );

        if (!stream_->codecpar->extradata) {
            throw std::runtime_error(
                "Allocating H264 extradata failed"
            );
        }

        std::memcpy(
            stream_->codecpar->extradata,
            parameters.data(),
            size
        );

        stream_->codecpar->extradata_size =
            static_cast<int>(size);
    }

    AVDictionary* options = nullptr;

    av_dict_set(
        &options,
        "rtsp_transport",
        "tcp",
        0
    );

    // 单次 TCP Socket I/O 超时：3秒
    av_dict_set(
        &options,
        "timeout",
        "3000000",
        0
    );

    setDeadline();

    int ret = avformat_write_header(
        format_, &options
    );

    av_dict_free(&options);

    checkResult(
        ret,
        "Connecting to MediaMTX"
    );

    started_ = true;

    std::cout
        << "[RTSP] Connected: "
        << url_ << "\n";
}

void RtspPublisher::publish(const AVPacket* packet) {
    if (!packet || !packet->data || packet->size <= 0) {
        throw std::invalid_argument(
            "Invalid H264 packet"
        );
    }

    if (!started_) {
        start(packet);
    }

    AVPacket* copy = av_packet_clone(packet);

    if (!copy) {
        throw std::runtime_error(
            "Cloning RTSP packet failed"
        );
    }

    try {
        if (copy->pts == AV_NOPTS_VALUE) {
            copy->pts =
                static_cast<int64_t>(published_packets_);
        }

        if (copy->dts == AV_NOPTS_VALUE) {
            copy->dts = copy->pts;
        }

        if (copy->duration <= 0)
            copy->duration = 1;

        av_packet_rescale_ts(
            copy,
            source_time_base_,
            stream_->time_base
        );

        copy->stream_index = stream_->index;
        copy->pos = -1;

        setDeadline();

        checkResult(
            av_interleaved_write_frame(
                format_, copy
            ),
            "Publishing H264 packet"
        );

        ++published_packets_;

    } catch (...) {
        av_packet_free(&copy);
        throw;
    }

    av_packet_free(&copy);
}

void RtspPublisher::close() {
    if (!format_)
        return;

    int ret = 0;

    if (started_) {
        setDeadline();

        ret = av_write_trailer(format_);
        started_ = false;
    }

    avformat_free_context(format_);
    format_ = nullptr;
    stream_ = nullptr;

    // 正在主动退出时忽略 TEARDOWN 中断
    if (ret < 0 &&
        !(stop_requested_ && stop_requested_->load())) {
        checkResult(ret, "Closing RTSP session");
    }
}

void RtspPublisher::cleanup() noexcept {
    if (!format_)
        return;

    if (started_) {
        setDeadline();
        av_write_trailer(format_);
        started_ = false;
    }

    avformat_free_context(format_);
    format_ = nullptr;
    stream_ = nullptr;
}