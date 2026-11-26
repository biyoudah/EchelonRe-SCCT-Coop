"use strict";
/* SCCT Co-op Launcher UI. Vanilla JS, CSP-safe (no inline styles/scripts), DOM built with textContent only. */

const $ = (id) => document.getElementById(id);
const S = {
  mode: "login", pending: "", csrf: "", user: null,
  relay: idleRelay(), game: false,
  friends: { friends: [], incoming: [], outgoing: [] }, rooms: [], packs: [],
  view: "play", busy: false, events: null, pollTimer: null,
};
function idleRelay() { return { running: false, ws_connected: false, partner_connected: false, room_code: "", tx_packets: 0, rx_packets: 0 }; }

/* ---------- helpers ---------- */
function h(tag, attrs, ...kids) {
  const n = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === false || v == null) continue;
    if (k === "class") n.className = v;
    else if (k === "text") n.textContent = v;
    else if (k.startsWith("on")) n.addEventListener(k.slice(2), v);
    else n.setAttribute(k, v === true ? "" : v);
  }
  for (const c of kids.flat()) if (c != null) n.append(c);
  return n;
}
function ico(name) {
  const s = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  s.setAttribute("class", "ico");
  const u = document.createElementNS("http://www.w3.org/2000/svg", "use");
  u.setAttribute("href", `#i-${name}`);
  s.append(u);
  return s;
}
function toast(msg, error = false) {
  const t = h("div", { class: "toast" + (error ? " error" : ""), role: error ? "alert" : "status", text: msg });
  $("toasts").append(t);
  while ($("toasts").children.length > 3) $("toasts").firstChild.remove();
  setTimeout(() => t.remove(), error ? 6000 : 3800);
}
function show(el, on) { el.classList.toggle("hidden", !on); }
function setText(el, t) { if (el.textContent !== t) el.textContent = t; }
async function busy(btn, fn) {
  if (btn.getAttribute("aria-busy") === "true") return;
  btn.setAttribute("aria-busy", "true");
  try { return await fn(); } finally { btn.removeAttribute("aria-busy"); }
}
/* Keyed list reconciliation: only touches nodes whose signature changed, so hover/focus survive refreshes. */
function syncList(box, items, key, sig, build) {
  const old = new Map([...box.children].map((c) => [c.dataset.key, c]));
  let prev = null;
  for (const it of items) {
    const k = String(key(it)), s = sig(it);
    let node = old.get(k);
    if (node && node.dataset.sig === s) old.delete(k);
    else {
      const fresh = build(it);
      fresh.dataset.key = k; fresh.dataset.sig = s;
      if (node) { node.replaceWith(fresh); old.delete(k); }
      node = fresh;
    }
    if (prev ? prev.nextSibling !== node : box.firstChild !== node) box.insertBefore(node, prev ? prev.nextSibling : box.firstChild);
    prev = node;
  }
  old.forEach((n) => n.remove());
}

async function api(path, method = "GET", body = null) {
  const headers = { "Content-Type": "application/json" };
  if (method !== "GET" && S.csrf) headers["X-CSRF-Token"] = S.csrf;
  const opts = { method, headers, credentials: "same-origin" };
  if (body !== null) opts.body = JSON.stringify(body);
  let resp;
  try { resp = await fetch(path, opts); } catch { return { status: 0, data: { ok: false, error: "Launcher backend unreachable." } }; }
  const data = await resp.json().catch(() => ({ ok: false, error: `HTTP ${resp.status}` }));
  if (resp.status === 401 && !path.startsWith("/api/auth/")) signedOut();
  return { status: resp.status, data };
}

