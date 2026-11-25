import json
import socket

from pathlib import Path
from typing import Any


PROJECT_ROOT = Path(__file__).resolve().parents[3]

METRICS_SOCKET = (
    PROJECT_ROOT
    / "runtime"
    / "data"
    / "gateway_metrics.sock"
)


class MetricsUnavailable(Exception):
    pass


def get_gateway_metrics() -> dict[str, Any]:
    data = bytearray()

    try:
        with socket.socket(
            socket.AF_UNIX,
            socket.SOCK_STREAM,
        ) as client:

            client.settimeout(1.0)

            client.connect(str(METRICS_SOCKET))

            while True:
                chunk = client.recv(4096)

                if not chunk:
                    break

                data.extend(chunk)

                if len(data) > 65536:
                    raise MetricsUnavailable(
                        "Metrics response too large"
                    )

    except (OSError, TimeoutError) as exc:
        raise MetricsUnavailable(
            f"C++ gateway metrics unavailable: {exc}"
        ) from exc

    if not data:
        raise MetricsUnavailable(
            "Empty metrics response"
        )

    try:
        result = json.loads(
            data.decode("utf-8")
        )

    except (ValueError, UnicodeError) as exc:
        raise MetricsUnavailable(
            "Invalid metrics JSON"
        ) from exc

    if not isinstance(result, dict):
        raise MetricsUnavailable(
            "Unexpected metrics format"
        )

    result["ipc_connected"] = True

    return result