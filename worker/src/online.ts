import { bearer, randomHex, sha256Hex } from "./book.js";
import type { Env } from "./env.js";
import { handleTurnCredentials } from "./turn.js";

const JSON_HEADERS: Record<string, string> = {
  "content-type": "application/json; charset=utf-8",
  "cache-control": "no-store",
  "x-content-type-options": "nosniff",
  "x-frame-options": "DENY",
  "referrer-policy": "no-referrer",
};

const USERNAME_RE = /^[A-Za-z0-9_-]{3,19}$/;
const EMAIL_RE = /^[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}$/;
const ROOM_CODE_RE = /^SCCT-[A-Z0-9]{6}$/;
const SPECIAL_CHARS = "!@#$%^&*()-_=+[]{}|;:'\",.<>?/`~\\";
const ROOM_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

const PBKDF2_ITERATIONS = 100_000;
const MAX_AUTH_FAILURES = 5;
const LOCKOUT_MS = 15 * 60_000;
const VERIFY_TTL_MS = 15 * 60_000;
const VERIFY_MAX_ATTEMPTS = 5;
const RESEND_COOLDOWN_MS = 60_000;
const ROOM_TTL_MS = 5 * 60_000;

interface OnlineAccountRow {
  id: string;
  display_name: string;
  username: string;
  email: string | null;
  email_verified: number;
  pw_salt: string | null;
  password_hash: string | null;
  active_room: string | null;
  friendly_fire: number;
  last_seen: string | null;
}

function jsonRes(status: number, body: unknown, extraHeaders?: Record<string, string>): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: extraHeaders ? { ...JSON_HEADERS, ...extraHeaders } : JSON_HEADERS,
  });
}

function sameHex(left: string, right: string): boolean {
  if (left.length !== right.length) return false;
  let diff = 0;
  for (let i = 0; i < left.length; i += 1) diff |= left.charCodeAt(i) ^ right.charCodeAt(i);
  return diff === 0;
}

function bytesToHex(bytes: Uint8Array): string {
  return [...bytes].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function hexToBytes(hex: string): Uint8Array {
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i += 1) {
    out[i] = Number.parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  }
  return out;
}

async function hashPasswordPbkdf2(password: string, saltHex?: string): Promise<{ saltHex: string; hashHex: string }> {
  const saltBytes = saltHex ? hexToBytes(saltHex) : crypto.getRandomValues(new Uint8Array(16));
  const usedSaltHex = saltHex ?? bytesToHex(saltBytes);
  const key = await crypto.subtle.importKey(
    "raw",
    new TextEncoder().encode(password),
    "PBKDF2",
    false,
    ["deriveBits"],
  );
  const bits = await crypto.subtle.deriveBits(
    {
      name: "PBKDF2",
      hash: "SHA-256",
      salt: saltBytes,
      iterations: PBKDF2_ITERATIONS,
    },
    key,
    256,
  );
  return { saltHex: usedSaltHex, hashHex: bytesToHex(new Uint8Array(bits)) };
}

function validatePasswordStrength(password: string): string {
  if (typeof password !== "string" || password.length < 8 || password.length > 128) {
    return "Password must be 8-128 characters.";
  }
  if (!/[A-Z]/.test(password)) return "Password must include at least one uppercase letter.";
  if (!/[a-z]/.test(password)) return "Password must include at least one lowercase letter.";
  if (!/[0-9]/.test(password)) return "Password must include at least one number.";
  if (![...password].some((ch) => SPECIAL_CHARS.includes(ch))) {
    return "Password must include at least one special character (e.g. !@#$%&*).";
  }
  return "";
}

function generateSixDigitCode(): string {
  const arr = new Uint32Array(1);
  // Rejection sampling removes modulo bias for 0..999999.
  for (;;) {
    crypto.getRandomValues(arr);
    if (arr[0] < 4_000_000_000) {
      return String(arr[0] % 1_000_000).padStart(6, "0");
    }
  }
}

function generateRoomCode(): string {
  const bytes = crypto.getRandomValues(new Uint8Array(6));
  let suffix = "";
  for (let i = 0; i < 6; i += 1) {
    suffix += ROOM_ALPHABET[bytes[i] % ROOM_ALPHABET.length];
  }
  return `SCCT-${suffix}`;
}

function maskEmail(email: string): string {
  const at = email.indexOf("@");
  if (at <= 0) return email;
  const local = email.slice(0, at);
  const domain = email.slice(at + 1);
  if (local.length <= 2) return `${local[0]}*@${domain}`;
  return `${local[0]}${"*".repeat(local.length - 2)}${local[local.length - 1]}@${domain}`;
}