/* ---------- auth ---------- */
function signedOut() {
  S.user = null; S.csrf = "";
  if (S.events) { S.events.close(); S.events = null; }
  clearInterval(S.pollTimer);
  show($("shell"), false); show($("auth"), true);
  authPanel("main"); setMode("login");
}
function authPanel(which) { show($("auth-main"), which === "main"); show($("auth-verify"), which === "verify"); }
function setMode(mode) {
  S.mode = mode;
  const reg = mode === "register";
  $("tab-login").setAttribute("aria-selected", String(!reg));
  $("tab-register").setAttribute("aria-selected", String(reg));
  show($("wrap-email"), reg); show($("rules"), reg); show($("wrap-confirm"), reg);
  setText($("lbl-user"), reg ? "Username (3–19 characters, your in-game name)" : "Username or email");
  $("f-pass").autocomplete = reg ? "new-password" : "current-password";
  setText($("auth-submit"), reg ? "Create account" : "Sign in");
  show($("auth-err"), false);
  if (reg) rules();
}
function rules() {
  const p = $("f-pass").value;
  const t = { "r-len": p.length >= 8, "r-up": /[A-Z]/.test(p), "r-low": /[a-z]/.test(p), "r-dig": /\d/.test(p), "r-sp": /[^A-Za-z0-9]/.test(p) };
  for (const [id, ok] of Object.entries(t)) $(id).classList.toggle("met", ok);
}
function err(boxId, msg) { const b = $(boxId); b.textContent = msg; show(b, true); }

function buildOtp() {
  const box = $("otp");
  box.replaceChildren();
  for (let i = 0; i < 6; i++) box.append(h("input", { type: "text", inputmode: "numeric", maxlength: "1", autocomplete: "one-time-code", "aria-label": `Digit ${i + 1}` }));
  const cells = [...box.children];
  const submitIfFull = () => { if (otpValue().length === 6) $("verify-form").requestSubmit(); };
  cells.forEach((c, i) => {
    c.addEventListener("input", () => {
      c.value = c.value.replace(/\D/g, "").slice(-1);
      if (c.value && cells[i + 1]) cells[i + 1].focus();
      submitIfFull();
    });
    c.addEventListener("keydown", (e) => {
      if (e.key === "Backspace" && !c.value && cells[i - 1]) { cells[i - 1].focus(); cells[i - 1].value = ""; }
      if (e.key === "ArrowLeft" && cells[i - 1]) cells[i - 1].focus();
      if (e.key === "ArrowRight" && cells[i + 1]) cells[i + 1].focus();
    });
    c.addEventListener("paste", (e) => {
      const d = (e.clipboardData.getData("text") || "").replace(/\D/g, "").slice(0, 6);
      if (!d) return;
      e.preventDefault();
      cells.forEach((x, j) => (x.value = d[j] || ""));
      (cells[Math.min(d.length, 5)]).focus();
      submitIfFull();
    });
  });
  cells[0].focus();
}
const otpValue = () => [...$("otp").children].map((c) => c.value).join("");

function showVerify(ident, email, delivery, devCode) {
  S.pending = ident;
  authPanel("verify");
  setText($("verify-target"), email || ident);
  show($("verify-err"), false);
  const note = $("verify-note");
  note.textContent = delivery === "dev_fallback" && devCode ? `Email delivery isn’t configured yet. Your code: ${devCode}` : "";
  show(note, Boolean(note.textContent));
  buildOtp();
}

async function afterSignIn(data) {
  S.csrf = data.csrf_token || "";
  await refreshAll();
  checkUpdates(true);
}

