import subprocess
from typing import Any

GATEWAY_SERVICE = "rpi-video-gateway.service"
MEDIAMTX_SERVICE = "rpi-mediamtx.service"

SYSTEMCTL = "/usr/bin/systemctl"
SUDO = "/usr/bin/sudo"


class ServiceCommandError(Exception):
    pass


def run_command(
    command: list[str],
    timeout: int = 10,
) -> str:
    try:
        result = subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise ServiceCommandError(
            "Command timed out"
        ) from exc
    except OSError as exc:
        raise ServiceCommandError(
            f"Cannot execute command: {exc}"
        ) from exc

    if result.returncode != 0:
        message = (
            result.stderr.strip()
            or result.stdout.strip()
            or f"Exit code {result.returncode}"
        )
        raise ServiceCommandError(message)

    return result.stdout.strip()


def get_service_status(
    service_name: str,
) -> dict[str, Any]:
    if service_name not in (
        GATEWAY_SERVICE,
        MEDIAMTX_SERVICE,
    ):
        raise ServiceCommandError(
            "Unknown service"
        )

    output = run_command(
        [
            SYSTEMCTL,
            "show",
            service_name,
            "--property=LoadState,ActiveState,SubState,UnitFileState,MainPID",
            "--no-pager",
        ],
        timeout=5,
    )

    properties = {}

    for line in output.splitlines():
        if "=" not in line:
            continue

        key, value = line.split("=", 1)
        properties[key] = value

    pid_text = properties.get("MainPID", "0")

    try:
        pid = int(pid_text)
    except ValueError:
        pid = 0

    active_state = properties.get(
        "ActiveState",
        "unknown",
    )

    return {
        "service": service_name,
        "loaded": (
            properties.get("LoadState")
            == "loaded"
        ),
        "running": active_state == "active",
        "active_state": active_state,
        "sub_state": properties.get(
            "SubState",
            "unknown",
        ),
        "enabled": (
            properties.get("UnitFileState")
            == "enabled"
        ),
        "main_pid": pid,
    }


def get_gateway_status() -> dict[str, Any]:
    return {
        "gateway": get_service_status(
            GATEWAY_SERVICE
        ),
        "mediamtx": get_service_status(
            MEDIAMTX_SERVICE
        ),
    }


def control_gateway(
    action: str,
) -> dict[str, Any]:
    if action not in ("start", "stop"):
        raise ServiceCommandError(
            "Unsupported action"
        )

    run_command(
        [
            SUDO,
            "-n",
            SYSTEMCTL,
            action,
            GATEWAY_SERVICE,
        ],
        timeout=15,
    )

    return {
        "requested_action": action,
        "services": get_gateway_status(),
    }
