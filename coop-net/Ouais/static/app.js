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
  try {
    resp = await fetch(path, opts);
  } catch {
    return { status: 0, data: { ok: false, error: "Launcher backend unreachable. Please check your connection." } };
  }
  const data = await resp.json().catch(() => ({ ok: false, error: `HTTP ${resp.status}` }));
  if (resp.status === 429 && !data.retry_after) {
    const hdr = resp.headers.get("retry-after") || resp.headers.get("Retry-After");
    if (hdr) data.retry_after = Number(hdr) || 60;
  }
  if (resp.status === 401 && path !== "/api/auth/login") signedOut();
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
function authPanel(which) {
  show($("auth-main"), which === "main");
  show($("auth-verify"), which === "verify");
  show($("auth-forgot"), which === "forgot-1" || which === "forgot-2");
  if (which === "forgot-1" || which === "forgot-2") {
    show($("forgot-step-1"), which === "forgot-1");
    show($("forgot-step-2"), which === "forgot-2");
  }
}
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
function checkPasswordRules(password, elementMap) {
  const p = password || "";
  const t = {
    [elementMap.len]: p.length >= 8 && p.length <= 128,
    [elementMap.up]: /[A-Z]/.test(p),
    [elementMap.low]: /[a-z]/.test(p),
    [elementMap.dig]: /\d/.test(p),
    [elementMap.sp]: /[^A-Za-z0-9]/.test(p),
  };
  let allMet = true;
  for (const [id, ok] of Object.entries(t)) {
    const el = $(id);
    if (el) el.classList.toggle("met", ok);
    if (!ok) allMet = false;
  }
  return allMet;
}
function rules() {
  return checkPasswordRules($("f-pass").value, {
    len: "r-len",
    up: "r-up",
    low: "r-low",
    dig: "r-dig",
    sp: "r-sp",
  });
}
function resetRules() {
  return checkPasswordRules($("f-reset-pass").value, {
    len: "rr-len",
    up: "rr-up",
    low: "rr-low",
    dig: "rr-dig",
    sp: "rr-sp",
  });
}
function err(boxId, msg) { const b = $(boxId); if (b) { b.textContent = msg; show(b, true); } }

function buildOtpBox(boxId, formId) {
  const box = $(boxId);
  if (!box) return;
  box.replaceChildren();
  for (let i = 0; i < 6; i++) box.append(h("input", { type: "text", inputmode: "numeric", maxlength: "1", autocomplete: "one-time-code", "aria-label": `Digit ${i + 1}` }));
  const cells = [...box.children];
  const submitIfFull = () => { if (otpBoxValue(boxId).length === 6) $(formId).requestSubmit(); };
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
  if (cells[0]) cells[0].focus();
}
const otpBoxValue = (boxId) => [...($(boxId)?.children || [])].map((c) => c.value).join("");
const buildOtp = () => buildOtpBox("otp", "verify-form");
const otpValue = () => otpBoxValue("otp");

function startCooldown(buttonEl, noticeEl, seconds, normalLabel) {
  if (buttonEl) buttonEl.disabled = true;
  let remaining = seconds;
  const update = () => {
    const txt = `Rate limited. Try again in ${remaining}s...`;
    if (noticeEl) { noticeEl.textContent = txt; show(noticeEl, true); }
    if (buttonEl && !noticeEl) buttonEl.textContent = `Wait ${remaining}s...`;
  };
  update();
  const timer = setInterval(() => {
    remaining -= 1;
    if (remaining <= 0) {
      clearInterval(timer);
      if (buttonEl) { buttonEl.disabled = false; if (normalLabel) buttonEl.textContent = normalLabel; }
      if (noticeEl) show(noticeEl, false);
    } else {
      update();
    }
  }, 1000);
  return timer;
}

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

function showForgotStep1() {
  authPanel("forgot-1");
  show($("forgot-1-err"), false);
  show($("forgot-1-cooldown"), false);
  $("f-forgot-email").focus();
}

