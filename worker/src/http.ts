import type { Account, AccountStore } from "./accounts.js";
import { bearer, LobbyBook, randomHex, sha256Hex, type PublicLobby } from "./book.js";
import { HEARTBEAT_SECONDS, MAX_BODY_BYTES, RateGate } from "./limits.js";
import { assignAccount, parseCreate, parseCredentials, parseModeQuery, parsePeer } from "./validate.js";

const JSON_HEADERS = {
  "content-type": "application/json; charset=utf-8",
  "cache-control": "no-store",
  "x-content-type-options": "nosniff",
  "x-frame-options": "DENY",
  "referrer-policy": "no-referrer",
};

export interface LobbyDeps {
  book: LobbyBook;
  /** Logged-in directory calls. Keyed by account id, 8 per minute. */
  accountRate: RateGate;
  /** Calls with no account token. Keyed by public address, smaller cap. */
  anonRate: RateGate;
  now: () => number;
  emulatorEnabled: boolean;
  clientKey: string;
}

export interface EdgeDeps {
  accounts: AccountStore;
  rate: RateGate;
  now: () => number;
  emulatorEnabled: boolean;
  clientKey: string;
  forwardLobby: (request: Request) => Promise<Response>;
}

export function handleLobby(request: Request, deps: LobbyDeps): Promise<Response> {
  const url = new URL(request.url);
  if (request.method === "OPTIONS") return Promise.resolve(empty(204));
  if (url.pathname === "/v1/health") return Promise.resolve(health(deps.emulatorEnabled));
  if (!allowDirectory(request, deps)) return Promise.resolve(problem(429, "rate_limited", "slow down"));

  if (url.pathname === "/v1/lobbies" && request.method === "GET") return listLobbies(url, deps);
  if (url.pathname === "/v1/lobbies" && request.method === "POST") return createLobby(request, deps);
  const heartbeat = /^\/v1\/lobbies\/([0-9a-f]{16})\/heartbeat$/.exec(url.pathname);
  if (heartbeat && request.method === "POST") return beat(request, deps, heartbeat[1]);
  const peer = /^\/v1\/lobbies\/([0-9a-f]{16})\/peer$/.exec(url.pathname);
  if (peer && request.method === "POST") return setPeer(request, deps, peer[1]);
  const lobby = /^\/v1\/lobbies\/([0-9a-f]{16})$/.exec(url.pathname);
  if (lobby && request.method === "DELETE") return removeLobby(request, deps, lobby[1]);
  if (url.pathname.startsWith("/v1/lobbies")) return Promise.resolve(problem(405, "method_not_allowed", "method"));
  return Promise.resolve(problem(404, "not_found", "route"));
}

export async function handleEdge(request: Request, deps: EdgeDeps): Promise<Response> {
  const url = new URL(request.url);
  if (url.pathname === "/v1/health" && request.method === "GET") return health(deps.emulatorEnabled);
  return problem(404, "not_found", "route");
}

function allowDirectory(request: Request, deps: LobbyDeps): boolean {
  const accountId = request.headers.get("x-scct-account-id");
  if (accountId) return deps.accountRate.allow(accountId, deps.now());
  return deps.anonRate.allow(deps.clientKey, deps.now());
}

export function withClientKey(
  request: Request,
  clientKey: string,
  account: { id: string; username: string } | null = null,
): Request {
  const headers = new Headers(request.headers);
  headers.set("x-scct-client", clientKey.slice(0, 64));
  headers.delete("x-account-token");
  headers.delete("x-scct-account-id");
  headers.delete("x-scct-account-name");
  if (account) {
    headers.set("x-scct-account-id", account.id);
    headers.set("x-scct-account-name", account.username);
  }
  const method = request.method;
  const init: RequestInit & { duplex?: "half" } = { method, headers };
  if (method !== "GET" && method !== "HEAD" && request.body) {
    init.body = request.body;
    init.duplex = "half";
  }
  return new Request(request.url, init);
}

function health(emulatorEnabled: boolean): Response {
  if (emulatorEnabled) {
    return json(200, { ok: true, emulator: "blocked", reason: "header unknown" });
  }
  return json(200, { ok: true, emulator: "off" });
}

async function listLobbies(url: URL, deps: LobbyDeps): Promise<Response> {
  if ([...url.searchParams.keys()].some((key) => key !== "mode")) {
    return problem(400, "bad_request", "query");
  }
  const mode = parseModeQuery(url.searchParams.get("mode"));
  if (!mode.ok) return problem(400, "bad_request", mode.detail);
  const lobbies = deps.book.list(mode.value, deps.now());
  return json(200, { lobbies, heartbeat_seconds: HEARTBEAT_SECONDS, ttl_seconds: 60 });
}

