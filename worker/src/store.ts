import type { Mode } from "./validate.js";

export interface LobbyRecord {
  id: string;
  host_name: string;
  mode: Mode;
  address: string;
  port: number;
  map: string | null;
  max_players: number;
  peer_address: string | null;
  peer_port: number | null;
  token_hash: string;
  created_at: number;
  last_seen: number;
  account_id: string | null;
}

export type TokenResult = "ok" | "missing" | "forbidden";

export interface LobbyStore {
  insert(row: LobbyRecord): void;
  setPeer(id: string, address: string, port: number, now: number): "ok" | "missing";
  touch(id: string, tokenHash: string, now: number, accountId: string | null): TokenResult;
  remove(id: string, tokenHash: string): TokenResult;
  list(mode: Mode | null, now: number, ttlMs: number): LobbyRecord[];
  expire(now: number, ttlMs: number): number;
  countActive(now: number, ttlMs: number): number;
}

export interface SqlCursor {
  toArray(): Array<Record<string, string | number | null>>;
}

export interface SqlDatabase {
  exec(query: string, ...bindings: unknown[]): SqlCursor;
}

export class MemoryLobbyStore implements LobbyStore {
  private readonly rows = new Map<string, LobbyRecord>();

  insert(row: LobbyRecord): void {
    this.rows.set(row.id, {
      ...row,
      peer_address: row.peer_address ?? null,
      peer_port: row.peer_port ?? null,
      account_id: row.account_id ?? null,
    });
  }

  setPeer(id: string, address: string, port: number, now: number): "ok" | "missing" {
    const row = this.rows.get(id);
    if (!row) return "missing";
    row.peer_address = address;
    row.peer_port = port;
    row.last_seen = now;
    return "ok";
  }

  touch(id: string, tokenHash: string, now: number, accountId: string | null): TokenResult {
    const row = this.rows.get(id);
    if (!row) return "missing";
    if (!sameHash(row.token_hash, tokenHash)) return "forbidden";
    if (row.account_id && row.account_id !== (accountId ?? "")) return "forbidden";
    row.last_seen = now;
    return "ok";
  }

  remove(id: string, tokenHash: string): TokenResult {
    const row = this.rows.get(id);
    if (!row) return "missing";
    if (!sameHash(row.token_hash, tokenHash)) return "forbidden";
    this.rows.delete(id);
    return "ok";
  }

  list(mode: Mode | null, now: number, ttlMs: number): LobbyRecord[] {
    return [...this.rows.values()].filter(
      (row) => (mode === null || row.mode === mode) && now - row.last_seen <= ttlMs,
    );
  }

  expire(now: number, ttlMs: number): number {
    let removed = 0;
    for (const [id, row] of this.rows) {
      if (now - row.last_seen > ttlMs) {
        this.rows.delete(id);
        removed += 1;
      }
    }
    return removed;
  }

  countActive(now: number, ttlMs: number): number {
    return this.list(null, now, ttlMs).length;
  }
}

export class SqliteLobbyStore implements LobbyStore {
  constructor(private readonly sql: SqlDatabase) {}

  migrate(): void {
    this.sql.exec(`
      CREATE TABLE IF NOT EXISTS lobbies (
        id TEXT PRIMARY KEY,
        host_name TEXT NOT NULL,
        mode TEXT NOT NULL,
        address TEXT NOT NULL,
        port INTEGER NOT NULL,
        map TEXT,
        max_players INTEGER NOT NULL,
        peer_address TEXT,
        peer_port INTEGER,
        token_hash TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        last_seen INTEGER NOT NULL,
        account_id TEXT
      )
    `);
    for (const statement of [
      `ALTER TABLE lobbies ADD COLUMN peer_address TEXT`,
      `ALTER TABLE lobbies ADD COLUMN peer_port INTEGER`,
      `ALTER TABLE lobbies ADD COLUMN account_id TEXT`,
    ]) {
      try {
        this.sql.exec(statement);
      } catch {
        // The column is already present on databases created after this field existed.
      }
    }
  }

