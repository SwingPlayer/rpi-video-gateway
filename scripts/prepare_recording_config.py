#!/usr/bin/env python3

from pathlib import Path

import yaml


PROJECT_ROOT = Path(__file__).resolve().parents[1]

SOURCE_CONFIG = (
    PROJECT_ROOT / "mediamtx" / "mediamtx.yml"
)

OUTPUT_CONFIG = (
    PROJECT_ROOT / "mediamtx" / "mediamtx.recording.yml"
)

RECORDING_DIR = (
    PROJECT_ROOT / "runtime" / "recordings"
)


def main():
    if not SOURCE_CONFIG.is_file():
        raise SystemExit(
            f"找不到原始配置：{SOURCE_CONFIG}"
        )

    with SOURCE_CONFIG.open(
        "r",
        encoding="utf-8",
    ) as file:
        config = yaml.safe_load(file) or {}

    if not isinstance(config, dict):
        raise SystemExit(
            "MediaMTX 配置格式错误"
        )

    RECORDING_DIR.mkdir(
        parents=True,
        exist_ok=True,
    )

    # 管理接口只允许树莓派本机访问。
    config["api"] = True
    config["apiAddress"] = "127.0.0.1:9997"

    # 录像回放服务同样只监听本机。
    config["playback"] = True
    config["playbackAddress"] = "127.0.0.1:9996"

    paths = config.get("paths") or {}

    if not isinstance(paths, dict):
        raise SystemExit(
            "MediaMTX paths 配置格式错误"
        )

    camera_path = paths.get("cam_cpp") or {}

    if not isinstance(camera_path, dict):
        raise SystemExit(
            "cam_cpp 配置格式错误"
        )

    # 保留当前视频源设置。
    camera_path.setdefault(
        "source",
        "publisher",
    )

    # 服务启动时默认不录像。
    # 之后由 FastAPI 调用 MediaMTX API 开启。
    camera_path["record"] = False

    camera_path["recordPath"] = str(
        RECORDING_DIR
        / "%path"
        / "%Y-%m-%d_%H-%M-%S-%f"
    )

    camera_path["recordFormat"] = "fmp4"
    camera_path["recordPartDuration"] = "1s"
    camera_path["recordSegmentDuration"] = "10m"

    # 自动删除超过 1 天的录像，避免无限占用磁盘。
    camera_path["recordDeleteAfter"] = "1d"

    paths["cam_cpp"] = camera_path
    config["paths"] = paths

    with OUTPUT_CONFIG.open(
        "w",
        encoding="utf-8",
    ) as file:
        yaml.safe_dump(
            config,
            file,
            allow_unicode=True,
            sort_keys=False,
        )

    print("[OK] MediaMTX recording config generated")
    print("[INFO] Source:", SOURCE_CONFIG)
    print("[INFO] Output:", OUTPUT_CONFIG)
    print("[INFO] Recordings:", RECORDING_DIR)
    print("[INFO] Recording enabled at startup: no")
    print("[INFO] Automatic retention: 1 day")


if __name__ == "__main__":
    main()