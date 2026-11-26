import { COOP_PLAYERS, VERSUS_PLAYERS_MAX, VERSUS_PLAYERS_MIN } from "./limits.js";

export type Mode = "coop" | "versus";

export interface CreateLobby {
  host_name: string;
  mode: Mode;
  address: string;
  port: number;
  map: string | null;
  max_players: number;
  account_id?: string | null;
}

export type ParseResult<T> = { ok: true; value: T } | { ok: false; detail: string };

const NAME = /^[A-Za-z0-9][A-Za-z0-9 ._-]{0,31}$/;
const MAP = /^[A-Za-z0-9_]{1,64}$/;
const HOSTNAME = /^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?(\.[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*$/;

const CREATE_KEYS = new Set(["host_name", "mode", "address", "port", "map", "max_players"]);

export function parseCreate(body: unknown): ParseResult<CreateLobby> {
  if (!isRecord(body)) return fail("JSON object required");
  const extra = Object.keys(body).filter((key) => !CREATE_KEYS.has(key));
  if (extra.length > 0) return fail("unknown field");

  const hostName = requiredString(body, "host_name");
  if (!hostName.ok) return hostName;
  if (!NAME.test(hostName.value) || hostName.value.length > 32) return fail("host_name");

  const mode = requiredString(body, "mode");
  if (!mode.ok) return mode;
  if (mode.value !== "coop" && mode.value !== "versus") return fail("mode");

  const address = requiredString(body, "address");
  if (!address.ok) return address;
  if (!addressAllowed(address.value)) return fail("address");

  const port = requiredInt(body, "port");
  if (!port.ok) return port;
  if (port.value < 1 || port.value > 65535) return fail("port");

  let map: string | null = null;
  if ("map" in body && body.map !== null) {
    if (typeof body.map !== "string" || !MAP.test(body.map)) return fail("map");
    map = body.map;
  }

  const maxPlayers = requiredInt(body, "max_players");
  if (!maxPlayers.ok) return maxPlayers;
  if (mode.value === "coop" && maxPlayers.value !== COOP_PLAYERS) {
    return fail("coop max_players must be 2");
  }
  if (
    mode.value === "versus" &&
    (maxPlayers.value < VERSUS_PLAYERS_MIN || maxPlayers.value > VERSUS_PLAYERS_MAX)
  ) {
    return fail("versus max_players");
  }

  return {
    ok: true,
    value: {
      host_name: hostName.value,
      mode: mode.value,
      address: address.value,
      port: port.value,
      map,
      max_players: maxPlayers.value,
    },
  };
}

export function assignAccount(lobby: CreateLobby, username: string, accountId: string): CreateLobby | null {
  if (!NAME.test(username) || username.length > 32) return null;
  if (!/^[0-9a-f]{16}$/.test(accountId)) return null;
  return { ...lobby, host_name: username, account_id: accountId };
}

const USERNAME = /^[A-Za-z][A-Za-z0-9._-]{1,14}$/;
const CREDENTIAL_KEYS = new Set(["username", "password"]);

export interface Credentials {
  username: string;
  password: string;
}

export function parseCredentials(body: unknown): ParseResult<Credentials> {
  if (!isRecord(body)) return fail("JSON object required");
  if (Object.keys(body).some((key) => !CREDENTIAL_KEYS.has(key))) return fail("unknown field");
  const username = requiredString(body, "username");
  if (!username.ok) return username;
  if (username.value.length === 0 || !USERNAME.test(username.value)) return fail("username");
  const password = requiredString(body, "password");
  if (!password.ok) return password;
  if (password.value.length < 2 || password.value.length > 64) return fail("password");
  for (let i = 0; i < password.value.length; i += 1) {
    if (password.value.charCodeAt(i) < 0x20) return fail("password");
  }
  return { ok: true, value: { username: username.value, password: password.value } };
}

export interface PeerEndpoint {
  address: string;
  port: number;
}

export function parsePeer(body: unknown): ParseResult<PeerEndpoint> {
  if (!isRecord(body)) return fail("JSON object required");
  const extra = Object.keys(body).filter((key) => key !== "address" && key !== "port");
  if (extra.length > 0) return fail("unknown field");
  const address = requiredString(body, "address");
  if (!address.ok) return address;
  if (!addressAllowed(address.value)) return fail("address");
  const port = requiredInt(body, "port");
  if (!port.ok) return port;
  if (port.value < 1 || port.value > 65535) return fail("port");
  return { ok: true, value: { address: address.value, port: port.value } };
}

export function parseModeQuery(value: string | null): ParseResult<Mode | null> {
  if (value === null || value === "") return { ok: true, value: null };
  if (value === "coop" || value === "versus") return { ok: true, value };
  return fail("mode");
}

function addressAllowed(value: string): boolean {
  if (value.length > 253 || value.includes(":") || value.includes("/") || value.includes("@")) {
    return false;
  }
  const ipv4 = parseIpv4(value);
  if (ipv4) return publicEnoughIpv4(ipv4);
  return HOSTNAME.test(value);
}

function parseIpv4(value: string): number[] | null {
  const parts = value.split(".");
  if (parts.length !== 4) return null;
  const nums: number[] = [];
  for (const part of parts) {
    if (!/^\d{1,3}$/.test(part)) return null;
    const num = Number(part);
    if (num > 255) return null;
    nums.push(num);
  }
  return nums;
}

function publicEnoughIpv4(parts: number[]): boolean {
  const [a, b] = parts;
  if (a === 0 || a === 127) return false;
  if (a === 169 && b === 254) return false;
  if (a >= 224) return false;
  return true;
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function requiredString(
  body: Record<string, unknown>,
  key: string,
): ParseResult<string> {
  const value = body[key];
  if (typeof value !== "string") return fail(key);
  return { ok: true, value };
}

function requiredInt(body: Record<string, unknown>, key: string): ParseResult<number> {
  const value = body[key];
  if (typeof value !== "number" || !Number.isInteger(value)) return fail(key);
  return { ok: true, value };
}

function fail(detail: string): ParseResult<never> {
  return { ok: false, detail };
}
