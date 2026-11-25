#!/usr/bin/env python3

import json
import logging
import os
import shutil
import time
import urllib.error
import urllib.request

from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]

RECORDINGS_DIR = (
    PROJECT_ROOT / "runtime" / "recordings"
)

STATE_FILE = (
    PROJECT_ROOT / "runtime" / "data"
    / "recording_guard_state.json"
)

CONTROL_URL = (
    "http://127.0.0.1:9997"
    "/v3/config/paths"
)

STREAM_NAME = "cam_cpp"

CHECK_INTERVAL = 5

MIN_FREE_BYTES = int(
    os.environ.get(
        "RPI_REC_MIN_FREE_BYTES",
        str(1024 ** 3),
    )
)

MAX_DIR_BYTES = int(
    os.environ.get(
        "RPI_REC_MAX_DIR_BYTES",
        str(4 * 1024 ** 3),
    )
)

MAX_SESSION_SECONDS = int(
    os.environ.get(
        "RPI_REC_MAX_SESSION_SECONDS",
        "1800",
    )
)


logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
)

logger = logging.getLogger("recording-guard")


def request_json(
    path: str,
    method: str = "GET",
    payload: dict | None = None,
) -> dict:
    body = None

    headers = {
        "Accept": "application/json",
    }

    if payload is not None:
        body = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"

    request = urllib.request.Request(
        CONTROL_URL + path,
        data=body,
        headers=headers,
        method=method,
    )

    with urllib.request.urlopen(
        request,
        timeout=3,
    ) as response:
        content = response.read(1024 * 1024)

    if not content:
        return {}

    result = json.loads(content.decode("utf-8"))

    if not isinstance(result, dict):
        raise ValueError(
            "Unexpected MediaMTX API response"
        )

    return result


def recording_enabled() -> bool:
    config = request_json(
        f"/get/{STREAM_NAME}"
    )

    return config.get("record") is True


def stop_recording() -> None:
    request_json(
        f"/patch/{STREAM_NAME}",
        method="PATCH",
        payload={"record": False},
    )

    logger.warning(
        "Recording disabled by safety guard"
    )


def directory_size(path: Path) -> int:
    if not path.exists():
        return 0

    total = 0

    for root, directories, files in os.walk(
        path,
        followlinks=False,
    ):
        for filename in files:
            full_path = os.path.join(
                root,
                filename,
            )

            # 不统计或追踪符号链接。
            if os.path.islink(full_path):
                continue

            try:
                total += os.path.getsize(
                    full_path
                )
            except OSError:
                # 文件可能正好被 MediaMTX 删除。
                # 下次轮询会重新统计。
                continue

    return total


def read_started_at() -> float | None:
    try:
        if not STATE_FILE.is_file():
            return None

        data = json.loads(
            STATE_FILE.read_text(
                encoding="utf-8"
            )
        )

        value = float(
            data["started_at"]
        )

        if value <= 0 or value > time.time() + 60:
            return None

        return value

    except (
        OSError,
        ValueError,
        TypeError,
        KeyError,
    ):
        logger.warning(
            "Ignoring invalid guard state"
        )

        return None


def write_started_at(timestamp: float) -> None:
    STATE_FILE.parent.mkdir(
        parents=True,
        exist_ok=True,
    )

    temporary = STATE_FILE.with_suffix(
        ".tmp"
    )

    flags = (
        os.O_WRONLY
        | os.O_CREAT
        | os.O_TRUNC
    )

    fd = os.open(
        temporary,
        flags,
        0o600,
    )

    try:
        with os.fdopen(
            fd,
            "w",
            encoding="utf-8",
        ) as file:
            json.dump(
                {"started_at": timestamp},
                file,
            )

            file.flush()
            os.fsync(file.fileno())

        os.replace(
            temporary,
            STATE_FILE,
        )

    finally:
        if temporary.exists():
            temporary.unlink()


def clear_started_at() -> None:
    try:
        STATE_FILE.unlink(
            missing_ok=True
        )
    except OSError as exc:
        logger.warning(
            "Failed to clear guard state: %s",
            exc,
        )


def get_storage_status() -> tuple[int, int]:
    RECORDINGS_DIR.mkdir(
        parents=True,
        exist_ok=True,
    )

    disk = shutil.disk_usage(
        RECORDINGS_DIR
    )

    total = directory_size(
        RECORDINGS_DIR
    )

    return disk.free, total


def main() -> None:
    logger.info(
        "Recording safety guard started"
    )

    logger.info(
        "Minimum free: %.2f GiB",
        MIN_FREE_BYTES / (1024 ** 3),
    )

    logger.info(
        "Maximum recording directory: %.2f GiB",
        MAX_DIR_BYTES / (1024 ** 3),
    )

    logger.info(
        "Maximum recording session: %d seconds",
        MAX_SESSION_SECONDS,
    )

    started_at = read_started_at()

    last_enabled = None
    server_offline = False

    while True:
        try:
            enabled = recording_enabled()

            if server_offline:
                logger.info(
                    "MediaMTX connection restored"
                )

            server_offline = False

            if not enabled:
                if last_enabled is True:
                    logger.info(
                        "Recording stopped normally"
                    )

                if started_at is not None:
                    clear_started_at()
                    started_at = None

                last_enabled = False

                time.sleep(CHECK_INTERVAL)
                continue

            # 检测到新一轮录像。
            if started_at is None:
                started_at = time.time()

                write_started_at(
                    started_at
                )

                logger.info(
                    "Recording session started"
                )

            last_enabled = True

            free_bytes, directory_bytes = (
                get_storage_status()
            )

            elapsed = time.time() - started_at

            reason = None

            if free_bytes < MIN_FREE_BYTES:
                reason = (
                    "Low disk space: "
                    f"{free_bytes / (1024 ** 3):.2f} GiB free"
                )

            elif directory_bytes >= MAX_DIR_BYTES:
                reason = (
                    "Recording directory limit reached: "
                    f"{directory_bytes / (1024 ** 3):.2f} GiB"
                )

            elif elapsed >= MAX_SESSION_SECONDS:
                reason = (
                    "Maximum recording session duration reached: "
                    f"{elapsed:.0f} seconds"
                )

            if reason:
                logger.warning(
                    "Safety limit triggered: %s",
                    reason,
                )

                # 如果 API 操作失败，保留开始时间，
                # 下一轮继续尝试停止录像。
                stop_recording()

                clear_started_at()

                started_at = None
                last_enabled = False

        except (
            urllib.error.URLError,
            TimeoutError,
            OSError,
            ValueError,
            KeyError,
        ) as exc:
            if not server_offline:
                logger.warning(
                    "MediaMTX or storage check failed: %s",
                    exc,
                )

            server_offline = True

        except Exception:
            logger.exception(
                "Unexpected recording guard error"
            )

        time.sleep(
            CHECK_INTERVAL
        )


if __name__ == "__main__":
    main()