async function readJsonBody(request: Request): Promise<Record<string, unknown> | Response> {
  const rawLen = request.headers.get("content-length");
  if (rawLen && Number(rawLen) > 16_384) return jsonRes(413, { ok: false, error: "Request body too large." });
  const ctype = (request.headers.get("content-type") ?? "").toLowerCase();
  if (!ctype.startsWith("application/json")) {
    return jsonRes(400, { ok: false, error: "Content-Type application/json required." });
  }
  const buf = new Uint8Array(await request.arrayBuffer());
  if (buf.byteLength > 16_384) return jsonRes(413, { ok: false, error: "Request body too large." });
  try {
    const parsed = JSON.parse(new TextDecoder().decode(buf));
    if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) {
      return jsonRes(400, { ok: false, error: "JSON object required." });
    }
    return parsed as Record<string, unknown>;
  } catch {
    return jsonRes(400, { ok: false, error: "Malformed JSON." });
  }
}

async function checkLockout(db: D1Database, keys: string[], nowMs: number): Promise<number> {
  let maxWait = 0;
  for (const key of keys) {
    const row = await db
      .prepare("SELECT locked_until FROM auth_attempts WHERE key = ?")
      .bind(key)
      .first<{ locked_until: string }>();
    if (row?.locked_until) {
      const untilMs = Date.parse(row.locked_until);
      if (Number.isFinite(untilMs) && untilMs > nowMs) {
        maxWait = Math.max(maxWait, Math.ceil((untilMs - nowMs) / 1000));
      } else {
        await db.prepare("DELETE FROM auth_attempts WHERE key = ?").bind(key).run();
      }
    }
  }
  return maxWait;
}

async function recordAuthFailure(db: D1Database, keys: string[], nowMs: number): Promise<number> {
  let lockedSecs = 0;
  const nowIso = new Date(nowMs).toISOString();
  for (const key of keys) {
    const row = await db
      .prepare("SELECT failures FROM auth_attempts WHERE key = ?")
      .bind(key)
      .first<{ failures: number }>();
    const fails = (row?.failures ?? 0) + 1;
    const lockedUntil = fails >= MAX_AUTH_FAILURES ? new Date(nowMs + LOCKOUT_MS).toISOString() : "";
    if (lockedUntil) lockedSecs = Math.max(lockedSecs, Math.ceil(LOCKOUT_MS / 1000));
    await db
      .prepare(
        `INSERT INTO auth_attempts (key, failures, locked_until, updated_at)
         VALUES (?, ?, ?, ?)
         ON CONFLICT(key) DO UPDATE SET
           failures = excluded.failures,
           locked_until = excluded.locked_until,
           updated_at = excluded.updated_at`,
      )
      .bind(key, fails, lockedUntil, nowIso)
      .run();
  }
  return lockedSecs;
}

async function clearAuthFailures(db: D1Database, keys: string[]): Promise<void> {
  for (const key of keys) {
    await db.prepare("DELETE FROM auth_attempts WHERE key = ?").bind(key).run();
  }
}

function escapeHtml(text: string): string {
  return text.replace(/[&<>"']/g, (ch) => {
    switch (ch) {
      case "&": return "&amp;";
      case "<": return "&lt;";
      case ">": return "&gt;";
      case '"': return "&quot;";
      default: return "&#39;";
    }
  });
}

function parseFromHeader(rawFrom: string): { name: string; email: string } {
  const m = /^(.+?)\s*<([^>]+)>$/.exec(rawFrom.trim());
  if (m) return { name: m[1].trim(), email: m[2].trim() };
  return { name: "SCCT Co-op", email: rawFrom.trim() };
}

async function sendVerificationEmail(
  env: Env,
  username: string,
  email: string,
  code: string,
): Promise<{ ok: true } | { ok: false; error: string }> {
  const brevoKey = (env.BREVO_API_KEY ?? "").trim();
  const resendKey = (env.RESEND_API_KEY ?? "").trim();
  const rawFrom = (env.EMAIL_FROM ?? "").trim();
  if ((!brevoKey && !resendKey) || !rawFrom) {
    return { ok: false, error: "Email delivery is not configured." };
  }
  const subject = "SCCT Co-op Verification Code";
  const textBody = `Agent ${username},\n\nYour Echelon:Re verification code is: ${code}\n\nThis code expires in 15 minutes.`;
  const htmlBody = `<div style="font-family:sans-serif;background:#090b0d;color:#eef2f5;padding:24px;border-radius:6px">`
    + `<h2 style="margin:0 0 12px;color:#64dc4b">SCCT Co-op Verification</h2>`
    + `<p>Agent <b>${escapeHtml(username)}</b>,</p>`
    + `<p>Your verification code is:</p>`
    + `<p style="font-size:28px;font-weight:700;letter-spacing:6px;color:#64dc4b;margin:16px 0">${escapeHtml(code)}</p>`
    + `<p style="color:#98a4b0;font-size:13px">This code expires in 15 minutes.</p></div>`;

  if (brevoKey) {
    const sender = parseFromHeader(rawFrom);
    try {
      const resp = await fetch("https://api.brevo.com/v3/smtp/email", {
        method: "POST",
        headers: {
          "api-key": brevoKey,
          "content-type": "application/json",
          accept: "application/json",
        },
        body: JSON.stringify({
          sender,
          to: [{ email, name: username }],
          subject,
          textContent: textBody,
          htmlContent: htmlBody,
        }),
        signal: AbortSignal.timeout(10_000),
      });
      if (!resp.ok) {
        return { ok: false, error: "Verification email could not be sent. Please try again later." };
      }
      return { ok: true };
    } catch {
      return { ok: false, error: "Verification email could not be sent. Please try again later." };
    }
  }

  try {
    const resp = await fetch("https://api.resend.com/emails", {
      method: "POST",
      headers: {
        authorization: `Bearer ${resendKey}`,
        "content-type": "application/json",
      },
      body: JSON.stringify({
        from: rawFrom,
        to: [email],
        subject,
        text: textBody,
        html: htmlBody,
      }),
      signal: AbortSignal.timeout(10_000),
    });
    if (!resp.ok) {
      return { ok: false, error: "Verification email could not be sent. Please try again later." };
    }
    return { ok: true };
  } catch {
    return { ok: false, error: "Verification email could not be sent. Please try again later." };
  }
}

