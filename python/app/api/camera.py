from fastapi import (
    APIRouter,
    HTTPException,
    Query,
)

from fastapi.responses import StreamingResponse

from app.services.camera_service import (
    ServiceCommandError,
    control_gateway,
    get_gateway_status,
)

from app.services.metrics_service import (
    MetricsUnavailable,
    get_gateway_metrics,
)

from app.services.recording_service import (
    RecordingError,
    get_recording_status,
    list_recordings,
    open_recording_download,
    set_recording,
    stream_recording,
)


router = APIRouter(
    prefix="/api",
    tags=["Video Gateway"],
)


# =====================================
# 摄像头状态与控制
# =====================================

@router.get("/camera/status")
def camera_status():
    try:
        return get_gateway_status()

    except ServiceCommandError as exc:
        raise HTTPException(
            status_code=503,
            detail=str(exc),
        ) from exc


@router.post("/camera/start")
def start_camera():
    try:
        return control_gateway("start")

    except ServiceCommandError as exc:
        raise HTTPException(
            status_code=503,
            detail=str(exc),
        ) from exc


@router.post("/camera/stop")
def stop_camera():
    try:
        return control_gateway("stop")

    except ServiceCommandError as exc:
        raise HTTPException(
            status_code=503,
            detail=str(exc),
        ) from exc


# =====================================
# C++ 实时指标
# =====================================

@router.get("/camera/metrics")
def camera_metrics():
    try:
        return get_gateway_metrics()

    except MetricsUnavailable as exc:
        raise HTTPException(
            status_code=503,
            detail=str(exc),
        ) from exc


# =====================================
# 录像状态与控制
# =====================================

@router.get("/recordings/status")
def recording_status():
    try:
        return get_recording_status()

    except RecordingError as exc:
        raise HTTPException(
            status_code=503,
            detail=str(exc),
        ) from exc


@router.post("/recordings/start")
def recording_start():
    try:
        status = get_gateway_status()

        if not status["gateway"]["running"]:
            raise HTTPException(
                status_code=409,
                detail="摄像头服务未运行，不能开始录像",
            )

        return set_recording(True)

    except ServiceCommandError as exc:
        raise HTTPException(
            status_code=503,
            detail=str(exc),
        ) from exc

    except RecordingError as exc:
        raise HTTPException(
            status_code=503,
            detail=str(exc),
        ) from exc


@router.post("/recordings/stop")
def recording_stop():
    try:
        return set_recording(False)

    except RecordingError as exc:
        raise HTTPException(
            status_code=503,
            detail=str(exc),
        ) from exc


# =====================================
# 录像列表
# =====================================

@router.get("/recordings/list")
def recording_list(
    limit: int = Query(
        default=30,
        ge=1,
        le=100,
    ),
):
    try:
        return list_recordings(limit)

    except RecordingError as exc:
        raise HTTPException(
            status_code=503,
            detail=str(exc),
        ) from exc


# =====================================
# MP4 下载
# =====================================

@router.get("/recordings/download")
def recording_download(
    start: str,
    duration: float = Query(
        gt=0,
        le=3600,
    ),
):
    try:
        response, filename = open_recording_download(
            start,
            duration,
        )

    except RecordingError as exc:
        raise HTTPException(
            status_code=503,
            detail=str(exc),
        ) from exc

    return StreamingResponse(
        stream_recording(response),
        media_type="video/mp4",
        headers={
            "Content-Disposition": (
                f'attachment; filename="{filename}"'
            ),
            "Cache-Control": "no-store",
            "X-Content-Type-Options": "nosniff",
        },
    )


# =====================================
# 浏览器内在线播放录像（新增）
# =====================================

@router.get("/recordings/play")
def recording_play(
    start: str,
    duration: float = Query(
        gt=0,
        le=3600,
    ),
):
    try:
        response, filename = open_recording_download(
            start,
            duration,
        )

    except RecordingError as exc:
        raise HTTPException(
            status_code=503,
            detail=str(exc),
        ) from exc

    return StreamingResponse(
        stream_recording(response),
        media_type="video/mp4",
        headers={
            "Content-Disposition": (
                f'inline; filename="{filename}"'
            ),
            "Cache-Control": "no-store",
            "X-Content-Type-Options": "nosniff",
        },
    )


# =====================================
# 视频流地址
# =====================================

@router.get("/streams")
def get_streams():
    return {
        "stream_path": "cam_cpp",
        "rtsp_local": (
            "rtsp://127.0.0.1:8554/cam_cpp"
        ),
        "rtsp_lan_template": (
            "rtsp://<树莓派IP>:8554/cam_cpp"
        ),
        "webrtc_lan_template": (
            "http://<树莓派IP>:8889/cam_cpp"
        ),
        "hls_lan_template": (
            "http://<树莓派IP>:8888/cam_cpp"
        ),
    }