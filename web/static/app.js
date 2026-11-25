"use strict";

const $ = (id) => document.getElementById(id);

const state = {
    host: "",
    streamPath: "cam_cpp",
    mode: "webrtc",

    gatewayRunning: null,
    mediamtxRunning: null,
    apiReachable: false,

    currentVideoUrl: "",
    busy: false,
    polling: false,
};

function validHost(value) {
    if (!value || value.length > 253) {
        return false;
    }

    if (value.includes("..")) {
        return false;
    }

    return /^[a-zA-Z0-9][a-zA-Z0-9.-]*$/.test(value);
}

function setMessage(message, type = "") {
    const element = $("actionMessage");

    element.textContent = message;
    element.className = "action-message";

    if (type === "success") {
        element.classList.add("is-success");
    }

    if (type === "error") {
        element.classList.add("is-error");
    }
}

async function apiRequest(path, method = "GET") {
    const response = await fetch(path, {
        method,
        cache: "no-store",
        headers: {
            "Accept": "application/json",
            "X-Requested-With": "XMLHttpRequest",
        },
    });

    let result = {};

    try {
        result = await response.json();
    } catch (_) {
        // 服务返回非 JSON 内容时，保留 HTTP 状态
    }

    if (!response.ok) {
        throw new Error(
            result.detail ||
            `HTTP ${response.status}`
        );
    }

    return result;
}

function setApiStatus(online) {
    state.apiReachable = online;

    const badge = $("apiBadge");

    badge.textContent = online
        ? "API ONLINE"
        : "API OFFLINE";

    badge.className = online
        ? "pill pill-success"
        : "pill pill-danger";

    updateButtons();
}

function setServiceStatus(id, running) {
    const element = $(id);

    if (running === null) {
        element.textContent = "未知";
        element.className = "pill pill-neutral";
        return;
    }

    element.textContent = running
        ? "运行中"
        : "已停止";

    element.className = running
        ? "pill pill-success"
        : "pill pill-danger";
}

function updateButtons() {
    const ready = state.apiReachable && !state.busy;

    $("startBtn").disabled =
        !ready || state.gatewayRunning === true;

    $("stopBtn").disabled =
        !ready || state.gatewayRunning === false;
}

function makeStreamUrl(mode = state.mode) {
    if (!validHost(state.host)) {
        return "";
    }

    const port = mode === "webrtc"
        ? 8889
        : 8888;

    return (
        `http://${state.host}:${port}/` +
        `${state.streamPath}` +
        "?autoplay=true&muted=true&controls=true"
    );
}

function makeRtspUrl() {
    if (!validHost(state.host)) {
        return "";
    }

    return (
        `rtsp://${state.host}:8554/` +
        state.streamPath
    );
}

function updateStreamLinks() {
    const webrtc = makeStreamUrl("webrtc");
    const hls = makeStreamUrl("hls");
    const rtsp = makeRtspUrl();

    $("webrtcUrl").textContent =
        webrtc || "请设置树莓派 IP";

    $("hlsUrl").textContent =
        hls || "请设置树莓派 IP";

    $("rtspUrl").textContent =
        rtsp || "请设置树莓派 IP";

    $("viewerUrl").textContent =
        makeStreamUrl() || "请设置树莓派 IP";

    $("copyRtspBtn").disabled = !rtsp;
}

function renderPlayer(forceReload = false) {
    updateStreamLinks();

    const available =
        state.gatewayRunning === true &&
        state.mediamtxRunning === true &&
        validHost(state.host);

    const target = available
        ? makeStreamUrl()
        : "about:blank";

    const frame = $("streamFrame");
    const placeholder = $("videoPlaceholder");

    $("openVideoBtn").disabled = !available;
    $("reloadVideoBtn").disabled = !available;

    placeholder.hidden = available;

    if (!available) {
        frame.src = "about:blank";
        state.currentVideoUrl = "about:blank";

        const title = placeholder.querySelector("strong");
        const description = placeholder.querySelector("p");

        if (!validHost(state.host)) {
            title.textContent = "尚未设置树莓派 IP";
            description.textContent =
                "请在右侧填写视频服务器地址";
        } else if (state.gatewayRunning === false) {
            title.textContent = "摄像头已停止";
            description.textContent =
                "点击启动摄像头以恢复直播";
        } else if (state.mediamtxRunning === false) {
            title.textContent = "MediaMTX 未运行";
            description.textContent =
                "请检查流媒体服务器状态";
        } else {
            title.textContent = "正在检查视频服务";
            description.textContent =
                "等待服务状态更新";
        }

        return;
    }

    if (!forceReload && state.currentVideoUrl === target) {
        return;
    }

    state.currentVideoUrl = target;

    if (forceReload) {
        frame.src = "about:blank";

        setTimeout(() => {
            const stillAvailable =
                state.gatewayRunning === true &&
                state.mediamtxRunning === true;

            if (
                stillAvailable &&
                state.currentVideoUrl === target
            ) {
                frame.src = target;
            }
        }, 100);
    } else {
        frame.src = target;
    }
}

