# SCCT Co-op Launcher 2.0

Run: double-click `Start-SCCT-Launcher.bat` (first run creates a venv and installs `requirements.txt`; no console window afterwards).
Package: `build_exe.bat` → `dist\SCCT-Launcher\SCCT-Launcher.exe` (+ Velopack installer if `vpk` is installed). Always run the exe from `dist\`, together with its `_internal` folder; never from a `build\` folder. Use Python 3.11-3.13.

- **Shell**: frameless WebView2 window (pywebview), single-instance mutex, native file dialog, Win11 rounded corners. `--browser` opens in the default browser instead.
- **Backend**: unchanged security model (CSP `'self'`, HttpOnly/SameSite cookie, CSRF, Origin check, MD5+size check). Served by Waitress. DB now lives in `%LOCALAPPDATA%\SCCTCoopLauncher\data`.
- **Realtime**: `/api/events` (SSE) pushes relay and game state; `ws_bridge.on_status_change` fires on connect, partner join/leave and (≤2 Hz) packet counters. Friends/rooms refresh every 15 s and on window focus.
- **Mod payload**: put `ScctCoopNet.dll`, `scct-coop-launch.exe`, `scct-coop-net.ini` in `mod/` (or keep `..\build\Release`, or set `SCCT_MOD_DIR`).
- **Art**: `static/assets/hero.jpg` (Play screen) and `static/assets/login.jpg` (sign-in screen); swap the files to change them.
