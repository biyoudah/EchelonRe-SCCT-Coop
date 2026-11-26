import atexit
import datetime
from functools import wraps
import hashlib
import hmac
import ipaddress
import json
import os
import queue
import re
import secrets
import shutil
import sqlite3
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

from flask import Flask, Response, g, jsonify, make_response, request, send_from_directory

FROZEN = bool(getattr(sys, "frozen", False))
# Resources (static/, bundled mod files) live in _internal when frozen with PyInstaller --onedir.
LAUNCHER_DIR = getattr(sys, "_MEIPASS", None) or os.path.dirname(os.path.abspath(__file__))
APP_DIR = os.path.dirname(os.path.abspath(sys.executable)) if FROZEN else LAUNCHER_DIR
if LAUNCHER_DIR not in sys.path:
    sys.path.insert(0, LAUNCHER_DIR)

import ws_bridge  # noqa: E402

COOP_NET_DIR = os.path.dirname(APP_DIR)


def _first_existing_dir(*cands):
    cands = [c for c in cands if c]
    for c in cands:
        if os.path.isdir(c):
            return c
    return cands[0]


# Mod payload (ScctCoopNet.dll, scct-coop-launch.exe, scct-coop-net.ini): ./mod next to the app,
# or the original ../build/Release layout of the dev tree. Override with SCCT_MOD_DIR.
BUILD_RELEASE_DIR = _first_existing_dir(
    os.environ.get("SCCT_MOD_DIR"),
    os.path.join(APP_DIR, "mod"),
    os.path.join(LAUNCHER_DIR, "mod"),
    os.path.join(COOP_NET_DIR, "build", "Release"),
)
INI_SOURCE_DIRS = [BUILD_RELEASE_DIR, COOP_NET_DIR]

if os.name == "nt" and os.environ.get("LOCALAPPDATA"):
    DATA_DIR = os.path.join(os.environ["LOCALAPPDATA"], "SCCTCoopLauncher", "data")
else:
    DATA_DIR = os.path.join(APP_DIR, "data")
DB_PATH = os.path.join(DATA_DIR, "launcher_local.db")
STATIC_DIR = os.path.join(LAUNCHER_DIR, "static")

CLOUD_API_URL = os.environ.get(
    "SCCT_CLOUD_URL",
    "https://scct-revival-directory.echelonre.workers.dev",
).rstrip("/")

KNOWN_EXES = {
    "9a65e326df1b273ce24eae904beabfb2": ("Retail v1.05", 9867264),
    "33c0951a4a80f2a97daf3e302c534232": ("Ubisoft v1.05", 9877160),
}
COMMON_EXE_CANDIDATES = [
    r"D:\Jeux\Tom Clancys Splinter Cell Chaos Theory\System\splintercell3.exe",
    r"C:\Program Files (x86)\Ubisoft\Tom Clancy's Splinter Cell Chaos Theory\System\splintercell3.exe",
    r"C:\Program Files (x86)\Ubisoft\Ubisoft Game Launcher\games\Splinter Cell Chaos Theory\System\splintercell3.exe",
    r"C:\Program Files (x86)\Steam\steamapps\common\Splintercell Chaos Theory\System\splintercell3.exe",
]

ROOM_CODE_RE = re.compile(r"^SCCT-[A-Z0-9]{6}$")
SESSION_TTL_DAYS = 7
CREATE_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
APP_VERSION = "2.2.0"
UPDATE_FEED_URL = os.environ.get(
    "SCCT_UPDATE_URL",
    "https://github.com/biyoudah/EchelonRe-SCCT-Coop/releases/latest/download",
).rstrip("/")
CONTENT_MANIFEST_URL = os.environ.get(
    "SCCT_CONTENT_MANIFEST_URL",
    "https://raw.githubusercontent.com/biyoudah/EchelonRe-SCCT-Coop/main/content-manifest.json",
).strip()
ALLOWED_DOWNLOAD_HOSTS = {
    "github.com",
    "objects.githubusercontent.com",
    "raw.githubusercontent.com",
    "release-assets.githubusercontent.com",
    "scct-revival-directory.echelonre.workers.dev",
}
PROTECTED_RETAIL_BASENAMES = {
    "splintercell3.exe",
    "maps.sm0",
    "stream.ss0",
    "maps.lm0",
    "stream.ls0",
}
SCDA_ORDERED_MAPS = [
    "scda_ice_a", "scda_ice_b", "scda_ice_c", "scda_ice_d", "scda_ice_e",
    "scda_pris_a", "scda_pris_b", "scda_pris_c", "scda_pris_d", "scda_pris_e",
    "scda_kin_a", "scda_kin_b", "scda_kin_c", "scda_kin_f",
]
STOCK_COOP_ROTATION = [
    "00_Training_COOP",
    "01_Panama",
    "02_seoulthree",
    "03_ChemBunker",
    "04_GCS",
    "07_UNhq",
    "05_NuclearPlant",
]
DEFAULT_CONTENT_CATALOG = [
    {
        "id": "scda-coop-maps",
        "name": "SCDA Co-op Maps Pack",
        "version": "1.0.0",
        "description": "All 14 ported Splinter Cell: Double Agent Xbox co-op missions (Iceland, Prison, Kinshasa) with DareAudio SFX/dialogue banks, loading screens, and localization.",
        "credits": "SCDA assets ported using SCDA-XBERecomp by TGP482 (https://github.com/TGP482/SCDA-XBERecomp)",
        "size_mb": 697,
        "download_url": "https://scct-revival-directory.echelonre.workers.dev/v2/content/scda-coop-maps",
        "sha256": "b9b25d3eec09ccfa3a4d3b521a705ebe2dccb14850ea559f1c2c7531fa8b6ea7",
        "check_files": [f"Data/Maps/{m}.scl" for m in SCDA_ORDERED_MAPS] + [
            "Data/Sounds/SCDA_MAPS.SM0",
            "System/Localization/scda_override.int",
        ],
    },
    {
        "id": "widescreen-fix",
        "name": "Widescreen Fix (ThirteenAG)",
        "version": "1.0",
        "description": "16:9 / ultrawide resolution, HUD scaling, and FOV support for Chaos Theory.",
        "credits": "By ThirteenAG (https://github.com/ThirteenAG/WidescreenFixesPack)",
        "size_mb": 13,
        "download_url": "https://github.com/ThirteenAG/WidescreenFixesPack/releases/download/scct/SplinterCellChaosTheory.WidescreenFix.zip",
        "sha256": "fffc1ac025eef78155a5b0d6912c1d25cb4a34f8104fafc5fcf1476ca619a1b5",
        "check_files": ["System/scripts/SplinterCellChaosTheory.WidescreenFix.asi"],
    },
]

os.makedirs(DATA_DIR, exist_ok=True)
app = Flask(__name__, static_folder=STATIC_DIR, static_url_path="")
_db_lock = threading.RLock()
_proc_lock = threading.Lock()
_bridge = None
_game_procs = {}  # username_lower -> Popen
_ingame_tokens = {}  # token -> {"cloud_token": str, "username_lower": str}
_content_jobs = {}  # pack_id -> dict
_SERVER_PORT = 27180
_subs = set()
_subs_lock = threading.Lock()
# Set by start_launcher.py to a callable returning a path (native pywebview file dialog).
NATIVE_BROWSE_HOOK = None


def _sse(event, data):
    return f"event: {event}\ndata: {json.dumps(data)}\n\n"


def publish(event, data):
    msg = _sse(event, data)
    with _subs_lock:
        subs = list(_subs)
    for q in subs:
        try:
            q.put_nowait(msg)
        except queue.Full:
            pass


def any_game_running():
    with _proc_lock:
        return any(p.poll() is None for p in _game_procs.values())


def idle_relay_status():
    return {"running": False, "room_code": "", "local_relay": "", "local_port": 0, "ws_connected": False,
            "partner_connected": False, "tx_packets": 0, "rx_packets": 0, "last_error": ""}


def _watch_game(username_lower, proc):
    proc.wait()
    with _proc_lock:
        if _game_procs.get(username_lower) is proc:
            _game_procs.pop(username_lower, None)
    publish("game", {"running": any_game_running()})


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc)


def utc_now_iso():
    return utc_now().isoformat(timespec="seconds")


