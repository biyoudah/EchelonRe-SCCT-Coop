# EchelonRe — Splinter Cell: Chaos Theory Online Co-op & SCDA Xbox Map Ports

**EchelonRe** restores online co-op for *Tom Clancy's Splinter Cell: Chaos Theory* (PC v1.05) without port forwarding, adds a modern native launcher with accounts, friends, voice chat, and auto-updates, and ports all **14 Xbox co-op missions** from *Splinter Cell: Double Agent* (Version 2) — Iceland, Prison, and Kinshasa — directly into Chaos Theory.

---

## Quick Start (Players)

1. Download **`EchelonRe.SCCTCoopLauncher-win-Setup.exe`** from [Releases](https://github.com/biyoudah/EchelonRe-SCCT-Coop/releases/latest).
2. Run the installer and sign in or create an account (verified via 6-digit email code).
3. In **Settings**, link your *Splinter Cell: Chaos Theory* (PC v1.05) folder — the launcher verifies your executable and automatically installs the co-op mod (`ScctCoopNet.dll`, `scct-coop-launch.exe`, `scct-coop-net.ini`).
4. *(Optional)* Open **Maps & Mods** to install the **SCDA Co-op Maps Pack** (all 14 ported Double Agent co-op missions with audio, loading screens, and localization) and **Widescreen Fix**.
5. Host or join a room from **Rooms**, or use the in-game **F8** overlay to manage friends and pick your microphone.

---

## Architecture & How It Works

The project is split into four main components:

### 1. Cloudflare Worker Backend (`worker/`)
Serverless edge directory, authentication, and relay built with TypeScript on Cloudflare Workers, D1 (SQLite), and Durable Objects:
- **Accounts & Email Verification (`worker/src/online.ts`, `worker/src/accounts.ts`)**: PBKDF2/SHA-256 credential hashing, rate-limited login/verification lockouts, and transactional 6-digit OTP email delivery via Brevo or Resend.
- **Room Directory & WebSocket Relay (`worker/src/lobby-do.ts`, `worker/src/online.ts`)**: Allocates `SCCT-XXXXXX` room codes in D1 and relays UDP co-op/voice packets over WebSockets via per-room Durable Objects, eliminating router port forwarding.
- **Friends & Presence (`worker/src/online.ts`)**: Friend requests, accepts, removals, online status, and active room codes synced to both the launcher and the in-game overlay.
- **Authenticated Content Delivery (`GET /v2/content/scda-coop-maps`)**: Requires a valid launcher Bearer token and issues short-lived signed redirects to the hosted SCDA Co-op Maps Pack archive, keeping the pack downloadable from the launcher rather than directly from this repository.

### 2. In-Game 32-bit C++ Hook DLL (`coop-net/src/`)
A 32-bit Windows DLL (`ScctCoopNet.dll`) using [MinHook](coop-net/third_party/minhook/) injected at launch by `scct-coop-launch.exe`:
- **Universal v1.05 Verification & Multi-Instance (`coop-net/src/dllmain.cpp`)**: Validates `splintercell3.exe` via PE/code-anchor checks across Retail and Ubisoft Connect v1.05 builds, bypasses the single-instance `SplinterCell3` mutex for local multi-instance testing, and hooks WinSock (`sendto`, `recvfrom`, `bind`, `select`, `WSARecvFrom`, etc.) to bridge LAN UDP traffic through the local WebSocket relay.
- **In-Game Overlay, Voice & Two-Way Sync (`coop-net/src/dllmain.cpp`, `coop-net/src/transport.cpp`)**: Direct3D/input hooks providing an **F8** overlay with live two-way friend requests/accepts and microphone device selection synced with the launcher's local API.
- **SCDA Runtime & Gameplay Hooks (`coop-net/src/scda_hooks.cpp`, `coop-net/src/scda_gameplay.cpp`)**:
  - Hooks `ConfigGetString` / `ConfigGetStringBuf` to read loose `System/MapRotation.ini` entries (`MissionCoop[7+]`) and `.int` localization fallbacks (`scda_override.int`, `scda_*.int`, `P_x0*_coop_*.int`).
  - Redirects `StaticLoadObject` (`EchelonPattern.V*` -> `<map>_pat.V*`) and DareAudio file opens (`MAPS.SM0`/`STREAM.SS0`/`MAPS.LM0`/`STREAM.LS0` -> `SCDA_*`) so retail Chaos Theory sound files are never modified.
  - Adds null-pointer safety guards for SCDA animation/hash lookups and implements **Back-to-Back Climb** and **Dual Interrogation** state machines in `scda_gameplay.cpp`.

### 3. Desktop Launcher (`coop-net/Ouais/`)
A Python **Flask** + **pywebview** desktop application packaged with **PyInstaller** and **Velopack** (`EchelonRe.SCCTCoopLauncher-win-Setup.exe`):
- **Local WebSocket-to-UDP Bridge (`coop-net/Ouais/ws_bridge.py`)**: Connects to the Cloudflare Durable Object WebSocket relay and exposes a local UDP endpoint for `ScctCoopNet.dll`.
- **Two-Way Local API & Auto-Updater (`coop-net/Ouais/server.py`, `coop-net/Ouais/start_launcher.py`)**: Serves the UI (`static/`), bridges in-game friend/mic changes in real time via SSE, and checks GitHub Releases for Velopack updates.
- **One-Click SCDA Pack Installer (`coop-net/Ouais/server.py`)**: Downloads `SCDA-Coop-Maps-Pack.zip` with SHA-256 verification and cross-host `Authorization` stripping, extracts all 14 maps + loading screens + localization, locally patches `SCDA_MAPS.SM0`, `SCDA_STREAM.SS0`, and per-language `SCDA_MAPS.LM0`/`SCDA_STREAM.LS0` from the player's own retail audio files, and merges `MissionCoop[7..20]` into `System/MapRotation.ini`.

### 4. SCDA Xbox -> SCCT PC Porting Pipeline (`tools/scda_port/`)
Offline Python + C++ reverse-engineering toolchain used to convert all 14 *Splinter Cell: Double Agent* Xbox co-op maps into Chaos Theory PC UE2.5 packages:
- **`iso_extract.py`**: Extracts `.lin` level archives, `common.lin`, `.ini`, `.occ`, `.int`, and `DynamicXbox.umd` metadata from SCDA Xbox media.
- **`lin_dumper.cpp` + `dump_map.py`**: Launches the SCDA Xbox static recompilation (`SCDA.exe`) suspended, injects `lin_dumper.dll`, and captures runtime `ULinkerLoad` / `UObject` / Havok `hkWorld` state for each co-op map.
- **`reconstruct_packages.py`**: Matches `.lin` package headers against runtime dump events to rebuild standalone `.scl`, `.u`, `.utx`, `.usx`, `.ukx`, and `.uax` packages.
- **`convert_package.py`, `mesh_convert.py`, `validate_pkg.py`**: Converts UE2.5 packages from Xbox `LicVer=127` to Chaos Theory PC `LicVer=124` (remapping SCDA-only UnrealScript classes, expanding Xbox tristrip `StaticMesh` sections to trilists, stripping Xbox `UModel` texture blocks, patching level actors/identities, and verifying all imports/exports).
- **`convert_audio.py`**: Decodes Xbox IMA ADPCM DareAudio SFX (`MAPS.SM2`) and dialogue (`MAPS.LM2`) banks into deduplicated 16-bit PCM streams and relocatable PC `.bank` files (`--export-pack`).
- **`convert_loading.py` & `build_scda_pack.py`**: Encodes 640×480 loading screens into split DXT5 `.dds` textures, extracts map icons, and builds the complete pack + `content-manifest.json`.

---

## Building from Source

### In-Game Mod (`coop-net/`)
Requires Visual Studio (MSVC) with C++17 and CMake (must be **32-bit `Win32`** to match `splintercell3.exe`):
```powershell
cmake -S coop-net -B coop-net\build -A Win32
cmake --build coop-net\build --config Release
```

### Desktop Launcher & Installer (`coop-net/Ouais/`)
Requires Python 3.12+ and (optionally) Velopack CLI (`dotnet tool install -g vpk`):
```powershell
cd coop-net\Ouais
.\build_exe.bat
```

### Cloudflare Worker (`worker/`)
Requires Node.js and a Cloudflare account with D1 + Durable Objects:
```powershell
cd worker
npm install
npx wrangler d1 migrations apply scct-echelonre --remote
npx wrangler deploy
```

---

## About the Author & Development Process

I'm a Computer Science engineering student, long-time *Splinter Cell* fan, and game modder (with prior modding experience on other titles). I used AI tooling as an engineering multiplier alongside reverse engineering and game-modding domain knowledge to analyze binaries, iterate on Unreal Engine 2.5 / DareAudio formats, and ship this full-stack co-op revival and map port. I'm always happy to talk about the methods, hooks, and tooling if asked!

---

## Credits & Attributions

- **TGP482** — [SCDA-XBERecomp](https://github.com/TGP482/SCDA-XBERecomp) & [XboxXBERecomp](https://github.com/TGP482/XboxXBERecomp)
  All *Splinter Cell: Double Agent* Xbox co-op map assets, meshes, textures, animations, DareAudio sound/dialogue banks, and loading screens were extracted and ported using the SCDA Xbox static recompilation.
- **ThirteenAG** — [SplinterCellChaosTheory.WidescreenFix](https://github.com/ThirteenAG/WidescreenFixesPack)
  Optional 16:9 / ultrawide resolution, HUD scaling, and FOV fix.
- **TsudaKageyu / MinHook** — [MinHook](https://github.com/TsudaKageyu/minhook) (bundled in `coop-net/third_party/minhook/`).