/* ---------- data refresh ---------- */
async function refreshAll() {
  const me = await api("/api/auth/me");
  if (me.status === 401 || !me.data.ok) { signedOut(); return false; }
  S.csrf = me.data.csrf_token || S.csrf;
  S.user = me.data.user;
  S.relay = me.data.relay || idleRelay();
  show($("auth"), false); show($("shell"), true);
  render();
  await refreshLists();
  await loadContent();
  await loadMics();
  connectEvents();
  clearInterval(S.pollTimer);
  S.pollTimer = setInterval(() => { if (!document.hidden) refreshLists(); }, 15000);
  return true;
}
async function refreshLists() {
  const [fr, rm, gm] = await Promise.all([api("/api/friends"), api("/api/rooms"), api("/api/game/status")]);
  if (fr.data.ok) S.friends = { friends: fr.data.friends || [], incoming: fr.data.incoming || [], outgoing: fr.data.outgoing || [] };
  if (rm.data.ok) S.rooms = rm.data.rooms || [];
  if (gm.data.ok) S.game = Boolean(gm.data.running);
  renderSocial(); renderRooms(); renderDock();
}
async function loadContent() {
  const r = await api("/api/content/catalog");
  if (r.data.ok) { S.packs = r.data.packs || []; renderContent(); }
}
async function loadMics() {
  const r = await api("/api/audio/mics");
  const sel = $("mic-device");
  if (!r.data.ok || !sel) return;
  sel.replaceChildren();
  for (const m of r.data.mics || []) {
    sel.append(h("option", { value: String(m.id), text: m.name }));
  }
  sel.value = String(r.data.selected ?? -1);
}
async function refreshUser() {
  const me = await api("/api/auth/me");
  if (me.data.ok) { S.user = me.data.user; S.relay = me.data.relay || S.relay; render(); }
}
function connectEvents() {
  if (S.events) return;
  const es = new EventSource("/api/events");
  es.addEventListener("relay", (e) => { S.relay = JSON.parse(e.data); renderDock(); renderRooms(); });
  es.addEventListener("game", (e) => { S.game = Boolean(JSON.parse(e.data).running); renderDock(); });
  es.addEventListener("friends", () => { refreshLists(); });
  es.addEventListener("mic", (e) => {
    const d = JSON.parse(e.data);
    if (S.user) S.user.mic_device = d.mic_device;
    if ($("mic-device")) $("mic-device").value = String(d.mic_device ?? -1);
  });
  es.addEventListener("content", (e) => {
    const job = JSON.parse(e.data);
    const p = S.packs.find((x) => x.id === job.id);
    if (p) {
      p.job = job;
      if (job.status === "done") { p.installed = true; p.lastError = ""; }
      if (job.status === "uninstalled") { p.installed = false; p.lastError = ""; }
      if (job.status === "error") { p.lastError = job.error || "Install failed."; }
      updatePackCard(p);
    }
    if (job.status === "done") toast(job.message || "Content installed.");
    if (job.status === "error") toast(job.error || "Download failed.", true);
  });
  es.onerror = () => { if (es.readyState === EventSource.CLOSED) { S.events = null; } };
  S.events = es;
}

/* ---------- rendering ---------- */
const connected = () => Boolean(S.relay.running && S.relay.ws_connected);
function render() { renderTitle(); renderDock(); renderSettings(); renderSocial(); renderRooms(); }

function renderTitle() {
  const u = S.user; if (!u) return;
  setText($("who-name"), u.username);
  setText($("avatar"), (u.username[0] || "A").toUpperCase());
}

function playState() {
  const u = S.user;
  if (S.game) return { label: "Stop game", sub: "In game", cls: "play stop" };
  if (!u.mod_ready) return { label: "Set up game", sub: "Link game folder", cls: "play neutral" };
  if (connected()) return { label: "Play co-op", sub: `Room ${S.relay.room_code}`, cls: "play" };
  return { label: "Host & play", sub: "Creates an online room", cls: "play" };
}
function renderDock() {
  const u = S.user; if (!u) return;
  const p = playState(), b = $("btn-play");
  b.className = p.cls;
  if (b.dataset.sig !== p.label + p.sub) { b.replaceChildren(h("span", { text: p.label }), h("small", { text: p.sub })); b.dataset.sig = p.label + p.sub; }
  const warn = !u.mod_ready;
  $("integrity").classList.toggle("warn", warn);
  setText($("integrity-text"), warn ? "Setup required" : `${u.exe_edition || "v1.05"} · mod verified`);
  show($("banner"), warn);
  $("q-voice").setAttribute("aria-checked", String(u.voice_chat));

  const box = $("dock-room"), r = S.relay;
  const mode = connected() ? "room" : "idle";
  if (box.dataset.mode !== mode) {
    box.dataset.mode = mode; box.replaceChildren();
    if (mode === "idle") {
      box.append(h("form", { class: "room", id: "join-form" },
        h("input", { id: "join-code", type: "text", maxlength: "11", placeholder: "SCCT-XXXXXX", "aria-label": "Room code", spellcheck: "false", autocomplete: "off" }),
        h("button", { class: "btn", type: "submit", id: "join-btn", text: "Join" })));
      $("join-form").addEventListener("submit", onJoinSubmit);
    } else {
      box.append(h("div", { class: "room" },
        h("button", { class: "chip", id: "chip-code", title: "Copy room code", onclick: copyCode }, h("span", { id: "chip-text" }), ico("copy")),
        h("div", { class: "link-state" }, h("b", { id: "ls-partner" }), h("span", { class: "mono", id: "ls-pkts" })),
        h("button", { class: "btn ghost icon", id: "btn-leave", "aria-label": "Close room", title: "Close room", onclick: (e) => busy(e.currentTarget, leaveRoom) }, ico("x"))));
    }
  }
  if (mode === "room") {
    setText($("chip-text"), r.room_code);
    const dot = h("span", { class: "dot" + (r.partner_connected ? " hosting" : "") });
    const lbl = r.partner_connected ? "Partner connected" : "Waiting for partner";
    const ls = $("ls-partner");
    if (ls.dataset.sig !== lbl) { ls.replaceChildren(dot, lbl); ls.dataset.sig = lbl; }
    setText($("ls-pkts"), `TX ${r.tx_packets || 0} · RX ${r.rx_packets || 0}`);
  }
}