def sha256_text(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest().lower()


def restrict_windows_acl(path, is_dir=False):
    user_acct = os.environ.get("USERNAME", "").strip()
    if not user_acct or not os.path.exists(path):
        return
    try:
        perm = f"{user_acct}:(OI)(CI)F" if is_dir else f"{user_acct}:F"
        subprocess.run(
            ["icacls", path, "/inheritance:r", "/grant:r", perm],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            creationflags=CREATE_NO_WINDOW,
            timeout=5,
        )
    except Exception:
        pass


def get_db():
    db_parent = os.path.dirname(os.path.abspath(DB_PATH))
    os.makedirs(db_parent, exist_ok=True)
    conn = sqlite3.connect(DB_PATH, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL;")
    conn.execute("PRAGMA foreign_keys=ON;")
    conn.execute("PRAGMA synchronous=FULL;")
    conn.execute("PRAGMA busy_timeout=5000;")
    return conn


def init_db():
    db_parent = os.path.dirname(os.path.abspath(DB_PATH))
    os.makedirs(db_parent, exist_ok=True)
    if os.path.normcase(db_parent) == os.path.normcase(os.path.abspath(DATA_DIR)):
        restrict_windows_acl(db_parent, is_dir=True)

    with _db_lock:
        conn = get_db()
        try:
            conn.executescript(
                """
                CREATE TABLE IF NOT EXISTS user_prefs (
                    username_lower TEXT PRIMARY KEY,
                    username TEXT NOT NULL,
                    exe_path TEXT NOT NULL DEFAULT '',
                    exe_md5 TEXT NOT NULL DEFAULT '',
                    exe_verified INTEGER NOT NULL DEFAULT 0,
                    friendly_fire INTEGER NOT NULL DEFAULT 0,
                    voice_chat INTEGER NOT NULL DEFAULT 1,
                    mic_device INTEGER NOT NULL DEFAULT -1,
                    max_fps INTEGER NOT NULL DEFAULT 60,
                    local_ip TEXT NOT NULL DEFAULT '',
                    updated_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS local_sessions (
                    local_token_hash TEXT PRIMARY KEY,
                    csrf_token TEXT NOT NULL,
                    cloud_token TEXT NOT NULL,
                    username_lower TEXT NOT NULL,
                    expires_at TEXT NOT NULL,
                    created_at TEXT NOT NULL
                );
                """
            )
            cols = {r["name"] for r in conn.execute("PRAGMA table_info(user_prefs)").fetchall()}
            if "mic_device" not in cols:
                conn.execute("ALTER TABLE user_prefs ADD COLUMN mic_device INTEGER NOT NULL DEFAULT -1")
            if "max_fps" not in cols:
                conn.execute("ALTER TABLE user_prefs ADD COLUMN max_fps INTEGER NOT NULL DEFAULT 60")
            conn.commit()
        finally:
            conn.close()
        restrict_windows_acl(DB_PATH, is_dir=False)


init_db()


def cloud_request(method, path, body=None, bearer_token=None, timeout=15):
    url = f"{CLOUD_API_URL}{path}"
    headers = {
        "User-Agent": "SCCT-Coop-Launcher/1.0",
        "Accept": "application/json",
    }
    data_bytes = None
    if body is not None:
        headers["Content-Type"] = "application/json"
        data_bytes = json.dumps(body).encode("utf-8")
    if bearer_token:
        headers["Authorization"] = f"Bearer {bearer_token}"

    req = urllib.request.Request(url, data=data_bytes, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read().decode("utf-8", errors="replace")
            return resp.status, (json.loads(raw) if raw else {})
    except urllib.error.HTTPError as err:
        raw = err.read().decode("utf-8", errors="replace")
        try:
            parsed = json.loads(raw) if raw else {}
        except Exception:
            parsed = {"ok": False, "error": f"Cloud service HTTP {err.code}"}
        return err.code, parsed
    except Exception as exc:
        return 502, {"ok": False, "error": f"Unable to reach cloud service: {exc}"}


def file_md5_and_size(path):
    h = hashlib.md5()
    total = 0
    with open(path, "rb") as fh:
        while True:
            chunk = fh.read(1 << 16)
            if not chunk:
                break
            h.update(chunk)
            total += len(chunk)
    return h.hexdigest().lower(), total


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        while True:
            chunk = fh.read(1 << 16)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest().lower()


def detect_existing_exe():
    for cand in COMMON_EXE_CANDIDATES:
        if os.path.isfile(cand):
            return os.path.dirname(os.path.dirname(cand))
    return ""


def is_safe_exe_path(raw_path):
    p = (raw_path or "").strip().strip('"')
    if not p or any(ord(c) < 32 for c in p):
        return False, ""
    if p.startswith("\\\\") or p.startswith("//"):
        return False, ""
    norm = os.path.normpath(p)
    if not os.path.isabs(norm):
        return False, ""
    return True, os.path.realpath(norm)


def resolve_game_exe(raw_path):
    ok_path, clean_path = is_safe_exe_path(raw_path)
    if not ok_path:
        return False, "", ""
    if os.path.isdir(clean_path):
        for cand in (
            os.path.join(clean_path, "System", "splintercell3.exe"),
            os.path.join(clean_path, "splintercell3.exe"),
        ):
            if os.path.isfile(cand):
                exe_real = os.path.realpath(cand)
                sys_dir = os.path.dirname(exe_real)
                root_dir = os.path.dirname(sys_dir) if os.path.basename(sys_dir).lower() == "system" else sys_dir
                return True, exe_real, root_dir
        return True, "", clean_path
    sys_dir = os.path.dirname(clean_path)
    root_dir = os.path.dirname(sys_dir) if os.path.basename(sys_dir).lower() == "system" else sys_dir
    return True, clean_path, root_dir


def verify_v105_pe_fingerprint(exe_path):
    """Verify 32-bit PE ImageBase=0x10900000 and v1.05 code/vtable anchors without external deps."""
    try:
        with open(exe_path, "rb") as fh:
            dos = fh.read(64)
            if len(dos) < 64 or dos[:2] != b"MZ":
                return False
            pe_off = struct.unpack_from("<I", dos, 0x3C)[0]
            fh.seek(pe_off)
            pe_hdr = fh.read(24)
            if len(pe_hdr) < 24 or pe_hdr[:4] != b"PE\x00\x00":
                return False
            num_sections = struct.unpack_from("<H", pe_hdr, 6)[0]
            opt_size = struct.unpack_from("<H", pe_hdr, 20)[0]
            opt = fh.read(opt_size)
            if len(opt) < 32 or struct.unpack_from("<H", opt, 0)[0] != 0x10B:
                return False
            image_base = struct.unpack_from("<I", opt, 28)[0]
            if image_base != 0x10900000:
                return False
            sections = []
            for _ in range(num_sections):
                sec = fh.read(40)
                if len(sec) < 40:
                    return False
                vsize, va, rsize, roff = struct.unpack_from("<IIII", sec, 8)
                sections.append((va, max(vsize, rsize), roff, rsize))

            def read_rva(rva, n):
                for va, span, roff, rsize in sections:
                    if va <= rva and rva + n <= va + min(span, rsize):
                        fh.seek(roff + (rva - va))
                        return fh.read(n)
                return b""

            if read_rva(0x143920, 10) != bytes.fromhex("83EC1C5355568BF18A86"):
                return False
            if read_rva(0x301BF0, 10) != bytes.fromhex("81ECC401000053558BD9"):
                return False
            if read_rva(0x7D6200, 4) != struct.pack("<I", 0x10A43920):
                return False
            return True
    except Exception:
        return False


def inspect_and_install_mod(exe_path, install=True):
    ok_path, clean_exe, game_root = resolve_game_exe(exe_path)
    info = {
        "exe_path": clean_exe or (exe_path or "").strip(),
        "game_dir": game_root,
        "exe_exists": False,
        "exe_size": 0,
        "exe_md5": "",
        "md5_ok": False,
        "exe_edition": "",
        "dll_installed": False,
        "launch_exe_installed": False,
        "ini_present": False,
        "source_dll_available": os.path.isfile(os.path.join(BUILD_RELEASE_DIR, "ScctCoopNet.dll")),
        "source_launch_available": os.path.isfile(os.path.join(BUILD_RELEASE_DIR, "scct-coop-launch.exe")),
        "installed_now": [],
        "ready": False,
        "error": "",
    }
    if not (exe_path or "").strip():
        return info
    if not ok_path:
        info["error"] = "Invalid or unsafe path (local absolute path required)."
        return info
    if not clean_exe or not os.path.isfile(clean_exe):
        info["error"] = "splintercell3.exe not found in the selected game folder."
        return info
    if os.path.basename(clean_exe).lower() != "splintercell3.exe":
        info["error"] = "Please select your Splinter Cell: Chaos Theory game folder or splintercell3.exe."
        return info

    info["exe_path"] = clean_exe
    info["game_dir"] = game_root
    info["exe_exists"] = True
    md5_hex, size = file_md5_and_size(clean_exe)
    info["exe_size"] = size
    info["exe_md5"] = md5_hex
    if md5_hex in KNOWN_EXES:
        edition, expected_sz = KNOWN_EXES[md5_hex]
        if size == expected_sz:
            info["md5_ok"] = True
            info["exe_edition"] = edition
    if not info["md5_ok"] and verify_v105_pe_fingerprint(clean_exe):
        info["md5_ok"] = True
        info["exe_edition"] = "v1.05 Compatible"

    game_dir = os.path.dirname(clean_exe)
    src_dll = os.path.join(BUILD_RELEASE_DIR, "ScctCoopNet.dll")
    dst_dll = os.path.join(game_dir, "ScctCoopNet.dll")
    src_proxy = os.path.join(BUILD_RELEASE_DIR, "AVIFIL32.dll")
    dst_proxy = os.path.join(game_dir, "AVIFIL32.dll")
    src_launch = os.path.join(BUILD_RELEASE_DIR, "scct-coop-launch.exe")
    dst_launch = os.path.join(game_dir, "scct-coop-launch.exe")
    src_ini = next((os.path.join(d, "scct-coop-net.ini") for d in INI_SOURCE_DIRS if os.path.isfile(os.path.join(d, "scct-coop-net.ini"))), "")
    dst_ini = os.path.join(game_dir, "scct-coop-net.ini")

    if install:
        try:
            if os.path.isfile(src_dll):
                need_dll = (not os.path.isfile(dst_dll)) or (file_sha256(src_dll) != file_sha256(dst_dll))
                if need_dll:
                    shutil.copy2(src_dll, dst_dll)
                    info["installed_now"].append("ScctCoopNet.dll")
            if os.path.isfile(src_proxy):
                need_proxy = (not os.path.isfile(dst_proxy)) or (file_sha256(src_proxy) != file_sha256(dst_proxy))
                if need_proxy:
                    shutil.copy2(src_proxy, dst_proxy)
                    info["installed_now"].append("AVIFIL32.dll")
            if os.path.isfile(src_launch):
                need_launch = (not os.path.isfile(dst_launch)) or (file_sha256(src_launch) != file_sha256(dst_launch))
                if need_launch:
                    shutil.copy2(src_launch, dst_launch)
                    info["installed_now"].append("scct-coop-launch.exe")
            if (not os.path.isfile(dst_ini)) and src_ini:
                shutil.copy2(src_ini, dst_ini)
                info["installed_now"].append("scct-coop-net.ini")
        except PermissionError:
            info["error"] = "Permission denied writing to the game folder. Run the launcher as Administrator (required for Program Files)."

    info["dll_installed"] = os.path.isfile(dst_dll)
    info["launch_exe_installed"] = os.path.isfile(dst_launch)
    info["ini_present"] = os.path.isfile(dst_ini)
    info["ready"] = info["exe_exists"] and info["md5_ok"] and info["dll_installed"] and info["launch_exe_installed"]
    return info


def get_or_create_prefs(username):
    low = username.lower()
    with _db_lock:
        conn = get_db()
        try:
            row = conn.execute("SELECT * FROM user_prefs WHERE username_lower=?", (low,)).fetchone()
            if not row:
                # Reuse last linked EXE path if another local profile already linked this PC's install.
                prev = conn.execute("SELECT exe_path, exe_md5, exe_verified FROM user_prefs WHERE exe_verified=1 LIMIT 1").fetchone()
                exe_path = prev["exe_path"] if prev else ""
                exe_md5 = prev["exe_md5"] if prev else ""
                exe_verified = prev["exe_verified"] if prev else 0
                conn.execute(
                    """
                    INSERT INTO user_prefs (username_lower, username, exe_path, exe_md5, exe_verified, friendly_fire, voice_chat, mic_device, max_fps, local_ip, updated_at)
                    VALUES (?, ?, ?, ?, ?, 0, 1, -1, 60, '', ?)
                    """,
                    (low, username, exe_path, exe_md5, exe_verified, utc_now_iso()),
                )
                conn.commit()
                row = conn.execute("SELECT * FROM user_prefs WHERE username_lower=?", (low,)).fetchone()
            return dict(row)
        finally:
            conn.close()


def issue_local_session(cloud_token, username):
    raw_local = secrets.token_urlsafe(32)
    csrf_token = secrets.token_urlsafe(32)
    local_hash = sha256_text(raw_local)
    now = utc_now()
    expires = now + datetime.timedelta(days=SESSION_TTL_DAYS)
    with _db_lock:
        conn = get_db()
        try:
            conn.execute("DELETE FROM local_sessions WHERE expires_at <= ?", (now.isoformat(timespec="seconds"),))
            conn.execute(
                """
                INSERT INTO local_sessions (local_token_hash, csrf_token, cloud_token, username_lower, expires_at, created_at)
                VALUES (?, ?, ?, ?, ?, ?)
                """,
                (local_hash, csrf_token, cloud_token, username.lower(), expires.isoformat(timespec="seconds"), now.isoformat(timespec="seconds")),
            )
            conn.commit()
        finally:
            conn.close()
    return raw_local, csrf_token


def get_bridge_status():
    with _proc_lock:
        if _bridge is None:
            return {
                "running": False,
                "room_code": "",
                "local_relay": "",
                "local_port": 0,
                "ws_connected": False,
                "partner_connected": False,
                "tx_packets": 0,
                "rx_packets": 0,
                "last_error": "",
            }
        return _bridge.get_status()


def stop_bridge():
    global _bridge
    with _proc_lock:
        bridge, _bridge = _bridge, None
    if bridge is not None:
        bridge.on_status_change = None
        try:
            bridge.stop()
        except Exception:
            pass
        publish("relay", idle_relay_status())


def start_bridge(room_code, cloud_token):
    global _bridge
    stop_bridge()
    bridge = ws_bridge.CloudRelayBridge(
        CLOUD_API_URL, room_code, cloud_token, local_port=27120,
        on_status_change=lambda st: publish("relay", st),
    )
    bridge.start()
    if not bridge.wait_connected(timeout=6.0):
        err = bridge.last_error or "Timed out connecting WebSocket relay."
        bridge.on_status_change = None
        bridge.stop()
        return False, err
    with _proc_lock:
        _bridge = bridge
    status = bridge.get_status()
    publish("relay", status)
    return True, status


atexit.register(stop_bridge)


@app.before_request
def check_origin_on_mutation():
    if request.method in ("POST", "PUT", "DELETE", "PATCH"):
        origin = request.headers.get("Origin", "").strip()
        referer = request.headers.get("Referer", "").strip()
        check_url = origin or referer
        if check_url:
            if check_url.lower() == "null":
                return jsonify({"ok": False, "error": "Invalid Origin."}), 403
            u = urllib.parse.urlparse(check_url)
            parts = (request.host or "").split(":", 1)
            req_host = parts[0].lower()
            try:
                req_port = int(parts[1]) if len(parts) > 1 else (443 if request.scheme == "https" else 80)
            except ValueError:
                req_port = 80
            u_port = u.port if u.port is not None else (443 if u.scheme == "https" else 80)
            if (u.hostname or "").lower() not in ("127.0.0.1", "localhost", req_host):
                return jsonify({"ok": False, "error": "Cross-origin request blocked."}), 403
            if u_port != req_port:
                return jsonify({"ok": False, "error": "Cross-port request blocked."}), 403


@app.after_request
def add_security_headers(resp):
    resp.headers["X-Content-Type-Options"] = "nosniff"
    resp.headers["X-Frame-Options"] = "DENY"
    resp.headers["Referrer-Policy"] = "no-referrer"
    resp.headers["Content-Security-Policy"] = (
        "default-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'"
    )
    if request.path.startswith("/api/"):
        resp.headers["Cache-Control"] = "no-store"
    return resp


def get_local_auth():
    raw_token = request.cookies.get("scct_session", "").strip()
    if not raw_token:
        return None
    local_hash = sha256_text(raw_token)
    now_iso = utc_now_iso()
    with _db_lock:
        conn = get_db()
        try:
            row = conn.execute("SELECT * FROM local_sessions WHERE local_token_hash=?", (local_hash,)).fetchone()
            if not row:
                return None
            if row["expires_at"] <= now_iso:
                conn.execute("DELETE FROM local_sessions WHERE local_token_hash=?", (local_hash,))
                conn.commit()
                return None
            return dict(row)
        finally:
            conn.close()


def require_auth(fn):
    @wraps(fn)
    def wrapper(*args, **kwargs):
        sess = get_local_auth()
        if not sess:
            return jsonify({"ok": False, "error": "Authentication required."}), 401
        if request.method in ("POST", "PUT", "DELETE", "PATCH"):
            provided_csrf = request.headers.get("X-CSRF-Token", "").strip()
            if not provided_csrf or not hmac.compare_digest(provided_csrf, sess["csrf_token"]):
                return jsonify({"ok": False, "error": "Invalid or missing CSRF token."}), 403
        g.csrf_token = sess["csrf_token"]
        return fn(sess, *args, **kwargs)

    return wrapper


def build_user_view(cloud_user):
    prefs = get_or_create_prefs(cloud_user["username"])
    mod = inspect_and_install_mod(prefs["exe_path"], install=False)
    relay = get_bridge_status()
    return {
        "id": cloud_user["id"],
        "username": cloud_user["username"],
        "email": cloud_user.get("email", ""),
        "email_verified": bool(cloud_user.get("email_verified", True)),
        "discord_id": cloud_user.get("discord_id", ""),
        "discord_username": cloud_user.get("discord_username", ""),
        "exe_path": mod["game_dir"] or prefs["exe_path"],
        "exe_verified": bool(mod["md5_ok"]),
        "exe_md5": mod["exe_md5"],
        "exe_edition": mod["exe_edition"],
        "dll_installed": mod["dll_installed"],
        "launch_exe_installed": mod["launch_exe_installed"],
        "mod_ready": mod["ready"],
        "friendly_fire": bool(prefs["friendly_fire"]),
        "voice_chat": bool(prefs["voice_chat"]),
        "mic_device": int(prefs.get("mic_device", -1) if prefs.get("mic_device") is not None else -1),
        "max_fps": int(prefs.get("max_fps", 60) if prefs.get("max_fps") is not None else 60),
        "local_ip": prefs["local_ip"],
        "active_room": relay["room_code"] or cloud_user.get("active_room", ""),
        "detected_exe_hint": detect_existing_exe(),
    }


@app.route("/")
def index():
    return send_from_directory(STATIC_DIR, "index.html")


@app.route("/api/health")
def health():
    return jsonify({"ok": True, "cloud_url": CLOUD_API_URL, "relay": get_bridge_status()})


@app.route("/api/auth/register", methods=["POST"])
def auth_register():
    data = request.get_json(silent=True) or {}
    st, body = cloud_request(
        "POST",
        "/v2/auth/register",
        {
            "username": data.get("username", ""),
            "email": data.get("email", ""),
            "password": data.get("password", ""),
        },
    )
    return jsonify(body), st


@app.route("/api/auth/verify-email", methods=["POST"])
def auth_verify_email():
    data = request.get_json(silent=True) or {}
    st, body = cloud_request(
        "POST",
        "/v2/auth/verify-email",
        {
            "username_or_email": data.get("username_or_email") or data.get("username") or data.get("email") or "",
            "code": data.get("code", ""),
        },
    )
    if st != 200 or not body.get("ok") or not body.get("token"):
        return jsonify(body), st

    cloud_token = body["token"]
    cloud_user = body["user"]
    raw_local, csrf_token = issue_local_session(cloud_token, cloud_user["username"])
    resp = make_response(
        jsonify(
            {
                "ok": True,
                "csrf_token": csrf_token,
                "user": build_user_view(cloud_user),
                "relay": get_bridge_status(),
            }
        )
    )
    resp.set_cookie("scct_session", raw_local, httponly=True, samesite="Strict", max_age=SESSION_TTL_DAYS * 86400)
    return resp


@app.route("/api/auth/resend-code", methods=["POST"])
def auth_resend_code():
    data = request.get_json(silent=True) or {}
    st, body = cloud_request(
        "POST",
        "/v2/auth/resend-code",
        {"username_or_email": data.get("username_or_email") or data.get("username") or data.get("email") or ""},
    )
    return jsonify(body), st


@app.route("/api/auth/forgot-password", methods=["POST"])
def auth_forgot_password():
    data = request.get_json(silent=True) or {}
    st, body = cloud_request(
        "POST",
        "/v2/auth/forgot-password",
        {"email": data.get("email", "")},
    )
    return jsonify(body), st


@app.route("/api/auth/reset-password", methods=["POST"])
def auth_reset_password():
    data = request.get_json(silent=True) or {}
    st, body = cloud_request(
        "POST",
        "/v2/auth/reset-password",
        {
            "email": data.get("email", ""),
            "code": data.get("code", ""),
            "password": data.get("password", ""),
        },
    )
    if st == 200 and body.get("ok") and body.get("token"):
        cloud_token = body["token"]
        cloud_user = body["user"]
        raw_local, csrf_token = issue_local_session(cloud_token, cloud_user["username"])
        resp = make_response(
            jsonify(
                {
                    "ok": True,
                    "csrf_token": csrf_token,
                    "user": build_user_view(cloud_user),
                    "relay": get_bridge_status(),
                }
            )
        )
        resp.set_cookie(
            "scct_session",
            raw_local,
            httponly=True,
            samesite="Strict",
            max_age=SESSION_TTL_DAYS * 86400,
        )
        return resp
    return jsonify(body), st


@app.route("/api/auth/discord/authorize-url", methods=["GET"])
def auth_discord_authorize_url():
    """Return the Discord OAuth2 authorize URL so the launcher can open it in the browser."""
    return jsonify({
        "ok": True,
        "url": f"{CLOUD_API_URL}/v2/auth/discord",
    })


@app.route("/api/auth/discord/link", methods=["POST"])
@require_auth
def auth_discord_link(sess):
    data = request.get_json(silent=True) or {}
    st, body = cloud_request(
        "POST",
        "/v2/auth/discord/link",
        {"link_token": data.get("link_token", "")},
        bearer_token=sess["cloud_token"],
    )
    return jsonify(body), st


@app.route("/api/auth/discord/unlink", methods=["POST"])
@require_auth
def auth_discord_unlink(sess):
    st, body = cloud_request("POST", "/v2/auth/discord/unlink", {}, bearer_token=sess["cloud_token"])
    return jsonify(body), st


@app.route("/api/open-url", methods=["POST"])
def open_url():
    data = request.get_json(silent=True) or {}
    target_url = str(data.get("url", "")).strip()
    if target_url.startswith("https://") or target_url.startswith("http://"):
        import webbrowser
        webbrowser.open(target_url)
        return jsonify({"ok": True})
    return jsonify({"ok": False, "error": "Invalid URL"}), 400


@app.route("/api/auth/login", methods=["POST"])
def auth_login():
    data = request.get_json(silent=True) or {}
    st, body = cloud_request(
        "POST",
        "/v2/auth/login",
        {"username": data.get("username", ""), "password": data.get("password", "")},
    )
    if st != 200 or not body.get("ok") or not body.get("token"):
        return jsonify(body), st

    cloud_token = body["token"]
    cloud_user = body["user"]
    raw_local, csrf_token = issue_local_session(cloud_token, cloud_user["username"])
    resp = make_response(
        jsonify(
            {
                "ok": True,
                "csrf_token": csrf_token,
                "user": build_user_view(cloud_user),
                "relay": get_bridge_status(),
            }
        )
    )
    resp.set_cookie("scct_session", raw_local, httponly=True, samesite="Strict", max_age=SESSION_TTL_DAYS * 86400)
    return resp


@app.route("/api/auth/logout", methods=["POST"])
@require_auth
def auth_logout(sess):
    relay = get_bridge_status()
    if relay["running"]:
        cloud_request("POST", "/v2/rooms/close", {}, bearer_token=sess["cloud_token"])
        stop_bridge()
    cloud_request("POST", "/v2/auth/logout", {}, bearer_token=sess["cloud_token"])
    with _db_lock:
        conn = get_db()
        try:
            conn.execute("DELETE FROM local_sessions WHERE local_token_hash=?", (sess["local_token_hash"],))
            conn.commit()
        finally:
            conn.close()
    resp = make_response(jsonify({"ok": True}))
    resp.delete_cookie("scct_session")
    return resp


@app.route("/api/auth/me", methods=["GET"])
@require_auth
def auth_me(sess):
    st, body = cloud_request("GET", "/v2/auth/me", bearer_token=sess["cloud_token"])
    if st == 401:
        with _db_lock:
            conn = get_db()
            try:
                conn.execute("DELETE FROM local_sessions WHERE local_token_hash=?", (sess["local_token_hash"],))
                conn.commit()
            finally:
                conn.close()
        resp = make_response(jsonify({"ok": False, "error": "Session expired."}), 401)
        resp.delete_cookie("scct_session")
        return resp
    if st != 200 or not body.get("ok"):
        return jsonify(body), st

    return jsonify(
        {
            "ok": True,
            "csrf_token": g.csrf_token,
            "user": build_user_view(body["user"]),
            "relay": get_bridge_status(),
        }
    )


@app.route("/api/setup/browse", methods=["POST"])
@require_auth
def setup_browse(sess):
    if NATIVE_BROWSE_HOOK is not None:
        try:
            return jsonify({"ok": True, "path": NATIVE_BROWSE_HOOK() or ""})
        except Exception as exc:
            return jsonify({"ok": False, "error": str(exc)}), 500
    if FROZEN:
        return jsonify({"ok": False, "error": "Native file dialog unavailable; paste the path instead."}), 501
    script = (
        "import tkinter as tk; from tkinter import filedialog; "
        "r = tk.Tk(); r.withdraw(); r.attributes('-topmost', True); "
        "p = filedialog.askdirectory(title='Select Splinter Cell: Chaos Theory folder'); "
        "print(p or '')"
    )
    try:
        proc = subprocess.run([sys.executable, "-c", script], capture_output=True, text=True, timeout=120)
        return jsonify({"ok": True, "path": proc.stdout.strip()})
    except Exception as exc:
        return jsonify({"ok": False, "error": str(exc)}), 500


@app.route("/api/setup/link", methods=["POST"])
@require_auth
def setup_link(sess):
    data = request.get_json(silent=True) or {}
    prefs = get_or_create_prefs(sess["username_lower"])
    exe_path = (data.get("exe_path") or prefs["exe_path"] or "").strip()
    mod = inspect_and_install_mod(exe_path, install=True)
    if not mod["exe_exists"]:
        return jsonify({"ok": False, "error": mod["error"] or "Executable not found.", "setup": mod}), 400
    if not mod["md5_ok"]:
        return jsonify({"ok": False, "error": f"Executable MD5 ({mod['exe_md5']}) is not a supported v1.05 build.", "setup": mod}), 400
    if not mod["ready"]:
        return jsonify({"ok": False, "error": "Failed to install ScctCoopNet.dll or scct-coop-launch.exe.", "setup": mod}), 500

    with _db_lock:
        conn = get_db()
        try:
            conn.execute(
                "UPDATE user_prefs SET exe_path=?, exe_md5=?, exe_verified=1, updated_at=? WHERE username_lower=?",
                (mod["exe_path"], mod["exe_md5"], utc_now_iso(), sess["username_lower"]),
            )
            conn.commit()
        finally:
            conn.close()
    return jsonify({"ok": True, "setup": mod})


@app.route("/api/setup/settings", methods=["POST"])
@require_auth
def setup_settings(sess):
    data = request.get_json(silent=True) or {}
    friendly_fire = 1 if data.get("friendly_fire") else 0
    voice_chat = 1 if data.get("voice_chat", True) else 0
    local_ip = (data.get("local_ip") or "").strip()
    prefs = get_or_create_prefs(sess["username_lower"])
    mic_device = prefs.get("mic_device", -1) if prefs.get("mic_device") is not None else -1
    if "mic_device" in data:
        try:
            mic_device = int(data["mic_device"])
        except (TypeError, ValueError):
            return jsonify({"ok": False, "error": "Invalid microphone device ID."}), 400
    if local_ip:
        try:
            ipaddress.IPv4Address(local_ip)
        except ipaddress.AddressValueError:
            return jsonify({"ok": False, "error": "Local bind IP must be valid IPv4 or blank."}), 400

    max_fps = None
    if "max_fps" in data:
        try:
            max_fps = max(0, min(240, int(data["max_fps"])))
        except (TypeError, ValueError):
            return jsonify({"ok": False, "error": "Invalid max FPS setting."}), 400

    with _db_lock:
        conn = get_db()
        try:
            if max_fps is not None:
                conn.execute(
                    "UPDATE user_prefs SET friendly_fire=?, voice_chat=?, mic_device=?, max_fps=?, local_ip=?, updated_at=? WHERE username_lower=?",
                    (friendly_fire, voice_chat, mic_device, max_fps, local_ip, utc_now_iso(), sess["username_lower"]),
                )
            else:
                conn.execute(
                    "UPDATE user_prefs SET friendly_fire=?, voice_chat=?, mic_device=?, local_ip=?, updated_at=? WHERE username_lower=?",
                    (friendly_fire, voice_chat, mic_device, local_ip, utc_now_iso(), sess["username_lower"]),
                )
            conn.commit()
        finally:
            conn.close()
    return jsonify({"ok": True})


@app.route("/api/friends", methods=["GET"])
@require_auth
def friends_list(sess):
    st, body = cloud_request("GET", "/v2/friends", bearer_token=sess["cloud_token"])
    return jsonify(body), st


@app.route("/api/friends/add", methods=["POST"])
@require_auth
def friends_add(sess):
    data = request.get_json(silent=True) or {}
    st, body = cloud_request(
        "POST",
        "/v2/friends/add",
        {"username": data.get("username", "")},
        bearer_token=sess["cloud_token"],
    )
    return jsonify(body), st


@app.route("/api/friends/accept", methods=["POST"])
@require_auth
def friends_accept(sess):
    data = request.get_json(silent=True) or {}
    st, body = cloud_request(
        "POST",
        "/v2/friends/accept",
        {"id": data.get("id", 0)},
        bearer_token=sess["cloud_token"],
    )
    return jsonify(body), st


@app.route("/api/friends/remove", methods=["POST"])
@require_auth
def friends_remove(sess):
    data = request.get_json(silent=True) or {}
    st, body = cloud_request(
        "POST",
        "/v2/friends/remove",
        {"id": data.get("id", 0)},
        bearer_token=sess["cloud_token"],
    )
    return jsonify(body), st


@app.route("/api/rooms", methods=["GET"])
@require_auth
def rooms_list(sess):
    st, body = cloud_request("GET", "/v2/rooms", bearer_token=sess["cloud_token"])
    return jsonify(body), st


@app.route("/api/rooms/host", methods=["POST"])
@require_auth
def rooms_host(sess):
    data = request.get_json(silent=True) or {}
    prefs = get_or_create_prefs(sess["username_lower"])
    ffire = bool(data.get("friendly_fire", prefs["friendly_fire"]))
    st, body = cloud_request(
        "POST",
        "/v2/rooms/create",
        {"friendly_fire": ffire},
        bearer_token=sess["cloud_token"],
    )
    if st != 201 or not body.get("ok"):
        return jsonify(body), st

    room = body["room"]
    ok, res = start_bridge(room["code"], sess["cloud_token"])
    if not ok:
        cloud_request("POST", "/v2/rooms/close", {}, bearer_token=sess["cloud_token"])
        return jsonify({"ok": False, "error": f"Cloud WebSocket bridge failed: {res}"}), 502

    if data.get("launch"):
        return _launch_for_session(sess, prefs, res["local_relay"])
    return jsonify({"ok": True, "room": room, "relay": res})


@app.route("/api/rooms/join", methods=["POST"])
@require_auth
def rooms_join(sess):
    data = request.get_json(silent=True) or {}
    code = (data.get("code") or "").strip().upper()
    if not ROOM_CODE_RE.match(code):
        return jsonify({"ok": False, "error": "Enter a valid room code (e.g. SCCT-7K4M9P)."}), 400

    ok, res = start_bridge(code, sess["cloud_token"])
    if not ok:
        return jsonify({"ok": False, "error": f"Could not connect to room {code}: {res}"}), 502

    if data.get("launch"):
        prefs = get_or_create_prefs(sess["username_lower"])
        return _launch_for_session(sess, prefs, res["local_relay"])
    return jsonify({"ok": True, "relay": res})


@app.route("/api/rooms/leave", methods=["POST"])
@require_auth
def rooms_leave(sess):
    stop_bridge()
    cloud_request("POST", "/v2/rooms/close", {}, bearer_token=sess["cloud_token"])
    return jsonify({"ok": True, "relay": get_bridge_status()})


@app.route("/api/relay/status", methods=["GET"])
@require_auth
def relay_status(sess):
    return jsonify({"ok": True, "relay": get_bridge_status()})


def _launch_for_session(sess, prefs, local_relay_addr):
    mod = inspect_and_install_mod(prefs["exe_path"], install=True)
    if mod.get("error"):
        return jsonify({"ok": False, "error": mod["error"]}), 403
    if not mod["exe_exists"]:
        return jsonify({"ok": False, "error": "Link your splintercell3.exe in Settings first."}), 400
    if not mod["md5_ok"]:
        return jsonify({"ok": False, "error": "Executable failed v1.05 MD5 verification."}), 400
    if not mod["ready"]:
        return jsonify({"ok": False, "error": "ScctCoopNet.dll or scct-coop-launch.exe is missing."}), 500

    game_dir = os.path.dirname(mod["exe_path"])
    launch_exe = os.path.join(game_dir, "scct-coop-launch.exe")
    safe_user = re.sub(r"[^A-Za-z0-9_-]", "_", prefs["username"])
    ingame_token = secrets.token_urlsafe(24)
    with _proc_lock:
        _ingame_tokens[ingame_token] = {
            "cloud_token": sess["cloud_token"],
            "username_lower": sess["username_lower"],
        }
    max_fps_val = int(prefs.get("max_fps", 60) if prefs.get("max_fps") is not None else 60)
    mic_dev_val = int(prefs.get("mic_device", -1) if prefs.get("mic_device") is not None else -1)
    voice_str = "1" if prefs["voice_chat"] else "0"
    ffire_str = "1" if prefs.get("friendly_fire") else "0"
    log_name = f"scct-coop-net-{safe_user}.log"
    api_url = f"http://127.0.0.1:{_SERVER_PORT}"

    # Persist launch session for Ubisoft Connect relaunch (where uplay32.dll exits Process #1
    # and upc.exe spawns Process #2 without inherited SCCT_* environment variables).
    session_ini_path = os.path.join(DATA_DIR, "launch_session.ini")
    try:
        session_lines = [
            f"timestamp={int(time.time())}",
            "SCCT_SHOW=1",
            "SCCT_ENABLED=1",
            "SCCT_TRANSPORT=1",
            f"SCCT_RELAY={local_relay_addr}",
            f"SCCT_NAME={prefs['username']}",
            f"SCCT_FFIRE={ffire_str}",
            f"SCCT_VOICE={voice_str}",
            f"SCCT_MIC_DEVICE={mic_dev_val}",
            f"SCCT_MAX_FPS={max_fps_val}",
            f"SCCT_LAUNCHER_API={api_url}",
            f"SCCT_INGAME_TOKEN={ingame_token}",
            f"SCCT_LOG={log_name}",
        ]
        if prefs["local_ip"]:
            session_lines.append(f"SCCT_LOCAL_IP={prefs['local_ip']}")
        with open(session_ini_path, "w", encoding="utf-8") as sf:
            sf.write("\n".join(session_lines) + "\n")
        restrict_windows_acl(session_ini_path, is_dir=False)
    except Exception:
        pass

    args = [
        launch_exe,
        "--visible",
        "--enabled",
        "1",
        "--transport",
        "1",
        "--relay",
        local_relay_addr,
        "--name",
        prefs["username"],
        "--voice",
        voice_str,
        "--mic-device",
        str(mic_dev_val),
        "--max-fps",
        str(max_fps_val),
        "--api-url",
        api_url,
        "--api-token",
        ingame_token,
        "--log",
        log_name,
    ]
    if prefs["local_ip"]:
        args.extend(["--local-ip", prefs["local_ip"]])
    args.append(mod["exe_path"])

    proc = subprocess.Popen(args, cwd=game_dir, creationflags=CREATE_NO_WINDOW)
    with _proc_lock:
        _game_procs[sess["username_lower"]] = proc
    threading.Thread(target=_watch_game, args=(sess["username_lower"], proc), daemon=True).start()
    publish("game", {"running": True})
    return jsonify({"ok": True, "pid": proc.pid, "relay": get_bridge_status()})


@app.route("/api/game/launch", methods=["POST"])
@require_auth
def game_launch(sess):
    prefs = get_or_create_prefs(sess["username_lower"])
    mod = inspect_and_install_mod(prefs["exe_path"], install=False)
    if not mod["ready"]:
        return jsonify({"ok": False, "error": "Link your splintercell3.exe in Settings first."}), 400

    relay = get_bridge_status()
    if not relay["running"] or not relay["ws_connected"]:
        # Automatically create an online cloud room and start WSS bridge so 1-click Play works online.
        st, body = cloud_request(
            "POST",
            "/v2/rooms/create",
            {"friendly_fire": bool(prefs["friendly_fire"])},
            bearer_token=sess["cloud_token"],
        )
        if st != 201 or not body.get("ok"):
            return jsonify(body), st
        ok, res = start_bridge(body["room"]["code"], sess["cloud_token"])
        if not ok:
            cloud_request("POST", "/v2/rooms/close", {}, bearer_token=sess["cloud_token"])
            return jsonify({"ok": False, "error": f"Failed to connect cloud relay: {res}"}), 502
        relay = res

    return _launch_for_session(sess, prefs, relay["local_relay"])


@app.route("/api/game/status", methods=["GET"])
@require_auth
def game_status(sess):
    with _proc_lock:
        proc = _game_procs.get(sess["username_lower"])
        running = proc is not None and proc.poll() is None
        pid = proc.pid if running else None
        if proc is not None and not running:
            _game_procs.pop(sess["username_lower"], None)
    return jsonify({"ok": True, "running": running, "pid": pid})


@app.route("/api/game/stop", methods=["POST"])
@require_auth
def game_stop(sess):
    with _proc_lock:
        proc = _game_procs.pop(sess["username_lower"], None)
    if proc is not None and proc.poll() is None:
        try:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=CREATE_NO_WINDOW)
        except Exception:
            pass
    publish("game", {"running": any_game_running()})
    return jsonify({"ok": True})


@app.route("/api/events", methods=["GET"])
@require_auth
def events_stream(sess):
    q = queue.Queue(maxsize=128)
    with _subs_lock:
        _subs.add(q)

    def gen():
        try:
            yield "retry: 2000\n\n"
            yield _sse("relay", get_bridge_status())
            yield _sse("game", {"running": any_game_running()})
            while True:
                try:
                    yield q.get(timeout=20)
                except queue.Empty:
                    yield ": ping\n\n"
        finally:
            with _subs_lock:
                _subs.discard(q)

    return Response(gen(), mimetype="text/event-stream",
                    headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


def _get_ingame_session():
    if (request.remote_addr or "") not in ("127.0.0.1", "::1"):
        return None
    tok = (request.headers.get("X-Ingame-Token") or "").strip()
    if not tok:
        return None
    with _proc_lock:
        return _ingame_tokens.get(tok)


def list_input_mics():
    mics = [{"id": -1, "name": "System Default"}]
    if os.name != "nt":
        return mics
    try:
        import ctypes
        from ctypes import wintypes

        class WAVEINCAPSW(ctypes.Structure):
            _fields_ = [
                ("wMid", wintypes.WORD),
                ("wPid", wintypes.WORD),
                ("vDriverVersion", wintypes.UINT),
                ("szPname", wintypes.WCHAR * 32),
                ("dwFormats", wintypes.DWORD),
                ("wChannels", wintypes.WORD),
                ("wReserved1", wintypes.WORD),
            ]

        winmm = ctypes.windll.winmm
        count = int(winmm.waveInGetNumDevs())
        for idx in range(count):
            caps = WAVEINCAPSW()
            if winmm.waveInGetDevCapsW(idx, ctypes.byref(caps), ctypes.sizeof(caps)) == 0:
                name = caps.szPname.strip() or f"Microphone #{idx + 1}"
                mics.append({"id": idx, "name": name})
    except Exception:
        pass
    return mics


@app.route("/api/audio/mics", methods=["GET"])
@require_auth
def audio_mics(sess):
    prefs = get_or_create_prefs(sess["username_lower"])
    sel = int(prefs.get("mic_device", -1) if prefs.get("mic_device") is not None else -1)
    return jsonify({"ok": True, "mics": list_input_mics(), "selected": sel})


@app.route("/api/ingame/friends", methods=["GET"])
def ingame_friends_list():
    sess = _get_ingame_session()
    if not sess:
        return Response("unauthorized\n", status=401, mimetype="text/plain")
    prefs = get_or_create_prefs(sess["username_lower"])
    mic_id = int(prefs.get("mic_device", -1) if prefs.get("mic_device") is not None else -1)
    st, body = cloud_request("GET", "/v2/friends", bearer_token=sess["cloud_token"])
    if st != 200 or not body.get("ok"):
        return Response(f"MIC\t{mic_id}\n", status=st, mimetype="text/plain")
    lines = [f"MIC\t{mic_id}"]
    for f in body.get("friends") or []:
        u = str(f.get("username", "")).replace("\t", " ").replace("\n", " ")
        s = str(f.get("status", "offline")).replace("\t", " ").replace("\n", " ")
        r = str(f.get("active_room", "")).replace("\t", " ").replace("\n", " ")
        lines.append(f"FRIEND\t{f.get('id', 0)}\t{u}\t{s}\t{r}")
    for f in body.get("incoming") or []:
        u = str(f.get("username", "")).replace("\t", " ").replace("\n", " ")
        lines.append(f"INCOMING\t{f.get('id', 0)}\t{u}")
    for f in body.get("outgoing") or []:
        u = str(f.get("username", "")).replace("\t", " ").replace("\n", " ")
        lines.append(f"OUTGOING\t{f.get('id', 0)}\t{u}")
    return Response("\n".join(lines) + ("\n" if lines else ""), mimetype="text/plain")


@app.route("/api/ingame/friends/add", methods=["POST"])
def ingame_friends_add():
    sess = _get_ingame_session()
    if not sess:
        return jsonify({"ok": False, "error": "Unauthorized."}), 401
    data = request.get_json(silent=True) or {}
    st, body = cloud_request("POST", "/v2/friends/add", {"username": data.get("username", "")}, bearer_token=sess["cloud_token"])
    if st == 200 and body.get("ok"):
        publish("friends", {"refresh": True})
    return jsonify(body), st


@app.route("/api/ingame/friends/accept", methods=["POST"])
def ingame_friends_accept():
    sess = _get_ingame_session()
    if not sess:
        return jsonify({"ok": False, "error": "Unauthorized."}), 401
    data = request.get_json(silent=True) or {}
    st, body = cloud_request("POST", "/v2/friends/accept", {"id": data.get("id", 0)}, bearer_token=sess["cloud_token"])
    if st == 200 and body.get("ok"):
        publish("friends", {"refresh": True})
    return jsonify(body), st


@app.route("/api/ingame/friends/remove", methods=["POST"])
def ingame_friends_remove():
    sess = _get_ingame_session()
    if not sess:
        return jsonify({"ok": False, "error": "Unauthorized."}), 401
    data = request.get_json(silent=True) or {}
    st, body = cloud_request("POST", "/v2/friends/remove", {"id": data.get("id", 0)}, bearer_token=sess["cloud_token"])
    if st == 200 and body.get("ok"):
        publish("friends", {"refresh": True})
    return jsonify(body), st


@app.route("/api/ingame/mic", methods=["POST"])
def ingame_mic_set():
    sess = _get_ingame_session()
    if not sess:
        return jsonify({"ok": False, "error": "Unauthorized."}), 401
    data = request.get_json(silent=True) or {}
    try:
        mic_id = int(data.get("mic_device", -1))
    except (TypeError, ValueError):
        return jsonify({"ok": False, "error": "Invalid mic_device."}), 400
    with _db_lock:
        conn = get_db()
        try:
            conn.execute(
                "UPDATE user_prefs SET mic_device=?, updated_at=? WHERE username_lower=?",
                (mic_id, utc_now_iso(), sess["username_lower"]),
            )
            conn.commit()
        finally:
            conn.close()
    publish("mic", {"mic_device": mic_id})
    return jsonify({"ok": True, "mic_device": mic_id})


def _load_content_catalog():
    packs = None
    if CONTENT_MANIFEST_URL:
        try:
            u = urllib.parse.urlparse(CONTENT_MANIFEST_URL)
            if u.scheme == "https" and (u.hostname or "").lower() in ALLOWED_DOWNLOAD_HOSTS:
                req = urllib.request.Request(CONTENT_MANIFEST_URL, headers={"User-Agent": f"SCCT-Coop-Launcher/{APP_VERSION}"})
                with urllib.request.urlopen(req, timeout=4) as resp:
                    parsed = json.loads(resp.read().decode("utf-8", errors="replace"))
                    if isinstance(parsed, dict) and isinstance(parsed.get("packs"), list):
                        packs = parsed["packs"]
        except Exception:
            pass
    if packs is None:
        packs = [dict(p) for p in DEFAULT_CONTENT_CATALOG]
    else:
        packs = [dict(p) for p in packs]

    fps_entry = {
        "id": "fps-cap-60",
        "name": "60 FPS Cap",
        "type": "builtin",
        "experimental": True,
        "description": "Locks the game loop at 60 FPS via ScctCoopNet.dll. Both co-op players should use the same setting.",
        "credits": "EchelonRe builtin",
    }
    if not any(p.get("id") == "fps-cap-60" for p in packs):
        packs.append(fps_entry)
    return packs


def _receipt_path(pack_id):
    safe_id = re.sub(r"[^A-Za-z0-9_-]", "_", pack_id)
    return os.path.join(DATA_DIR, f"installed_{safe_id}.json")


def _pack_installed(pack, game_root):
    if pack.get("id") == "fps-cap-60":
        return True
    if not game_root or not os.path.isdir(game_root):
        return os.path.isfile(_receipt_path(pack["id"]))
    rec_path = _receipt_path(pack["id"])
    if os.path.isfile(rec_path):
        return True
    check_files = pack.get("check_files") or []
    if check_files:
        if pack["id"] == "widescreen-fix":
            asi = os.path.join(game_root, "System", "scripts", "SplinterCellChaosTheory.WidescreenFix.asi")
            asi_dis = asi + ".disabled"
            return os.path.isfile(asi) or os.path.isfile(asi_dis)
        return all(os.path.isfile(os.path.join(game_root, rel)) for rel in check_files)
    return os.path.isfile(_receipt_path(pack["id"]))


def _pack_enabled(pack, game_root, prefs=None):
    if pack.get("id") == "fps-cap-60":
        if prefs:
            return int(prefs.get("max_fps", 60) if prefs.get("max_fps") is not None else 60) != 0
        return True
    rec_path = _receipt_path(pack["id"])
    if os.path.isfile(rec_path):
        try:
            with open(rec_path, "r", encoding="utf-8") as rf:
                rec = json.load(rf)
            if "enabled" in rec:
                return bool(rec["enabled"])
        except Exception:
            pass
    if pack.get("id") == "widescreen-fix" and game_root:
        asi = os.path.join(game_root, "System", "scripts", "SplinterCellChaosTheory.WidescreenFix.asi")
        asi_dis = asi + ".disabled"
        if os.path.isfile(asi_dis) and not os.path.isfile(asi):
            return False
        return True
    if pack.get("id") == "scda-coop-maps" and game_root:
        rot_path = os.path.join(game_root, "System", "MapRotation.ini")
        if os.path.isfile(rot_path):
            try:
                with open(rot_path, "r", encoding="latin1") as f:
                    content = f.read()
                if "MissionCoop[7]" not in content:
                    return False
            except Exception:
                pass
        return True
    return True


def _parse_dare_dir(data, dir_off, count):
    ents = []
    for i in range(count):
        o = dir_off + i * 0x34
        flag, zero, off, size = struct.unpack_from("<IIII", data, o)
        name = data[o + 0x10 : o + 0x30].split(b"\x00", 1)[0].decode("latin1")
        tail = struct.unpack_from("<I", data, o + 0x30)[0]
        ents.append({"name": name, "off": off, "size": size, "flag": flag, "zero": zero, "tail": tail})
    return ents


def _relocate_bank_blob(blob, stream_base):
    if not stream_base:
        return blob
    out = bytearray(blob)
    _, _, _, sec2_off, n2, _, _, _, _ = struct.unpack_from("<9I", out, 0)
    for i in range(n2):
        rec_off = sec2_off + i * 96
        w1, w2 = struct.unpack_from("<II", out, rec_off + 4)
        if w1 == 1 and w2 > 0:
            w4 = struct.unpack_from("<I", out, rec_off + 16)[0]
            struct.pack_into("<I", out, rec_off + 16, w4 + stream_base)
    return bytes(out)


def _append_banks_batch(src_path, dst_path, bank_entries):
    src_size = os.path.getsize(src_path)
    with open(src_path, "rb") as f:
        prefix = f.read(0x0C)
    ver_a, ver_b, dir_off, count = struct.unpack_from("<HHII", prefix, 0)
    if dir_off != 0x0C:
        raise ValueError(f"{src_path} directory offset {dir_off:#x}, expected 0x0c")
    with open(src_path, "rb") as f:
        f.seek(dir_off)
        directory = f.read(count * 0x34)
    head = prefix + directory
    old_dir_end = dir_off + count * 0x34
    ents = _parse_dare_dir(head, dir_off, count)
    existing = {e["name"].lower() for e in ents}
    to_add = [(nm, bl) for nm, bl in bank_entries if nm.lower() not in existing]
    shift = 0x34 * len(to_add)
    out_dir = bytearray()
    for ent in ents:
        out_dir += struct.pack("<IIII", ent["flag"], ent["zero"], ent["off"] + shift, ent["size"])
        out_dir += ent["name"].encode("latin1")[:32].ljust(32, b"\x00")
        out_dir += struct.pack("<I", ent["tail"])
    cursor = src_size + shift
    for nm, bl in to_add:
        out_dir += struct.pack("<IIII", 0, 0, cursor, len(bl))
        out_dir += nm.encode("latin1")[:32].ljust(32, b"\x00")
        out_dir += struct.pack("<I", 0x04D71B00)
        cursor += len(bl)
    header = struct.pack("<HHII", ver_a, ver_b, 0x0C, count + len(to_add))
    os.makedirs(os.path.dirname(dst_path), exist_ok=True)
    with open(dst_path, "wb") as out, open(src_path, "rb") as src:
        out.write(header)
        out.write(out_dir)
        src.seek(old_dir_end)
        shutil.copyfileobj(src, out, length=1024 * 1024)
        for _, bl in to_add:
            out.write(bl)


def _append_stream_file(retail_stream, out_stream, pcm_path):
    base = os.path.getsize(retail_stream)
    if os.path.abspath(retail_stream) == os.path.abspath(out_stream):
        raise ValueError("Refusing to overwrite retail stream file.")
    with open(retail_stream, "rb") as src, open(out_stream, "wb") as dst:
        shutil.copyfileobj(src, dst, length=1024 * 1024)
        with open(pcm_path, "rb") as pf:
            shutil.copyfileobj(pf, dst, length=1024 * 1024)
    return base


def _apply_scda_audio_pack(game_real, installed_files, set_job):
    pack_dir = os.path.join(game_real, "Data", "Sounds", "scda_audio_pack")
    manifest_path = os.path.join(pack_dir, "manifest.json")
    if not os.path.isfile(manifest_path):
        return
    set_job("patching", 90, "Patching DareAudio sound & dialogue banks...")
    with open(manifest_path, "r", encoding="utf-8") as mf:
        manifest = json.load(mf)
    sounds_dir = os.path.join(game_real, "Data", "Sounds")
    sm0 = os.path.join(sounds_dir, "MAPS.SM0")
    ss0 = os.path.join(sounds_dir, "STREAM.SS0")
    sfx_pcm = os.path.join(pack_dir, manifest.get("sfx_pcm", "sfx.pcm"))
    if os.path.isfile(sm0) and os.path.isfile(ss0) and os.path.isfile(sfx_pcm):
        out_ss = os.path.join(sounds_dir, "SCDA_STREAM.SS0")
        out_sm = os.path.join(sounds_dir, "SCDA_MAPS.SM0")
        sfx_base = _append_stream_file(ss0, out_ss, sfx_pcm)
        sfx_entries = []
        for m in manifest.get("maps") or []:
            bp = os.path.join(pack_dir, m["sfx_bank"])
            if os.path.isfile(bp):
                with open(bp, "rb") as bf:
                    sfx_entries.append((m["out_name"], _relocate_bank_blob(bf.read(), sfx_base)))
        _append_banks_batch(sm0, out_sm, sfx_entries)
        installed_files.extend([os.path.relpath(out_ss, game_real), os.path.relpath(out_sm, game_real)])

    set_job("patching", 94, "Patching DareAudio dialogue banks...")
    dlg_pcm = os.path.join(pack_dir, manifest.get("dlg_pcm", "dlg.pcm"))
    if os.path.isfile(dlg_pcm):
        for lang in ("int", "org", "fra", "deu", "esp", "ita"):
            ldir = os.path.join(sounds_dir, lang)
            lm0 = os.path.join(ldir, "MAPS.LM0")
            ls0 = os.path.join(ldir, "STREAM.LS0")
            if not (os.path.isfile(lm0) and os.path.isfile(ls0)):
                continue
            out_ls = os.path.join(ldir, "SCDA_STREAM.LS0")
            out_lm = os.path.join(ldir, "SCDA_MAPS.LM0")
            dlg_base = _append_stream_file(ls0, out_ls, dlg_pcm)
            dlg_entries = []
            for m in manifest.get("maps") or []:
                bp = os.path.join(pack_dir, m["dlg_bank"])
                if os.path.isfile(bp):
                    with open(bp, "rb") as bf:
                        dlg_entries.append((m["out_name"], _relocate_bank_blob(bf.read(), dlg_base)))
            _append_banks_batch(lm0, out_lm, dlg_entries)
            installed_files.extend([os.path.relpath(out_ls, game_real), os.path.relpath(out_lm, game_real)])

    shutil.rmtree(pack_dir, ignore_errors=True)
    prefix = os.path.join("Data", "Sounds", "scda_audio_pack").lower() + os.sep
    installed_files[:] = [p for p in installed_files if not p.lower().startswith(prefix)]


def _merge_map_rotation(game_real, scda_maps):
    rot_path = os.path.join(game_real, "System", "MapRotation.ini")
    if os.path.isfile(rot_path):
        with open(rot_path, "r", encoding="latin1", errors="replace") as f:
            raw = f.read().splitlines()
    else:
        raw = ["[MAPS]"] + [f'MissionCoop[{i}]="{m}"' for i, m in enumerate(STOCK_COOP_ROTATION)]

    out_lines = []
    in_maps = False
    wrote_scda = False
    for line in raw:
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            if in_maps and not wrote_scda:
                for idx, m in enumerate(scda_maps, start=len(STOCK_COOP_ROTATION)):
                    out_lines.append(f'MissionCoop[{idx}]="{m}"')
                wrote_scda = True
            in_maps = s.lower() == "[maps]"
            out_lines.append(line)
            continue
        if in_maps:
            m_kv = re.match(r"^MissionCoop\[(\d+)\]\s*=\s*(.+)$", s, re.IGNORECASE)
            if m_kv and int(m_kv.group(1)) >= len(STOCK_COOP_ROTATION):
                continue
        out_lines.append(line)

    if not in_maps and not wrote_scda:
        out_lines.append("[MAPS]")
        for idx, m in enumerate(STOCK_COOP_ROTATION):
            out_lines.append(f'MissionCoop[{idx}]="{m}"')
    if not wrote_scda:
        for idx, m in enumerate(scda_maps, start=len(STOCK_COOP_ROTATION)):
            out_lines.append(f'MissionCoop[{idx}]="{m}"')

    os.makedirs(os.path.dirname(rot_path), exist_ok=True)
    with open(rot_path, "w", encoding="latin1", newline="\r\n") as rf:
        rf.write("\n".join(out_lines) + "\n")


def _strip_scda_from_map_rotation(game_real):
    rot_path = os.path.join(game_real, "System", "MapRotation.ini")
    if not os.path.isfile(rot_path):
        return
    with open(rot_path, "r", encoding="latin1", errors="replace") as f:
        raw = f.read().splitlines()
    out_lines = []
    in_maps = False
    for line in raw:
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            in_maps = s.lower() == "[maps]"
            out_lines.append(line)
            continue
        if in_maps:
            m_kv = re.match(r"^MissionCoop\[(\d+)\]\s*=\s*(.+)$", s, re.IGNORECASE)
            if m_kv and int(m_kv.group(1)) >= len(STOCK_COOP_ROTATION):
                continue
        out_lines.append(line)
    with open(rot_path, "w", encoding="latin1", newline="\r\n") as rf:
        rf.write("\n".join(out_lines) + "\n")


class _SafeRedirectHandler(urllib.request.HTTPRedirectHandler):
    """Strip Authorization on cross-host redirects and enforce ALLOWED_DOWNLOAD_HOSTS."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        target = urllib.parse.urljoin(req.full_url, newurl)
        pu = urllib.parse.urlparse(target)
        host = (pu.hostname or "").lower()
        if pu.scheme != "https" or host not in ALLOWED_DOWNLOAD_HOSTS:
            raise urllib.error.HTTPError(target, 403, f"Redirect host '{host}' not in allowlist", headers, fp)
        new_req = super().redirect_request(req, fp, code, msg, headers, newurl)
        if new_req is not None:
            orig_host = (urllib.parse.urlparse(req.full_url).hostname or "").lower()
            if host != orig_host:
                new_req.remove_header("Authorization")
        return new_req


def _find_local_pack_archive(pack_id, expected_sha):
    """Look for an existing local archive file matching the expected sha256 to avoid redundant downloads."""
    cands = [
        os.path.join(DATA_DIR, f"{pack_id}.zip"),
        os.path.join(APP_DIR, f"{pack_id}.zip"),
        os.path.join(LAUNCHER_DIR, f"{pack_id}.zip"),
        os.path.join(COOP_NET_DIR, f"{pack_id}.zip"),
    ]
    if pack_id == "scda-coop-maps":
        cands.extend([
            os.path.join(COOP_NET_DIR, "..", "SCDA_Coop_Maps", "SCDA-Coop-Maps-Pack.zip"),
            os.path.join(APP_DIR, "..", "SCDA_Coop_Maps", "SCDA-Coop-Maps-Pack.zip"),
            os.path.join(LAUNCHER_DIR, "..", "SCDA_Coop_Maps", "SCDA-Coop-Maps-Pack.zip"),
        ])
    for cand in cands:
        cand_real = os.path.realpath(cand)
        if os.path.isfile(cand_real):
            try:
                if not expected_sha or file_sha256(cand_real).lower() == expected_sha:
                    return cand_real
            except Exception:
                pass
    return None


def _install_pack_worker(pack, game_root, cloud_token=None):
    pack_id = pack["id"]
    url = str(pack.get("download_url") or "").strip()
    expected_sha = str(pack.get("sha256") or "").strip().lower()
    tmp_zip = os.path.join(DATA_DIR, f"_dl_{pack_id}.zip")

    def set_job(status, progress, message, error=""):
        st = {"id": pack_id, "status": status, "progress": progress, "message": message, "error": error}
        with _proc_lock:
            _content_jobs[pack_id] = st
        publish("content", st)

    archive_to_extract = None
    try:
        local_archive = _find_local_pack_archive(pack_id, expected_sha)
        if local_archive:
            set_job("extracting", 85, f"Using local {os.path.basename(local_archive)}...")
            archive_to_extract = local_archive
        else:
            u = urllib.parse.urlparse(url)
            if u.scheme != "https" or (u.hostname or "").lower() not in ALLOWED_DOWNLOAD_HOSTS:
                raise ValueError("Download URL host is not in the allowlist.")

            set_job("downloading", 0, f"Downloading {pack['name']}...")
            headers = {"User-Agent": f"SCCT-Coop-Launcher/{APP_VERSION}"}
            if cloud_token and url.startswith(CLOUD_API_URL + "/"):
                headers["Authorization"] = f"Bearer {cloud_token}"
            req = urllib.request.Request(url, headers=headers)
            opener = urllib.request.build_opener(_SafeRedirectHandler)
            h = hashlib.sha256()
            downloaded = 0
            last_reported_pct = -1
            last_reported_time = time.time()
            with opener.open(req, timeout=60) as resp, open(tmp_zip, "wb") as out:
                total = int(resp.headers.get("Content-Length") or 0)
                while True:
                    chunk = resp.read(65536)
                    if not chunk:
                        break
                    out.write(chunk)
                    h.update(chunk)
                    downloaded += len(chunk)
                    now = time.time()
                    if total > 0:
                        pct = min(85, int(downloaded * 85 / total))
                        if pct > last_reported_pct or (now - last_reported_time) >= 0.5:
                            last_reported_pct = pct
                            last_reported_time = now
                            mb_cur = downloaded // (1024 * 1024)
                            mb_tot = total // (1024 * 1024)
                            set_job("downloading", pct, f"Downloading {mb_cur} / {mb_tot} MB ({pct}%)")
                    else:
                        if (now - last_reported_time) >= 0.5:
                            last_reported_time = now
                            mb_cur = downloaded // (1024 * 1024)
                            set_job("downloading", 40, f"Downloading {mb_cur} MB...")

            if expected_sha and h.hexdigest().lower() != expected_sha:
                raise ValueError("Downloaded archive failed SHA-256 verification.")
            archive_to_extract = tmp_zip

        set_job("extracting", 86, "Extracting files...")
        game_real = os.path.realpath(game_root)
        installed_files = []
        with zipfile.ZipFile(archive_to_extract, "r") as zf:
            members = [m for m in zf.infolist() if not m.is_dir()]
            total_members = len(members)
            last_ext_time = time.time()
            for idx, member in enumerate(members):
                rel = member.filename.replace("\\", "/").lstrip("/")
                if not rel or ".." in rel.split("/"):
                    continue
                if os.path.basename(rel).lower() in PROTECTED_RETAIL_BASENAMES:
                    continue
                dst = os.path.realpath(os.path.join(game_real, rel))
                if not dst.lower().startswith((game_real + os.sep).lower()):
                    continue
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with zf.open(member, "r") as src, open(dst, "wb") as dst_f:
                    shutil.copyfileobj(src, dst_f)
                installed_files.append(os.path.relpath(dst, game_real))
                now = time.time()
                if (now - last_ext_time) >= 0.5 and total_members > 0:
                    last_ext_time = now
                    ext_pct = 86 + int(idx * 7 / total_members)
                    set_job("extracting", ext_pct, f"Extracting {idx + 1}/{total_members} files...")

        with open(_receipt_path(pack_id), "w", encoding="utf-8") as rf:
            json.dump({"id": pack_id, "version": pack.get("version", ""), "files": installed_files, "enabled": True}, rf, indent=2)

        _apply_scda_audio_pack(game_real, installed_files, set_job)
        pack_mf_path = os.path.join(game_real, "Data", "Maps", "scda_pack_manifest.json")
        if os.path.isfile(pack_mf_path):
            set_job("patching", 98, "Updating MapRotation.ini...")
            with open(pack_mf_path, "r", encoding="utf-8") as pmf:
                scda_maps = json.load(pmf).get("maps") or SCDA_ORDERED_MAPS
            _merge_map_rotation(game_real, scda_maps)

        with open(_receipt_path(pack_id), "w", encoding="utf-8") as rf:
            json.dump({"id": pack_id, "version": pack.get("version", ""), "files": installed_files, "enabled": True}, rf, indent=2)
        set_job("done", 100, f"Installed {pack['name']}.")
    except Exception as exc:
        set_job("error", 0, "Install failed.", str(exc))
    finally:
        try:
            if archive_to_extract == tmp_zip and os.path.isfile(tmp_zip):
                os.remove(tmp_zip)
        except Exception:
            pass


@app.route("/api/content/catalog", methods=["GET"])
@require_auth
def content_catalog(sess):
    prefs = get_or_create_prefs(sess["username_lower"])
    mod = inspect_and_install_mod(prefs["exe_path"], install=False)
    game_root = mod["game_dir"]
    packs = []
    with _proc_lock:
        jobs = dict(_content_jobs)
    for p in _load_content_catalog():
        item = dict(p)
        if item.get("id") == "fps-cap-60":
            item["installed"] = True
            max_fps = int(prefs.get("max_fps", 60) if prefs.get("max_fps") is not None else 60)
            item["enabled"] = (max_fps != 0)
            item["value"] = max_fps
            item["job"] = None
        else:
            item["installed"] = _pack_installed(p, game_root)
            item["enabled"] = _pack_enabled(p, game_root, prefs) if item["installed"] else False
            item["job"] = jobs.get(p["id"])
        packs.append(item)
    return jsonify({"ok": True, "game_linked": bool(game_root), "packs": packs})


@app.route("/api/content/install", methods=["POST"])
@require_auth
def content_install(sess):
    data = request.get_json(silent=True) or {}
    pack_id = str(data.get("id") or "").strip()
    if pack_id == "fps-cap-60":
        return jsonify({"ok": False, "error": "60 FPS Cap is a built-in launch setting, not an installable package."}), 400
    prefs = get_or_create_prefs(sess["username_lower"])
    mod = inspect_and_install_mod(prefs["exe_path"], install=False)
    if not mod["exe_exists"] or not mod["game_dir"]:
        return jsonify({"ok": False, "error": "Link your Chaos Theory game folder in Settings first."}), 400
    pack = next((p for p in _load_content_catalog() if p.get("id") == pack_id), None)
    if not pack:
        return jsonify({"ok": False, "error": "Unknown content pack."}), 404
    with _proc_lock:
        cur = _content_jobs.get(pack_id)
        if cur and cur.get("status") in ("downloading", "extracting", "patching"):
            return jsonify({"ok": True, "job": cur})
        _content_jobs[pack_id] = {"id": pack_id, "status": "downloading", "progress": 0, "message": "Starting...", "error": ""}
    threading.Thread(target=_install_pack_worker, args=(pack, mod["game_dir"], sess.get("cloud_token")), daemon=True).start()
    return jsonify({"ok": True})


@app.route("/api/content/uninstall", methods=["POST"])
@require_auth
def content_uninstall(sess):
    data = request.get_json(silent=True) or {}
    pack_id = str(data.get("id") or "").strip()
    if pack_id == "fps-cap-60":
        return jsonify({"ok": False, "error": "60 FPS Cap cannot be uninstalled."}), 400
    prefs = get_or_create_prefs(sess["username_lower"])
    mod = inspect_and_install_mod(prefs["exe_path"], install=False)
    game_root = mod["game_dir"]
    rec_path = _receipt_path(pack_id)
    if not os.path.isfile(rec_path):
        return jsonify({"ok": False, "error": "No install receipt found for this pack."}), 404
    try:
        rec = json.load(open(rec_path, "r", encoding="utf-8"))
        if game_root and os.path.isdir(game_root):
            game_real = os.path.realpath(game_root)
            if pack_id == "scda-coop-maps":
                _strip_scda_from_map_rotation(game_real)
            for rel in rec.get("files") or []:
                dst = os.path.realpath(os.path.join(game_real, rel))
                if dst.startswith(game_real + os.sep) and os.path.basename(dst).lower() not in PROTECTED_RETAIL_BASENAMES:
                    if os.path.isfile(dst):
                        os.remove(dst)
        os.remove(rec_path)
        publish("content", {"id": pack_id, "status": "uninstalled", "progress": 0, "message": "Removed.", "error": ""})
        return jsonify({"ok": True})
    except Exception as exc:
        return jsonify({"ok": False, "error": str(exc)}), 500


@app.route("/api/content/toggle", methods=["POST"])
@require_auth
def content_toggle(sess):
    data = request.get_json(silent=True) or {}
    pack_id = str(data.get("id") or "").strip()
    enabled = bool(data.get("enabled", True))

    with _proc_lock:
        cur_job = _content_jobs.get(pack_id)
        if cur_job and cur_job.get("status") in ("downloading", "extracting", "patching"):
            return jsonify({"ok": False, "error": "Cannot change mod while an install job is running."}), 409

    if pack_id == "fps-cap-60":
        val = 60 if enabled else 0
        with _db_lock:
            conn = get_db()
            try:
                conn.execute(
                    "UPDATE user_prefs SET max_fps=?, updated_at=? WHERE username_lower=?",
                    (val, utc_now_iso(), sess["username_lower"]),
                )
                conn.commit()
            finally:
                conn.close()
        publish("content", {"id": pack_id, "enabled": enabled, "value": val, "message": "Applies at next game launch."})
        return jsonify({"ok": True, "id": pack_id, "enabled": enabled, "value": val, "note": "Applies at next game launch."})

    if any_game_running():
        return jsonify({"ok": False, "error": "Stop the game before changing mods."}), 409

    prefs = get_or_create_prefs(sess["username_lower"])
    mod = inspect_and_install_mod(prefs["exe_path"], install=False)
    game_root = mod["game_dir"]
    if not game_root or not os.path.isdir(game_root):
        return jsonify({"ok": False, "error": "Link your Chaos Theory game folder in Settings first."}), 400

    game_real = os.path.realpath(game_root)
    pack = next((p for p in _load_content_catalog() if p.get("id") == pack_id), None)
    if not pack:
        return jsonify({"ok": False, "error": "Unknown content pack."}), 404

    if not _pack_installed(pack, game_real):
        return jsonify({"ok": False, "error": "Mod is not installed."}), 409

    rec_path = _receipt_path(pack_id)
    rec = {}
    if os.path.isfile(rec_path):
        try:
            with open(rec_path, "r", encoding="utf-8") as rf:
                rec = json.load(rf)
        except Exception:
            rec = {}
    if not rec:
        rec = {"id": pack_id, "version": pack.get("version", ""), "files": []}

    if pack_id == "widescreen-fix":
        scripts_dir = os.path.join(game_real, "System", "scripts")
        asi = os.path.join(scripts_dir, "SplinterCellChaosTheory.WidescreenFix.asi")
        asi_dis = asi + ".disabled"
        if enabled:
            if os.path.isfile(asi) and os.path.isfile(asi_dis):
                app.logger.warning("Both .asi and .asi.disabled exist; keeping .asi")
                try:
                    os.remove(asi_dis)
                except Exception:
                    pass
            elif os.path.isfile(asi_dis) and not os.path.isfile(asi):
                os.rename(asi_dis, asi)
            elif not os.path.isfile(asi) and not os.path.isfile(asi_dis):
                return jsonify({"ok": False, "error": "Widescreen Fix files not found."}), 409
        else:
            if os.path.isfile(asi):
                if os.path.isfile(asi_dis):
                    try:
                        os.remove(asi_dis)
                    except Exception:
                        pass
                os.rename(asi, asi_dis)
            elif not os.path.isfile(asi_dis):
                return jsonify({"ok": False, "error": "Widescreen Fix files not found."}), 409

    elif pack_id == "scda-coop-maps":
        if enabled:
            pack_mf_path = os.path.join(game_real, "Data", "Maps", "scda_pack_manifest.json")
            scda_maps = SCDA_ORDERED_MAPS
            if os.path.isfile(pack_mf_path):
                try:
                    with open(pack_mf_path, "r", encoding="utf-8") as pmf:
                        scda_maps = json.load(pmf).get("maps") or SCDA_ORDERED_MAPS
                except Exception:
                    pass
            _merge_map_rotation(game_real, scda_maps)
        else:
            _strip_scda_from_map_rotation(game_real)

    rec["enabled"] = enabled
    try:
        with open(rec_path, "w", encoding="utf-8") as rf:
            json.dump(rec, rf, indent=2)
    except Exception as exc:
        app.logger.error("Failed to write receipt: %s", exc)

    publish("content", {"id": pack_id, "enabled": enabled, "status": "installed" if enabled else "disabled"})
    return jsonify({"ok": True, "id": pack_id, "enabled": enabled})


@app.route("/api/update/check", methods=["GET"])
@require_auth
def update_check(sess):
    res = {
        "ok": True,
        "current_version": APP_VERSION,
        "installed": False,
        "update_capable": False,
        "update_available": False,
        "target_version": "",
        "error": "",
    }
    try:
        import velopack
        mgr = velopack.UpdateManager(UPDATE_FEED_URL)
        installed = bool(mgr.is_installed())
        res["installed"] = installed
        res["update_capable"] = installed
        if not installed:
            return jsonify(res)
        info = mgr.check_for_updates()
        if info is not None and getattr(info, "TargetFullRelease", None) is not None:
            res["update_available"] = True
            res["target_version"] = str(getattr(info.TargetFullRelease, "Version", ""))
    except Exception as exc:
        msg = str(exc)
        if "notinstalled" in msg.lower().replace(" ", ""):
            res["installed"] = False
            res["update_capable"] = False
        else:
            res["error"] = msg
    return jsonify(res)


@app.route("/api/update/apply", methods=["POST"])
@require_auth
def update_apply(sess):
    try:
        import velopack
        mgr = velopack.UpdateManager(UPDATE_FEED_URL)
        info = mgr.check_for_updates()
        if info is None:
            return jsonify({"ok": False, "error": "No update available."}), 404
        mgr.download_updates(info)
        mgr.apply_updates_and_restart(info)
        return jsonify({"ok": True})
    except Exception as exc:
        return jsonify({"ok": False, "error": str(exc)}), 500


def run_server(port, host="127.0.0.1"):
    """Blocking. Waitress (pure-Python, Windows-safe) with a Werkzeug fallback."""
    global _SERVER_PORT
    _SERVER_PORT = port
    try:
        from waitress import serve
        serve(app, host=host, port=port, threads=16, send_bytes=1, ident="scct")
    except ImportError:
        app.run(host=host, port=port, debug=False, use_reloader=False, threaded=True)


if __name__ == "__main__":
    host = os.environ.get("SCCT_LAUNCHER_HOST", "127.0.0.1")
    port = int(os.environ.get("SCCT_LAUNCHER_PORT", "27180"))
    run_server(port, host)
