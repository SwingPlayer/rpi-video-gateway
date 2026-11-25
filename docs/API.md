# FastAPI / IPC 接口说明

## 1. 基本约定

- API 根地址：`http://<树莓派IP>:8000`（可信局域网）；本机 `http://127.0.0.1:8000`。
- 数据格式：JSON；编码 UTF-8。
- 管理 API：需要登录后由浏览器自动附带 `rpi_video_session` Cookie。
- 失败时可能返回 `401`（未认证）、`403`（跨源写入请求）、`409`（摄像头未运行）、`422`（参数错误）、`503`（依赖服务不可用）。
- API 只控制固定的 C++ 网关与 `cam_cpp` 录像流，不接受用户任意系统命令。

## 2. 认证与页面

| Method | URL | 认证 | 说明 |
| --- | --- | --- | --- |
| GET | `/` | 是 | 视频控制台 |
| GET | `/history` | 是 | 历史趋势与事件 |
| GET | `/login` | 否 | 登录页面 |
| POST | `/api/auth/login` | 否 | 账户密码登录，设置 HttpOnly Cookie |
| GET | `/logout` | 是 | 清除当前浏览器会话 Cookie 并重定向 |
| GET | `/api/health` | 否 | API 进程健康检查，不代表视频业务也健康 |

登录请求体（示例，不要在终端历史中输入真实密码）：

```json
{
  "username": "admin",
  "password": "<自行设置的管理员密码>"
}
```

会话有效期约 8 小时，Cookie 使用 `HttpOnly`、`SameSite=Strict`；仅在 HTTPS 访问时启用 `Secure` 标志。当前 HTTP 登录不保证密码与会话传输机密性。

## 3. 摄像头控制

| Method | URL | 功能 |
| --- | --- | --- |
| GET | `/api/camera/status` | systemd 下 C++ 和 MediaMTX 服务状态 |
| POST | `/api/camera/start` | 启动 `rpi-video-gateway.service` |
| POST | `/api/camera/stop` | 停止 `rpi-video-gateway.service` |
| GET | `/api/camera/metrics` | C++ Unix Socket 实时统计 |
| GET | `/api/streams` | RTSP、WebRTC、HLS 地址模板 |
| GET | `/api/ui/config` | 网页流路径、推荐局域网 IP/端口 |

`/api/camera/status` 响应结构示例：

```json
{
  "gateway": {
    "service": "rpi-video-gateway.service",
    "loaded": true,
    "running": true,
    "active_state": "active",
    "sub_state": "running",
    "enabled": true,
    "main_pid": 12345
  },
  "mediamtx": {
    "service": "rpi-mediamtx.service",
    "loaded": true,
    "running": true,
    "active_state": "active",
    "sub_state": "running",
    "enabled": true,
    "main_pid": 12346
  }
}
```

## 4. 实时指标 `/api/camera/metrics`

由 C++ `runtime/data/gateway_metrics.sock` 提供，FastAPI 读取后增加 `ipc_connected` 字段。

| 字段 | 单位/含义 |
| --- | --- |
| `running` | C++ 网关当前未请求停止 |
| `uptime_seconds` | 当前 C++ 进程运行秒数 |
| `sample_ready` | 首个约 10 秒 FPS 采样是否完成 |
| `sample_window_ms` | 最近一次 FPS 统计窗口毫秒数 |
| `capture_fps` | 最近采样窗口的平均采集 FPS |
| `encode_fps` | H.264 **编码包**速率（包/秒） |
| `captured_frames` | 累计采集帧数 |
| `decoded_frames` | 累计成功解码帧数 |
| `encoded_packets` | 累计编码 H.264 包数 |
| `published_packets` | 成功提交 RTSP 的包数 |
| `mjpeg_dropped` | MJPEG 队列累计丢帧 |
| `h264_dropped` | H.264 包队列累计丢包 |
| `network_errors` | 网络错误次数 |
| `rtsp_sessions` | 成功建立的 RTSP 会话数 |
| `rtsp_reconnections` | 不含首次会话的重连次数 |
| `sync_discards` | 等待首个有效 IDR、重连期间丢弃包数 |
| `shutdown_discards` | 关闭期间主动丢弃包数 |
| `ipc_connected` | FastAPI 成功连接 IPC 的标识 |