function renderSettings() {
  const u = S.user; if (!u) return;
  const set = (id, ok, yes, no) => { const e = $(id); setText(e, ok ? yes : no); e.className = ok ? "ok" : "bad"; };
  const exe = $("exe-path");
  if (document.activeElement !== exe && !exe.dataset.dirty) exe.value = u.exe_path || "";
  show($("btn-detect"), !u.exe_path && Boolean(u.detected_exe_hint));
  set("st-exe", Boolean(u.exe_path && u.exe_md5), "Found", "Not linked");
  set("st-md5", u.exe_verified, `${u.exe_edition} · ${u.exe_md5.slice(0, 8)}`, u.exe_md5 ? "Unsupported build" : "Pending");
  set("st-dll", u.dll_installed, "Installed", "Missing");
  set("st-launch", u.launch_exe_installed, "Installed", "Missing");
  const ip = $("local-ip"); if (document.activeElement !== ip) ip.value = u.local_ip || "";
  const mic = $("mic-device"); if (mic && document.activeElement !== mic && mic.options.length) mic.value = String(u.mic_device ?? -1);
}

function row(dot, name, sub, ...acts) {
  return h("div", { class: "friend" }, h("span", { class: "dot " + dot }),
    h("div", { class: "meta" }, h("div", { class: "name", text: name }), sub ? h("div", { class: "sub", text: sub }) : null),
    h("div", { class: "acts" }, acts));
}
const joinBtn = (code) => h("button", { class: "btn sm primary", "data-join": code, text: "Join" });

function renderSocial() {
  const { friends, incoming, outgoing } = S.friends;
  setText($("req-badge"), String(incoming.length)); show($("req-badge"), incoming.length > 0);
  show($("g-in"), incoming.length > 0); show($("g-out"), outgoing.length > 0); show($("fr-empty"), friends.length === 0);
  syncList($("l-in"), incoming, (f) => f.id, (f) => f.username,
    (f) => row("online", f.username, "Wants to be friends",
      h("button", { class: "btn sm primary", "data-accept": f.id, text: "Accept" }), h("button", { class: "btn sm icon ghost", "data-remove": f.id, "aria-label": "Decline" }, ico("x"))));
  const rank = (f) => (f.active_room ? 0 : f.status === "online" ? 1 : 2);
  const sorted = [...friends].sort((a, b) => rank(a) - rank(b) || a.username.localeCompare(b.username));
  syncList($("l-fr"), sorted, (f) => f.id, (f) => [f.username, f.status, f.active_room].join("|"),
    (f) => row(f.active_room ? "hosting" : f.status === "online" ? "online" : "", f.username,
      f.active_room ? `Hosting ${f.active_room}` : f.status === "online" ? "Online" : "Offline",
      f.active_room ? joinBtn(f.active_room) : null,
      h("button", { class: "btn sm icon ghost", "data-remove": f.id, "aria-label": `Remove ${f.username}`, title: "Remove" }, ico("x"))));
  syncList($("l-out"), outgoing, (f) => f.id, (f) => f.username,
    (f) => row("", f.username, "Pending", h("button", { class: "btn sm ghost", "data-remove": f.id, text: "Cancel" })));
}

