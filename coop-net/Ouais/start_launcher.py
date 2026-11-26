"""SCCT Co-op Launcher - native shell (pywebview/WebView2) around the local Flask backend."""
import argparse
import ctypes
import os
import socket
import sys
import threading
import time
import urllib.request
import webbrowser

# Velopack must run first: it handles install/update hooks and may exit or relaunch the process.
try:
    import velopack

    velopack.App().run()
except Exception:
    pass

BASE_DIR = getattr(sys, "_MEIPASS", None) or os.path.dirname(os.path.abspath(__file__))
if BASE_DIR not in sys.path:
    sys.path.insert(0, BASE_DIR)

import server  # noqa: E402

WINDOW_TITLE = "SCCT Co-op Launcher"
MUTEX_NAME = r"Local\SCCT_Echelon_Coop_Launcher"
BG = "#090B0D"
IS_WIN = os.name == "nt"


def ensure_single_instance():
    """Allow multiple launcher instances for local multi-instance testing."""
    return None


def pick_free_port(preferred=27180):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        try:
            s.bind(("127.0.0.1", preferred))
            return preferred
        except OSError:
            pass
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_server_ready(url, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(f"{url}/api/health", timeout=1.0) as resp:
                if resp.status == 200:
                    return True
        except Exception:
            time.sleep(0.15)
    return False


class WindowApi:
    """Exposed to the page as window.pywebview.api.*"""

    def __init__(self):
        self._window = None

    def bind(self, win):
        self._window = win

    def minimize(self):
        self._window.minimize()

    def close(self):
        self._window.destroy()


def round_corners(win):
    if not IS_WIN:
        return
    try:
        hwnd = int(win.native.Handle.ToInt32())
        pref = ctypes.c_int(2)  # DWMWCP_ROUND
        ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, 33, ctypes.byref(pref), ctypes.sizeof(pref))
    except Exception:
        pass


def run_native(url):
    import webview

    try:
        webview.settings["DRAG_REGION_DIRECT_TARGET_ONLY"] = True
    except Exception:
        pass

    api = WindowApi()
    win = webview.create_window(
        WINDOW_TITLE, url=url, js_api=api, width=1280, height=760,
        resizable=False, frameless=True, easy_drag=False, shadow=True, background_color=BG,
    )
    api.bind(win)

    def browse():
        dlg = getattr(getattr(webview, "FileDialog", None), "FOLDER", None) or getattr(webview, "FOLDER_DIALOG", None)
        if dlg is None:
            dlg = getattr(getattr(webview, "FileDialog", None), "OPEN", None) or webview.OPEN_DIALOG
            res = win.create_file_dialog(
                dlg, allow_multiple=False,
                file_types=("Splinter Cell Chaos Theory (splintercell3.exe)", "Executable (*.exe)", "All files (*.*)"),
            )
        else:
            res = win.create_file_dialog(dlg, allow_multiple=False)
        return res[0] if res else ""

    server.NATIVE_BROWSE_HOOK = browse
    win.events.shown += lambda: round_corners(win)
    webview.start(debug=False)


def main():
    parser = argparse.ArgumentParser(description="SCCT Echelon Co-op Launcher")
    parser.add_argument("--port", type=int, default=None)
    parser.add_argument("--browser", action="store_true", help="Open in the default browser instead of a native window.")
    args = parser.parse_args()

    _mutex = ensure_single_instance()  # noqa: F841 (keep alive)
    preferred = args.port if args.port is not None else int(os.environ.get("SCCT_LAUNCHER_PORT") or 27180)
    port = pick_free_port(preferred)
    url = f"http://127.0.0.1:{port}"

    threading.Thread(target=server.run_server, args=(port,), daemon=True).start()
    if not wait_server_ready(url):
        print("Failed to start the launcher backend.", file=sys.stderr)
        sys.exit(1)

    try:
        if args.browser:
            raise ImportError("browser mode requested")
        run_native(url)
    except ImportError:
        print(f"Native window unavailable; open {url}")
        webbrowser.open(url)
        try:
            while True:
                time.sleep(1.0)
        except KeyboardInterrupt:
            pass
    finally:
        server.stop_bridge()


if __name__ == "__main__":
    main()
