"use strict";

(() => {
    const $ = (id) => document.getElementById(id);

    const numberFormat = new Intl.NumberFormat("zh-CN");

    let polling = false;

    function formatNumber(value) {
        const number = Number(value);

        if (!Number.isFinite(number)) {
            return "—";
        }

        return numberFormat.format(number);
    }

    function formatFps(value) {
        const number = Number(value);

        if (!Number.isFinite(number)) {
            return "—";
        }

        return number.toFixed(2);
    }

    function formatDuration(seconds) {
        const value = Math.max(
            0,
            Math.floor(Number(seconds) || 0)
        );

        const hours = Math.floor(value / 3600);
        const minutes = Math.floor(
            (value % 3600) / 60
        );
        const remaining = value % 60;

        if (hours > 0) {
            return `${hours}时${minutes}分${remaining}秒`;
        }

        if (minutes > 0) {
            return `${minutes}分${remaining}秒`;
        }

        return `${remaining}秒`;
    }

    function setBadge(label, type) {
        const badge = $("metricsBadge");

        badge.textContent = label;

        badge.className = (
            type === "success"
                ? "pill pill-success"
                : type === "danger"
                    ? "pill pill-danger"
                    : "pill pill-neutral"
        );
    }

    function setText(id, value) {
        const element = $(id);

        if (element) {
            element.textContent = value;
        }
    }

    function resetMetrics(message) {
        const fields = [
            "metricCaptureFps",
            "metricEncodeFps",
            "metricCaptured",
            "metricPublished",
            "metricMjpegDropped",
            "metricH264Dropped",
            "metricReconnects",
            "metricNetworkErrors",
            "metricSyncDropped",
            "metricUptime",
        ];

        for (const field of fields) {
            setText(field, "—");
        }

        setBadge("暂不可用", "danger");
        setText("metricsUpdated", message);
    }

    function renderMetrics(metrics) {
        const ready = metrics.sample_ready === true;

        setBadge(
            metrics.running === true
                ? "实时连接"
                : "正在停止",
            metrics.running === true
                ? "success"
                : "neutral"
        );

        setText(
            "metricCaptureFps",
            ready
                ? formatFps(metrics.capture_fps)
                : "采样中"
        );

        setText(
            "metricEncodeFps",
            ready
                ? formatFps(metrics.encode_fps)
                : "采样中"
        );

        setText(
            "metricCaptured",
            formatNumber(metrics.captured_frames)
        );

        setText(
            "metricPublished",
            formatNumber(metrics.published_packets)
        );

        setText(
            "metricMjpegDropped",
            formatNumber(metrics.mjpeg_dropped)
        );

        setText(
            "metricH264Dropped",
            formatNumber(metrics.h264_dropped)
        );

        setText(
            "metricReconnects",
            formatNumber(metrics.rtsp_reconnections)
        );

        setText(
            "metricNetworkErrors",
            formatNumber(metrics.network_errors)
        );

        setText(
            "metricSyncDropped",
            formatNumber(metrics.sync_discards)
        );

        setText(
            "metricUptime",
            formatDuration(metrics.uptime_seconds)
        );

        const now = new Date();

        const updateTime = now.toLocaleTimeString(
            "zh-CN",
            { hour12: false }
        );

        const sampleSeconds =
            Number(metrics.sample_window_ms) / 1000;

        const sampleDescription = ready
            ? `最近 ${sampleSeconds.toFixed(1)} 秒采样`
            : "等待首次 FPS 采样";

        setText(
            "metricsUpdated",
            `最后刷新：${updateTime} · ${sampleDescription}`
        );
    }

    async function refreshMetrics() {
        if (polling) {
            return;
        }

        polling = true;

        try {
            const response = await fetch(
                "/api/camera/metrics",
                {
                    cache: "no-store",
                    credentials: "same-origin",
                    headers: {
                        "Accept": "application/json",
                    },
                }
            );

            if (response.status === 401) {
                resetMetrics("登录已失效，请重新登录。");
                setBadge("未登录", "danger");
                return;
            }

            if (!response.ok) {
                throw new Error(
                    `HTTP ${response.status}`
                );
            }

            const metrics = await response.json();

            if (!metrics.ipc_connected) {
                throw new Error(
                    "C++ IPC disconnected"
                );
            }

            renderMetrics(metrics);

        } catch (error) {
            resetMetrics(
                "C++ 运行指标暂不可用。"
                + "可能是摄像头服务已经停止或正在重启。"
            );

            console.warn(
                "Metrics polling failed:",
                error
            );

        } finally {
            polling = false;
        }
    }

    document.addEventListener(
        "DOMContentLoaded",
        () => {
            refreshMetrics();

            setInterval(
                refreshMetrics,
                3000
            );
        }
    );
})();