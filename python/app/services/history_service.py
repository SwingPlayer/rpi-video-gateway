from __future__ import annotations

import logging
import sqlite3
import threading
import time

from contextlib import contextmanager
from pathlib import Path
from typing import Any, Iterator

from app.services.metrics_service import (
    MetricsUnavailable,
    get_gateway_metrics,
)


logger = logging.getLogger("gateway.history")

PROJECT_ROOT = Path(__file__).resolve().parents[3]

DATABASE_PATH = (
    PROJECT_ROOT
    / "runtime"
    / "data"
    / "gateway_history.sqlite3"
)

SAMPLE_INTERVAL = 5
RETENTION_SECONDS = 7 * 24 * 3600

# 每隔约 30 分钟清理一次过期记录
CLEANUP_INTERVAL_POLLS = 360

# 不同历史区间对应的曲线聚合间隔
HISTORY_BUCKETS = {
    10: 5,
    60: 15,
    360: 90,
}


@contextmanager
def database() -> Iterator[sqlite3.Connection]:
    conn = sqlite3.connect(
        str(DATABASE_PATH),
        timeout=3.0,
    )

    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA busy_timeout=3000")

    try:
        yield conn
        conn.commit()
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def initialize_database() -> None:
    DATABASE_PATH.parent.mkdir(
        parents=True,
        exist_ok=True,
    )

    with database() as conn:
        conn.execute("PRAGMA journal_mode=WAL")
        conn.execute("PRAGMA synchronous=NORMAL")

        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS metric_samples (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                recorded_at INTEGER NOT NULL,

                capture_fps REAL NOT NULL,
                encode_fps REAL NOT NULL,

                captured_frames INTEGER NOT NULL,
                published_packets INTEGER NOT NULL,

                mjpeg_dropped INTEGER NOT NULL,
                h264_dropped INTEGER NOT NULL,

                network_errors INTEGER NOT NULL,
                rtsp_reconnections INTEGER NOT NULL,

                uptime_seconds INTEGER NOT NULL
            )
            """
        )

        conn.execute(
            """
            CREATE INDEX IF NOT EXISTS
            idx_metric_samples_time
            ON metric_samples(recorded_at)
            """
        )

        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS gateway_events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                recorded_at INTEGER NOT NULL,

                level TEXT NOT NULL,
                event_type TEXT NOT NULL,
                message TEXT NOT NULL
            )
            """
        )

        conn.execute(
            """
            CREATE INDEX IF NOT EXISTS
            idx_gateway_events_time
            ON gateway_events(recorded_at)
            """
        )


def insert_event(
    conn: sqlite3.Connection,
    timestamp: int,
    level: str,
    event_type: str,
    message: str,
) -> None:
    conn.execute(
        """
        INSERT INTO gateway_events (
            recorded_at,
            level,
            event_type,
            message
        )
        VALUES (?, ?, ?, ?)
        """,
        (
            timestamp,
            level,
            event_type,
            message,
        ),
    )


