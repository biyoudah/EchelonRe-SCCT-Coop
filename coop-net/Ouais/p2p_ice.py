"""64-bit libjuice ICE P2P + TURN helper for CloudRelayBridge."""

import ctypes
import json
import os
import sys
import threading
import urllib.parse
import urllib.request

# libjuice juice_state_t values
ICE_DISCONNECTED = 0
ICE_GATHERING = 1
ICE_CONNECTING = 2
ICE_CONNECTED = 3
ICE_COMPLETED = 4
ICE_FAILED = 5

PKT_TYPE_GAME = 0x01
PKT_TYPE_VOICE = 0x02

_STATE_CB = ctypes.WINFUNCTYPE(None, ctypes.c_int, ctypes.c_void_p)
_CAND_CB = ctypes.WINFUNCTYPE(None, ctypes.c_char_p, ctypes.c_void_p)
_GATHER_DONE_CB = ctypes.WINFUNCTYPE(None, ctypes.c_void_p)
_RECV_CB = ctypes.WINFUNCTYPE(None, ctypes.POINTER(ctypes.c_uint8), ctypes.c_int, ctypes.c_void_p)


def _find_p2p_dll():
    candidates = []
    if getattr(sys, "frozen", False) and hasattr(sys, "_MEIPASS"):
        candidates.append(os.path.join(sys._MEIPASS, "scct_p2p_x64.dll"))
        candidates.append(os.path.join(os.path.dirname(sys.executable), "scct_p2p_x64.dll"))
    candidates.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), "scct_p2p_x64.dll"))
    for path in candidates:
        if os.path.isfile(path):
            return path
    return None


def _load_p2p_dll():
    path = _find_p2p_dll()
    if not path:
        return None
    try:
        dll = ctypes.WinDLL(path)
        dll.scct_ice_create.argtypes = [
            ctypes.c_char_p,
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_char_p,
            _STATE_CB,
            _CAND_CB,
            _GATHER_DONE_CB,
            _RECV_CB,
            ctypes.c_void_p,
        ]
        dll.scct_ice_create.restype = ctypes.c_void_p
        dll.scct_ice_destroy.argtypes = [ctypes.c_void_p]
        dll.scct_ice_destroy.restype = None
        dll.scct_ice_gather.argtypes = [ctypes.c_void_p]
        dll.scct_ice_gather.restype = ctypes.c_int
        dll.scct_ice_get_local_sdp.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
        dll.scct_ice_get_local_sdp.restype = ctypes.c_int
        dll.scct_ice_set_remote_sdp.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        dll.scct_ice_set_remote_sdp.restype = ctypes.c_int
        dll.scct_ice_add_remote_candidate.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        dll.scct_ice_add_remote_candidate.restype = ctypes.c_int
        dll.scct_ice_set_remote_gathering_done.argtypes = [ctypes.c_void_p]
        dll.scct_ice_set_remote_gathering_done.restype = ctypes.c_int
        dll.scct_ice_send.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
        dll.scct_ice_send.restype = ctypes.c_int
        return dll
    except Exception:
        return None


_DLL = _load_p2p_dll()


def is_available():
    return _DLL is not None


def _parse_host_port(uri, default_port):
    # Handle "stun:host:port" or "turn:host:port?transport=udp"
    rest = uri.split(":", 1)[1] if ":" in uri else uri
    rest = rest.split("?", 1)[0].strip("/")
    if ":" in rest:
        host, port_str = rest.rsplit(":", 1)
        try:
            return host.strip("[]"), int(port_str)
        except ValueError:
            return host.strip("[]"), default_port
    return rest.strip("[]"), default_port


def fetch_ice_config(worker_base_url, bearer_token, room_code, timeout=4.0):
    """Fetch STUN/TURN credentials from Worker /v2/turn/credentials with safe STUN fallback."""
    cfg = {
        "stun_host": "stun.l.google.com",
        "stun_port": 19302,
        "turn_host": "",
        "turn_port": 0,
        "turn_user": "",
        "turn_pass": "",
        "mode": "stun-only",
    }
    if not worker_base_url or not bearer_token:
        return cfg
    url = worker_base_url.rstrip("/") + "/v2/turn/credentials"
    body = json.dumps({"room_code": room_code}).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=body,
        method="POST",
        headers={
            "Authorization": f"Bearer {bearer_token}",
            "Content-Type": "application/json",
            "User-Agent": "SCCT-Revival-Launcher/2.1",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read().decode("utf-8"))
        cfg["mode"] = str(data.get("mode") or "stun-only")
        for srv in data.get("iceServers") or []:
            urls = srv.get("urls") or []
            if isinstance(urls, str):
                urls = [urls]
            for u in urls:
                if not isinstance(u, str):
                    continue
                low = u.lower()
                if low.startswith("stun:") and not cfg.get("_stun_set"):
                    host, port = _parse_host_port(u, 19302)
                    if host:
                        cfg["stun_host"] = host
                        cfg["stun_port"] = port
                        cfg["_stun_set"] = True
                elif low.startswith("turn:") and not cfg["turn_host"]:
                    host, port = _parse_host_port(u, 3478)
                    if host:
                        cfg["turn_host"] = host
                        cfg["turn_port"] = port
                        cfg["turn_user"] = str(srv.get("username") or "")
                        cfg["turn_pass"] = str(srv.get("credential") or "")
    except Exception:
        pass
    cfg.pop("_stun_set", None)
    return cfg


