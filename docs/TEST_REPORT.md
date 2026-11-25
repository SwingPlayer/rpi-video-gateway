# 项目阶段性测试报告

**项目**：Raspberry Pi Video Gateway  
**设备与环境**：Raspberry Pi 4B，USB UVC 摄像头，Linux，C++17，FFmpeg，MediaMTX，FastAPI，systemd。  
**报告性质**：基于本项目开发过程中的用户终端日志、`ffprobe` 输出与网页截图整理的阶段性记录；不是自动化测试平台生成的最终证书。用户没有上传的设备参数、源码版本和录像文件内容不作虚构。  
**记录时段**：2026 年 11 月开发测试，具体实验时间以原始日志为准。

## 1. 测试目标

验证视频从 USB 摄像头采集、MJPEG 解码、H.264 硬件编码、RTSP 发布到浏览器和 VLC 播放的完整链路；检查采集帧率、丢帧、服务管理、异常恢复、指标展示、历史记录、录像与安全防护。

## 2. 已实测：视频采集与编码

| 项目 | 已观察结果 | 结论 |
| --- | --- | --- |
| USB 摄像头 V4L2 采集 | 1280×720、MJPEG、30 FPS；1000 帧采集，0 失效帧，0 超时 | 通过 |
| FFmpeg MJPEG 解码 | 1000/1000 解码成功 | 通过 |
| H.264 硬件编码 | `h264_v4l2m2m` 使用 `/dev/video11`；1000 帧产生 1000 包 | 通过 |
| 裸 H.264 解码完整性 | `ffprobe` 确认 H.264 1280×720，FFmpeg 对输出文件 1000 帧解码无异常 | 通过 |
| RTSP 发送（首轮） | 采集 3000 帧、MJPEG 队列丢 1 帧、成功发布 2999 包 | 功能通过，有短暂积压 |
| RTSP 发送（第二轮） | 采集 3000 帧、发布 3000 包、队列丢帧 0、队列峰值 2、FPS 30.01 | 通过 |

**代表性第二轮统计**：

```text
Captured frames   : 3000
Valid MJPEG       : 3000
Invalid MJPEG     : 0
Queue pushed      : 3000
Queue dropped     : 0
Queue peak depth  : 2
Decoded frames    : 3000
Decode failures   : 0
Encoder submitted : 3000
H.264 packets     : 3000
RTSP packets      : 3000
Capture timeouts  : 0
Capture FPS       : 30.01
```

## 3. 已实测：播放方式对比

| 客户端 | 测试观察 | 说明 |
| --- | --- | --- |
| VLC RTSP（初始配置） | 频繁卡顿；增加到 800 ms 缓存仅部分改善 | 可能与传输方式和接收处理相关 |
| VLC RTSP over TCP，300 ms 缓存 | 无明显卡顿，与 HLS 观看几乎同步 | 本网络环境适用 |
| 浏览器 WebRTC | 整体流畅，偶发 1–2 次短暂卡顿 | 适合低延迟网页查看 |
| 浏览器 HLS | 观看稳定，延迟略大于 WebRTC | 适合兼容播放 |

**推断边界**：并未进行原先 VLC 的网络抓包验证，不能绝对声称最初的 VLC 必然使用了 UDP 且存在丢包；但强制 TCP 后卡顿消失是有实测依据的。

## 4. 已实测：持续运行和 RTSP 重连（v0.6）

| 统计项 | 记录值 |
| --- | ---: |
| 运行时长 | 约 10 分 17 秒（根据帧数/帧率估算） |
| 采集帧数 | 18,510 |
| 平均采集 FPS | 30.01 |
| MJPEG 帧队列丢帧 | 0 |
| H.264 包队列丢包 | 0 |
| RTSP 发布包数 | 18,274 |
| RTSP 网络错误 | 3 |
| RTSP 会话数 | 2 |
| 重连等待同步丢弃 | 231 |
| 退出期间主动丢弃 | 2 |

故障注入方法：运行中停止 MediaMTX，再启动 MediaMTX，观察 C++ 输出 `Broken pipe`、`Connection refused` 和 `Session 2 established`、`Reconnected successfully`。测试证明在此次故障场景下能够重新建立推流。Ctrl+C 后日志出现 `Video worker stopped`、`RTSP network worker stopped` 和 `Video gateway stopped cleanly`。

**注意**：重连期间 `published_packets < captured_frames` 是策略性丢弃过期视频数据，不代表底层采集出错；231 个同步丢弃包不能简单等同为精确 7.7 秒断网。

## 5. 已实测：systemd / Web / 历史监控

| 检查项 | 证据 | 状态 |
| --- | --- | --- |
| MediaMTX 系统服务 | `systemctl status` 显示 enabled、active (running)；MediaMTX 输出 cam_cpp 流可用 | 已通过 |
| C++ 网关系统服务 | `systemctl status` 显示 enabled、active (running)、实时解码与编码日志 | 已通过 |
| FastAPI 系统服务 | 曾因把 `cat > ...` 写进 Python/service 文件启动失败；修复后用户确认 FastAPI 控制链路通过 | 修复后通过 |
| 管理员登录 | 用户确认登录页面和受保护控制 API 已完成 | 用户确认 |
| 实时监控指标 | 截图显示 FPS 30.00、累计采集 55,890 帧、网络错误 0 等 | 已观察 |
| SQLite 历史曲线 | 修复 `minutes` 参数 422 错误后，截图显示最近 10 分钟 120 个数据点、约 30 FPS 曲线 | 已通过 |
| 历史页顶部数值 | 曾显示 `—`；发现 `Intl.NumberFormat` 被错误直接调用，已提供修复代码 | 待确认修复后截图 |