注意：累计计数**只对当前 C++ 进程有效**，进程重启后重置；与 SQLite 历史记录不是同一个计数生命周期。

## 5. 历史指标与事件

| Method | URL | 说明 |
| --- | --- | --- |
| GET | `/api/metrics/history?minutes=10` | 最近 10 分钟，5 秒桶 |
| GET | `/api/metrics/history?minutes=60` | 最近 1 小时，15 秒桶 |
| GET | `/api/metrics/history?minutes=360` | 最近 6 小时，90 秒桶 |
| GET | `/api/metrics/events?limit=20` | 最近 20 条运行/故障事件，上限 100 |

历史 API 示例：

```json
{
  "minutes": 10,
  "bucket_seconds": 5,
  "count": 1,
  "points": [
    {"timestamp": 1795360000, "capture_fps": 30.01, "encode_fps": 30.0, "mjpeg_dropped": 0, "h264_dropped": 0, "network_errors": 0}
  ]
}
```

历史数据每约 5 秒由 FastAPI 单进程后台线程采集一次，SQLite 保存时间目标为 7 天。离线期间可能存在时间戳空档，不应自行补零。

## 6. 录像管理

| Method | URL | 功能 |
| --- | --- | --- |
| GET | `/api/recordings/status` | `cam_cpp` 录像开关、可用磁盘空间 |
| POST | `/api/recordings/start` | 对 MediaMTX 动态开启录像 |
| POST | `/api/recordings/stop` | 对 MediaMTX 动态关闭录像 |
| GET | `/api/recordings/list?limit=30` | 查询可用录像时间段，最大 100 条 |
| GET | `/api/recordings/download?start=<ISO时间>&duration=<秒>` | 导出 MP4，下载附件 |
| GET | `/api/recordings/play?start=<ISO时间>&duration=<秒>` | 导出 MP4，浏览器内播放 |

录像时间戳应包含时区；导出时长需大于零、不超过 3600 秒。录像列表返回：

```json
{
  "stream": "cam_cpp",
  "count": 1,
  "recordings": [
    {"start": "2026-11-25T10:00:00+08:00", "duration": 60.0, "download_url": "/api/recordings/download?start=...&duration=60.0"}
  ]
}
```

此数据仅说明字段格式，不代表实际存在该录像。下载与回放依赖本机 MediaMTX Playback API；现有回放代理未完整实现 HTTP Range 支持。

## 7. 内部端口 / IPC

| 端口或路径 | 用途 | 暴露范围 |
| --- | --- | --- |
| 8000/TCP | FastAPI 控制界面 | 配置为局域网监听；仅限可信网络 |
| 8554/TCP | RTSP | 允许的视频客户端 |
| 8889/TCP/相关 UDP | WebRTC | 允许的视频客户端 |
| 8888/TCP | HLS | 允许的视频客户端 |
| 9997/TCP | MediaMTX Control API | **仅回环 `127.0.0.1`** |
| 9996/TCP | MediaMTX Playback API | **仅回环 `127.0.0.1`** |
| `runtime/data/gateway_metrics.sock` | C++ 指标快照 IPC | 仅本机 `kai` 用户 |

具体 WebRTC 端口还应以实际 `mediamtx.yml` 和网络配置为准。此列表不应替代防火墙规则。

## 8. API 自检

```bash
# 无需认证
curl -i http://127.0.0.1:8000/api/health

# 未登录时应返回 401
curl -i http://127.0.0.1:8000/api/camera/status

# MediaMTX 内部配置应只允许树莓派本机访问
curl -i http://127.0.0.1:9997/v3/config/paths/get/cam_cpp
```

在浏览器登录后访问 `/api/camera/metrics`、`/api/metrics/history?minutes=10`，检查返回 JSON。写入 API 应从网页同源调用；不要通过随意加上 `Origin` 头的 curl 请求来模拟登录控制，因为签名 Cookie 才是实际权限凭据。
