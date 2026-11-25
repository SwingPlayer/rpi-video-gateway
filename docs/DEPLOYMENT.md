# 部署与运行说明

> 适用于已有工程 `~/projects/rpi_video_gateway`，设备为 Raspberry Pi 4B（Linux）。以下用户名 `kai`、绝对路径和端口配置来自本次实验；移植时必须按实际环境调整。本文是部署说明，不会修改现有程序。

## 1. 先确认基础硬件与工具

```bash
uname -a
cat /etc/os-release
ls -l /dev/video0 /dev/video11
v4l2-ctl -d /dev/video0 --list-formats-ext
ffmpeg -hide_banner -encoders | grep h264_v4l2m2m
```

原实验中 `/dev/video0` 为 USB UVC 摄像头；`/dev/video11` 为 Pi 4B 的 `bcm2835-codec` H.264 编码设备。设备编号会随系统/驱动变化，不能假设所有 Raspberry Pi 都相同。

安装构建依赖（已经安装时可以跳过）：

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake pkg-config \
  libavcodec-dev libavutil-dev libavformat-dev libswscale-dev \
  ffmpeg v4l-utils python3-venv unzip
```

## 2. 编译 C++ 服务

**先停止正在运行的服务再替换可执行程序**：

```bash
cd ~/projects/rpi_video_gateway
sudo systemctl stop rpi-video-gateway.service
cmake -S cpp -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
```

可执行程序：`build/v4l2_capture_test`（历史命名，实际是长期运行网关）。编译通过后由 systemd 启动，不要与手动运行实例同时占用摄像头。

## 3. 安装 Python 依赖

```bash
cd ~/projects/rpi_video_gateway
python3 -m venv python/.venv
python/.venv/bin/python -m pip install -r python/requirements.txt
```

如虚拟环境已存在，通常只需执行最后一条安装命令。

## 4. MediaMTX 安装与配置

本项目使用已经下载并部署到 `mediamtx/bin/mediamtx` 的 arm64 MediaMTX。请检查：

```bash
cd ~/projects/rpi_video_gateway
./mediamtx/bin/mediamtx --version
```

现有工程同时保留：

- `mediamtx/mediamtx.yml`：原始配置；
- `scripts/prepare_recording_config.py`：生成录像版配置；
- `mediamtx/mediamtx.recording.yml`：systemd 使用的配置文件。

若已安装 PyYAML，修改原始配置后可重新生成：

```bash
python/.venv/bin/python scripts/prepare_recording_config.py
```

**注意**：脚本会覆盖 `mediamtx.recording.yml` 中手工改动的内容。正式部署前检查生成结果，确认 `cam_cpp`、WebRTC/HLS 参数和管理接口地址没有被意外更改。

回环接口要求：

| 功能 | 监听地址 |
| --- | --- |
| RTSP | Pi 的 8554 端口（供播放器使用） |
| WebRTC | Pi 的 8889 端口 |
| HLS | Pi 的 8888 端口 |
| Control API | `127.0.0.1:9997` |
| Playback API | `127.0.0.1:9996` |

**重要**：Control API、Playback API 不应暴露到局域网。普通 FastAPI 登录并不自动保护 MediaMTX 直接播放端口。

## 5. FastAPI 管理员认证配置

已有认证文件：`config/api-auth.env`，权限目标为 `0600`，由 `kai` 所有；其内容包括用户名、PBKDF2 盐与哈希、会话签名密钥。

```bash
cd ~/projects/rpi_video_gateway
ls -l config/api-auth.env
sudo systemctl cat rpi-video-api.service
```

若配置丢失，不要从 GitHub 恢复旧密钥。应按项目最初的密码初始化流程**重新生成盐、PBKDF2 哈希与会话密钥**；重新生成会使已登录会话失效。不要把密码、哈希或会话密钥粘贴到 Issue、聊天截图或 Git 仓库。

FastAPI 通过 `EnvironmentFile=` 读取凭据；服务 `ExecStart` 运行 `uvicorn app.main:app`。历史监控由 FastAPI 的单进程后台采样线程负责：**目前保持单 worker**。

## 6. 安装 systemd 服务

四个服务：

- `rpi-mediamtx.service`：MediaMTX
- `rpi-video-gateway.service`：C++ 采集与推流
- `rpi-video-api.service`：FastAPI/Web
- `rpi-recording-guard.service`：录像安全守护

已有文件在工程目录 `systemd/` 中。安装或更新前检查它们没有混入 `cat > ...`、`EOF` 等 Shell 语句：

```bash
cd ~/projects/rpi_video_gateway
head -n 3 systemd/rpi-video-api.service
sudo install -m 644 systemd/rpi-mediamtx.service /etc/systemd/system/
sudo install -m 644 systemd/rpi-video-gateway.service /etc/systemd/system/
sudo install -m 644 systemd/rpi-video-api.service /etc/systemd/system/
sudo install -m 644 systemd/rpi-recording-guard.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemd-analyze verify /etc/systemd/system/rpi-*.service
```

检查服务文件绝对路径和账户名一致后，再启用：

```bash
sudo systemctl enable --now rpi-mediamtx.service
sudo systemctl enable --now rpi-video-gateway.service
sudo systemctl enable --now rpi-video-api.service
sudo systemctl enable --now rpi-recording-guard.service
```

如果只更新了 Python 文件，一般无需重启 C++；如果只更新网页 HTML/CSS/JS，一般无需重启 C++ 和 MediaMTX。

## 7. 启停权限与 LAN 访问

FastAPI 中的摄像头启停操作通过固定命令调用 systemctl，部署时曾为 `kai` 配置两条**精确匹配**的 sudoers 规则：

```text
kai ALL=(root) NOPASSWD: /usr/bin/systemctl start rpi-video-gateway.service, /usr/bin/systemctl stop rpi-video-gateway.service
```

检查：

```bash
sudo visudo -cf /etc/sudoers.d/rpi-video-api
```

不要换成 `kai ALL=(ALL) NOPASSWD: ALL`，也不要允许 Web 提交任意服务名。

当前控制台监听 `0.0.0.0:8000`，可从局域网浏览器直接访问 `http://<树莓派IP>:8000`。**HTTP 无加密，不能部署到公共网络或公网**。在不可信网络中，应恢复监听 `127.0.0.1` 并使用 SSH 转发，或先完成 HTTPS 反向代理与访问控制。