### 不同运行场景中的丢帧

除 v0.6 的零队列丢帧测试外，另一次实时仪表盘截图显示：

```text
Capture FPS      : 30.00
Encoded packets/s: 30.00
Captured frames  : 55890
Published packets: 55883
MJPEG dropped    : 34
H.264 dropped    : 0
RTSP reconnects  : 0
Network errors   : 0
```

这些是**不同时间/不同运行进程**中的观察值。不能合并成同一次测试，也不能把“v0.6 约 10 分钟无丢帧”推广为“所有长时间运行都无丢帧”。下一阶段应调查采集队列在更长运行中出现积压的原因，并记录发生时的系统负载与网络条件。

## 6. 待补测：按需录像 v0.9 与安全守护 v1.0

当前聊天中虽已给出录像管理和保护代码，但**没有收到对应的完整通过日志**。因此以下验收项保持待测：

| 测试编号 | 操作 | 预期 | 结果 |
| --- | --- | --- | --- |
| REC-01 | 网页开始录像后录制约 60 秒 | MediaMTX 返回 `record=true`，录像目录产生分片 | 待测 |
| REC-02 | 网页停止录像 | `record=false`，直播保持正常 | 待测 |
| REC-03 | 查看录像列表 | 出现对应录像时间段，起止时间合理 | 待测 |
| REC-04 | 下载 MP4 并用 FFprobe/VLC 验证 | 文件可打开、H.264 视频正常、时长接近实际 | 待测 |
| REC-05 | 浏览器内在线播放 MP4 | 可播放；记录是否支持拖动进度条 | 待测 |
| GUARD-01 | 临时将最长录制时间设为 60 秒 | 守护程序自动关闭录像，WebRTC 直播不中断 | 待测 |
| GUARD-02 | 受控测试低磁盘阈值 | 新录像受到保护；不得人为填满系统盘 | 待测 |
| GUARD-03 | 检查录像保留/空间占用 | 超期文件按策略清理，空间限制可观测 | 待测 |
| BOOT-01 | 重启树莓派 | 所有启用服务自启动，画面和网页恢复 | 待测/待补证据 |
| LONG-24H | 连续运行 24 小时 | 记录采集 FPS、丢帧、资源与故障次数 | 待测 |

**安全提示**：不建议通过制造真实磁盘写满来验证保护。可以在单独测试环境提高阈值、修改安全守护测试参数，以可恢复的方式验证触发逻辑。

## 7. 可复现的验收命令

### 7.1 systemd 服务

```bash
systemctl is-active rpi-mediamtx rpi-video-gateway rpi-video-api rpi-recording-guard
```

### 7.2 实时 RTSP

```bash
# 在 C++ 实时网关和 MediaMTX 运行时执行
ffprobe -v error -rtsp_transport tcp \
  -i rtsp://127.0.0.1:8554/cam_cpp \
  -show_entries stream=codec_name,width,height \
  -of default=noprint_wrappers=1
```

### 7.3 Unix Socket 指标

```bash
cd ~/projects/rpi_video_gateway
python3 - <<'PY'
import json
import socket

path = "runtime/data/gateway_metrics.sock"
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
    sock.settimeout(2)
    sock.connect(path)
    raw = bytearray()
    while not raw.endswith(b"\n"):
        chunk = sock.recv(4096)
        if not chunk:
            break
        raw.extend(chunk)
    print(json.dumps(json.loads(raw), indent=2, ensure_ascii=False))
PY
```

### 7.4 SQLite 历史采样

```bash
cd ~/projects/rpi_video_gateway
python3 - <<'PY'
import sqlite3
p = "runtime/data/gateway_history.sqlite3"
with sqlite3.connect(p) as db:
    print("metric_samples:", db.execute("SELECT COUNT(*) FROM metric_samples").fetchone()[0])
    print("gateway_events:", db.execute("SELECT COUNT(*) FROM gateway_events").fetchone()[0])
PY
```

### 7.5 录像文件与存储

```bash
cd ~/projects/rpi_video_gateway
find runtime/recordings -type f | head -20
du -sh runtime/recordings
df -h .
```

将导出的 MP4 下载到测试机后，可执行：

```bash
ffprobe -v error -show_entries format=duration,size \
  -show_entries stream=codec_name,width,height \
  -of default=noprint_wrappers=1 test_recording.mp4
```

## 8. 目前结论

1. **视频核心路径已跑通并具备短时/中时稳定性证据**：720p30、H.264 硬编码、RTSP/WebRTC/HLS 分发均已测试。
2. **RTSP 断线恢复已通过一次真实故障测试**：业务正常采集与推流会话恢复得到验证。
3. **网页管理及历史曲线可用**：出现过的 422 和历史页 JS 格式化问题已定位，其中 422 通过后历史曲线已成功展示。
4. **仍不能声称“工业级/7×24 小时全功能验收完成”**：需要补测录像、磁盘守护、启停恢复、24 小时稳定性与 HTTPS 访问控制。
