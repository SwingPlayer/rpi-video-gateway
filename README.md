# Raspberry Pi Video Gateway

> 基于 Raspberry Pi 4B 的 Linux 实时视频网关：C++ V4L2 采集、FFmpeg H.264 硬件编码、MediaMTX 多协议分发，以及 FastAPI + Web 管理与监控。

**项目状态**：实时采集、推流、断线重连、systemd 服务、网页控制、运行指标与历史曲线已有实测记录；按需录像、自动录像保护和浏览器回放仍需按本仓库的验收表补测，不应仅因代码存在就宣称验收完成。

## 1. 功能概览

| 层级 | 技术 / 实现 | 状态 |
| --- | --- | --- |
| 视频采集 | C++17、V4L2、`/dev/video0`、MJPEG/MMAP | 已实测 |
| 视频编解码 | FFmpeg MJPEG 解码、`h264_v4l2m2m` 硬件编码 | 已实测 |
| 实时传输 | C++ RTSP Publisher → MediaMTX → RTSP/WebRTC/HLS | 已实测 |
| 长期运行 | 有界队列、统计、RTSP 断线重连、Ctrl+C 退出 | 已实测 |
| 系统管理 | systemd 管理 MediaMTX、C++、FastAPI | 已实测启动 |
| 网页控制 | FastAPI 摄像头启停、状态、管理员登录 | 已实测 |
| 运行监控 | Unix Domain Socket、实时 FPS/丢帧/重连指标 | 已实测 |
| 历史数据 | SQLite、FPS 历史曲线、运行事件 | 历史曲线已实测 |
| 按需录像 | MediaMTX fMP4、录像下载/回放 | 已实现；待完整验收 |
| 安全守护 | 录像空间/时长阈值保护 | 已设计；待故障注入验收 |

## 2. 数据通路

```text
USB Camera (/dev/video0, 1280x720 @30fps MJPEG)
    |
    v
C++ V4L2 capture -----> bounded MJPEG queue (8)
    |                               |
    |                               v
    |                    FFmpeg MJPEG decode + swscale
    |                               |
    |                      H.264 hardware encoder
    |                               |
    |                      bounded H.264 queue (60)
    |                               |
    |                               v
    |                        C++ RTSP publisher
    |                               |
    |                          TCP/8554
    |                               v
    |                           MediaMTX
    |                           /       \
    |                     Live view    fMP4 recording
    |                    RTSP/WebRTC/HLS
    |
    +-- counters --> Unix Socket --> FastAPI --> Web dashboard
                                            `--> SQLite history
```

详细说明见 [架构设计](docs/ARCHITECTURE.md)。

## 3. 已验证的视频参数

| 参数 | 当前配置 |
| --- | --- |
| 设备 | Raspberry Pi 4B + USB UVC 摄像头 |
| 采集格式 | MJPEG |
| 分辨率 | 1280 × 720 |
| 目标帧率 | 30 FPS |
| 编码 | H.264，FFmpeg `h264_v4l2m2m`（`/dev/video11`） |
| 目标码率 | 3 Mbps |
| RTSP 发布路径 | `rtsp://127.0.0.1:8554/cam_cpp` |
| RTSP 播放 | `rtsp://<树莓派IP>:8554/cam_cpp`（VLC 建议强制 RTSP over TCP） |
| WebRTC 播放 | `http://<树莓派IP>:8889/cam_cpp` |
| HLS 播放 | `http://<树莓派IP>:8888/cam_cpp` |
| Web 控制台 | `http://<树莓派IP>:8000`（仅建议可信局域网内使用） |
| 历史监控 | `http://<树莓派IP>:8000/history` |

> 网页界面显示的目标帧率与码率是**配置值**；实际 FPS、帧数、队列丢帧和重连计数来自 C++ 运行数据。

## 4. 目录结构

