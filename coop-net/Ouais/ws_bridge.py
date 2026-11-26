import asyncio
import json
import socket
import struct
import threading
import time
import urllib.parse

import websockets

try:
    from . import p2p_ice
except Exception:
    try:
        import p2p_ice
    except Exception:
        p2p_ice = None

MAGIC = 0x31524353  # 'SCR1' little-endian
VERSION = 1
MARKER = 0x7EC3

MSG_REGISTER = 1
MSG_REGISTER_OK = 2
MSG_HEARTBEAT = 3
MSG_REFUSED = 5
MSG_ADDRESS = 6
MSG_ATTACH = 7

REFUSE_UNKNOWN = 4
GAME_PORTS = (9100, 9103)
SYNTHETIC_PEER_IP_BYTES = bytes((10, 99, 0, 2))


def pick_free_udp_pair(preferred_port=27120):
    candidates = [preferred_port] if preferred_port and preferred_port > 0 else []
    for base in range(27120, 27300, 2):
        if base not in candidates:
            candidates.append(base)
    for port in candidates:
        s1 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s1.bind(("127.0.0.1", port))
            s2.bind(("127.0.0.1", port + 1))
            return port
        except OSError:
            pass
        finally:
            s1.close()
            s2.close()
    raise RuntimeError("No free UDP port pair available on 127.0.0.1")


def build_register_ok(peer_id, has_other, peer_ip_bytes=b"\x00\x00\x00\x00", peer_port=0):
    ip_bytes = peer_ip_bytes if has_other else b"\x00\x00\x00\x00"
    p_host = peer_port if has_other else 0
    return struct.pack(
        "<IHHIB4sHH",
        MAGIC,
        VERSION,
        MSG_REGISTER_OK,
        peer_id,
        1 if has_other else 0,
        ip_bytes,
        p_host,
        MARKER,
    )


def build_address(peer_ip_bytes, peer_port, game_port):
    return struct.pack("<IHH4sHHH", MAGIC, VERSION, MSG_ADDRESS, peer_ip_bytes, peer_port, game_port, MARKER)


def build_refused(reason):
    return struct.pack("<IHHHH", MAGIC, VERSION, MSG_REFUSED, reason, MARKER)


class _LocalPeer:
    def __init__(self, peer_id, game_port, ctrl_addr):
        self.peer_id = peer_id
        self.game_port = game_port
        self.ctrl_addr = ctrl_addr
        self.data_addr = None
        self.last_seen = time.monotonic()


