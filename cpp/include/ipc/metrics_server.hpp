#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

class MetricsServer {
public:
    using SnapshotCallback =
        std::function<std::string()>;

    MetricsServer(
        std::string socketPath,
        SnapshotCallback callback
    );

    ~MetricsServer();

    MetricsServer(const MetricsServer&) = delete;
    MetricsServer& operator=(const MetricsServer&) = delete;

    void start();
    void stop() noexcept;

private:
    void run() noexcept;

    std::string socket_path_;
    SnapshotCallback snapshot_callback_;

    int listen_fd_ = -1;

    std::atomic<bool> stop_requested_{false};
    std::thread worker_;
};