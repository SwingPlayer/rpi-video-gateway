"use strict";

(() => {
    const $ = (id) => document.getElementById(id);

    const state = {
        recording: null,
        busy: false,
        statusAvailable: false,
        refreshing: false,
    };

    let playbackVideo = null;
    let playbackContainer = null;

    // =====================================
    // HTTP API
    // =====================================

    async function request(
        path,
        method = "GET",
    ) {
        const response = await fetch(path, {
            method,
            credentials: "same-origin",
            cache: "no-store",
            headers: {
                "Accept": "application/json",
                "X-Requested-With": "XMLHttpRequest",
            },
        });

        if (response.status === 401) {
            window.location.assign("/login");
            throw new Error("登录已过期");
        }

        let data = {};

        try {
            data = await response.json();
        } catch (_) {
            // 非 JSON 响应保留 HTTP 状态码
        }

        if (!response.ok) {
            throw new Error(
                data.detail || `HTTP ${response.status}`
            );
        }

        return data;
    }

    // =====================================
    // 状态与格式化
    // =====================================

    function setMessage(
        message,
        type = "",
    ) {
        const element = $("recordMessage");

        element.textContent = message;
        element.className = "action-message";

        if (type === "success") {
            element.classList.add("is-success");
        }

        if (type === "error") {
            element.classList.add("is-error");
        }
    }

    function formatBytes(bytes) {
        const value = Number(bytes);

        if (!Number.isFinite(value)) {
            return "—";
        }

        if (value >= 1024 ** 3) {
            return (
                (value / 1024 ** 3).toFixed(2)
                + " GiB"
            );
        }

        return (
            (value / 1024 ** 2).toFixed(1)
            + " MiB"
        );
    }

    function formatDuration(seconds) {
        const total = Math.max(
            0,
            Math.round(Number(seconds) || 0)
        );

        const hours = Math.floor(total / 3600);
        const minutes = Math.floor(
            (total % 3600) / 60
        );
        const remaining = total % 60;

        if (hours > 0) {
            return `${hours}小时${minutes}分${remaining}秒`;
        }

        if (minutes > 0) {
            return `${minutes}分${remaining}秒`;
        }

        return `${remaining}秒`;
    }

    function updateControls() {
        const ready =
            state.statusAvailable &&
            !state.busy;

        $("recordStartBtn").disabled =
            !ready || state.recording === true;

        $("recordStopBtn").disabled =
            !ready || state.recording !== true;
    }

    function renderStatus(data) {
        state.statusAvailable = true;
        state.recording =
            data.recording_enabled === true;

        const badge = $("recordBadge");
        const indicator = $("recordIndicator");

        badge.textContent = state.recording
            ? "录制已开启"
            : "未录像";

        badge.className = state.recording
            ? "pill pill-danger"
            : "pill pill-neutral";

        indicator.classList.toggle(
            "is-active",
            state.recording
        );

        $("recordDisk").textContent =
            formatBytes(data.disk_free_bytes);

        updateControls();
    }

    async function refreshStatus() {
        try {
            const data = await request(
                "/api/recordings/status"
            );

            renderStatus(data);

        } catch (error) {
            state.statusAvailable = false;
            state.recording = null;

            $("recordBadge").textContent =
                "录像服务不可用";

            $("recordBadge").className =
                "pill pill-neutral";

            $("recordIndicator").classList.remove(
                "is-active"
            );

            updateControls();

            console.warn(
                "Recording status unavailable:",
                error
            );
        }
    }

    // =====================================
    // 创建回放播放器
    // =====================================

    function initializePlayback() {
        const style = document.createElement("style");

        style.textContent = `
            .recording-preview {
                display: none;
                margin-top: 20px;
                padding: 18px;
                border: 1px solid #344665;
                border-radius: 12px;
                background: #101a2c;
            }

            .recording-preview.is-visible {
                display: block;
            }

            .recording-preview-header {
                display: flex;
                align-items: center;
                justify-content: space-between;
                gap: 12px;
                margin-bottom: 14px;
            }

            .recording-preview video {
                display: block;
                width: 100%;
                max-height: 520px;
                aspect-ratio: 16 / 9;
                background: #050a12;
                border-radius: 9px;
            }

            .recording-preview-note {
                margin-top: 12px;
                color: #96a5c2;
                font-size: 12px;
                line-height: 1.6;
            }

            .recording-item-actions {
                display: flex;
                flex-wrap: wrap;
                align-items: center;
                gap: 8px;
            }
        `;

        document.head.appendChild(style);

        playbackContainer =
            document.createElement("div");

        playbackContainer.className =
            "recording-preview";

        const header =
            document.createElement("div");

        header.className =
            "recording-preview-header";

        const heading =
            document.createElement("strong");

        heading.textContent =
            "历史录像回放";

        const closeButton =
            document.createElement("button");

        closeButton.type = "button";
        closeButton.className =
            "btn btn-secondary";

        closeButton.textContent =
            "关闭回放";

        closeButton.addEventListener(
            "click",
            closePlayback
        );

        header.append(
            heading,
            closeButton
        );

        playbackVideo =
            document.createElement("video");

        playbackVideo.controls = true;
        playbackVideo.playsInline = true;
        playbackVideo.preload = "metadata";

        const note =
            document.createElement("p");

        note.className =
            "recording-preview-note";

        note.textContent =
            "录像由 MediaMTX 导出后在线播放。"
            + "大型录像可能需要等待加载，"
            + "部分浏览器的进度跳转功能可能受限。";

        playbackVideo.addEventListener(
            "error",
            () => {
                setMessage(
                    "录像回放失败。请检查该记录是否已完成，"
                    + "或尝试下载 MP4 后使用 VLC 播放。",
                    "error"
                );
            }
        );

        playbackContainer.append(
            header,
            playbackVideo,
            note
        );

        $("recordList").insertAdjacentElement(
            "afterend",
            playbackContainer
        );
    }

    function closePlayback() {
        if (!playbackVideo) {
            return;
        }

        playbackVideo.pause();

        playbackVideo.removeAttribute(
            "src"
        );

        playbackVideo.load();

        playbackContainer.classList.remove(
            "is-visible"
        );
    }

    function playRecording(recording) {
        if (
            !recording ||
            !recording.start ||
            !Number.isFinite(
                Number(recording.duration)
            )
        ) {
            setMessage(
                "录像记录无效，无法回放。",
                "error"
            );
            return;
        }

        closePlayback();

        const query = new URLSearchParams({
            start: recording.start,
            duration: String(recording.duration),
        });

        playbackVideo.src =
            "/api/recordings/play?"
            + query.toString();

        playbackContainer.classList.add(
            "is-visible"
        );

        playbackVideo.load();

        playbackContainer.scrollIntoView({
            behavior: "smooth",
            block: "center",
        });

        setMessage(
            "已打开历史录像播放器。"
            + "如有加载延迟，请稍候或使用下载功能。",
            "success"
        );
    }

    // =====================================
    // 录像列表
    // =====================================

    function createRecordingItem(recording) {
        const row =
            document.createElement("div");

        row.className =
            "recording-item";

        const details =
            document.createElement("div");

        const title =
            document.createElement("div");

        title.className =
            "recording-title";

        const startTime = new Date(
            recording.start
        );

        title.textContent =
            "录像时间：" +
            (
                Number.isNaN(startTime.getTime())
                    ? recording.start
                    : startTime.toLocaleString("zh-CN")
            );

        const duration =
            document.createElement("div");

        duration.className =
            "recording-details";

        duration.textContent =
            "录像时长：" +
            formatDuration(recording.duration);

        details.append(
            title,
            duration
        );

        const actions =
            document.createElement("div");

        actions.className =
            "recording-item-actions";

        const playButton =
            document.createElement("button");

        playButton.type = "button";
        playButton.className =
            "btn btn-secondary";

        playButton.textContent =
            "▶ 在线播放";

        playButton.addEventListener(
            "click",
            () => playRecording(recording)
        );

        const download =
            document.createElement("a");

        download.className =
            "recording-download";

        download.textContent =
            "下载 MP4";

        download.href =
            recording.download_url;

        actions.append(
            playButton,
            download
        );

        row.append(
            details,
            actions
        );

        return row;
    }

    async function refreshList() {
        const container = $("recordList");

        try {
            const data = await request(
                "/api/recordings/list?limit=30"
            );

            container.replaceChildren();

            if (!data.recordings.length) {
                const empty =
                    document.createElement("div");

                empty.className =
                    "recording-empty";

                empty.textContent =
                    "暂无已完成的录像";

                container.appendChild(empty);
                return;
            }

            for (const recording of data.recordings) {
                container.appendChild(
                    createRecordingItem(recording)
                );
            }

        } catch (error) {
            container.replaceChildren();

            const message =
                document.createElement("div");

            message.className =
                "recording-empty";

            message.textContent =
                "读取录像列表失败：" +
                error.message;

            container.appendChild(message);
        }
    }

    // =====================================
    // 启动 / 停止录像
    // =====================================

    async function controlRecording(action) {
        if (
            state.busy ||
            !state.statusAvailable
        ) {
            return;
        }

        state.busy = true;
        updateControls();

        const actionName =
            action === "start"
                ? "开始"
                : "停止";

        setMessage(
            `正在${actionName}录像……`
        );

        try {
            const result = await request(
                `/api/recordings/${action}`,
                "POST"
            );

            renderStatus(result);

            setMessage(
                action === "start"
                    ? "录像已开启。安全守护程序会监控磁盘空间和录制时长。"
                    : "录像停止命令已完成，正在刷新录像列表。",
                "success"
            );

            await refreshList();

        } catch (error) {
            setMessage(
                `${actionName}录像失败：${error.message}`,
                "error"
            );

        } finally {
            state.busy = false;

            await refreshStatus();
            updateControls();
        }
    }

    // =====================================
    // 自动刷新
    // =====================================

    async function refreshAll() {
        if (
            state.refreshing ||
            state.busy
        ) {
            return;
        }

        state.refreshing = true;

        try {
            await Promise.all([
                refreshStatus(),
                refreshList(),
            ]);

        } finally {
            state.refreshing = false;
        }
    }

    document.addEventListener(
        "DOMContentLoaded",
        () => {
            initializePlayback();

            $("recordStartBtn").addEventListener(
                "click",
                () => controlRecording("start")
            );

            $("recordStopBtn").addEventListener(
                "click",
                () => controlRecording("stop")
            );

            $("recordRefreshBtn").addEventListener(
                "click",
                refreshList
            );

            refreshAll();

            setInterval(
                refreshAll,
                5000
            );
        }
    );
})();