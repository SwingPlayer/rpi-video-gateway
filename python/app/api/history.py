import sqlite3

from fastapi import APIRouter, HTTPException, Query

from app.services.history_service import (
    query_events,
    query_history,
)


router = APIRouter(
    prefix="/api/metrics",
    tags=["Metrics History"],
)


# ==========================================
# 查询 FPS 历史曲线
# ==========================================

@router.get("/history")
def get_history(
    minutes: int = Query(
        default=10,
        description="历史时间范围，单位分钟",
    ),
):
    # 只允许三种时间范围
    if minutes not in (10, 60, 360):
        raise HTTPException(
            status_code=422,
            detail="minutes must be 10, 60 or 360",
        )

    try:
        return query_history(minutes)

    except (sqlite3.Error, OSError) as exc:
        raise HTTPException(
            status_code=503,
            detail=f"History database unavailable: {exc}",
        ) from exc


# ==========================================
# 查询故障与运行事件
# ==========================================

@router.get("/events")
def get_events(
    limit: int = Query(
        default=20,
        ge=1,
        le=100,
    ),
):
    try:
        return query_events(limit)

    except (sqlite3.Error, OSError) as exc:
        raise HTTPException(
            status_code=503,
            detail=f"Event database unavailable: {exc}",
        ) from exc