function showForgotStep2(email, maskedEmail, devCode) {
  S.resetEmail = email;
  authPanel("forgot-2");
  setText($("reset-target"), maskedEmail || email);
  show($("reset-err"), false);
  const note = $("reset-note");
  note.textContent = devCode ? `Dev fallback code: ${devCode}` : "";
  show(note, Boolean(note.textContent));
  $("f-reset-pass").value = "";
  $("f-reset-confirm").value = "";
  resetRules();
  buildOtpBox("otp-reset", "forgot-form-2");
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
    const p = (S.packs || []).find((x) => x.id === job.id);
    if (p) {
      if ("enabled" in job) {
        p.enabled = Boolean(job.enabled);
      }
      p.job = job;
      if (job.status === "done") { p.installed = true; p.enabled = true; p.lastError = ""; }
      if (job.status === "uninstalled") { p.installed = false; p.enabled = false; p.lastError = ""; }
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

  // Discord integration
  if (u.discord_id) {
    show($("discord-linked"), true);
    show($("discord-unlinked"), false);
    setText($("discord-username-display"), `@${u.discord_username || u.discord_id}`);
    show($("discord-unlink-err"), false);
  } else {
    show($("discord-linked"), false);
    show($("discord-unlinked"), true);
    show($("discord-err"), false);
  }
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
  const isBuiltin = p.id === "fps-cap-60" || p.type === "builtin";

  let badgeCls = "pack-badge";
  let badgeText = "Not installed";
  if (busyJob) {
    badgeText = j.message || "Installing...";
  } else if (p.installed) {
    if (p.enabled) {
      badgeCls += " ok";
      badgeText = isBuiltin ? "Active" : "Installed";
    } else {
      badgeCls += " disabled";
      badgeText = "Disabled";
    }
  } else if (isErr) {
    badgeCls += " err";
    badgeText = "Failed";
  }

  const badge = h("span", { id: `pack-badge-${p.id}`, class: badgeCls, text: badgeText });
  const badges = h("div", { class: "pack-badges" });
  if (p.experimental) {
    badges.append(h("span", { class: "pack-badge warn", text: "Experimental" }));
  }
  badges.append(badge);

  const acts = h("div", { id: `pack-acts-${p.id}`, class: "pack-actions-row" });
  const leftActs = h("div", { class: "row" });
  const rightActs = h("div", { class: "row" });

  if (p.installed || isBuiltin) {
    const toggleBtn = h("button", {
      id: `pack-toggle-${p.id}`,
      class: "switch",
      role: "switch",
      "aria-checked": String(Boolean(p.enabled)),
      "aria-label": `Enable ${p.name}`,
      disabled: busyJob,
      onclick: () => togglePack(p.id, !p.enabled),
    });
    leftActs.append(h("label", { class: "pack-toggle-row" },
      toggleBtn,
      h("span", { id: `pack-toggle-lbl-${p.id}`, class: "pack-toggle-lbl", text: p.enabled ? "Active" : "Disabled" })
    ));
  }

  if (!isBuiltin) {
    rightActs.append(h("button", {
      id: `pack-btn-${p.id}`,
      class: "btn sm " + (p.installed ? "" : "primary"),
      disabled: busyJob,
      text: busyJob ? `${j.progress || 0}%` : (p.installed ? "Reinstall" : "Install"),
      onclick: () => installPack(p.id),
    }));
    if (p.installed && !busyJob) {
      rightActs.append(h("button", { class: "btn sm ghost", text: "Remove", onclick: () => uninstallPack(p.id) }));
    }
  }

  acts.append(leftActs, rightActs);

  const errEl = (isErr && (p.lastError || j.error)) ? h("div", { id: `pack-err-${p.id}`, class: "pack-err", text: p.lastError || j.error }) : null;
  const progEl = busyJob ? h("progress", { id: `pack-prog-${p.id}`, class: "prog", max: "100", value: String(j.progress || 0) }) : null;
  const metaText = isBuiltin ? "Built into ScctCoopNet.dll" : `v${p.version} · ~${p.size_mb} MB`;
  const noteEl = isBuiltin ? h("div", { class: "pack-note", text: "Applies at next game launch. Keep both co-op players on the same setting." }) : null;

  return h("div", { id: `pack-${p.id}`, class: "card pack", dataset: { packId: p.id } },
    h("div", { class: "pack-head" }, h("h3", { text: p.name }), badges),
    h("div", { class: "pack-meta", text: metaText }),
    h("p", { class: "pack-desc", text: p.description }),
    noteEl,
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
  const isBuiltin = p.id === "fps-cap-60" || p.type === "builtin";
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
      if (p.enabled) {
        badge.className = "pack-badge ok";
        badge.textContent = isBuiltin ? "Active" : "Installed";
      } else {
        badge.className = "pack-badge disabled";
        badge.textContent = "Disabled";
      }
    } else if (j.status === "error" || p.lastError) {
      badge.className = "pack-badge err";
      badge.textContent = "Failed";
    } else {
      badge.className = "pack-badge";
      badge.textContent = "Not installed";
    }
  }

  const toggle = $(`pack-toggle-${p.id}`);
  if (toggle) {
    toggle.setAttribute("aria-checked", String(Boolean(p.enabled)));
    toggle.disabled = busyJob;
  }
  const toggleLbl = $(`pack-toggle-lbl-${p.id}`);
  if (toggleLbl) {
    toggleLbl.textContent = p.enabled ? "Active" : "Disabled";
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

async function togglePack(id, newEnabled) {
  const p = (S.packs || []).find((x) => x.id === id);
  if (!p) return;
  const oldEnabled = Boolean(p.enabled);
  p.enabled = newEnabled;
  updatePackCard(p);

  const { status, data } = await api("/api/content/toggle", "POST", { id, enabled: newEnabled });
  if (!data || !data.ok) {
    p.enabled = oldEnabled;
    updatePackCard(p);
    const msg = data && data.error ? data.error : "Could not toggle mod.";
    toast(msg, true);
    return;
  }
  if (data.note) {
    toast(data.note);
  } else {
    toast(`${p.name} ${newEnabled ? "activated" : "deactivated"}.`);
  }
  loadContent();
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
        if (!rules()) return err("auth-err", "Password must meet all complexity requirements.");
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
    const { status, data } = await api("/api/auth/resend-code", "POST", { username_or_email: S.pending });
    if (status === 429) {
      startCooldown($("btn-resend"), null, data.retry_after || 60, "Resend code");
      return;
    }
    if (!data.ok) return err("verify-err", data.error || "Couldn’t resend the code.");
    startCooldown($("btn-resend"), null, 60, "Resend code");
    showVerify(S.pending, data.email, data.delivery, data.dev_code || ""); toast("New code sent.");
  }));
  $("btn-back").addEventListener("click", () => { authPanel("main"); setMode("login"); });

  // password reset wiring
  $("link-forgot")?.addEventListener("click", (e) => { e.preventDefault(); showForgotStep1(); });
  $("btn-forgot-back-1")?.addEventListener("click", () => { authPanel("main"); setMode("login"); });
  $("btn-forgot-back-2")?.addEventListener("click", () => { authPanel("main"); setMode("login"); });

  $("forgot-form-1")?.addEventListener("submit", async (e) => {
    e.preventDefault();
    show($("forgot-1-err"), false);
    const email = $("f-forgot-email").value.trim();
    if (!email) return err("forgot-1-err", "Enter your email address.");
    await busy($("btn-forgot-send"), async () => {
      const { status, data } = await api("/api/auth/forgot-password", "POST", { email });
      if (status === 429) {
        startCooldown($("btn-forgot-send"), $("forgot-1-cooldown"), data.retry_after || 60, "Send Reset Code");
        return;
      }
      if (!data.ok) return err("forgot-1-err", data.error || "Failed to send reset code.");
      showForgotStep2(email, data.email, data.dev_code || "");
      toast("Reset code sent.");
    });
  });

  $("f-reset-pass")?.addEventListener("input", resetRules);
  $("forgot-form-2")?.addEventListener("submit", async (e) => {
    e.preventDefault();
    show($("reset-err"), false);
    const code = otpBoxValue("otp-reset");
    if (code.length !== 6) return err("reset-err", "Enter all 6 digits of the reset code.");
    const password = $("f-reset-pass").value;
    if (!resetRules()) return err("reset-err", "Password must meet all complexity requirements.");
    if (password !== $("f-reset-confirm").value) return err("reset-err", "Passwords don’t match.");
    await busy($("btn-reset-submit"), async () => {
      const { data } = await api("/api/auth/reset-password", "POST", { email: S.resetEmail, code, password });
      if (!data.ok) {
        if (data.error && data.error.includes("expired")) return err("reset-err", "Reset code expired. Request a new one.");
        if (data.error && data.error.includes("attempt")) return err("reset-err", "Too many failed attempts. Request a new code.");
        return err("reset-err", data.error || "Password reset failed.");
      }
      await afterSignIn(data);
      toast(`Password reset. Signed in as ${data.user.username}.`);
    });
  });

  $("btn-reset-resend")?.addEventListener("click", (e) => busy(e.currentTarget, async () => {
    const { status, data } = await api("/api/auth/forgot-password", "POST", { email: S.resetEmail });
    if (status === 429) {
      startCooldown($("btn-reset-resend"), null, data.retry_after || 60, "Resend code");
      return;
    }
    if (!data.ok) return err("reset-err", data.error || "Couldn’t resend reset code.");
    startCooldown($("btn-reset-resend"), null, 60, "Resend code");
    showForgotStep2(S.resetEmail, data.email, data.dev_code || "");
    toast("New reset code sent.");
  }));

  // Discord integration wiring
  let discordAuthUrl = "";
  $("btn-discord-connect")?.addEventListener("click", (e) => busy(e.currentTarget, async () => {
    show($("discord-err"), false);
    const { data } = await api("/api/auth/discord/authorize-url");
    if (!data.ok || !data.url) return err("discord-err", data.error || "Failed to retrieve Discord authorization URL.");
    discordAuthUrl = data.url;
    show($("discord-connect-flow"), true);
    setText($("discord-url-text"), discordAuthUrl);
    show($("discord-url-box"), true);
    api("/api/open-url", "POST", { url: discordAuthUrl });
    try { window.open(discordAuthUrl, "_blank"); } catch {}
    $("discord-token")?.focus();
  }));

  $("btn-discord-copy-url")?.addEventListener("click", () => {
    if (discordAuthUrl && navigator.clipboard) {
      navigator.clipboard.writeText(discordAuthUrl);
      toast("Discord URL copied to clipboard.");
    }
  });

  $("btn-discord-link")?.addEventListener("click", (e) => busy(e.currentTarget, async () => {
    show($("discord-err"), false);
    const link_token = $("discord-token").value.trim();
    if (!link_token) return err("discord-err", "Paste the 32-character link code from your browser.");
    const { data } = await api("/api/auth/discord/link", "POST", { link_token });
    if (!data.ok) return err("discord-err", data.error || "Failed to link Discord account.");
    $("discord-token").value = "";
    if (data.discord) {
      S.user.discord_id = data.discord.id;
      S.user.discord_username = data.discord.username;
    }
    renderSettings();
    toast("Discord account linked!");
  }));

  $("btn-discord-unlink")?.addEventListener("click", (e) => busy(e.currentTarget, async () => {
    if (!confirm("Are you sure you want to unlink your Discord account?")) return;
    show($("discord-unlink-err"), false);
    const { data } = await api("/api/auth/discord/unlink", "POST", {});
    if (!data.ok) return err("discord-unlink-err", data.error || "Failed to unlink Discord account.");
    S.user.discord_id = "";
    S.user.discord_username = "";
    renderSettings();
    toast("Discord account unlinked.");
  }));

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