async function issueVerificationCode(
  env: Env,
  accountId: string,
  username: string,
  email: string,
  nowMs: number,
): Promise<{ ok: true } | { ok: false; error: string }> {
  const code = generateSixDigitCode();
  const codeSalt = randomHex(16);
  const codeHash = await sha256Hex(`${codeSalt}:${code}`);
  const nowIso = new Date(nowMs).toISOString();
  const expiresIso = new Date(nowMs + VERIFY_TTL_MS).toISOString();

  await env.DB.prepare("DELETE FROM email_verifications WHERE account_id = ?").bind(accountId).run();
  await env.DB.prepare(
    `INSERT INTO email_verifications (account_id, code_salt, code_hash, attempts, expires_at, created_at)
     VALUES (?, ?, ?, 0, ?, ?)`,
  )
    .bind(accountId, codeSalt, codeHash, expiresIso, nowIso)
    .run();

  return sendVerificationEmail(env, username, email, code);
}

async function authenticateBearer(request: Request, db: D1Database, nowMs: number): Promise<OnlineAccountRow | null> {
  const rawToken = bearer(request) ?? request.headers.get("x-account-token")?.trim() ?? null;
  if (!rawToken || rawToken.length < 32) return null;
  const tokenHash = await sha256Hex(rawToken);
  const row = await db
    .prepare(
      `SELECT id, display_name, username, email, email_verified, pw_salt, password_hash, active_room, friendly_fire, last_seen
       FROM accounts WHERE token_hash = ?`,
    )
    .bind(tokenHash)
    .first<OnlineAccountRow>();
  if (!row || row.email_verified !== 1) return null;
  const lastSeenMs = row.last_seen ? Date.parse(row.last_seen) : NaN;
  if (!Number.isFinite(lastSeenMs) || nowMs - lastSeenMs >= 180_000) {
    await db.prepare("UPDATE accounts SET last_seen = ? WHERE id = ?").bind(new Date(nowMs).toISOString(), row.id).run();
  }
  return row;
}