class IceSession:
    """Manages one libjuice ICE agent for P2P UDP + TURN transport."""

    def __init__(self, ice_cfg, on_state, on_candidate, on_gather_done, on_recv):
        self._on_state = on_state
        self._on_candidate = on_candidate
        self._on_gather_done = on_gather_done
        self._on_recv = on_recv
        self._lock = threading.Lock()
        self._closed = False
        self._handle = None
        self.state = ICE_DISCONNECTED
        self.connected = False

        if _DLL is None:
            return

        # Keep callback references alive on self for the lifetime of the agent.
        self._cb_state = _STATE_CB(self._handle_state)
        self._cb_cand = _CAND_CB(self._handle_cand)
        self._cb_gather_done = _GATHER_DONE_CB(self._handle_gather_done)
        self._cb_recv = _RECV_CB(self._handle_recv)

        stun_host = (ice_cfg.get("stun_host") or "stun.l.google.com").encode("utf-8")
        stun_port = int(ice_cfg.get("stun_port") or 19302)
        turn_host = (ice_cfg.get("turn_host") or "").encode("utf-8")
        turn_port = int(ice_cfg.get("turn_port") or 0)
        turn_user = (ice_cfg.get("turn_user") or "").encode("utf-8")
        turn_pass = (ice_cfg.get("turn_pass") or "").encode("utf-8")

        self._handle = _DLL.scct_ice_create(
            stun_host,
            stun_port,
            turn_host if turn_host else None,
            turn_port,
            turn_user if turn_user else None,
            turn_pass if turn_pass else None,
            self._cb_state,
            self._cb_cand,
            self._cb_gather_done,
            self._cb_recv,
            None,
        )

    @property
    def ready(self):
        return self._handle is not None and not self._closed

    def _handle_state(self, state, _user):
        self.state = int(state)
        self.connected = state in (ICE_CONNECTED, ICE_COMPLETED)
        if self._on_state:
            try:
                self._on_state(int(state))
            except Exception:
                pass

    def _handle_cand(self, sdp_bytes, _user):
        if sdp_bytes and self._on_candidate:
            try:
                self._on_candidate(sdp_bytes.decode("utf-8", errors="replace"))
            except Exception:
                pass

    def _handle_gather_done(self, _user):
        if self._on_gather_done:
            try:
                self._on_gather_done()
            except Exception:
                pass

    def _handle_recv(self, data_ptr, size, _user):
        if size > 0 and data_ptr and self._on_recv:
            try:
                raw = ctypes.string_at(data_ptr, size)
                self._on_recv(raw)
            except Exception:
                pass

    def get_local_sdp(self):
        with self._lock:
            if not self.ready:
                return ""
            buf = ctypes.create_string_buffer(8192)
            rc = _DLL.scct_ice_get_local_sdp(self._handle, buf, len(buf))
            if rc > 0:
                return buf.value.decode("utf-8", errors="replace")
            return ""

    def gather(self):
        with self._lock:
            if not self.ready:
                return False
            return _DLL.scct_ice_gather(self._handle) == 0

    def set_remote_sdp(self, sdp):
        with self._lock:
            if not self.ready or not sdp:
                return False
            return _DLL.scct_ice_set_remote_sdp(self._handle, sdp.encode("utf-8")) == 0

    def add_remote_candidate(self, sdp):
        with self._lock:
            if not self.ready or not sdp:
                return False
            return _DLL.scct_ice_add_remote_candidate(self._handle, sdp.encode("utf-8")) == 0

    def set_remote_gathering_done(self):
        with self._lock:
            if not self.ready:
                return False
            return _DLL.scct_ice_set_remote_gathering_done(self._handle) == 0

    def send(self, payload: bytes):
        with self._lock:
            if not self.ready or not self.connected or not payload:
                return False
            rc = _DLL.scct_ice_send(self._handle, payload, len(payload))
            return rc == 0

    def close(self):
        with self._lock:
            self._closed = True
            self.connected = False
            h = self._handle
            self._handle = None
        if h is not None and _DLL is not None:
            try:
                _DLL.scct_ice_destroy(h)
            except Exception:
                pass
