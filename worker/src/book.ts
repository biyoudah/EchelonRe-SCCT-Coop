import { MAX_LOBBIES, TTL_MS } from "./limits.js";
import type { LobbyRecord, LobbyStore } from "./store.js";
import type { CreateLobby } from "./validate.js";

export interface PublicLobby {
  id: string;
  host_name: string;
  mode: LobbyRecord["mode"];
  address: string;
  port: number;
  map: string | null;
  max_players: number;
  peer_address: string | null;
  peer_port: number | null;
  last_seen: number;
}

export type BookError = "full" | "missing" | "forbidden";

export class LobbyBook {
  constructor(
    private readonly store: LobbyStore,
    private readonly ttlMs = TTL_MS,
    private readonly maxLobbies = MAX_LOBBIES,
  ) {}

  create(input: CreateLobby, id: string, tokenHash: string, now: number): BookError | PublicLobby {
    this.store.expire(now, this.ttlMs);
    if (this.store.countActive(now, this.ttlMs) >= this.maxLobbies) return "full";
    const row: LobbyRecord = {
      id,
      host_name: input.host_name,
      mode: input.mode,
      address: input.address,
      port: input.port,
      map: input.map,
      max_players: input.max_players,
      peer_address: null,
      peer_port: null,
      token_hash: tokenHash,
      created_at: now,
      last_seen: now,
      account_id: input.account_id ?? null,
    };
    this.store.insert(row);
    return toPublic(row);
  }

  heartbeat(id: string, tokenHash: string, now: number, accountId: string | null = null): BookError | PublicLobby {
    const result = this.store.touch(id, tokenHash, now, accountId);
    if (result !== "ok") return result;
    const row = this.store.list(null, now, this.ttlMs).find((item) => item.id === id);
    if (!row) return "missing";
    return toPublic(row);
  }

  setPeer(id: string, address: string, port: number, now: number): BookError | PublicLobby {
    this.store.expire(now, this.ttlMs);
    const result = this.store.setPeer(id, address, port, now);
    if (result !== "ok") return result;
    const row = this.store.list(null, now, this.ttlMs).find((item) => item.id === id);
    if (!row) return "missing";
    return toPublic(row);
  }

  delete(id: string, tokenHash: string): BookError | "ok" {
    const result = this.store.remove(id, tokenHash);
    return result === "ok" ? "ok" : result;
  }

  list(mode: LobbyRecord["mode"] | null, now: number): PublicLobby[] {
    this.store.expire(now, this.ttlMs);
    return this.store.list(mode, now, this.ttlMs).map(toPublic);
  }

  expire(now: number): number {
    return this.store.expire(now, this.ttlMs);
  }
}

export function toPublic(row: LobbyRecord): PublicLobby {
  return {
    id: row.id,
    host_name: row.host_name,
    mode: row.mode,
    address: row.address,
    port: row.port,
    map: row.map,
    max_players: row.max_players,
    peer_address: row.peer_address,
    peer_port: row.peer_port,
    last_seen: row.last_seen,
  };
}

export async function sha256Hex(text: string): Promise<string> {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
  return [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
}

export function randomHex(bytes: number): string {
  const buffer = new Uint8Array(bytes);
  crypto.getRandomValues(buffer);
  return [...buffer].map((byte) => byte.toString(16).padStart(2, "0")).join("");
}

export function bearer(request: Request): string | null {
  const header = request.headers.get("Authorization");
  if (!header) return null;
  const match = /^Bearer ([A-Za-z0-9_-]{16,128})$/.exec(header);
  return match ? match[1] : null;
}
