#include "ipc/metrics_server.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <utility>

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

std::runtime_error systemError(
    const std::string& action
) {
    return std::runtime_error(
        action + ": " + std::strerror(errno)
    );
}

} // namespace

MetricsServer::MetricsServer(
    std::string socketPath,
    SnapshotCallback callback
)
    : socket_path_(std::move(socketPath)),
      snapshot_callback_(std::move(callback)) {

    if (socket_path_.empty() || !snapshot_callback_) {
        throw std::invalid_argument(
            "Invalid metrics server configuration"
        );
    }
}

MetricsServer::~MetricsServer() {
    stop();
}

void MetricsServer::start() {
    if (listen_fd_ >= 0) {
        throw std::runtime_error(
            "Metrics server already started"
        );
    }

    if (worker_.joinable()) {
        throw std::runtime_error(
            "Metrics worker still running"
        );
    }

    sockaddr_un address{};

    if (socket_path_.size() >= sizeof(address.sun_path)) {
        throw std::runtime_error(
            "Unix socket path too long"
        );
    }

    std::filesystem::path path(socket_path_);

    if (path.has_parent_path()) {
        std::filesystem::create_directories(
            path.parent_path()
        );
    }

    // 检测旧 socket。若已有活跃服务，拒绝覆盖。
    struct stat existing{};

    if (::lstat(socket_path_.c_str(), &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode)) {
            throw std::runtime_error(
                "Metrics path exists but is not a socket"
            );
        }

        int probe = ::socket(
            AF_UNIX,
            SOCK_STREAM | SOCK_CLOEXEC,
            0
        );

        if (probe < 0) {
            throw systemError(
                "Creating socket probe"
            );
        }

        sockaddr_un probeAddress{};
        probeAddress.sun_family = AF_UNIX;

        std::strncpy(
            probeAddress.sun_path,
            socket_path_.c_str(),
            sizeof(probeAddress.sun_path) - 1
        );

        int result = ::connect(
            probe,
            reinterpret_cast<sockaddr*>(&probeAddress),
            sizeof(probeAddress)
        );

        int savedErrno = errno;
        ::close(probe);

        if (result == 0) {
            throw std::runtime_error(
                "Another metrics server is running"
            );
        }

        if (savedErrno != ECONNREFUSED &&
            savedErrno != ENOENT) {
            throw std::runtime_error(
                "Cannot verify existing metrics socket"
            );
        }

        if (::unlink(socket_path_.c_str()) != 0 &&
            errno != ENOENT) {
            throw systemError(
                "Removing stale metrics socket"
            );
        }
    }

    listen_fd_ = ::socket(
        AF_UNIX,
        SOCK_STREAM | SOCK_CLOEXEC,
        0
    );

    if (listen_fd_ < 0) {
        throw systemError(
            "Creating metrics socket"
        );
    }

    address.sun_family = AF_UNIX;

    std::strncpy(
        address.sun_path,
        socket_path_.c_str(),
        sizeof(address.sun_path) - 1
    );

    if (::bind(
            listen_fd_,
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)
        ) != 0) {
        int error = errno;
        ::close(listen_fd_);
        listen_fd_ = -1;
        errno = error;

        throw systemError(
            "Binding metrics socket"
        );
    }

    // 仅允许 socket 所有者访问。
    // C++ Gateway 与 FastAPI 都使用 kai 用户。
    if (::chmod(socket_path_.c_str(), 0600) != 0) {
        int error = errno;
        stop();
        errno = error;

        throw systemError(
            "Setting socket permissions"
        );
    }

    if (::listen(listen_fd_, 8) != 0) {
        int error = errno;
        stop();
        errno = error;

        throw systemError(
            "Listening on metrics socket"
        );
    }

    stop_requested_.store(false);

    try {
        worker_ = std::thread(
            &MetricsServer::run,
            this
        );
    } catch (...) {
        stop();
        throw;
    }

    std::cout
        << "[METRICS] Unix socket ready: "
        << socket_path_ << "\n";
}

void MetricsServer::run() noexcept {
    while (!stop_requested_.load()) {
        pollfd descriptor{};

        descriptor.fd = listen_fd_;
        descriptor.events = POLLIN;

        int result = ::poll(
            &descriptor,
            1,
            250
        );

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            std::cerr
                << "[METRICS] poll failed: "
                << std::strerror(errno) << "\n";
            break;
        }

        if (result == 0) {
            continue;
        }

        if (!(descriptor.revents & POLLIN)) {
            continue;
        }

        int client = ::accept4(
            listen_fd_,
            nullptr,
            nullptr,
            SOCK_CLOEXEC | SOCK_NONBLOCK
        );

        if (client < 0) {
            if (errno == EINTR ||
                errno == EAGAIN ||
                errno == EWOULDBLOCK) {
                continue;
            }

            std::cerr
                << "[METRICS] accept failed: "
                << std::strerror(errno) << "\n";
            continue;
        }

        try {
            std::string data = snapshot_callback_();
            data += "\n";

            std::size_t offset = 0;

            while (offset < data.size()) {
                ssize_t sent = ::send(
                    client,
                    data.data() + offset,
                    data.size() - offset,
                    MSG_NOSIGNAL | MSG_DONTWAIT
                );

                if (sent > 0) {
                    offset += static_cast<std::size_t>(sent);
                    continue;
                }

                if (sent < 0 && errno == EINTR) {
                    continue;
                }

                // 客户端不读取时不阻塞视频网关。
                break;
            }

        } catch (const std::exception& e) {
            std::cerr
                << "[METRICS] Snapshot error: "
                << e.what() << "\n";
        } catch (...) {
            std::cerr
                << "[METRICS] Unknown snapshot error\n";
        }

        ::close(client);
    }
}

void MetricsServer::stop() noexcept {
    stop_requested_.store(true);

    if (worker_.joinable()) {
        worker_.join();
    }

    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;

        ::unlink(socket_path_.c_str());
    }
}