function renderRooms() {
  const rooms = S.rooms || [];
  show($("rooms-empty"), rooms.length === 0);
  const cur = connected() ? S.relay.room_code : "";
  syncList($("rooms-list"), rooms, (r) => r.code, (r) => [r.host_name, r.friendly_fire, r.code === cur].join("|"),
    (r) => h("div", { class: "friend" }, h("span", { class: "dot hosting" }),
      h("div", { class: "meta" }, h("div", { class: "name", text: r.host_name }), h("div", { class: "sub mono", text: `${r.code} · Friendly fire ${r.friendly_fire ? "on" : "off"}` })),
      r.code === cur ? h("span", { class: "sub", text: "Connected" }) : joinBtn(r.code)));
  $("btn-host").disabled = connected();
}

function renderContent() {
  const box = $("content-list");
  if (!box) return;
  box.replaceChildren();
  for (const p of S.packs || []) {
    box.append(buildPackCard(p));
  }
}

function buildPackCard(p) {
  const j = p.job || {};
  const busyJob = j.status === "downloading" || j.status === "extracting" || j.status === "patching";
  const isErr = j.status === "error" || Boolean(p.lastError);
  let badgeCls = "pack-badge";
  let badgeText = "Not installed";
  if (busyJob) {
    badgeText = j.message || "Installing...";
  } else if (p.installed) {
    badgeCls += " ok";
    badgeText = "Installed";
  } else if (isErr) {
    badgeCls += " err";
    badgeText = "Failed";
  }

  const badge = h("span", { id: `pack-badge-${p.id}`, class: badgeCls, text: badgeText });
  const acts = h("div", { id: `pack-acts-${p.id}`, class: "row" });
  acts.append(h("button", {
    id: `pack-btn-${p.id}`,
    class: "btn sm " + (p.installed ? "" : "primary"),
    disabled: busyJob,
    text: busyJob ? `${j.progress || 0}%` : (p.installed ? "Reinstall" : "Install"),
    onclick: () => installPack(p.id),
  }));
  if (p.installed && !busyJob) {
    acts.append(h("button", { class: "btn sm ghost", text: "Remove", onclick: () => uninstallPack(p.id) }));
  }

  const errEl = (isErr && (p.lastError || j.error)) ? h("div", { id: `pack-err-${p.id}`, class: "pack-err", text: p.lastError || j.error }) : null;
  const progEl = busyJob ? h("progress", { id: `pack-prog-${p.id}`, class: "prog", max: "100", value: String(j.progress || 0) }) : null;

  return h("div", { id: `pack-${p.id}`, class: "card pack", dataset: { packId: p.id } },
    h("div", { class: "pack-head" }, h("h3", { text: p.name }), badge),
    h("div", { class: "pack-meta", text: `v${p.version} · ~${p.size_mb} MB` }),
    h("p", { class: "pack-desc", text: p.description }),
    p.credits ? h("div", { class: "pack-credits", text: p.credits }) : null,
    progEl,
    errEl,
    acts);
}

