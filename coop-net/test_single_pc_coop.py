import ctypes
from ctypes import wintypes
import glob
import os
import re
import subprocess
import time
from PIL import Image, ImageGrab

REPO_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME_DIR = r"D:\Jeux\Tom Clancys Splinter Cell Chaos Theory\System"
GAME_EXE = os.path.join(GAME_DIR, "splintercell3.exe")
LAUNCH_EXE = os.path.join(GAME_DIR, "scct-coop-launch.exe")
RELAY_EXE = os.path.join(REPO_DIR, "coop-net", "build", "Release", "scct-lan-relay.exe")
RELAY_LOG = os.path.join(REPO_DIR, "coop-net", "build", "Release", "live-relay.log")
HOST_LOG_NAME = "scct-coop-net-host.log"
GUEST_LOG_NAME = "scct-coop-net-guest.log"
HOST_LOG = os.path.join(GAME_DIR, HOST_LOG_NAME)
GUEST_LOG = os.path.join(GAME_DIR, GUEST_LOG_NAME)
ARTIFACT_DIR = r"C:\Users\Ayoub\.gemini\antigravity\brain\a69a4742-dc3f-4f88-b89f-332ef57f63c9"

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32
gdi32 = ctypes.windll.gdi32
try:
    user32.SetProcessDPIAware()
except Exception:
    pass

WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

SW_RESTORE = 9
VK_RETURN = 0x0D
VK_DOWN = 0x28
KEYEVENTF_EXTENDEDKEY = 0x0001
KEYEVENTF_SCANCODE = 0x0008
KEYEVENTF_KEYUP = 0x0002
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
WM_COMMAND = 0x0111
IDOK = 1
IDCANCEL = 2


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx", wintypes.LONG),
        ("dy", wintypes.LONG),
        ("mouseData", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.c_size_t),
    ]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", wintypes.WORD),
        ("wScan", wintypes.WORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.c_size_t),
    ]


class INPUT_UNION(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT), ("pad", ctypes.c_ubyte * 28)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", INPUT_UNION)]


def send_key(vk, scan, extended=False, hold_s=0.08):
    flags = KEYEVENTF_SCANCODE | (KEYEVENTF_EXTENDEDKEY if extended else 0)
    down = INPUT(type=1)
    down.u.ki = KEYBDINPUT(vk, scan, flags, 0, 0)
    up = INPUT(type=1)
    up.u.ki = KEYBDINPUT(vk, scan, flags | KEYEVENTF_KEYUP, 0, 0)
    user32.SendInput(1, ctypes.byref(down), ctypes.sizeof(INPUT))
    time.sleep(hold_s)
    user32.SendInput(1, ctypes.byref(up), ctypes.sizeof(INPUT))


def enum_windows_for_pid(target_pid):
    results = []

    def cb(hwnd, lparam):
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value == target_pid:
            cls_buf = ctypes.create_unicode_buffer(256)
            txt_buf = ctypes.create_unicode_buffer(512)
            user32.GetClassNameW(hwnd, cls_buf, 256)
            user32.GetWindowTextW(hwnd, txt_buf, 512)
            rect = wintypes.RECT()
            user32.GetWindowRect(hwnd, ctypes.byref(rect))
            vis = bool(user32.IsWindowVisible(hwnd))
            results.append(
                {
                    "hwnd": hwnd,
                    "pid": pid.value,
                    "visible": vis,
                    "cls": cls_buf.value,
                    "title": txt_buf.value,
                    "rect": (rect.left, rect.top, rect.right, rect.bottom),
                }
            )
        return True

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return results


def dismiss_dialogs(pid, tag):
    wins = enum_windows_for_pid(pid)
    for w in wins:
        if w["cls"] == "#32770" and w["visible"]:
            children_txt = []

            def child_cb(chwnd, lp):
                buf = ctypes.create_unicode_buffer(512)
                user32.GetWindowTextW(chwnd, buf, 512)
                if buf.value:
                    children_txt.append(buf.value)
                return True

            user32.EnumChildWindows(w["hwnd"], WNDENUMPROC(child_cb), 0)
            print(f"[{tag}] Detected dialog hwnd=0x{w['hwnd']:X} title='{w['title']}' children={children_txt}")
            user32.PostMessageW(w["hwnd"], WM_COMMAND, IDCANCEL, 0)
            time.sleep(0.2)
            user32.PostMessageW(w["hwnd"], WM_COMMAND, IDOK, 0)