export async function handleOnlineV2(
  request: Request,
  env: Env,
  clientKey: string,
  nowMs: number,
): Promise<Response> {
  const url = new URL(request.url);
  if (request.method === "OPTIONS") return new Response(null, { status: 204, headers: JSON_HEADERS });

  // WebSocket relay route: GET /v2/relay/SCCT-XXXXXX
  const relayMatch = /^\/v2\/relay\/(SCCT-[A-Z0-9]{6})$/.exec(url.pathname);
  if (relayMatch && request.method === "GET") {
    if ((request.headers.get("upgrade") ?? "").toLowerCase() !== "websocket") {
      return jsonRes(426, { ok: false, error: "WebSocket upgrade required." });
    }
    const account = await authenticateBearer(request, env.DB, nowMs);
    if (!account) return jsonRes(401, { ok: false, error: "Authentication required." });
    const code = relayMatch[1];
    const room = await env.DB.prepare("SELECT code FROM coop_rooms WHERE code = ?").bind(code).first<{ code: string }>();
    if (!room) return jsonRes(404, { ok: false, error: "Room not found." });
    await env.DB.prepare("UPDATE coop_rooms SET updated_at = ? WHERE code = ?")
      .bind(new Date(nowMs).toISOString(), code)
      .run();

    const stub = env.LOBBY.get(env.LOBBY.idFromName(`relay:${code}`));
    const doUrl = new URL(request.url);
    doUrl.searchParams.set("uid", account.id);
    doUrl.searchParams.set("user", account.username);
    return stub.fetch(new Request(doUrl.toString(), request));
  }

  if (url.pathname === "/v2/turn/credentials" && request.method === "POST") {
    const account = await authenticateBearer(request, env.DB, nowMs);
    if (!account) return jsonRes(401, { ok: false, error: "Authentication required." });
    const turnRes = await handleTurnCredentials(account.id, env, nowMs);
    return jsonRes(turnRes.status, turnRes.body);
  }

  if (url.pathname === "/v2/content/scda-coop-maps" && request.method === "GET") {
    const account = await authenticateBearer(request, env.DB, nowMs);
    if (!account) return jsonRes(401, { ok: false, error: "Authentication required." });
    const token = (env.CONTENT_GITHUB_TOKEN ?? "").trim();
    const repo = (env.CONTENT_GITHUB_REPO ?? "").trim();
    const assetId = (env.CONTENT_SCDA_ASSET_ID ?? "").trim();
    if (!token || !repo || !assetId) {
      return jsonRes(503, { ok: false, error: "Content delivery is not configured." });
    }
    const assetUrl = `https://api.github.com/repos/${repo}/releases/assets/${assetId}`;
    const ghRes = await fetch(assetUrl, {
      method: "GET",
      headers: {
        Authorization: `Bearer ${token}`,
        Accept: "application/octet-stream",
        "User-Agent": "SCCT-Coop-Worker/1.0",
      },
      redirect: "manual",
      signal: AbortSignal.timeout(15_000),
    });
    const signedUrl = ghRes.headers.get("location");
    if (!signedUrl) {
      return jsonRes(502, { ok: false, error: "Content download is temporarily unavailable." });
    }
    return new Response(null, {
      status: 302,
      headers: {
        Location: signedUrl,
        "Cache-Control": "no-store",
      },
    });
  }

  if (url.pathname === "/v2/auth/register" && request.method === "POST") {
    return registerV2(request, env, clientKey, nowMs);
  }
  if (url.pathname === "/v2/auth/verify-email" && request.method === "POST") {
    return verifyEmailV2(request, env, clientKey, nowMs);
  }
  if (url.pathname === "/v2/auth/resend-code" && request.method === "POST") {
    return resendCodeV2(request, env, clientKey, nowMs);
  }
  if (url.pathname === "/v2/auth/login" && request.method === "POST") {
    return loginV2(request, env, clientKey, nowMs);
  }
  if (url.pathname === "/v2/auth/me" && request.method === "GET") {
    const account = await authenticateBearer(request, env.DB, nowMs);
    if (!account) return jsonRes(401, { ok: false, error: "Unauthorized." });
    return jsonRes(200, {
      ok: true,
      user: {
        id: account.id,
        username: account.username,
        email: account.email ?? "",
        email_verified: true,
        active_room: account.active_room ?? "",
        friendly_fire: Boolean(account.friendly_fire),
      },
    });
  }
  if (url.pathname === "/v2/auth/logout" && request.method === "POST") {
    const account = await authenticateBearer(request, env.DB, nowMs);
    if (!account) return jsonRes(401, { ok: false, error: "Unauthorized." });
    await env.DB.prepare("UPDATE accounts SET token_hash = ? WHERE id = ?")
      .bind(`revoked:${randomHex(28)}`, account.id)
      .run();
    return jsonRes(200, { ok: true });
  }
  if (url.pathname === "/v2/auth/account" && request.method === "DELETE") {
    const account = await authenticateBearer(request, env.DB, nowMs);
    if (!account) return jsonRes(401, { ok: false, error: "Unauthorized." });
    await env.DB.prepare("DELETE FROM friendships WHERE user_id = ? OR friend_user_id = ?")
      .bind(account.id, account.id)
      .run();
    await env.DB.prepare("DELETE FROM coop_rooms WHERE host_id = ?").bind(account.id).run();
    await env.DB.prepare("DELETE FROM email_verifications WHERE account_id = ?").bind(account.id).run();
    await env.DB.prepare("DELETE FROM host_stats WHERE account_id = ?").bind(account.id).run();
    await env.DB.prepare("DELETE FROM accounts WHERE id = ?").bind(account.id).run();
    return jsonRes(200, { ok: true });
  }

  // Friends routes
  if (url.pathname === "/v2/friends" && request.method === "GET") {
    const account = await authenticateBearer(request, env.DB, nowMs);
    if (!account) return jsonRes(401, { ok: false, error: "Unauthorized." });
    return listFriendsV2(env.DB, account.id, nowMs);
  }
  if (url.pathname === "/v2/friends/add" && request.method === "POST") {
    const account = await authenticateBearer(request, env.DB, nowMs);
    if (!account) return jsonRes(401, { ok: false, error: "Unauthorized." });
    return addFriendV2(request, env.DB, account, nowMs);
  }
  if (url.pathname === "/v2/friends/accept" && request.method === "POST") {
    const account = await authenticateBearer(request, env.DB, nowMs);
    if (!account) return jsonRes(401, { ok: false, error: "Unauthorized." });
    return acceptFriendV2(request, env.DB, account);
  }
  if (url.pathname === "/v2/friends/remove" && request.method === "POST") {
    const account = await authenticateBearer(request, env.DB, nowMs);
    if (!account) return jsonRes(401, { ok: false, error: "Unauthorized." });
    return removeFriendV2(request, env.DB, account);
  }

  // Rooms routes
  if (url.pathname === "/v2/rooms" && request.method === "GET") {
    const account = await authenticateBearer(request, env.DB, nowMs);
    if (!account) return jsonRes(401, { ok: false, error: "Unauthorized." });
    return listRoomsV2(env.DB, nowMs);
  }
  if (url.pathname === "/v2/rooms/create" && request.method === "POST") {
    const account = await authenticateBearer(request, env.DB, nowMs);
    if (!account) return jsonRes(401, { ok: false, error: "Unauthorized." });
    return createRoomV2(request, env.DB, account, nowMs);
  }
  if (url.pathname === "/v2/rooms/close" && request.method === "POST") {
    const account = await authenticateBearer(request, env.DB, nowMs);
    if (!account) return jsonRes(401, { ok: false, error: "Unauthorized." });
    await env.DB.prepare("DELETE FROM coop_rooms WHERE host_id = ?").bind(account.id).run();
    await env.DB.prepare("UPDATE accounts SET active_room = NULL WHERE id = ?").bind(account.id).run();
    return jsonRes(200, { ok: true });
  }

  return jsonRes(404, { ok: false, error: "Route not found." });
}