  insert(row: LobbyRecord): void {
    this.sql.exec(
      `INSERT INTO lobbies (
        id, host_name, mode, address, port, map, max_players, peer_address, peer_port, token_hash, created_at, last_seen, account_id
      ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
      row.id,
      row.host_name,
      row.mode,
      row.address,
      row.port,
      row.map,
      row.max_players,
      row.peer_address,
      row.peer_port,
      row.token_hash,
      row.created_at,
      row.last_seen,
      row.account_id,
    );
  }

  setPeer(id: string, address: string, port: number, now: number): "ok" | "missing" {
    const rows = this.sql.exec(`SELECT id FROM lobbies WHERE id = ?`, id).toArray();
    if (rows.length === 0) return "missing";
    this.sql.exec(
      `UPDATE lobbies SET peer_address = ?, peer_port = ?, last_seen = ? WHERE id = ?`,
      address,
      port,
      now,
      id,
    );
    return "ok";
  }

  touch(id: string, tokenHash: string, now: number, accountId: string | null): TokenResult {
    const gate = this.tokenGate(id, tokenHash);
    if (gate !== "ok") return gate;
    const rows = this.sql.exec(`SELECT account_id FROM lobbies WHERE id = ?`, id).toArray();
    const stored = rows[0]?.account_id == null ? "" : String(rows[0].account_id);
    if (stored && stored !== (accountId ?? "")) return "forbidden";
    this.sql.exec(`UPDATE lobbies SET last_seen = ? WHERE id = ?`, now, id);
    return "ok";
  }

  remove(id: string, tokenHash: string): TokenResult {
    const gate = this.tokenGate(id, tokenHash);
    if (gate !== "ok") return gate;
    this.sql.exec(`DELETE FROM lobbies WHERE id = ?`, id);
    return "ok";
  }

  list(mode: Mode | null, now: number, ttlMs: number): LobbyRecord[] {
    const cutoff = now - ttlMs;
    const cursor =
      mode === null
        ? this.sql.exec(
            `SELECT id, host_name, mode, address, port, map, max_players, peer_address, peer_port, token_hash, created_at, last_seen, account_id
             FROM lobbies WHERE last_seen >= ?`,
            cutoff,
          )
        : this.sql.exec(
            `SELECT id, host_name, mode, address, port, map, max_players, peer_address, peer_port, token_hash, created_at, last_seen, account_id
             FROM lobbies WHERE last_seen >= ? AND mode = ?`,
            cutoff,
            mode,
          );
    return cursor.toArray().map(readRow);
  }

  expire(now: number, ttlMs: number): number {
    const doomed = this.sql
      .exec(`SELECT id FROM lobbies WHERE last_seen < ?`, now - ttlMs)
      .toArray();
    this.sql.exec(`DELETE FROM lobbies WHERE last_seen < ?`, now - ttlMs);
    return doomed.length;
  }

  countActive(now: number, ttlMs: number): number {
    const rows = this.sql
      .exec(`SELECT COUNT(*) AS n FROM lobbies WHERE last_seen >= ?`, now - ttlMs)
      .toArray();
    return Number(rows[0]?.n ?? 0);
  }

  private tokenGate(id: string, tokenHash: string): TokenResult {
    const rows = this.sql.exec(`SELECT token_hash FROM lobbies WHERE id = ?`, id).toArray();
    if (rows.length === 0) return "missing";
    const stored = String(rows[0].token_hash ?? "");
    return sameHash(stored, tokenHash) ? "ok" : "forbidden";
  }
}

function readRow(row: Record<string, string | number | null>): LobbyRecord {
  const mode = row.mode === "versus" ? "versus" : "coop";
  return {
    id: String(row.id),
    host_name: String(row.host_name),
    mode,
    address: String(row.address),
    port: Number(row.port),
    map: row.map === null || row.map === undefined ? null : String(row.map),
    max_players: Number(row.max_players),
    peer_address: row.peer_address === null || row.peer_address === undefined ? null : String(row.peer_address),
    peer_port: row.peer_port === null || row.peer_port === undefined ? null : Number(row.peer_port),
    token_hash: String(row.token_hash),
    created_at: Number(row.created_at),
    last_seen: Number(row.last_seen),
    account_id: row.account_id == null ? null : String(row.account_id),
  };
}

export function sameHash(left: string, right: string): boolean {
  if (left.length !== right.length) return false;
  let diff = 0;
  for (let i = 0; i < left.length; i += 1) {
    diff |= left.charCodeAt(i) ^ right.charCodeAt(i);
  }
  return diff === 0;
}