def insert_sample(
    conn: sqlite3.Connection,
    timestamp: int,
    metrics: dict[str, Any],
) -> None:
    conn.execute(
        """
        INSERT INTO metric_samples (
            recorded_at,
            capture_fps,
            encode_fps,
            captured_frames,
            published_packets,
            mjpeg_dropped,
            h264_dropped,
            network_errors,
            rtsp_reconnections,
            uptime_seconds
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        (
            timestamp,
            float(metrics.get("capture_fps", 0)),
            float(metrics.get("encode_fps", 0)),
            int(metrics.get("captured_frames", 0)),
            int(metrics.get("published_packets", 0)),
            int(metrics.get("mjpeg_dropped", 0)),
            int(metrics.get("h264_dropped", 0)),
            int(metrics.get("network_errors", 0)),
            int(metrics.get("rtsp_reconnections", 0)),
            int(metrics.get("uptime_seconds", 0)),
        ),
    )


def cleanup_old_records() -> None:
    cutoff = int(time.time()) - RETENTION_SECONDS

    with database() as conn:
        conn.execute(
            """
            DELETE FROM metric_samples
            WHERE recorded_at < ?
            """,
            (cutoff,),
        )

        conn.execute(
            """
            DELETE FROM gateway_events
            WHERE recorded_at < ?
            """,
            (cutoff,),
        )


class MetricsHistoryCollector:
    def __init__(self) -> None:
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

        self._previous: dict[str, Any] | None = None
        self._online: bool | None = None
        self._polls = 0

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return

        initialize_database()

        self._stop.clear()

        self._thread = threading.Thread(
            target=self._run,
            name="gateway-metrics-history",
            daemon=True,
        )

        self._thread.start()

        logger.info(
            "Metrics history collector started: %s",
            DATABASE_PATH,
        )

    def stop(self) -> None:
        self._stop.set()

        if self._thread:
            self._thread.join(timeout=3.0)
            self._thread = None

        logger.info("Metrics history collector stopped")

    def _detect_events(
        self,
        metrics: dict[str, Any],
    ) -> list[tuple[str, str, str]]:
        events = []

        previous = self._previous

        if previous is None:
            return events

        current_uptime = int(
            metrics.get("uptime_seconds", 0)
        )

        previous_uptime = int(
            previous.get("uptime_seconds", 0)
        )

        # 新进程启动后累计指标会重新计数
        if current_uptime < previous_uptime:
            events.append(
                (
                    "info",
                    "gateway_restart",
                    "检测到 C++ 视频网关重新启动",
                )
            )

            return events

        checks = [
            (
                "network_errors",
                "warning",
                "network_error",
                "RTSP 网络错误",
            ),
            (
                "rtsp_reconnections",
                "info",
                "rtsp_reconnect",
                "RTSP 自动重连",
            ),
            (
                "mjpeg_dropped",
                "warning",
                "mjpeg_drop",
                "MJPEG 队列丢帧",
            ),
            (
                "h264_dropped",
                "warning",
                "h264_drop",
                "H.264 队列丢包",
            ),
        ]

        for key, level, event_type, label in checks:
            current = int(metrics.get(key, 0))
            old = int(previous.get(key, 0))

            increase = current - old

            if increase > 0:
                events.append(
                    (
                        level,
                        event_type,
                        f"{label}：新增 {increase} 次",
                    )
                )

        return events

    def _collect_once(self) -> None:
        timestamp = int(time.time())

        try:
            metrics = get_gateway_metrics()

        except (MetricsUnavailable, OSError):
            if self._online is not False:
                with database() as conn:
                    insert_event(
                        conn,
                        timestamp,
                        "warning",
                        "gateway_offline",
                        "C++ 视频网关运行指标不可用",
                    )

                logger.warning(
                    "Gateway metrics unavailable"
                )

            self._online = False
            return

        events = []

        if self._online is not True:
            events.append(
                (
                    "info",
                    "gateway_online",
                    "C++ 视频网关运行指标连接成功",
                )
            )

        events.extend(
            self._detect_events(metrics)
        )

        with database() as conn:
            if metrics.get("sample_ready") is True:
                insert_sample(
                    conn,
                    timestamp,
                    metrics,
                )

            for level, event_type, message in events:
                insert_event(
                    conn,
                    timestamp,
                    level,
                    event_type,
                    message,
                )

        for level, _, message in events:
            if level == "warning":
                logger.warning(message)
            else:
                logger.info(message)

        self._online = True
        self._previous = metrics

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                self._collect_once()

                self._polls += 1

                if (
                    self._polls
                    % CLEANUP_INTERVAL_POLLS
                    == 0
                ):
                    cleanup_old_records()

            except Exception:
                logger.exception(
                    "Failed to collect or save metrics"
                )

            if self._stop.wait(SAMPLE_INTERVAL):
                break


def query_history(
    minutes: int,
) -> dict[str, Any]:
    if minutes not in HISTORY_BUCKETS:
        raise ValueError(
            "Supported ranges: 10, 60, 360 minutes"
        )

    bucket = HISTORY_BUCKETS[minutes]

    since = int(time.time()) - minutes * 60

    with database() as conn:
        rows = conn.execute(
            """
            SELECT
                (recorded_at / ?) * ? AS timestamp,

                ROUND(AVG(capture_fps), 2)
                    AS capture_fps,

                ROUND(AVG(encode_fps), 2)
                    AS encode_fps,

                MAX(mjpeg_dropped)
                    AS mjpeg_dropped,

                MAX(h264_dropped)
                    AS h264_dropped,

                MAX(network_errors)
                    AS network_errors

            FROM metric_samples

            WHERE recorded_at >= ?

            GROUP BY (recorded_at / ?)

            ORDER BY timestamp ASC
            """,
            (
                bucket,
                bucket,
                since,
                bucket,
            ),
        ).fetchall()

    points = [
        dict(row)
        for row in rows
    ]

    return {
        "minutes": minutes,
        "bucket_seconds": bucket,
        "count": len(points),
        "points": points,
    }


def query_events(
    limit: int = 20,
) -> dict[str, Any]:
    limit = max(1, min(limit, 100))

    with database() as conn:
        rows = conn.execute(
            """
            SELECT
                id,
                recorded_at,
                level,
                event_type,
                message

            FROM gateway_events

            ORDER BY id DESC

            LIMIT ?
            """,
            (limit,),
        ).fetchall()

    return {
        "events": [
            dict(row)
            for row in rows
        ]
    }