async function registerV2(request: Request, env: Env, clientKey: string, nowMs: number): Promise<Response> {
  const body = await readJsonBody(request);
  if (body instanceof Response) return body;

  const username = String(body.username ?? "").trim();
  const email = String(body.email ?? "").trim();
  const password = String(body.password ?? "");

  if (!USERNAME_RE.test(username)) {
    return jsonRes(400, { ok: false, error: "Username must be 3-19 chars (letters, digits, _ or -)." });
  }
  if (!email || email.length > 254 || !EMAIL_RE.test(email)) {
    return jsonRes(400, { ok: false, error: "Enter a valid email address." });
  }
  const pwErr = validatePasswordStrength(password);
  if (pwErr) return jsonRes(400, { ok: false, error: pwErr });

  const ipKey = `v2_reg:${clientKey}`;
  const wait = await checkLockout(env.DB, [ipKey], nowMs);
  if (wait > 0) return jsonRes(429, { ok: false, error: `Too many attempts. Wait ${wait}s.`, retry_after: wait });

  const dupName = await env.DB.prepare("SELECT id FROM accounts WHERE username = ? COLLATE NOCASE")
    .bind(username)
    .first<{ id: string }>();
  if (dupName) return jsonRes(409, { ok: false, error: "Username is already registered." });

  const dupEmail = await env.DB.prepare("SELECT id FROM accounts WHERE email = ? COLLATE NOCASE")
    .bind(email)
    .first<{ id: string }>();
  if (dupEmail) return jsonRes(409, { ok: false, error: "Email address is already registered." });

  const id = randomHex(8);
  const placeholderTokenHash = `unverified:${randomHex(26)}`;
  const { saltHex, hashHex } = await hashPasswordPbkdf2(password);
  const nowIso = new Date(nowMs).toISOString();

  await env.DB.prepare(
    `INSERT INTO accounts (id, display_name, token_hash, created_at, username, password_hash, email, email_verified, pw_salt, last_seen)
     VALUES (?, ?, ?, ?, ?, ?, ?, 0, ?, ?)`,
  )
    .bind(id, username, placeholderTokenHash, nowIso, username, hashHex, email, saltHex, nowIso)
    .run();

  const mailRes = await issueVerificationCode(env, id, username, email, nowMs);
  if (!mailRes.ok) {
    await env.DB.prepare("DELETE FROM accounts WHERE id = ?").bind(id).run();
    return jsonRes(502, { ok: false, error: mailRes.error });
  }

  return jsonRes(201, {
    ok: true,
    needs_verification: true,
    username,
    email: maskEmail(email),
  });
}