function updatePackCard(p) {
  const card = $(`pack-${p.id}`);
  if (!card) return renderContent();
  const j = p.job || {};
  const busyJob = j.status === "downloading" || j.status === "extracting" || j.status === "patching";
  if (j.status === "done" || j.status === "uninstalled") {
    const fresh = buildPackCard(p);
    card.replaceWith(fresh);
    return;
  }

  const badge = $(`pack-badge-${p.id}`);
  if (badge) {
    if (busyJob) {
      badge.className = "pack-badge";
      badge.textContent = j.message || "Installing...";
    } else if (p.installed) {
      badge.className = "pack-badge ok";
      badge.textContent = "Installed";
    } else if (j.status === "error" || p.lastError) {
      badge.className = "pack-badge err";
      badge.textContent = "Failed";
    }
  }

  const btn = $(`pack-btn-${p.id}`);
  if (btn) {
    btn.disabled = busyJob;
    btn.textContent = busyJob ? `${j.progress || 0}%` : (p.installed ? "Reinstall" : "Install");
  }

  let prog = $(`pack-prog-${p.id}`);
  if (busyJob) {
    if (!prog) {
      prog = h("progress", { id: `pack-prog-${p.id}`, class: "prog", max: "100", value: String(j.progress || 0) });
      const acts = $(`pack-acts-${p.id}`);
      if (acts && acts.parentNode) acts.parentNode.insertBefore(prog, acts);
    } else {
      prog.value = String(j.progress || 0);
    }
  } else if (prog) {
    prog.remove();
  }

  let err = $(`pack-err-${p.id}`);
  const errMsg = p.lastError || (j.status === "error" ? j.error : "");
  if (errMsg && !busyJob) {
    if (!err) {
      err = h("div", { id: `pack-err-${p.id}`, class: "pack-err", text: errMsg });
      const acts = $(`pack-acts-${p.id}`);
      if (acts && acts.parentNode) acts.parentNode.insertBefore(err, acts);
    } else {
      err.textContent = errMsg;
    }
  } else if (err && !errMsg) {
    err.remove();
  }
}

async function installPack(id) {
  const p = (S.packs || []).find((x) => x.id === id);
  if (p) { p.lastError = ""; updatePackCard(p); }
  const { data } = await api("/api/content/install", "POST", { id });
  if (!data.ok) {
    if (p) { p.lastError = data.error || "Could not start download."; updatePackCard(p); }
    return toast(data.error || "Could not start download.", true);
  }
  loadContent();
}
async function uninstallPack(id) {
  const { data } = await api("/api/content/uninstall", "POST", { id });
  if (!data.ok) return toast(data.error || "Could not remove pack.", true);
  toast("Pack removed.");
  loadContent();
}

async function checkUpdates(silent = false) {
  const { data } = await api("/api/update/check");
  if (!data.ok) return;
  setText($("st-ver"), data.current_version || "2.1.0");
  const capable = data.update_capable !== false && data.installed;
  if ($("btn-check-upd")) show($("btn-check-upd"), capable);
  if (data.update_available) {
    setText($("st-upd"), `v${data.target_version} available`);
    $("st-upd").className = "ok";
    show($("btn-apply-upd"), capable);
    show($("btn-update-pill"), capable);
    setText($("btn-update-pill"), `Update v${data.target_version}`);
  } else {
    setText($("st-upd"), capable ? "Up to date" : "Dev / portable build");
    $("st-upd").className = "";
    show($("btn-apply-upd"), false);
    show($("btn-update-pill"), false);
    if (!silent && capable) toast(data.error ? `Update check: ${data.error}` : "Launcher is up to date.");
  }
}
async function applyUpdate() {
  const { data } = await api("/api/update/apply", "POST", {});
  if (!data.ok) toast(data.error || "Failed to apply update.", true);
}