class CloudRelayBridge:
    """Bridges local ScctCoopNet.dll SCR1/UDP sockets to a Cloudflare Durable Object WSS relay."""

    def __init__(self, worker_base_url, room_code, bearer_token, local_port=27120, on_status_change=None):
        self.worker_base_url = worker_base_url.rstrip("/")
        self.room_code = room_code
        self.bearer_token = bearer_token
        self.local_port = pick_free_udp_pair(local_port)
        self.on_status_change = on_status_change
        self._last_counts = (0, 0)

        parsed = urllib.parse.urlparse(self.worker_base_url)
        ws_scheme = "wss" if parsed.scheme == "https" else "ws"
        self.ws_url = f"{ws_scheme}://{parsed.netloc}/v2/relay/{room_code}"

        self._lock = threading.Lock()
        self._stop_event = threading.Event()
        self._loop = None
        self._outbound_queue = None
        self._thread = None

        self._ctrl_sock = None
        self._data_sock = None
        self._peers_by_id = {}
        self._peers_by_ctrl = {}
        self._peers_by_data = {}
        self._peers_by_game_port = {}
        self._next_id = 1

        self._ice = None
        self._signal_queue = None
        self.ws_connected = False
        self.partner_connected = False
        self.transport_mode = "ws-relay"
        self._ice_sent_offer = False
        self._ice_got_remote_sdp = False
        self._pending_ice_candidates = []
        self._pending_ice_ready = False
        self.last_error = ""
        self.tx_packets = 0
        self.rx_packets = 0

    def start(self):
        if self._thread and self._thread.is_alive():
            return
        self._stop_event.clear()

        self._ctrl_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._ctrl_sock.bind(("127.0.0.1", self.local_port))
        self._ctrl_sock.setblocking(False)

        self._data_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._data_sock.bind(("127.0.0.1", self.local_port + 1))
        self._data_sock.setblocking(False)

        ready = threading.Event()
        self._thread = threading.Thread(target=self._run_thread, args=(ready,), daemon=True)
        self._thread.start()
        ready.wait(timeout=3.0)

    def stop(self):
        self._stop_event.set()
        if self._loop and self._loop.is_running():
            self._loop.call_soon_threadsafe(lambda: None)
        if self._thread:
            self._thread.join(timeout=3.0)
            self._thread = None
        for sock in (self._ctrl_sock, self._data_sock):
            if sock is not None:
                try:
                    sock.close()
                except Exception:
                    pass
        self._ctrl_sock = None
        self._data_sock = None
        self._close_ice()
        self.ws_connected = False
        self.partner_connected = False
        self.transport_mode = "ws-relay"
        self._ice_sent_offer = False
        self._ice_got_remote_sdp = False

    def wait_connected(self, timeout=5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.ws_connected:
                return True
            if self.last_error and ("401" in self.last_error or "404" in self.last_error or "409" in self.last_error):
                return False
            time.sleep(0.05)
        return self.ws_connected

    def _emit(self):
        cb = self.on_status_change
        if cb is None:
            return
        try:
            cb(self.get_status())
        except Exception:
            pass

    def get_status(self):
        return {
            "running": bool(self._thread and self._thread.is_alive() and not self._stop_event.is_set()),
            "room_code": self.room_code,
            "local_relay": f"127.0.0.1:{self.local_port}",
            "local_port": self.local_port,
            "ws_connected": self.ws_connected,
            "partner_connected": self.partner_connected,
            "transport_mode": self.transport_mode,
            "tx_packets": self.tx_packets,
            "rx_packets": self.rx_packets,
            "last_error": self.last_error,
        }

    def _run_thread(self, ready):
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        try:
            self._loop.run_until_complete(self._async_main(ready))
        finally:
            self._loop.close()

    async def _async_main(self, ready):
        self._outbound_queue = asyncio.Queue(maxsize=256)
        ready.set()
        ctrl_task = asyncio.create_task(self._udp_ctrl_loop())
        data_task = asyncio.create_task(self._udp_data_loop())
        ws_task = asyncio.create_task(self._ws_loop())
        ticks = 0
        while not self._stop_event.is_set():
            await asyncio.sleep(0.1)
            ticks += 1
            if ticks % 5 == 0:
                counts = (self.tx_packets, self.rx_packets)
                if counts != self._last_counts:
                    self._last_counts = counts
                    self._emit()
        for task in (ctrl_task, data_task, ws_task):
            task.cancel()
        await asyncio.gather(ctrl_task, data_task, ws_task, return_exceptions=True)

    async def _udp_ctrl_loop(self):
        loop = asyncio.get_running_loop()
        while not self._stop_event.is_set():
            try:
                data, addr = await loop.sock_recvfrom(self._ctrl_sock, 4096)
            except asyncio.CancelledError:
                break
            except Exception:
                await asyncio.sleep(0.02)
                continue
            self._handle_control_datagram(data, addr)

    async def _udp_data_loop(self):
        loop = asyncio.get_running_loop()
        while not self._stop_event.is_set():
            try:
                data, addr = await loop.sock_recvfrom(self._data_sock, 16384)
            except asyncio.CancelledError:
                break
            except Exception:
                await asyncio.sleep(0.02)
                continue
            with self._lock:
                peer = self._peers_by_data.get(addr)
            if peer is None or len(data) == 0 or len(data) > 8192:
                continue
            frame = struct.pack("<H", peer.game_port) + data
            ice = self._ice
            if ice is not None and ice.connected and p2p_ice is not None:
                if ice.send(bytes((p2p_ice.PKT_TYPE_GAME,)) + frame):
                    self.tx_packets += 1
                    continue
            try:
                if self._outbound_queue.full():
                    self._outbound_queue.get_nowait()
                self._outbound_queue.put_nowait(frame)
            except Exception:
                pass

    def _handle_control_datagram(self, buf, addr):
        if len(buf) < 10:
            return
        magic, ver, mtype = struct.unpack_from("<IHH", buf, 0)
        if magic != MAGIC or ver != VERSION:
            return

        if mtype == MSG_REGISTER and len(buf) == 12:
            game_port, marker = struct.unpack_from("<HH", buf, 8)
            if marker != MARKER or game_port not in GAME_PORTS:
                self._ctrl_sock.sendto(build_refused(REFUSE_UNKNOWN), addr)
                return
            with self._lock:
                peer = self._peers_by_ctrl.get(addr)
                if peer is None:
                    peer = _LocalPeer(self._next_id, game_port, addr)
                    self._next_id += 1
                    self._peers_by_id[peer.peer_id] = peer
                    self._peers_by_ctrl[addr] = peer
                    self._peers_by_game_port[game_port] = peer
                else:
                    peer.last_seen = time.monotonic()
            self._send_ok(peer)
            return

        if mtype == MSG_HEARTBEAT and len(buf) == 10:
            marker = struct.unpack_from("<H", buf, 8)[0]
            if marker != MARKER:
                return
            with self._lock:
                peer = self._peers_by_ctrl.get(addr)
            if peer is None:
                self._ctrl_sock.sendto(build_refused(REFUSE_UNKNOWN), addr)
                return
            peer.last_seen = time.monotonic()
            self._send_ok(peer)
            return

        if mtype == MSG_ATTACH and len(buf) == 14:
            peer_id, marker = struct.unpack_from("<IH", buf, 8)
            if marker != MARKER:
                return
            with self._lock:
                peer = self._peers_by_id.get(peer_id)
                if peer is None or peer.ctrl_addr[0] != addr[0]:
                    return
                if peer.data_addr and peer.data_addr != addr:
                    self._peers_by_data.pop(peer.data_addr, None)
                peer.data_addr = addr
                peer.last_seen = time.monotonic()
                self._peers_by_data[addr] = peer
            self._send_ok(peer)

    def _send_ok(self, peer):
        has_other = self.partner_connected
        pkt = build_register_ok(
            peer.peer_id,
            has_other,
            SYNTHETIC_PEER_IP_BYTES,
            40000 + peer.game_port,
        )
        try:
            self._ctrl_sock.sendto(pkt, peer.ctrl_addr)
        except Exception:
            pass

    def _notify_peers_address(self):
        with self._lock:
            peers = list(self._peers_by_id.values())
        for peer in peers:
            pkt = build_address(SYNTHETIC_PEER_IP_BYTES, 40000 + peer.game_port, peer.game_port)
            try:
                self._ctrl_sock.sendto(pkt, peer.ctrl_addr)
            except Exception:
                pass

    def _queue_signal(self, payload):
        loop = self._loop
        q = self._signal_queue
        if loop is None or q is None or self._stop_event.is_set():
            return
        msg = json.dumps(payload)

        def _put():
            try:
                if q.full():
                    q.get_nowait()
                q.put_nowait(msg)
            except Exception:
                pass

        try:
            loop.call_soon_threadsafe(_put)
        except Exception:
            pass

    def _close_ice(self):
        ice = self._ice
        self._ice = None
        self.transport_mode = "ws-relay"
        self._ice_sent_offer = False
        self._ice_got_remote_sdp = False
        self._pending_ice_candidates = []
        self._pending_ice_ready = False
        if ice is not None:
            ice.close()

    def _flush_pending_ice(self):
        if self._ice is None or not self._ice_got_remote_sdp:
            return
        for cand in self._pending_ice_candidates:
            self._ice.add_remote_candidate(cand)
        self._pending_ice_candidates.clear()
        if self._pending_ice_ready:
            self._pending_ice_ready = False
            self._ice.set_remote_gathering_done()

    def _on_ice_state(self, state):
        if p2p_ice is None:
            return
        if state in (p2p_ice.ICE_CONNECTED, p2p_ice.ICE_COMPLETED):
            self.transport_mode = "p2p-ice"
            self._emit()
        elif state == p2p_ice.ICE_FAILED:
            self.transport_mode = "ws-relay"
            self._queue_signal({"type": "ice-fallback", "reason": "ice_failed"})
            self._emit()
        elif state == p2p_ice.ICE_DISCONNECTED and self.transport_mode == "p2p-ice":
            self.transport_mode = "ws-relay"
            self._emit()

    def _on_ice_recv(self, raw):
        if len(raw) < 4:
            return
        pkt_type = raw[0]
        if p2p_ice is not None and pkt_type not in (p2p_ice.PKT_TYPE_GAME, p2p_ice.PKT_TYPE_VOICE):
            return
        game_port = struct.unpack_from("<H", raw, 1)[0]
        payload = raw[3:]
        with self._lock:
            peer = self._peers_by_game_port.get(game_port)
        if peer and peer.data_addr and self._data_sock:
            try:
                self._data_sock.sendto(payload, peer.data_addr)
                self.rx_packets += 1
            except Exception:
                pass

    async def _start_ice_session(self, send_offer=False):
        if p2p_ice is None or not p2p_ice.is_available():
            return
        if self._ice is None:
            loop = asyncio.get_running_loop()
            ice_cfg = await loop.run_in_executor(
                None,
                p2p_ice.fetch_ice_config,
                self.worker_base_url,
                self.bearer_token,
                self.room_code,
            )
            if self._stop_event.is_set():
                return
            ice = p2p_ice.IceSession(
                ice_cfg,
                on_state=self._on_ice_state,
                on_candidate=lambda sdp: self._queue_signal({"type": "ice-candidate", "candidate": sdp}),
                on_gather_done=lambda: self._queue_signal({"type": "ice-ready"}),
                on_recv=self._on_ice_recv,
            )
            if not ice.ready:
                return
            self._ice = ice
            ice.gather()
        if send_offer and self._ice is not None and not self._ice_sent_offer:
            sdp = self._ice.get_local_sdp()
            if sdp:
                self._ice_sent_offer = True
                self._queue_signal({"type": "ice-offer", "sdp": sdp})

    async def _ws_loop(self):
        headers = {"Authorization": f"Bearer {self.bearer_token}"}
        while not self._stop_event.is_set():
            try:
                async with websockets.connect(
                    self.ws_url,
                    additional_headers=headers,
                    compression=None,
                    ping_interval=20,
                    ping_timeout=20,
                    max_size=65536,
                ) as ws:
                    self._signal_queue = asyncio.Queue(maxsize=256)
                    self.ws_connected = True
                    self.last_error = ""
                    self._emit()
                    sender = asyncio.create_task(self._ws_sender(ws))
                    sig_sender = asyncio.create_task(self._ws_signal_sender(ws))
                    try:
                        async for msg in ws:
                            if self._stop_event.is_set():
                                break
                            if isinstance(msg, bytes) and len(msg) >= 3:
                                game_port = struct.unpack_from("<H", msg, 0)[0]
                                payload = msg[2:]
                                with self._lock:
                                    peer = self._peers_by_game_port.get(game_port)
                                if peer and peer.data_addr and self._data_sock:
                                    self._data_sock.sendto(payload, peer.data_addr)
                                    self.rx_packets += 1
                            elif isinstance(msg, str):
                                try:
                                    ev = json.loads(msg)
                                except Exception:
                                    continue
                                etype = ev.get("type")
                                if etype == "peer_joined":
                                    self.partner_connected = True
                                    self._notify_peers_address()
                                    self._emit()
                                    asyncio.create_task(self._start_ice_session(send_offer=True))
                                elif etype == "ice-offer":
                                    sdp = ev.get("sdp") or ""
                                    if sdp:
                                        await self._start_ice_session(send_offer=False)
                                        if self._ice is not None:
                                            self._ice.set_remote_sdp(sdp)
                                            self._ice_got_remote_sdp = True
                                            self._flush_pending_ice()
                                            if not self._ice_sent_offer:
                                                local_sdp = self._ice.get_local_sdp()
                                                if local_sdp:
                                                    self._ice_sent_offer = True
                                                    self._queue_signal({"type": "ice-answer", "sdp": local_sdp})
                                elif etype == "ice-answer":
                                    sdp = ev.get("sdp") or ""
                                    if sdp and self._ice is not None and not self._ice_got_remote_sdp:
                                        self._ice.set_remote_sdp(sdp)
                                        self._ice_got_remote_sdp = True
                                        self._flush_pending_ice()
                                elif etype == "ice-candidate":
                                    cand = ev.get("candidate") or ""
                                    if cand:
                                        if self._ice is not None and self._ice_got_remote_sdp:
                                            self._ice.add_remote_candidate(cand)
                                        else:
                                            self._pending_ice_candidates.append(cand)
                                elif etype == "ice-ready":
                                    if self._ice is not None and self._ice_got_remote_sdp:
                                        self._ice.set_remote_gathering_done()
                                    else:
                                        self._pending_ice_ready = True
                                elif etype == "ice-fallback":
                                    self._close_ice()
                                    self._emit()
                                elif etype in ("peer_left", "waiting"):
                                    self.partner_connected = False
                                    self._close_ice()
                                    self._emit()
                    finally:
                        sender.cancel()
                        sig_sender.cancel()
                        await asyncio.gather(sender, sig_sender, return_exceptions=True)
            except asyncio.CancelledError:
                break
            except Exception as exc:
                self.last_error = str(exc)
            finally:
                self._close_ice()
                self._signal_queue = None
                self.ws_connected = False
                self.partner_connected = False
                if not self._stop_event.is_set():
                    self._emit()
            if not self._stop_event.is_set():
                await asyncio.sleep(1.5)

    async def _ws_sender(self, ws):
        while not self._stop_event.is_set():
            frame = await self._outbound_queue.get()
            await ws.send(frame)
            self.tx_packets += 1

    async def _ws_signal_sender(self, ws):
        while not self._stop_event.is_set():
            msg = await self._signal_queue.get()
            await ws.send(msg)
