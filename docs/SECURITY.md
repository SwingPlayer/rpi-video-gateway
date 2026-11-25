# 安全与运维注意事项

## 1. 当前安全边界

本项目仍为实验级局域网系统。FastAPI 的管理员密码和签名 Cookie 能控制 Web API 的访问，但**普通 HTTP 并不加密密码、Cookie 和业务数据**，因此不应把 8000 端口直接开放到校园公网、公共 Wi-Fi 或互联网。

### 优先级最高的风险

1. **HTTP 明文**：可信局域网内测试可以使用；正式部署需要 HTTPS（如受控反向代理或受信任的 VPN）。
2. **MediaMTX 播放权限**：FastAPI 登录只保护 FastAPI，本机 RTSP/WebRTC/HLS 的独立播放端口可能没有认证；需要在 MediaMTX 配置媒体用户认证/访问策略或使用网络隔离。
3. **MediaMTX Control API / Playback API**：必须保持 `127.0.0.1:9997` 和 `127.0.0.1:9996`，不能对局域网开放。
4. **systemctl 权限**：只授权 `kai` 执行固定服务的 `start` 和 `stop`，禁止开放任意系统命令。
5. **认证密钥与私人录像**：`config/api-auth.env`、`runtime/recordings/`、`runtime/data/` 不提交 Git，不上传公开 issue。
6. **磁盘保护**：守护程序采用周期性轮询，有响应延迟；应保留磁盘空间和 MediaMTX 删除策略，不能把安全守护等同于硬配额。

## 2. 发布 GitHub 前检查

```bash
cd ~/projects/rpi_video_gateway

git status --short

git check-ignore -v config/api-auth.env || true
git check-ignore -v runtime/data/gateway_history.sqlite3 || true
git check-ignore -v runtime/recordings || true
```

如果敏感文件**已经被 Git 跟踪**，`.gitignore` 不会自动取消跟踪；需要先检查 Git 历史是否已经包含敏感内容，必要时撤销并轮换密钥，不能只删掉当前文件就认定安全。

## 3. 守护服务与日志

```bash
systemctl status rpi-recording-guard --no-pager
journalctl -u rpi-recording-guard -n 80 --no-pager -l
```

限制日志中包含真实客户端 IP、录像时间和部分网络路径的公开传播。不要为调试而在日志打印用户明文密码或签名会话密钥。

## 4. 待增强项

- HTTPS 与 `Secure` Cookie；
- 正确的媒体访问鉴权（RTSP/WebRTC/HLS）；
- 登录强制退出/会话撤销及更细粒度访问日志；
- 录像空间硬上限、独立数据分区或文件系统配额；
- 服务异常告警、长时间存储和掉电恢复测试；
- 自动化测试与依赖版本固定；
- 设备热插拔及异常帧处理的长期稳定性验证。