async function verifyEmailV2(request: Request, env: Env, clientKey: string, nowMs: number): Promise<Response> {
  const body = await readJsonBody(request);
  if (body instanceof Response) return body;

  const ident = String(body.username_or_email ?? body.username ?? body.email ?? "").trim();
  const code = String(body.code ?? "").trim();
  if (!ident || !/^\d{6}$/.test(code)) {
    return jsonRes(400, { ok: false, error: "Account and 6-digit verification code are required." });
  }

  const lockKeys = [`v2_ver_ip:${clientKey}`, `v2_ver_id:${ident.toLowerCase()}`];
  const wait = await checkLockout(env.DB, lockKeys, nowMs);
  if (wait > 0) return jsonRes(429, { ok: false, error: `Too many attempts. Wait ${wait}s.`, retry_after: wait });

  const user = await env.DB.prepare(
    `SELECT id, username, email, email_verified, active_room, friendly_fire
     FROM accounts WHERE username = ? COLLATE NOCASE OR email = ? COLLATE NOCASE`,
  )
    .bind(ident, ident)
    .first<OnlineAccountRow>();

  if (!user) {
    await recordAuthFailure(env.DB, lockKeys, nowMs);
    return jsonRes(400, { ok: false, error: "Invalid verification code." });
  }

  const vRow = await env.DB.prepare(
    `SELECT id, code_salt, code_hash, attempts, expires_at
     FROM email_verifications WHERE account_id = ? ORDER BY id DESC LIMIT 1`,
  )
    .bind(user.id)
    .first<{ id: number; code_salt: string; code_hash: string; attempts: number; expires_at: string }>();

  if (!vRow) return jsonRes(400, { ok: false, error: "No pending verification code. Request a new code." });
  if (Date.parse(vRow.expires_at) <= nowMs) {
    return jsonRes(400, { ok: false, error: "Verification code expired. Request a new code." });
  }
  if (vRow.attempts >= VERIFY_MAX_ATTEMPTS) {
    return jsonRes(429, { ok: false, error: "Too many wrong codes. Request a new verification code." });
  }

  const candidateHash = await sha256Hex(`${vRow.code_salt}:${code}`);
  if (!sameHex(candidateHash, vRow.code_hash)) {
    await env.DB.prepare("UPDATE email_verifications SET attempts = attempts + 1 WHERE id = ?").bind(vRow.id).run();
    await recordAuthFailure(env.DB, lockKeys, nowMs);
    return jsonRes(400, { ok: false, error: "Invalid verification code." });
  }

  const rawToken = randomHex(32);
  const tokenHash = await sha256Hex(rawToken);
  const nowIso = new Date(nowMs).toISOString();

  await env.DB.prepare("UPDATE accounts SET email_verified = 1, token_hash = ?, last_seen = ? WHERE id = ?")
    .bind(tokenHash, nowIso, user.id)
    .run();
  await env.DB.prepare("DELETE FROM email_verifications WHERE account_id = ?").bind(user.id).run();
  await clearAuthFailures(env.DB, lockKeys);

  return jsonRes(200, {
    ok: true,
    token: rawToken,
    user: {
      id: user.id,
      username: user.username,
      email: user.email ?? "",
      email_verified: true,
      active_room: user.active_room ?? "",
      friendly_fire: Boolean(user.friendly_fire),
    },
  });
}

async function resendCodeV2(request: Request, env: Env, clientKey: string, nowMs: number): Promise<Response> {
  const body = await readJsonBody(request);
  if (body instanceof Response) return body;

  const ident = String(body.username_or_email ?? body.username ?? body.email ?? "").trim();
  if (!ident) return jsonRes(400, { ok: false, error: "Username or email is required." });

  const ipKey = `v2_res_ip:${clientKey}`;
  const wait = await checkLockout(env.DB, [ipKey], nowMs);
  if (wait > 0) return jsonRes(429, { ok: false, error: `Wait ${wait}s.`, retry_after: wait });

  const user = await env.DB.prepare(
    `SELECT id, username, email, email_verified
     FROM accounts WHERE username = ? COLLATE NOCASE OR email = ? COLLATE NOCASE`,
  )
    .bind(ident, ident)
    .first<OnlineAccountRow>();
  if (!user || !user.email || user.email_verified === 1) {
    await recordAuthFailure(env.DB, [ipKey], nowMs);
    return jsonRes(200, {
      ok: true,
      message: "If an unverified account matches, a verification code has been sent.",
    });
  }

  const lastRow = await env.DB.prepare(
    "SELECT created_at FROM email_verifications WHERE account_id = ? ORDER BY id DESC LIMIT 1",
  )
    .bind(user.id)
    .first<{ created_at: string }>();
  if (lastRow) {
    const age = nowMs - Date.parse(lastRow.created_at);
    if (age >= 0 && age < RESEND_COOLDOWN_MS) {
      const rem = Math.ceil((RESEND_COOLDOWN_MS - age) / 1000);
      return jsonRes(429, { ok: false, error: `Wait ${rem}s before requesting another code.`, retry_after: rem });
    }
  }

  const mailRes = await issueVerificationCode(env, user.id, user.username, user.email, nowMs);
  if (!mailRes.ok) return jsonRes(502, { ok: false, error: mailRes.error });
  return jsonRes(200, {
    ok: true,
    email: maskEmail(user.email),
    message: "If an unverified account matches, a verification code has been sent.",
  });
}

