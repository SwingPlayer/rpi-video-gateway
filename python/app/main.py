from contextlib import asynccontextmanager
from pathlib import Path

import base64
import binascii
import hashlib
import hmac
import ipaddress
import os
import secrets
import subprocess
import threading
import time

from fastapi import FastAPI, Request
from fastapi.responses import (
    FileResponse,
    JSONResponse,
    RedirectResponse,
)
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from app.api.camera import router as camera_router
from app.api.history import router as history_router

from app.services.history_service import (
    MetricsHistoryCollector,
)


# ==================================================
# 工程目录
# ==================================================

PROJECT_ROOT = Path(__file__).resolve().parents[2]

WEB_ROOT = PROJECT_ROOT / "web"
STATIC_DIR = WEB_ROOT / "static"

INDEX_FILE = WEB_ROOT / "templates" / "index.html"
LOGIN_FILE = WEB_ROOT / "templates" / "login.html"
HISTORY_FILE = WEB_ROOT / "templates" / "history.html"


# ==================================================
# 认证配置
# ==================================================

def required_setting(name: str) -> str:
    value = os.environ.get(name, "")

    if not value:
        raise RuntimeError(
            f"Missing authentication setting: {name}"
        )

    return value


USERNAME = required_setting("VIDEO_API_USER")

PASSWORD_SALT = bytes.fromhex(
    required_setting("VIDEO_API_PASSWORD_SALT")
)

PASSWORD_HASH = bytes.fromhex(
    required_setting("VIDEO_API_PASSWORD_HASH")
)

SESSION_SECRET = bytes.fromhex(
    required_setting("VIDEO_API_SESSION_SECRET")
)

if (
    len(PASSWORD_SALT) < 16
    or len(PASSWORD_HASH) != 32
    or len(SESSION_SECRET) < 32
):
    raise RuntimeError(
        "Invalid authentication configuration"
    )


COOKIE_NAME = "rpi_video_session"

SESSION_LIFETIME = 8 * 60 * 60

PASSWORD_ITERATIONS = 600000

LOGIN_FAILURE_LIMIT = 6
LOGIN_WINDOW_SECONDS = 60

failed_logins = {}
failed_logins_lock = threading.Lock()


# ==================================================
# 认证工具
# ==================================================

def verify_password(
    username: str,
    password: str,
) -> bool:

    calculated_hash = hashlib.pbkdf2_hmac(
        "sha256",
        password.encode("utf-8"),
        PASSWORD_SALT,
        PASSWORD_ITERATIONS,
    )

    username_correct = hmac.compare_digest(
        username,
        USERNAME,
    )

    password_correct = hmac.compare_digest(
        calculated_hash,
        PASSWORD_HASH,
    )

    return username_correct and password_correct


def is_login_blocked(ip: str) -> bool:
    now = time.monotonic()

    with failed_logins_lock:
        for address in list(failed_logins):
            valid_records = [
                timestamp
                for timestamp in failed_logins[address]
                if now - timestamp < LOGIN_WINDOW_SECONDS
            ]

            if valid_records:
                failed_logins[address] = valid_records
            else:
                del failed_logins[address]

        return (
            len(failed_logins.get(ip, []))
            >= LOGIN_FAILURE_LIMIT
        )


def record_failed_login(ip: str) -> None:
    with failed_logins_lock:
        failed_logins.setdefault(ip, []).append(
            time.monotonic()
        )


def clear_failed_login(ip: str) -> None:
    with failed_logins_lock:
        failed_logins.pop(ip, None)


# ==================================================
# 登录会话
# ==================================================

def create_session() -> str:
    expires = int(time.time()) + SESSION_LIFETIME
    nonce = secrets.token_urlsafe(20)

    payload = f"{USERNAME}:{expires}:{nonce}"

    encoded_payload = base64.urlsafe_b64encode(
        payload.encode("utf-8")
    ).decode("ascii").rstrip("=")

    signature = hmac.new(
        SESSION_SECRET,
        encoded_payload.encode("ascii"),
        hashlib.sha256,
    ).hexdigest()

    return f"{encoded_payload}.{signature}"


def validate_session(token: str | None) -> bool:
    if not token or len(token) > 512:
        return False

    try:
        encoded_payload, signature = token.split(".", 1)

        expected_signature = hmac.new(
            SESSION_SECRET,
            encoded_payload.encode("ascii"),
            hashlib.sha256,
        ).hexdigest()

        if not hmac.compare_digest(
            signature,
            expected_signature,
        ):
            return False

        padding = "=" * (-len(encoded_payload) % 4)

        payload = base64.urlsafe_b64decode(
            encoded_payload + padding
        ).decode("utf-8")

        username, expires_text, nonce = payload.split(
            ":",
            2,
        )

        if not hmac.compare_digest(
            username,
            USERNAME,
        ):
            return False

        if not nonce:
            return False

        expires = int(expires_text)

        return expires > int(time.time())

    except (
        ValueError,
        UnicodeError,
        binascii.Error,
    ):
        return False


# ==================================================
# FastAPI 生命周期
# ==================================================

@asynccontextmanager
async def lifespan(app: FastAPI):

    collector = MetricsHistoryCollector()

    collector.start()

    try:
        yield
    finally:
        collector.stop()