/* ---------- actions ---------- */
function switchView(v) {
  S.view = v;
  document.querySelectorAll(".tab").forEach((t) => t.setAttribute("aria-selected", String(t.dataset.view === v)));
  document.querySelectorAll(".view").forEach((s) => s.classList.toggle("active", s.id === `view-${v}`));
  if (v === "content") loadContent();
}
async function copyCode() {
  const code = S.relay.room_code; if (!code) return;
  try { await navigator.clipboard.writeText(code); toast(`Room code ${code} copied.`); } catch { toast(code); }
}
async function leaveRoom() {
  await api("/api/rooms/leave", "POST", {});
  S.relay = idleRelay(); render(); refreshLists();
}
async function joinRoom(code, launch) {
  const { data } = await api("/api/rooms/join", "POST", { code, launch: launch && Boolean(S.user.mod_ready) });
  if (!data.ok) return toast(data.error || "Couldn’t join that room.", true);
  if (data.relay) S.relay = data.relay;
  toast(data.pid ? `Joined ${code}. Game launched.` : `Connected to ${code}.`);
  render();
}
async function onJoinSubmit(e) {
  e.preventDefault();
  let v = $("join-code").value.trim().toUpperCase().replace(/[^A-Z0-9-]/g, "");
  if (/^[A-Z0-9]{6}$/.test(v)) v = `SCCT-${v}`;
  if (!/^SCCT-[A-Z0-9]{6}$/.test(v)) return toast("Enter a room code like SCCT-7K4M9P.", true);
  await busy($("join-btn"), () => joinRoom(v, true));
}
async function onPlay() {
  const btn = $("btn-play");
  if (S.game) { await busy(btn, async () => { await api("/api/game/stop", "POST", {}); S.game = false; renderDock(); }); return; }
  if (!S.user.mod_ready) { switchView("settings"); toast("Link your Chaos Theory game folder to install the co-op mod.", true); return; }
  await busy(btn, async () => {
    const { data } = await api("/api/game/launch", "POST", {});
    if (!data.ok) return toast(data.error || "Launch failed.", true);
    S.game = true; if (data.relay) S.relay = data.relay; render();
    toast(`Launching in ${data.relay.room_code}…`);
    refreshLists();
  });
}
async function saveSettings(patch) {
  const u = S.user;
  const body = { friendly_fire: u.friendly_fire, voice_chat: u.voice_chat, local_ip: u.local_ip, ...patch };
  Object.assign(S.user, patch); render();
  const { data } = await api("/api/setup/settings", "POST", body);
  if (!data.ok) { toast(data.error || "Couldn’t save settings.", true); await refreshUser(); return false; }
  return true;
}
function flip(key) { return () => saveSettings({ [key]: !S.user[key] }); }

