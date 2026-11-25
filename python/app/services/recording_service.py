from __future__ import annotations

import json
import shutil
import urllib.error
import urllib.parse
import urllib.request

from datetime import datetime
from pathlib import Path
from typing import Any, Iterator


PROJECT_ROOT = Path(__file__).resolve().parents[3]

RECORDINGS_DIR = (
    PROJECT_ROOT / "runtime" / "recordings"
)

STREAM_NAME = "cam_cpp"

CONTROL_URL = "http://127.0.0.1:9997"
PLAYBACK_URL = "http://127.0.0.1:9996"

# 剩余空间低于 1 GiB 时禁止开始新的录像。
MIN_FREE_BYTES = 1024 ** 3


class RecordingError(Exception):
    pass


def read_json(
    base_url: str,
    path: str,
    method: str = "GET",
    payload: dict[str, Any] | None = None,
) -> Any:

    data = None

    headers = {
        "Accept": "application/json",
    }

    if payload is not None:
        data = json.dumps(
            payload
        ).encode("utf-8")

        headers["Content-Type"] = "application/json"

    request = urllib.request.Request(
        base_url + path,
        data=data,
        method=method,
        headers=headers,
    )

    try:
        with urllib.request.urlopen(
            request,
            timeout=5,
        ) as response:

            body = response.read(1024 * 1024)

            if not body:
                return {}

            return json.loads(
                body.decode("utf-8")
            )

    except urllib.error.HTTPError as exc:
        message = exc.read(2048).decode(
            "utf-8",
            errors="replace",
        )

        raise RecordingError(
            f"MediaMTX HTTP {exc.code}: {message}"
        ) from exc

    except (
        urllib.error.URLError,
        TimeoutError,
        OSError,
        ValueError,
    ) as exc:
        raise RecordingError(
            f"MediaMTX request failed: {exc}"
        ) from exc


def get_path_config() -> dict[str, Any]:
    result = read_json(
        CONTROL_URL,
        f"/v3/config/paths/get/{STREAM_NAME}",
    )

    if not isinstance(result, dict):
        raise RecordingError(
            "Unexpected MediaMTX configuration response"
        )

    return result


def get_recording_status() -> dict[str, Any]:
    config = get_path_config()

    RECORDINGS_DIR.mkdir(
        parents=True,
        exist_ok=True,
    )

    usage = shutil.disk_usage(
        RECORDINGS_DIR
    )

    return {
        "stream": STREAM_NAME,
        "recording_enabled": (
            config.get("record") is True
        ),
        "format": "fmp4",
        "retention": "1d",
        "disk_free_bytes": usage.free,
        "disk_total_bytes": usage.total,
        "recording_directory": str(RECORDINGS_DIR),
    }


def set_recording(
    enabled: bool,
) -> dict[str, Any]:

    # 先确认配置存在，避免误修改其他直播路径。
    current = get_path_config()

    already_enabled = (
        current.get("record") is True
    )

    if already_enabled == enabled:
        return get_recording_status()

    if enabled:
        RECORDINGS_DIR.mkdir(
            parents=True,
            exist_ok=True,
        )

        usage = shutil.disk_usage(
            RECORDINGS_DIR
        )

        if usage.free < MIN_FREE_BYTES:
            raise RecordingError(
                "磁盘剩余空间不足 1 GiB，拒绝开始录像"
            )

    read_json(
        CONTROL_URL,
        f"/v3/config/paths/patch/{STREAM_NAME}",
        method="PATCH",
        payload={
            "record": enabled,
        },
    )

    return get_recording_status()


def list_recordings(
    limit: int = 30,
) -> dict[str, Any]:

    if limit < 1 or limit > 100:
        raise RecordingError(
            "Invalid recording list limit"
        )

    query = urllib.parse.urlencode({
        "path": STREAM_NAME,
    })

    try:
        result = read_json(
            PLAYBACK_URL,
            f"/list?{query}",
        )
    except RecordingError as exc:
        # 某些版本在没有录像时可能返回 404。
        if "HTTP 404" in str(exc):
            return {
                "stream": STREAM_NAME,
                "count": 0,
                "recordings": [],
            }

        raise

    if result is None:
        result = []

    if not isinstance(result, list):
        raise RecordingError(
            "Unexpected recordings list response"
        )

    recordings = []

    for item in result:
        if not isinstance(item, dict):
            continue

        start = item.get("start")
        duration = item.get("duration")

        if not isinstance(start, str):
            continue

        try:
            duration = float(duration)

            # 保证时间格式正确。
            parsed = datetime.fromisoformat(
                start.replace("Z", "+00:00")
            )

            if parsed.tzinfo is None:
                continue

        except (ValueError, TypeError):
            continue

        if not (0 < duration <= 86400):
            continue

        download_query = urllib.parse.urlencode({
            "start": start,
            "duration": duration,
        })

        recordings.append({
            "start": start,
            "duration": duration,
            "download_url": (
                "/api/recordings/download?"
                + download_query
            ),
        })

    recordings.sort(
        key=lambda item: item["start"],
        reverse=True,
    )

    recordings = recordings[:limit]

    return {
        "stream": STREAM_NAME,
        "count": len(recordings),
        "recordings": recordings,
    }


def open_recording_download(
    start: str,
    duration: float,
):

    try:
        timestamp = datetime.fromisoformat(
            start.replace("Z", "+00:00")
        )

        if timestamp.tzinfo is None:
            raise ValueError(
                "Timestamp must include timezone"
            )

    except ValueError as exc:
        raise RecordingError(
            "Invalid recording start timestamp"
        ) from exc

    if not (0 < duration <= 3600):
        raise RecordingError(
            "Recording duration must be within 1 hour"
        )

    query = urllib.parse.urlencode({
        "path": STREAM_NAME,
        "start": start,
        "duration": duration,
        "format": "mp4",
    })

    request = urllib.request.Request(
        PLAYBACK_URL + "/get?" + query,
        headers={
            "Accept": "video/mp4",
        },
    )

    try:
        response = urllib.request.urlopen(
            request,
            timeout=15,
        )

    except (
        urllib.error.HTTPError,
        urllib.error.URLError,
        TimeoutError,
        OSError,
    ) as exc:
        raise RecordingError(
            f"Unable to open recording: {exc}"
        ) from exc

    filename = (
        "cam_cpp_"
        + timestamp.strftime("%Y%m%d_%H%M%S")
        + ".mp4"
    )

    return response, filename


def stream_recording(
    response,
) -> Iterator[bytes]:

    try:
        while True:
            chunk = response.read(
                256 * 1024
            )

            if not chunk:
                break

            yield chunk

    finally:
        response.close()