def wait_for_pid_from_log(log_path, timeout=15.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(log_path):
            try:
                with open(log_path, "r", encoding="utf-8", errors="ignore") as f:
                    text = f.read()
                m = re.search(r"pid=(\d+)", text)
                if m:
                    return int(m.group(1))
            except Exception:
                pass
        time.sleep(0.2)
    return None


def wait_for_viewport(pid, tag, timeout=25.0):
    deadline = time.time() + timeout
    vp = None
    running_seen = False
    while time.time() < deadline:
        dismiss_dialogs(pid, tag)
        wins = enum_windows_for_pid(pid)
        for w in wins:
            if w["cls"] == "SplinterCell3UnrealWLog" and "(Running)" in w["title"]:
                running_seen = True
            if w["cls"] == "SplinterCell3UnrealWWindowsViewportWindow" and w["visible"]:
                l, t, r, b = w["rect"]
                if (r - l) > 100 and (b - t) > 100:
                    vp = w
        if vp is not None and running_seen:
            return vp
        time.sleep(0.3)
    return vp


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD),
        ("biWidth", wintypes.LONG),
        ("biHeight", wintypes.LONG),
        ("biPlanes", wintypes.WORD),
        ("biBitCount", wintypes.WORD),
        ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD),
        ("biXPelsPerMeter", wintypes.LONG),
        ("biYPelsPerMeter", wintypes.LONG),
        ("biClrUsed", wintypes.DWORD),
        ("biClrImportant", wintypes.DWORD),
    ]


def capture_printwindow(hwnd, width, height):
    hdc_win = user32.GetWindowDC(hwnd)
    hdc_mem = gdi32.CreateCompatibleDC(hdc_win)
    hbmp = gdi32.CreateCompatibleBitmap(hdc_win, width, height)
    old = gdi32.SelectObject(hdc_mem, hbmp)
    ok = user32.PrintWindow(hwnd, hdc_mem, 2)  # PW_RENDERFULLCONTENT
    if not ok:
        ok = user32.PrintWindow(hwnd, hdc_mem, 0)
    img = None
    if ok:
        bmi = BITMAPINFOHEADER()
        bmi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        bmi.biWidth = width
        bmi.biHeight = -height
        bmi.biPlanes = 1
        bmi.biBitCount = 32
        bmi.biCompression = 0  # BI_RGB
        buf = ctypes.create_string_buffer(width * height * 4)
        rows = gdi32.GetDIBits(hdc_mem, hbmp, 0, height, buf, ctypes.byref(bmi), 0)
        if rows > 0:
            img = Image.frombuffer("RGBA", (width, height), buf, "raw", "BGRA", 0, 1).convert("RGB")
    gdi32.SelectObject(hdc_mem, old)
    gdi32.DeleteObject(hbmp)
    gdi32.DeleteDC(hdc_mem)
    user32.ReleaseDC(hwnd, hdc_win)
    return img


def request_d3d_shot(pid, tag, out_png, timeout=10.0):
    if not pid:
        return False
    stem = os.path.splitext(out_png)[0]
    bmp_path = os.path.join(ARTIFACT_DIR, stem + ".bmp")
    png_path = os.path.join(ARTIFACT_DIR, out_png)
    req_path = os.path.join(ARTIFACT_DIR, f"shot_{pid}.req")
    for p in (bmp_path, bmp_path + ".tmp"):
        if os.path.exists(p):
            try:
                os.remove(p)
            except Exception:
                pass
    with open(req_path, "w", encoding="utf-8") as f:
        f.write(bmp_path + "\n")
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(bmp_path) and os.path.getsize(bmp_path) > 54:
            time.sleep(0.05)
            try:
                img = Image.open(bmp_path)
                img.load()
                img.save(png_path)
                extrema = img.getextrema()
                print(f"[{tag}] D3D9 backbuffer shot saved ({img.size[0]}x{img.size[1]}) extrema={extrema} -> {png_path}")
                try:
                    os.remove(bmp_path)
                except Exception:
                    pass
                return True
            except Exception as exc:
                print(f"[{tag}] Failed reading BMP {bmp_path}: {exc}")
                return False
        time.sleep(0.08)
    print(f"[{tag}] WARNING: Timed out waiting for D3D9 shot {bmp_path}")
    return False