function applyServiceStatus(data) {
    const oldGateway = state.gatewayRunning;
    const oldMediamtx = state.mediamtxRunning;

    const gateway = data.gateway || {};
    const mediamtx = data.mediamtx || {};

    state.gatewayRunning = gateway.running === true;
    state.mediamtxRunning = mediamtx.running === true;

    setServiceStatus(
        "gatewayStatus",
        state.gatewayRunning
    );

    setServiceStatus(
        "mediamtxStatus",
        state.mediamtxRunning
    );

    $("gatewayPid").textContent =
        `PID: ${gateway.main_pid || "—"}`;

    $("mediamtxPid").textContent =
        `PID: ${mediamtx.main_pid || "—"}`;

    $("lastUpdated").textContent =
        new Date().toLocaleTimeString(
            "zh-CN",
            { hour12: false }
        );

    updateButtons();

    const changed =
        oldGateway !== state.gatewayRunning ||
        oldMediamtx !== state.mediamtxRunning;

    if (changed) {
        renderPlayer(true);
    }
}

async function refreshStatus() {
    if (state.polling) {
        return;
    }

    state.polling = true;

    try {
        const data = await apiRequest(
            "/api/camera/status"
        );

        setApiStatus(true);
        applyServiceStatus(data);

    } catch (error) {
        setApiStatus(false);

        $("lastUpdated").textContent =
            "连接异常";

        console.error(
            "Failed to get service status:",
            error
        );
    } finally {
        state.polling = false;
    }
}

async function controlCamera(action) {
    if (state.busy || !state.apiReachable) {
        return;
    }

    state.busy = true;
    updateButtons();

    const actionText = action === "start"
        ? "启动"
        : "停止";

    setMessage(
        `正在请求${actionText}摄像头...`
    );

    try {
        const data = await apiRequest(
            `/api/camera/${action}`,
            "POST"
        );

        setApiStatus(true);

        if (data.services) {
            applyServiceStatus(data.services);
        }

        setMessage(
            `${actionText}命令执行成功。`,
            "success"
        );

        // 重新启动后等待 RTSP 发布端恢复，
        // 再刷新 iframe，减少首次无画面的概率。
        if (action === "start") {
            setTimeout(async () => {
                await refreshStatus();
                renderPlayer(true);
            }, 2000);
        } else {
            renderPlayer(true);
        }

    } catch (error) {
        setMessage(
            `${actionText}失败：${error.message}`,
            "error"
        );
    } finally {
        state.busy = false;
        updateButtons();
    }
}

function applyHost() {
    const host = $("serverHost").value.trim();

    if (!validHost(host)) {
        setMessage(
            "请输入正确的 IP 或主机名，不要包含 http:// 或端口号。",
            "error"
        );
        return;
    }

    state.host = host;

    try {
        localStorage.setItem(
            "rpiGatewayHost",
            host
        );
    } catch (_) {
        // 浏览器禁用本地存储时不影响使用
    }

    renderPlayer(true);

    setMessage(
        `已应用视频服务器地址：${host}`,
        "success"
    );
}

async function loadUiConfig() {
    let savedHost = "";

    try {
        savedHost =
            localStorage.getItem("rpiGatewayHost") || "";
    } catch (_) {
        savedHost = "";
    }

    try {
        const config = await apiRequest(
            "/api/ui/config"
        );

        state.streamPath =
            config.stream_path || "cam_cpp";

        const suggested =
            config.suggested_host || "";

        state.host = validHost(savedHost)
            ? savedHost
            : suggested;

    } catch (error) {
        if (validHost(savedHost)) {
            state.host = savedHost;
        }

        console.warn(
            "Unable to load UI config:",
            error
        );
    }

    $("serverHost").value = state.host;

    updateStreamLinks();
}

async function copyRtspUrl() {
    const url = makeRtspUrl();

    if (!url) {
        setMessage(
            "请先配置树莓派 IP 地址。",
            "error"
        );
        return;
    }

    try {
        await navigator.clipboard.writeText(url);

        setMessage(
            "RTSP 地址已复制。",
            "success"
        );

    } catch (_) {
        setMessage(
            "复制失败，请手动选择并复制右侧 RTSP 地址。",
            "error"
        );
    }
}

function bindEvents() {
    $("startBtn").addEventListener(
        "click",
        () => controlCamera("start")
    );

    $("stopBtn").addEventListener(
        "click",
        () => controlCamera("stop")
    );

    $("reloadVideoBtn").addEventListener(
        "click",
        () => renderPlayer(true)
    );

    $("saveHostBtn").addEventListener(
        "click",
        applyHost
    );

    $("serverHost").addEventListener(
        "keydown",
        (event) => {
            if (event.key === "Enter") {
                applyHost();
            }
        }
    );

    $("streamMode").addEventListener(
        "change",
        (event) => {
            state.mode = event.target.value;
            renderPlayer(true);
        }
    );

    $("openVideoBtn").addEventListener(
        "click",
        () => {
            const url = makeStreamUrl();

            if (url) {
                window.open(
                    url,
                    "_blank",
                    "noopener,noreferrer"
                );
            }
        }
    );

    $("copyRtspBtn").addEventListener(
        "click",
        copyRtspUrl
    );
}

async function initialize() {
    state.mode = $("streamMode").value;

    bindEvents();
    updateButtons();

    await loadUiConfig();
    await refreshStatus();

    renderPlayer();

    // 只刷新服务状态，不反复重载视频播放器
    setInterval(
        refreshStatus,
        3000
    );
}

document.addEventListener(
    "DOMContentLoaded",
    initialize
);