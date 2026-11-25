#!/usr/bin/env bash
# Read-only status check. Does not start/stop services or alter project files.
set -u

cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 1

printf '\n=== Services ===\n'
for service in rpi-mediamtx rpi-video-gateway rpi-video-api rpi-recording-guard; do
    printf '%-25s ' "$service"
    systemctl is-active "$service" 2>/dev/null || true
done

printf '\n=== Local API health ===\n'
curl --max-time 3 -sS http://127.0.0.1:8000/api/health || true
printf '\n'

printf '\n=== Metrics socket ===\n'
if [[ -S runtime/data/gateway_metrics.sock ]]; then
    ls -l runtime/data/gateway_metrics.sock
else
    echo 'not found (gateway stopped or IPC failure)'
fi

printf '\n=== RTSP stream information ===\n'
if command -v ffprobe >/dev/null; then
    timeout 8s ffprobe -v error -rtsp_transport tcp \
        -i rtsp://127.0.0.1:8554/cam_cpp \
        -show_entries stream=codec_name,width,height \
        -of default=noprint_wrappers=1 || true
else
    echo 'ffprobe not installed'
fi

printf '\n=== Storage ===\n'
df -h .
du -sh runtime/recordings runtime/data 2>/dev/null || true

printf '\n=== Local control ports ===\n'
ss -lnt | grep -E ':(8000|8554|8888|8889|9996|9997)([[:space:]]|$)' || true

printf '\nRead-only check complete. See docs/TEST_REPORT.md for full acceptance.\n'