async function createLobby(request: Request, deps: LobbyDeps): Promise<Response> {
  const body = await readJson(request);
  if (!body.ok) return problem(body.status, "bad_request", body.detail);
  const parsed = parseCreate(body.value);
  if (!parsed.ok) return problem(400, "bad_request", parsed.detail);
  const accountName = request.headers.get("x-scct-account-name");
  const accountId = request.headers.get("x-scct-account-id");
  let input = parsed.value;
  if (accountName || accountId) {
    if (!accountName || !accountId) return problem(400, "bad_request", "account");
    const stamped = assignAccount(input, accountName, accountId);
    if (!stamped) return problem(400, "bad_request", "account");
    input = stamped;
  }
  const id = randomHex(8);
  const token = randomHex(32);
  const tokenHash = await sha256Hex(token);
  const created = deps.book.create(input, id, tokenHash, deps.now());
  if (created === "full") return problem(503, "full", "lobby list is full");
  return json(201, { lobby: created, token, heartbeat_seconds: HEARTBEAT_SECONDS });
}

async function setPeer(request: Request, deps: LobbyDeps, id: string): Promise<Response> {
  const body = await readJson(request);
  if (!body.ok) return problem(body.status, "bad_request", body.detail);
  const parsed = parsePeer(body.value);
  if (!parsed.ok) return problem(400, "bad_request", parsed.detail);
  const updated = deps.book.setPeer(id, parsed.value.address, parsed.value.port, deps.now());
  if (updated === "missing") return problem(404, "not_found", "lobby");
  if (updated === "full" || updated === "forbidden") return problem(404, "not_found", "lobby");
  return json(200, { lobby: updated });
}

async function beat(request: Request, deps: LobbyDeps, id: string): Promise<Response> {
  const token = bearer(request);
  if (!token) return problem(401, "unauthorized", "lobby token");
  if (request.headers.get("content-length") && request.headers.get("content-length") !== "0") {
    return problem(400, "bad_request", "heartbeat body");
  }
  const tokenHash = await sha256Hex(token);
  const accountId = request.headers.get("x-scct-account-id");
  const result = deps.book.heartbeat(id, tokenHash, deps.now(), accountId);
  if (result === "missing") return problem(404, "not_found", "lobby");
  if (result === "forbidden") return problem(403, "forbidden", "lobby token");
  return json(200, { lobby: result, heartbeat_seconds: HEARTBEAT_SECONDS });
}

async function removeLobby(request: Request, deps: LobbyDeps, id: string): Promise<Response> {
  const token = bearer(request);
  if (!token) return problem(401, "unauthorized", "lobby token");
  const tokenHash = await sha256Hex(token);
  const result = deps.book.delete(id, tokenHash);
  if (result === "missing") return problem(404, "not_found", "lobby");
  if (result === "forbidden") return problem(403, "forbidden", "lobby token");
  return empty(204);
}

async function readJson(
  request: Request,
): Promise<{ ok: true; value: unknown } | { ok: false; status: number; detail: string }> {
  const declared = request.headers.get("content-length");
  if (declared && Number(declared) > MAX_BODY_BYTES) {
    return { ok: false, status: 413, detail: "body too large" };
  }
  const type = request.headers.get("content-type") ?? "";
  if (!type.toLowerCase().startsWith("application/json")) {
    return { ok: false, status: 400, detail: "content-type" };
  }
  const bytes = new Uint8Array(await request.arrayBuffer());
  if (bytes.byteLength > MAX_BODY_BYTES) return { ok: false, status: 413, detail: "body too large" };
  try {
    return { ok: true, value: JSON.parse(new TextDecoder().decode(bytes)) };
  } catch {
    return { ok: false, status: 400, detail: "json" };
  }
}

function problem(status: number, error: string, detail: string): Response {
  const headers = new Headers(JSON_HEADERS);
  if (status === 429) headers.set("retry-after", "60");
  return json(status, { error, detail }, headers);
}

function json(status: number, body: unknown, headers: HeadersInit = JSON_HEADERS): Response {
  return new Response(JSON.stringify(body), { status, headers });
}

function empty(status: number): Response {
  return new Response(null, { status, headers: JSON_HEADERS });
}

export type { PublicLobby };