```text
rpi_video_gateway/
├── cpp/
│   ├── include/{capture,codec,pipeline,publish,ipc}/
│   ├── src/{capture,codec,publish,ipc}/
│   └── CMakeLists.txt
├── python/
│   ├── app/
│   │   ├── api/                 # 摄像头、录像和历史指标路由
│   │   ├── services/            # systemd、录像、SQLite、Socket 业务逻辑
│   │   └── main.py              # FastAPI、身份认证和网页路由
│   └── requirements.txt
├── web/
│   ├── templates/              # 首页、登录、历史页面
│   └── static/                 # JavaScript 和样式
├── mediamtx/
│   ├── bin/                    # MediaMTX 可执行文件（本地安装）
│   ├── mediamtx.yml
│   └── mediamtx.recording.yml  # 由脚本生成
├── systemd/                   # 服务单元文件
├── scripts/                   # 配置及运行工具
├── config/api-auth.env        # 认证密钥（严禁提交）
├── runtime/                   # 数据库、Socket、录像、运行日志（忽略）
├── docs/
├── .gitignore
└── README.md
```

## 5. 已安装工程的快速检查

以下命令在**树莓派终端**运行：

```bash
cd ~/projects/rpi_video_gateway

# 查看全部服务
systemctl is-active rpi-mediamtx rpi-video-gateway rpi-video-api rpi-recording-guard

# 查询 API（健康检查开放、其他管理 API 需要登录）
curl -s http://127.0.0.1:8000/api/health

# 查看日志
journalctl -u rpi-video-gateway -n 30 --no-pager
journalctl -u rpi-mediamtx -n 30 --no-pager
```

详细安装和排障参考 [部署说明](docs/DEPLOYMENT.md)。

## 6. API 与指标

API 使用 FastAPI，管理路由需要管理员登录 Cookie：

- `GET /api/camera/status`、`POST /api/camera/start`、`POST /api/camera/stop`
- `GET /api/camera/metrics`：来自 C++ Unix Socket 的真实统计
- `GET /api/metrics/history?minutes=10|60|360`
- `GET /api/metrics/events?limit=20`
- `GET /api/recordings/status`、`POST /api/recordings/start`、`POST /api/recordings/stop`
- `GET /api/recordings/list`、`GET /api/recordings/download`、`GET /api/recordings/play`

详见 [API 文档](docs/API.md)。

## 7. 性能与测试证据

- **3000 帧 RTSP 测试**：720p30、3000/3000 帧成功，0 丢帧、0 超时，MediaMTX 成功识别 H.264 1280×720。
- **约 10 分钟稳定性 + 断线重连测试**：18510 帧采集，采集率约 30.01 FPS；MJPEG 和 H.264 队列 0 丢帧；MediaMTX 中断与恢复后 RTSP 会话由 1 增至 2。
- **播放对照**：浏览器 WebRTC、HLS 表现稳定；VLC 原设置频繁卡顿，强制 RTSP over TCP + 300 ms 缓存后流畅。
- **历史曲线**：最近 10 分钟已有 120 个历史数据点、约 30 FPS 的两条近似重合曲线。
- **不同运行工况提醒**：另一次更长时间的网页截图显示累计 55,890 帧、MJPEG 丢帧 34；不能将所有不同测试都描述成全程零丢帧。

完整的实测与待补测项目见 [测试报告](docs/TEST_REPORT.md)。

## 8. 安全注意事项

- `http://<IP>:8000` 当前为 **HTTP 明文通信**，管理员登录不能代替 HTTPS；只在可信实验局域网使用。
- FastAPI 登录不等于 MediaMTX 播放权限控制，RTSP/WebRTC/HLS 的播放端口需另行保护。
- MediaMTX Control API `9997`、Playback API `9996` 应只监听 `127.0.0.1`。
- `config/api-auth.env`、录像文件和 SQLite 数据库绝不提交 Git。
- 仅给运行 API 的账户授权**指定** systemd 服务的启动/停止命令，不给予通用 sudo 权限。

详见 [安全与运维](docs/SECURITY.md)。

## 9. 下一步

当前优先级：**确认 v0.9/v1.0 录像下载、浏览器回放、60 秒自动停止及空间阈值保护已通过实际验收**，然后做 24 小时运行测试与 HTTPS/视频播放权限完善；不要仅凭现有代码就认定这些功能已经验证。