def send_menu_key(hwnd, pid, tag, vk, scan, extended=False, wait_s=0.6):
    ext_bit = (1 << 24) if extended else 0
    lparam_down = 1 | (scan << 16) | ext_bit
    lparam_up = 1 | (scan << 16) | ext_bit | (1 << 30) | (1 << 31)
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, lparam_down)
    time.sleep(0.08)
    user32.PostMessageW(hwnd, WM_KEYUP, vk, lparam_up)
    time.sleep(wait_s)
    dismiss_dialogs(pid, tag)


def request_join(pid, tag):
    req = os.path.join(ARTIFACT_DIR, f"join_{pid}.req")
    with open(req, "w", encoding="ascii") as f:
        f.write("1\n")
    print(f"[{tag}] wrote join trigger: {req}")


def click_menu_rel(hwnd, pid, tag, fx, fy, double=False, wait_s=0.5):
    rect = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    w = max(1, rect.right - rect.left)
    h = max(1, rect.bottom - rect.top)
    x = rect.left + int(w * fx)
    y = rect.top + int(h * fy)
    user32.ShowWindow(hwnd, SW_RESTORE)
    user32.SetForegroundWindow(hwnd)
    user32.SetCursorPos(x, y)
    time.sleep(0.08)

    def _left_click():
        down = INPUT(type=0)
        down.u.mi = MOUSEINPUT(0, 0, 0, 0x0002, 0, 0)  # MOUSEEVENTF_LEFTDOWN
        up = INPUT(type=0)
        up.u.mi = MOUSEINPUT(0, 0, 0, 0x0004, 0, 0)    # MOUSEEVENTF_LEFTUP
        user32.SendInput(1, ctypes.byref(down), ctypes.sizeof(INPUT))
        time.sleep(0.05)
        user32.SendInput(1, ctypes.byref(up), ctypes.sizeof(INPUT))

    _left_click()
    if double:
        time.sleep(0.08)
        _left_click()
    time.sleep(wait_s)
    dismiss_dialogs(pid, tag)


def filter_log_lines(path, keywords, limit=40):
    low_keys = tuple(k.lower() for k in keywords)
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            matches = [ln.rstrip("\r\n") for ln in fh if any(k in ln.lower() for k in low_keys)]
        total = len(matches)
        if total > limit:
            return f"(showing last {limit} of {total} matches)\n" + "\n".join(matches[-limit:])
        return f"({total} matches)\n" + "\n".join(matches)
    except Exception as exc:
        return f"<error reading {path}: {exc}>"


def has_log_note(log_path, note_substr):
    return count_log_note(log_path, note_substr) > 0


def count_log_note(log_path, note_substr):
    if not os.path.exists(log_path):
        return 0
    try:
        with open(log_path, "r", encoding="utf-8", errors="ignore") as f:
            return f.read().count(note_substr)
    except Exception:
        return 0


def wait_for_log_note(log_path, note_substr, timeout=10.0, min_count=1):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if count_log_note(log_path, note_substr) >= min_count:
            return True
        time.sleep(0.2)
    return False


def enter_coop_online(vp, pid, tag, log_path, coop_shot, login_shot, lobbylist_shot, menuonline_shot):
    """Navigate GAME MODE SELECTION -> COOP -> ONLINE -> P_Login -> P_MenuLobbyList -> P_MenuOnline."""
    hwnd = vp["hwnd"]
    print(f"[{tag}] Viewport found: hwnd=0x{hwnd:X} rect={vp['rect']} title='{vp['title']}'")
    # Main Menu: Down -> COOP -> Enter
    send_menu_key(hwnd, pid, tag, VK_DOWN, 0x50, extended=True, wait_s=0.5)
    send_menu_key(hwnd, pid, tag, VK_RETURN, 0x1C, extended=False, wait_s=2.0)
    request_d3d_shot(pid, tag, coop_shot)
    # P_OfflineOnline: Down (from LAN to ONLINE) -> Enter -> P_Login
    send_menu_key(hwnd, pid, tag, VK_DOWN, 0x50, extended=True, wait_s=0.5)
    send_menu_key(hwnd, pid, tag, VK_RETURN, 0x1C, extended=False, wait_s=2.5)
    request_d3d_shot(pid, tag, login_shot)
    # P_Login: press Enter to submit login (fallback Down+Enter if focus is on an EditBox/List)
    send_menu_key(hwnd, pid, tag, VK_RETURN, 0x1C, extended=False, wait_s=1.8)
    if not wait_for_log_note(log_path, "note=online_login_success", timeout=2.0):
        print(f"[{tag}] P_Login Enter did not submit directly; cycling Down/Tab -> Enter")
        for _ in range(4):
            send_menu_key(hwnd, pid, tag, VK_DOWN, 0x50, extended=True, wait_s=0.25)
            send_menu_key(hwnd, pid, tag, VK_RETURN, 0x1C, extended=False, wait_s=1.5)
            if has_log_note(log_path, "note=online_login_success"):
                break
    time.sleep(1.0)
    request_d3d_shot(pid, tag, lobbylist_shot)
    # P_MenuOnline auto-joins COOP ONLINE lobby group in Tick unless manual selection is needed.
    if not wait_for_log_note(log_path, "note=online_lobby_group_joined", timeout=4.0):
        send_menu_key(hwnd, pid, tag, VK_RETURN, 0x1C, extended=False, wait_s=2.0)
        wait_for_log_note(log_path, "note=online_lobby_group_joined", timeout=3.0)
    time.sleep(0.8)
    request_d3d_shot(pid, tag, menuonline_shot)