async function loginV2(request: Request, env: Env, clientKey: string, nowMs: number): Promise<Response> {
  const body = await readJsonBody(request);
  if (body instanceof Response) return body;

  const ident = String(body.username ?? "").trim();
  const password = String(body.password ?? "");
  if (!ident || !password) return jsonRes(400, { ok: false, error: "Username and password are required." });

  const lockKeys = [`v2_log_ip:${clientKey}`, `v2_log_id:${ident.toLowerCase()}`];
  const wait = await checkLockout(env.DB, lockKeys, nowMs);
  if (wait > 0) return jsonRes(429, { ok: false, error: `Locked for ${wait}s due to failed logins.`, retry_after: wait });

  const row = await env.DB.prepare(
    `SELECT id, username, email, email_verified, pw_salt, password_hash, active_room, friendly_fire
     FROM accounts WHERE username = ? COLLATE NOCASE OR email = ? COLLATE NOCASE`,
  )
    .bind(ident, ident)
    .first<OnlineAccountRow>();

  if (!row || !row.password_hash || !row.pw_salt) {
    // Run constant-work PBKDF2 dummy check to reduce account existence timing leak.
    await hashPasswordPbkdf2(password, "00000000000000000000000000000000");
    const locked = await recordAuthFailure(env.DB, lockKeys, nowMs);
    if (locked > 0) return jsonRes(429, { ok: false, error: `Locked for ${locked}s.`, retry_after: locked });
    return jsonRes(401, { ok: false, error: "Invalid username or password." });
  }

  const { hashHex } = await hashPasswordPbkdf2(password, row.pw_salt);
  if (!sameHex(hashHex, row.password_hash)) {
    const locked = await recordAuthFailure(env.DB, lockKeys, nowMs);
    if (locked > 0) return jsonRes(429, { ok: false, error: `Locked for ${locked}s.`, retry_after: locked });
    return jsonRes(401, { ok: false, error: "Invalid username or password." });
  }

  await clearAuthFailures(env.DB, lockKeys);
  if (row.email_verified !== 1) {
    return jsonRes(403, {
      ok: false,
      needs_verification: true,
      username: row.username,
      email: maskEmail(row.email ?? ""),
      error: "Email verification required before signing in.",
    });
  }

  const rawToken = randomHex(32);
  const tokenHash = await sha256Hex(rawToken);
  const nowIso = new Date(nowMs).toISOString();
  await env.DB.prepare("UPDATE accounts SET token_hash = ?, last_seen = ? WHERE id = ?")
    .bind(tokenHash, nowIso, row.id)
    .run();

  return jsonRes(200, {
    ok: true,
    token: rawToken,
    user: {
      id: row.id,
      username: row.username,
      email: row.email ?? "",
      email_verified: true,
      active_room: row.active_room ?? "",
      friendly_fire: Boolean(row.friendly_fire),
    },
  });
}

async function listFriendsV2(db: D1Database, uid: string, nowMs: number): Promise<Response> {
  const { results } = await db
    .prepare(
      `SELECT f.id, f.user_id, f.friend_user_id, f.status, f.created_at,
              u1.username AS src_name, u1.active_room AS src_room, u1.last_seen AS src_seen, u1.friendly_fire AS src_ff,
              u2.username AS dst_name, u2.active_room AS dst_room, u2.last_seen AS dst_seen, u2.friendly_fire AS dst_ff
       FROM friendships f
       JOIN accounts u1 ON u1.id = f.user_id
       JOIN accounts u2 ON u2.id = f.friend_user_id
       WHERE f.user_id = ? OR f.friend_user_id = ?
       ORDER BY f.id DESC`,
    )
    .bind(uid, uid)
    .all<{
      id: number;
      user_id: string;
      friend_user_id: string;
      status: string;
      created_at: string;
      src_name: string;
      src_room: string | null;
      src_seen: string | null;
      src_ff: number;
      dst_name: string;
      dst_room: string | null;
      dst_seen: string | null;
      dst_ff: number;
    }>();

  const friends: unknown[] = [];
  const incoming: unknown[] = [];
  const outgoing: unknown[] = [];

  for (const r of results) {
    if (r.status === "pending") {
      if (r.friend_user_id === uid) {
        incoming.push({ id: r.id, username: r.src_name, created_at: r.created_at });
      } else {
        outgoing.push({ id: r.id, username: r.dst_name, created_at: r.created_at });
      }
      continue;
    }

    const isSrc = r.user_id === uid;
    const otherName = isSrc ? r.dst_name : r.src_name;
    const room = isSrc ? r.dst_room : r.src_room;
    const seen = isSrc ? r.dst_seen : r.src_seen;
    const ff = isSrc ? Boolean(r.dst_ff) : Boolean(r.src_ff);

    let status = "offline";
    let activeRoom = "";
    const seenMs = seen ? Date.parse(seen) : 0;
    const recentlyActive = Number.isFinite(seenMs) && nowMs - seenMs <= 300_000;
    if (room && recentlyActive) {
      status = "hosting";
      activeRoom = room;
    } else if (recentlyActive) {
      status = "online";
    }

    friends.push({
      id: r.id,
      username: otherName,
      status,
      active_room: activeRoom,
      friendly_fire: ff,
    });
  }

  return jsonRes(200, { ok: true, friends, incoming, outgoing });
}