# ==================================================
# FastAPI 应用
# ==================================================

app = FastAPI(
    title="Raspberry Pi Video Gateway",
    description=(
        "Authenticated Video Gateway "
        "with Historical Metrics"
    ),
    version="0.4.0",
    lifespan=lifespan,
)

app.include_router(camera_router)
app.include_router(history_router)

app.mount(
    "/static",
    StaticFiles(directory=str(STATIC_DIR)),
    name="static",
)


# ==================================================
# 身份认证中间件
# ==================================================

@app.middleware("http")
async def authentication_middleware(
    request: Request,
    call_next,
):
    path = request.url.path

    public_paths = {
        "/login",
        "/api/auth/login",
        "/api/health",
        "/favicon.ico",
    }

    if (
        path in public_paths
        or path.startswith("/static/")
    ):
        return await call_next(request)

    token = request.cookies.get(COOKIE_NAME)

    if not validate_session(token):
        if path.startswith("/api/"):
            return JSONResponse(
                {"detail": "Authentication required"},
                status_code=401,
            )

        return RedirectResponse(
            url="/login",
            status_code=303,
        )

    # 对修改状态的请求执行同源检查
    if request.method not in {
        "GET",
        "HEAD",
        "OPTIONS",
    }:
        origin = request.headers.get("origin", "")

        expected_origin = (
            f"{request.url.scheme}://"
            f"{request.headers.get('host', '')}"
        )

        if origin != expected_origin:
            return JSONResponse(
                {"detail": "Invalid request origin"},
                status_code=403,
            )

    response = await call_next(request)

    if (
        path in {"/", "/history"}
        or path.startswith("/api/")
    ):
        response.headers["Cache-Control"] = "no-store"

    return response


# ==================================================
# 登录
# ==================================================

@app.get("/login", include_in_schema=False)
def login_page(request: Request):

    if validate_session(
        request.cookies.get(COOKIE_NAME)
    ):
        return RedirectResponse(
            url="/",
            status_code=303,
        )

    return FileResponse(
        LOGIN_FILE,
        media_type="text/html",
        headers={"Cache-Control": "no-store"},
    )


class LoginRequest(BaseModel):
    username: str = Field(max_length=100)
    password: str = Field(max_length=256)


@app.post("/api/auth/login")
def login(
    credentials: LoginRequest,
    request: Request,
):
    ip = (
        request.client.host
        if request.client
        else "unknown"
    )

    if is_login_blocked(ip):
        return JSONResponse(
            {
                "detail": (
                    "登录尝试过多，请稍后再试"
                )
            },
            status_code=429,
        )

    if not verify_password(
        credentials.username,
        credentials.password,
    ):
        record_failed_login(ip)

        return JSONResponse(
            {"detail": "账号或密码错误"},
            status_code=401,
        )

    clear_failed_login(ip)

    token = create_session()

    response = JSONResponse(
        {
            "ok": True,
            "username": USERNAME,
        }
    )

    response.set_cookie(
        key=COOKIE_NAME,
        value=token,
        max_age=SESSION_LIFETIME,
        path="/",
        httponly=True,
        secure=(request.url.scheme == "https"),
        samesite="strict",
    )

    response.headers["Cache-Control"] = "no-store"

    return response


# ==================================================
# 退出登录
# ==================================================

@app.get("/logout", include_in_schema=False)
def logout():

    response = RedirectResponse(
        url="/login",
        status_code=303,
    )

    response.delete_cookie(
        COOKIE_NAME,
        path="/",
    )

    response.headers["Cache-Control"] = "no-store"

    return response


# ==================================================
# 网页路由
# ==================================================

@app.get("/", include_in_schema=False)
def index():

    return FileResponse(
        INDEX_FILE,
        media_type="text/html",
        headers={"Cache-Control": "no-store"},
    )


@app.get("/history", include_in_schema=False)
def history_page():

    return FileResponse(
        HISTORY_FILE,
        media_type="text/html",
        headers={"Cache-Control": "no-store"},
    )


@app.get("/api/health")
def health():

    return {
        "ok": True,
        "service": "rpi-video-api",
        "version": "0.4.0",
    }


# ==================================================
# 局域网地址
# ==================================================

def get_ipv4_addresses() -> list[str]:
    try:
        result = subprocess.run(
            ["hostname", "-I"],
            capture_output=True,
            text=True,
            timeout=3,
            check=True,
        )

    except (OSError, subprocess.SubprocessError):
        return []

    addresses = []

    for item in result.stdout.split():
        try:
            address = ipaddress.ip_address(item)
        except ValueError:
            continue

        if address.version != 4:
            continue

        if address.is_loopback or address.is_link_local:
            continue

        value = str(address)

        if value not in addresses:
            addresses.append(value)

    return addresses


@app.get("/api/ui/config")
def ui_config():

    addresses = get_ipv4_addresses()

    return {
        "stream_path": "cam_cpp",
        "suggested_host": (
            addresses[0] if addresses else ""
        ),
        "available_hosts": addresses,
        "webrtc_port": 8889,
        "hls_port": 8888,
        "rtsp_port": 8554,
        "api_listen": "0.0.0.0:8000",
    }