def dismiss_dialogs_for_hwnd_pid(hwnd, tag):
    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    if pid.value:
        dismiss_dialogs(pid.value, tag)


def is_pid_alive(pid):
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    h = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return False
    code = wintypes.DWORD()
    ok = kernel32.GetExitCodeProcess(h, ctypes.byref(code))
    kernel32.CloseHandle(h)
    return bool(ok) and code.value == 259  # STILL_ACTIVE


def kill_pid(pid):
    if pid:
        subprocess.run(["taskkill", "/F", "/PID", str(pid)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def tail_file(path, n=15):
    if not os.path.exists(path):
        return f"<missing: {path}>"
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        lines = f.readlines()
    return "".join(lines[-n:])


def main():
    os.makedirs(ARTIFACT_DIR, exist_ok=True)
    for p in (HOST_LOG, GUEST_LOG, RELAY_LOG):
        if os.path.exists(p):
            try:
                os.remove(p)
            except Exception:
                pass
    for pat in ("shot_*.req", "kick_*.req", "join_*.req", "voice_*.req", "online_*.bmp", "online_*.bmp.tmp", "online_*.png"):
        for f in glob.glob(os.path.join(ARTIFACT_DIR, pat)):
            try:
                os.remove(f)
            except Exception:
                pass

    env = os.environ.copy()
    env["SCCT_SHOT_DIR"] = ARTIFACT_DIR

    relay_proc = None
    relay_log_f = None
    host_launch = None
    guest_launch = None
    host_pid = None
    guest_pid = None

    try:
        relay_log_f = open(RELAY_LOG, "w", encoding="utf-8")
        relay_proc = subprocess.Popen(
            [RELAY_EXE, "--listen", "127.0.0.1:27100", "--expire", "30", "--rate", "300"],
            stdout=relay_log_f,
            stderr=subprocess.STDOUT,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        time.sleep(0.5)

        host_launch = subprocess.Popen(
            [
                LAUNCH_EXE,
                "--visible",
                "--enabled",
                "1",
                "--transport",
                "1",
                "--relay",
                "127.0.0.1:27100",
                "--local-ip",
                "127.0.0.1",
                "--name",
                "AgentSam",
                "--friendly-fire",
                "0",
                "--voice",
                "1",
                "--log",
                HOST_LOG_NAME,
                GAME_EXE,
            ],
            cwd=GAME_DIR,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        host_pid = wait_for_pid_from_log(HOST_LOG, timeout=15.0)
        print(f"[HOST] game pid={host_pid}")

        time.sleep(1.5)

        guest_launch = subprocess.Popen(
            [
                LAUNCH_EXE,
                "--visible",
                "--enabled",
                "1",
                "--transport",
                "1",
                "--relay",
                "127.0.0.1:27100",
                "--local-ip",
                "127.0.0.2",
                "--name",
                "AgentBob",
                "--friendly-fire",
                "0",
                "--voice",
                "1",
                "--log",
                GUEST_LOG_NAME,
                GAME_EXE,
            ],
            cwd=GAME_DIR,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        guest_pid = wait_for_pid_from_log(GUEST_LOG, timeout=15.0)
        print(f"[GUEST] game pid={guest_pid}")

        host_vp = wait_for_viewport(host_pid, "HOST", timeout=25.0) if host_pid else None
        guest_vp = wait_for_viewport(guest_pid, "GUEST", timeout=25.0) if guest_pid else None
        time.sleep(1.5)

        print("[HOST] All windows:", enum_windows_for_pid(host_pid) if host_pid else [])
        print("[GUEST] All windows:", enum_windows_for_pid(guest_pid) if guest_pid else [])

        if host_vp:
            enter_coop_online(
                host_vp,
                host_pid,
                "HOST",
                HOST_LOG,
                "online_host_coop.png",
                "online_host_login.png",
                "online_host_lobbylist.png",
                "online_host_menuonline.png",
            )
            # P_MenuOnline: Row 0=QUICK MATCH, Row 1=CREATE GAME, Row 2=GAMES LIST, Row 3=FRIENDS
            send_menu_key(host_vp["hwnd"], host_pid, "HOST", VK_DOWN, 0x50, extended=True, wait_s=0.5)
            send_menu_key(host_vp["hwnd"], host_pid, "HOST", VK_RETURN, 0x1C, extended=False, wait_s=2.5)
            request_d3d_shot(host_pid, "HOST", "online_host_create.png")
            # P_OnlineCreate has 6 rows: MATCH NAME(0), PASSWORD(1), MODE(2), MISSION(3), DIFFICULTY(4), FRIENDLY FIRE(5)
            for _ in range(5):
                send_menu_key(host_vp["hwnd"], host_pid, "HOST", VK_DOWN, 0x50, extended=True, wait_s=0.25)
            send_menu_key(host_vp["hwnd"], host_pid, "HOST", VK_RETURN, 0x1C, extended=False, wait_s=0.4)
            request_d3d_shot(host_pid, "HOST", "online_host_create_ffire.png")
            send_menu_key(host_vp["hwnd"], host_pid, "HOST", VK_DOWN, 0x50, extended=True, wait_s=0.25)
            send_menu_key(host_vp["hwnd"], host_pid, "HOST", VK_RETURN, 0x1C, extended=False, wait_s=3.5)
            request_d3d_shot(host_pid, "HOST", "online_host_room.png")
        else:
            print("[HOST] ERROR: Viewport not found!")

        if guest_vp:
            enter_coop_online(
                guest_vp,
                guest_pid,
                "GUEST",
                GUEST_LOG,
                "online_guest_coop.png",
                "online_guest_login.png",
                "online_guest_lobbylist.png",
                "online_guest_menuonline.png",
            )
            # P_MenuOnline: Row 2=CUSTOM MATCH -> P_LobbyOnline server browser
            send_menu_key(guest_vp["hwnd"], guest_pid, "GUEST", VK_DOWN, 0x50, extended=True, wait_s=0.4)
            send_menu_key(guest_vp["hwnd"], guest_pid, "GUEST", VK_DOWN, 0x50, extended=True, wait_s=0.4)
            send_menu_key(guest_vp["hwnd"], guest_pid, "GUEST", VK_RETURN, 0x1C, extended=False, wait_s=4.5)
            wait_for_log_note(GUEST_LOG, "note=online_browse_found", timeout=5.0)
            time.sleep(0.5)
            request_d3d_shot(guest_pid, "GUEST", "online_guest_serverlist.png")
            # Trigger JOIN on selected Row 0 in P_LobbyOnline
            request_join(guest_pid, "GUEST")
            wait_for_log_note(GUEST_LOG, "note=online_join_requested", timeout=3.0)
            wait_for_log_note(GUEST_LOG, "note=online_quazal_join", timeout=5.0)
            time.sleep(2.0)
            request_d3d_shot(guest_pid, "GUEST", "online_guest_room.png")
            if host_vp:
                request_d3d_shot(host_pid, "HOST", "online_host_room_joined.png")
                kick_req = os.path.join(ARTIFACT_DIR, f"kick_{host_pid}.req")
                with open(kick_req, "w", encoding="ascii") as f:
                    f.write("1\n")
                print(f"[HOST] wrote kick trigger: {kick_req}")
                time.sleep(3.5)
                request_d3d_shot(host_pid, "HOST", "online_host_after_kick.png")
                request_d3d_shot(guest_pid, "GUEST", "online_guest_kicked.png")
                # Guest was returned to P_LobbyOnline; wait for browse refresh and rejoin P_RoomOnline.
                prev_joins = count_log_note(GUEST_LOG, "note=online_quazal_join")
                time.sleep(2.5)
                request_join(guest_pid, "GUEST")
                wait_for_log_note(GUEST_LOG, "note=online_quazal_join", timeout=4.0, min_count=prev_joins + 1)
                time.sleep(2.5)
                request_d3d_shot(guest_pid, "GUEST", "online_guest_rejoined.png")
                request_d3d_shot(host_pid, "HOST", "online_host_rejoined.png")
            # Guest presses VK_SPACE for READY, Host waits for ready confirmation then presses VK_SPACE/Enter for LAUNCH.
            send_menu_key(guest_vp["hwnd"], guest_pid, "GUEST", 0x20, 0x39, extended=False, wait_s=1.0)
            if not wait_for_log_note(HOST_LOG, "note=online_guest_ready", timeout=8.0):
                send_menu_key(guest_vp["hwnd"], guest_pid, "GUEST", 0x20, 0x39, extended=False, wait_s=1.0)
                wait_for_log_note(HOST_LOG, "note=online_guest_ready", timeout=4.0)
            if host_vp:
                send_menu_key(host_vp["hwnd"], host_pid, "HOST", 0x20, 0x39, extended=False, wait_s=2.0)
                if not wait_for_log_note(HOST_LOG, "note=online_launch_game", timeout=6.0):
                    send_menu_key(host_vp["hwnd"], host_pid, "HOST", VK_RETURN, 0x1C, extended=False, wait_s=2.0)
                    wait_for_log_note(HOST_LOG, "note=online_launch_game", timeout=4.0)
                wait_for_log_note(GUEST_LOG, "note=online_launch_game", timeout=15.0)
                time.sleep(12.0)
                for role, pid_val in [("HOST", host_pid), ("GUEST", guest_pid)]:
                    vreq = os.path.join(ARTIFACT_DIR, f"voice_{pid_val}.req")
                    with open(vreq, "w", encoding="ascii") as f:
                        f.write("0\n")
                    print(f"[{role}] wrote quiet voice trigger: {vreq}")
                send_menu_key(host_vp["hwnd"], host_pid, "HOST", 0x20, 0x39, extended=False, wait_s=0.5)
                send_menu_key(guest_vp["hwnd"], guest_pid, "GUEST", 0x20, 0x39, extended=False, wait_s=1.0)
                request_d3d_shot(host_pid, "HOST", "online_host_ingame.png")
                request_d3d_shot(guest_pid, "GUEST", "online_guest_ingame.png")
                for role, pid_val in [("HOST", host_pid), ("GUEST", guest_pid)]:
                    vreq = os.path.join(ARTIFACT_DIR, f"voice_{pid_val}.req")
                    with open(vreq, "w", encoding="ascii") as f:
                        f.write("85\n")
                    print(f"[{role}] wrote active voice trigger: {vreq}")
                time.sleep(1.0)
                request_d3d_shot(host_pid, "HOST", "online_host_voice_meter.png")
                request_d3d_shot(guest_pid, "GUEST", "online_guest_voice_meter.png")
            else:
                request_d3d_shot(guest_pid, "GUEST", "online_guest_ingame.png")
        else:
            print("[GUEST] ERROR: Viewport not found!")

        time.sleep(2.0)
        log_keys = (
            "9100",
            "9103",
            "inject",
            "relais",
            "payload",
            "send",
            "recv",
            "bind",
            "transport",
            "feature",
            "custom_name",
            "friendly_fire",
            "ffire",
            "kick",
            "voice",
            "online_",
        )
        print(f"[STATUS] host_alive={is_pid_alive(host_pid) if host_pid else False} guest_alive={is_pid_alive(guest_pid) if guest_pid else False}")
        print("\n--- LIVE RELAY LOG (tail 25) ---\n" + tail_file(RELAY_LOG, 25))
        print("\n--- HOST LOG (filtered 9100/9103/Winsock/Online, max 35) ---\n" + filter_log_lines(HOST_LOG, log_keys, 35))
        print("\n--- GUEST LOG (filtered 9100/9103/Winsock/Online, max 35) ---\n" + filter_log_lines(GUEST_LOG, log_keys, 35))

    finally:
        for pat in ("shot_*.req", "kick_*.req", "join_*.req", "voice_*.req", "online_*.bmp.tmp"):
            for f in glob.glob(os.path.join(ARTIFACT_DIR, pat)):
                try:
                    os.remove(f)
                except Exception:
                    pass
        kill_pid(host_pid)
        kill_pid(guest_pid)
        for p in (host_launch, guest_launch, relay_proc):
            if p is not None:
                try:
                    p.terminate()
                    p.wait(timeout=3)
                except Exception:
                    try:
                        p.kill()
                    except Exception:
                        pass
        if relay_log_f is not None:
            try:
                relay_log_f.close()
            except Exception:
                pass


if __name__ == "__main__":
    main()