async function addFriendV2(request: Request, db: D1Database, me: OnlineAccountRow, nowMs: number): Promise<Response> {
  const body = await readJsonBody(request);
  if (body instanceof Response) return body;

  const targetName = String(body.username ?? "").trim();
  if (!USERNAME_RE.test(targetName)) {
    return jsonRes(400, { ok: false, error: "Friend username must be 3-19 valid characters." });
  }
  const other = await db
    .prepare("SELECT id, username FROM accounts WHERE username = ? COLLATE NOCASE AND email_verified = 1")
    .bind(targetName)
    .first<{ id: string; username: string }>();
  if (!other) return jsonRes(404, { ok: false, error: `Agent '${targetName}' not found.` });
  if (other.id === me.id) return jsonRes(400, { ok: false, error: "You cannot add yourself." });

  const existing = await db
    .prepare(
      `SELECT id, user_id, friend_user_id, status FROM friendships
       WHERE (user_id = ? AND friend_user_id = ?) OR (user_id = ? AND friend_user_id = ?)`,
    )
    .bind(me.id, other.id, other.id, me.id)
    .first<{ id: number; user_id: string; friend_user_id: string; status: string }>();

  if (existing) {
    if (existing.status === "accepted") {
      return jsonRes(200, { ok: true, message: `${other.username} is already your friend.` });
    }
    if (existing.friend_user_id === me.id) {
      await db.prepare("UPDATE friendships SET status = 'accepted' WHERE id = ?").bind(existing.id).run();
      return jsonRes(200, { ok: true, message: `Accepted friend request from ${other.username}.` });
    }
    return jsonRes(200, { ok: true, message: `Friend request to ${other.username} is already pending.` });
  }

  await db
    .prepare("INSERT INTO friendships (user_id, friend_user_id, status, created_at) VALUES (?, ?, 'pending', ?)")
    .bind(me.id, other.id, new Date(nowMs).toISOString())
    .run();
  return jsonRes(200, { ok: true, message: `Friend request sent to ${other.username}.` });
}

async function acceptFriendV2(request: Request, db: D1Database, me: OnlineAccountRow): Promise<Response> {
  const body = await readJsonBody(request);
  if (body instanceof Response) return body;
  const id = Number(body.id ?? 0);
  if (!Number.isInteger(id) || id <= 0) return jsonRes(400, { ok: false, error: "Invalid request id." });

  const row = await db
    .prepare("SELECT id FROM friendships WHERE id = ? AND friend_user_id = ? AND status = 'pending'")
    .bind(id, me.id)
    .first<{ id: number }>();
  if (!row) return jsonRes(404, { ok: false, error: "Pending request not found." });

  await db.prepare("UPDATE friendships SET status = 'accepted' WHERE id = ?").bind(row.id).run();
  return jsonRes(200, { ok: true });
}

async function removeFriendV2(request: Request, db: D1Database, me: OnlineAccountRow): Promise<Response> {
  const body = await readJsonBody(request);
  if (body instanceof Response) return body;
  const id = Number(body.id ?? 0);
  if (!Number.isInteger(id) || id <= 0) return jsonRes(400, { ok: false, error: "Invalid friendship id." });

  await db
    .prepare("DELETE FROM friendships WHERE id = ? AND (user_id = ? OR friend_user_id = ?)")
    .bind(id, me.id, me.id)
    .run();
  return jsonRes(200, { ok: true });
}

async function listRoomsV2(db: D1Database, nowMs: number): Promise<Response> {
  const cutoffIso = new Date(nowMs - ROOM_TTL_MS).toISOString();
  await db.prepare("DELETE FROM coop_rooms WHERE updated_at < ?").bind(cutoffIso).run();
  await db
    .prepare("UPDATE accounts SET active_room = NULL WHERE active_room IS NOT NULL AND active_room NOT IN (SELECT code FROM coop_rooms)")
    .run();

  const { results } = await db
    .prepare("SELECT code, host_name, friendly_fire, created_at, updated_at FROM coop_rooms ORDER BY updated_at DESC LIMIT 50")
    .all<{ code: string; host_name: string; friendly_fire: number; created_at: string; updated_at: string }>();

  return jsonRes(200, {
    ok: true,
    rooms: results.map((r) => ({
      code: r.code,
      host_name: r.host_name,
      friendly_fire: Boolean(r.friendly_fire),
      created_at: r.created_at,
      updated_at: r.updated_at,
    })),
  });
}

async function createRoomV2(request: Request, db: D1Database, me: OnlineAccountRow, nowMs: number): Promise<Response> {
  const body = await readJsonBody(request);
  if (body instanceof Response) return body;
  const ffire = body.friendly_fire ? 1 : 0;
  const nowIso = new Date(nowMs).toISOString();

  await db.prepare("DELETE FROM coop_rooms WHERE host_id = ?").bind(me.id).run();
  let code = "";
  for (let i = 0; i < 6; i += 1) {
    const cand = generateRoomCode();
    if (!ROOM_CODE_RE.test(cand)) continue;
    const clash = await db.prepare("SELECT code FROM coop_rooms WHERE code = ?").bind(cand).first();
    if (!clash) {
      code = cand;
      break;
    }
  }
  if (!code) return jsonRes(503, { ok: false, error: "Could not allocate unique room code." });

  await db
    .prepare(
      `INSERT INTO coop_rooms (code, host_id, host_name, friendly_fire, created_at, updated_at)
       VALUES (?, ?, ?, ?, ?, ?)`,
    )
    .bind(code, me.id, me.username, ffire, nowIso, nowIso)
    .run();
  await db
    .prepare("UPDATE accounts SET active_room = ?, friendly_fire = ? WHERE id = ?")
    .bind(code, ffire, me.id)
    .run();

  return jsonRes(201, {
    ok: true,
    room: {
      code,
      host_name: me.username,
      friendly_fire: Boolean(ffire),
      created_at: nowIso,
    },
    ws_path: `/v2/relay/${code}`,
  });
}