/* ---------- wiring ---------- */
document.addEventListener("DOMContentLoaded", () => {
  const needsApi = () => window.pywebview && window.pywebview.api;
  const native = () => { if (needsApi()) document.body.classList.add("native"); };
  window.addEventListener("pywebviewready", native); native();
  document.addEventListener("click", (e) => {
    const w = e.target.closest("[data-win]");
    if (w && needsApi()) window.pywebview.api[w.dataset.win]();
  });

  document.querySelectorAll("[data-mode]").forEach((b) => b.addEventListener("click", () => setMode(b.dataset.mode)));
  $("f-pass").addEventListener("input", () => { if (S.mode === "register") rules(); });

  $("auth-form").addEventListener("submit", async (e) => {
    e.preventDefault(); show($("auth-err"), false);
    const username = $("f-user").value.trim(), password = $("f-pass").value;
    if (!username || !password) return err("auth-err", "Enter your username and password.");
    await busy($("auth-submit"), async () => {
      if (S.mode === "register") {
        if (password !== $("f-confirm").value) return err("auth-err", "Passwords don’t match.");
        const { data } = await api("/api/auth/register", "POST", { username, email: $("f-email").value.trim(), password });
        if (!data.ok) return err("auth-err", data.error || "Registration failed.");
        return showVerify(username, data.email, data.delivery, data.dev_code || "");
      }
      const { data } = await api("/api/auth/login", "POST", { username, password });
      if (!data.ok) {
        if (data.needs_verification) return showVerify(data.username || username, data.email || "", data.delivery || "", data.dev_code || "");
        return err("auth-err", data.error || "Sign-in failed.");
      }
      $("f-pass").value = "";
      await afterSignIn(data);
      toast(`Signed in as ${data.user.username}.`);
    });
  });
  $("verify-form").addEventListener("submit", async (e) => {
    e.preventDefault(); show($("verify-err"), false);
    const code = otpValue();
    if (code.length !== 6) return err("verify-err", "Enter all 6 digits.");
    await busy($("verify-submit"), async () => {
      const { data } = await api("/api/auth/verify-email", "POST", { username_or_email: S.pending, code });
      if (!data.ok) return err("verify-err", data.error || "Verification failed.");
      await afterSignIn(data);
      toast(`Welcome, ${data.user.username}.`);
    });
  });
  $("btn-resend").addEventListener("click", (e) => busy(e.currentTarget, async () => {
    const { data } = await api("/api/auth/resend-code", "POST", { username_or_email: S.pending });
    if (!data.ok) return err("verify-err", data.error || "Couldn’t resend the code.");
    showVerify(S.pending, data.email, data.delivery, data.dev_code || ""); toast("New code sent.");
  }));
  $("btn-back").addEventListener("click", () => { authPanel("main"); setMode("login"); });

  document.querySelectorAll("[data-view]").forEach((b) => b.addEventListener("click", () => switchView(b.dataset.view)));
  $("integrity").addEventListener("click", () => switchView("settings"));
  $("btn-social").addEventListener("click", (e) => {
    const on = e.currentTarget.getAttribute("aria-pressed") !== "true";
    e.currentTarget.setAttribute("aria-pressed", String(on)); show($("social"), on);
  });
  $("btn-logout").addEventListener("click", async () => { await api("/api/auth/logout", "POST", {}); signedOut(); });

  $("btn-play").addEventListener("click", onPlay);
  $("q-voice").addEventListener("click", flip("voice_chat"));
  $("btn-check-upd").addEventListener("click", (e) => busy(e.currentTarget, () => checkUpdates(false)));
  $("btn-apply-upd").addEventListener("click", (e) => busy(e.currentTarget, applyUpdate));
  $("btn-update-pill").addEventListener("click", (e) => busy(e.currentTarget, applyUpdate));
  $("btn-save-ip").addEventListener("click", (e) => busy(e.currentTarget, async () => {
    if (await saveSettings({ local_ip: $("local-ip").value.trim() })) toast("Saved.");
  }));
  $("mic-device").addEventListener("change", async (e) => {
    const mic_device = Number(e.target.value);
    if (await saveSettings({ mic_device })) toast("Microphone updated.");
  });

  $("exe-path").addEventListener("input", (e) => { e.currentTarget.dataset.dirty = "1"; });
  $("btn-browse").addEventListener("click", (e) => busy(e.currentTarget, async () => {
    const { data } = await api("/api/setup/browse", "POST", {});
    if (!data.ok) return toast(data.error || "Couldn’t open the file dialog.", true);
    if (data.path) { $("exe-path").value = data.path; $("exe-path").dataset.dirty = "1"; }
  }));
  $("btn-detect").addEventListener("click", () => { $("exe-path").value = S.user.detected_exe_hint; $("exe-path").dataset.dirty = "1"; });
  $("btn-link").addEventListener("click", (e) => busy(e.currentTarget, async () => {
    const { data } = await api("/api/setup/link", "POST", { exe_path: $("exe-path").value.trim() });
    if (!data.ok) return toast(data.error || "Couldn’t verify that executable.", true);
    delete $("exe-path").dataset.dirty;
    await refreshUser();
    const n = (data.setup.installed_now || []).length;
    toast(n ? `Verified. Installed ${data.setup.installed_now.join(", ")}.` : "Verified. Mod is up to date.");
  }));

  $("btn-host").addEventListener("click", (e) => busy(e.currentTarget, async () => {
    const { data } = await api("/api/rooms/host", "POST", {});
    if (!data.ok) return toast(data.error || "Couldn’t host a room.", true);
    S.relay = data.relay; render(); toast(`Room ${data.room.code} is open.`); refreshLists();
  }));

  // delegated list actions (rooms + friends)
  document.addEventListener("click", async (e) => {
    const b = e.target.closest("button[data-join],button[data-accept],button[data-remove]");
    if (!b) return;
    if (b.dataset.join) return busy(b, () => joinRoom(b.dataset.join, true));
    const isAccept = Boolean(b.dataset.accept);
    await busy(b, async () => {
      await api(isAccept ? "/api/friends/accept" : "/api/friends/remove", "POST", { id: Number(b.dataset.accept || b.dataset.remove) });
      await refreshLists();
    });
  });
  $("friend-form").addEventListener("submit", async (e) => {
    e.preventDefault();
    const { data } = await api("/api/friends/add", "POST", { username: $("friend-name").value.trim() });
    if (!data.ok) return toast(data.error || "Couldn’t add that friend.", true);
    $("friend-name").value = ""; toast(data.message || "Request sent."); refreshLists();
  });

  window.addEventListener("focus", () => { if (S.user) refreshLists(); });
  refreshAll().then((ok) => { if (!ok) $("f-user").focus(); });
});