## 8. 录像安全保护

录像守护服务周期性检查 MediaMTX 的录制状态、磁盘剩余空间、录像目录大小及录制时长。默认阈值：

| 环境变量 | 默认值 |
| --- | --- |
| `RPI_REC_MIN_FREE_BYTES` | `1073741824`（1 GiB） |
| `RPI_REC_MAX_DIR_BYTES` | `4294967296`（4 GiB） |
| `RPI_REC_MAX_SESSION_SECONDS` | `1800`（30 分钟） |

MediaMTX 另有录制保留策略（在录像版 YAML 中目标为 `1d`）。守护服务启动成功不等于阈值测试通过；可参照 [测试报告](TEST_REPORT.md) 中的 60 秒受控验证，并及时恢复默认配置。

## 9. 常用运维命令

```bash
# 查看全部服务
systemctl is-active rpi-mediamtx rpi-video-gateway rpi-video-api rpi-recording-guard

# 查看实时日志
journalctl -u rpi-video-gateway -f
journalctl -u rpi-mediamtx -f
journalctl -u rpi-video-api -f
journalctl -u rpi-recording-guard -f

# 检查监听端口
ss -lntp | grep -E ':(8000|8554|8888|8889|9996|9997)\b'

# 摄像头设备占用
fuser /dev/video0 /dev/video11

# 数据占用
du -sh runtime/recordings runtime/data 2>/dev/null

df -h .
```

## 10. 常见故障

| 现象 | 优先检查 |
| --- | --- |
| `NameError: cat is not defined` | 错把 shell heredoc 包装行写入 Python 文件；Python 第一行应为正常 import |
| systemd `Assignment outside of section` | `.service` 文件第一行不应有 `cat > ...`；应为 `[Unit]` |
| `Port already in use` | 是否同时手动启动同一服务，或有旧进程仍在运行 |
| FastAPI 登录正常但视频黑屏 | MediaMTX/C++ 服务状态、`cam_cpp` 路径、树莓派 IP、浏览器控制台和网络连通性 |
| VLC 频繁卡顿 | 将 VLC RTSP 接收设为 RTP over RTSP (TCP)，再测 300 ms 缓存 |
| 指标接口 503 | C++ 进程是否运行、`gateway_metrics.sock` 是否存在、权限是否为 `kai` |
| 历史曲线 HTTP 422 | 检查 `history.py` 的 `minutes` 是否定义为 `int` 并允许 10/60/360 |
| 录像列表为空 | 确认开启过录像，检查 Playback API 和 `runtime/recordings` 目录 |
| 登录状态接口返回 401 | 未登录或 Cookie 失效；从网页登录后重试 |

## 11. 发布前检查

确保 `.gitignore` 排除了 `config/api-auth.env`、`runtime/`、`python/.venv/`、`build/`、私有录像和测试原始码流。不要发布含密码、个人 IP 地址或私有视频的真实日志/截图。测试报告区分实测与待测。
