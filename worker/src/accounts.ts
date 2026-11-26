export interface Account {
  id: string;
  display_name: string;
  username: string;
}

export interface AccountStore {
  create(
    id: string,
    username: string,
    passwordHash: string,
    tokenHash: string,
    createdAt: string,
  ): Promise<"ok" | "conflict">;
  findByTokenHash(tokenHash: string): Promise<Account | null>;
  verify(username: string, passwordHash: string): Promise<Account | null>;
  replaceToken(accountId: string, tokenHash: string): Promise<void>;
  incrementHosted(accountId: string, updatedAt: string): Promise<void>;
  remove(accountId: string): Promise<void>;
}

interface MemoryRow {
  id: string;
  display_name: string;
  username: string;
  password_hash: string;
  token_hash: string;
}

function sameHex(left: string, right: string): boolean {
  if (left.length !== right.length) return false;
  let diff = 0;
  for (let i = 0; i < left.length; i += 1) diff |= left.charCodeAt(i) ^ right.charCodeAt(i);
  return diff === 0;
}

function nameKey(username: string): string {
  return username.toLowerCase();
}

export class MemoryAccounts implements AccountStore {
  private readonly byToken = new Map<string, MemoryRow>();
  private readonly byName = new Map<string, MemoryRow>();
  readonly hosted = new Map<string, number>();

  async create(
    id: string,
    username: string,
    passwordHash: string,
    tokenHash: string,
    _createdAt: string,
  ): Promise<"ok" | "conflict"> {
    const key = nameKey(username);
    if (this.byName.has(key)) return "conflict";
    const row: MemoryRow = {
      id,
      display_name: username,
      username,
      password_hash: passwordHash,
      token_hash: tokenHash,
    };
    this.byName.set(key, row);
    this.byToken.set(tokenHash, row);
    return "ok";
  }

  async findByTokenHash(tokenHash: string): Promise<Account | null> {
    const row = this.byToken.get(tokenHash);
    return row ? publicAccount(row) : null;
  }

  async verify(username: string, passwordHash: string): Promise<Account | null> {
    const row = this.byName.get(nameKey(username));
    if (!row || !sameHex(row.password_hash, passwordHash)) return null;
    return publicAccount(row);
  }

  async replaceToken(accountId: string, tokenHash: string): Promise<void> {
    let row: MemoryRow | undefined;
    for (const [key, candidate] of this.byToken) {
      if (candidate.id === accountId) {
        row = candidate;
        this.byToken.delete(key);
        break;
      }
    }
    if (!row) return;
    row.token_hash = tokenHash;
    this.byToken.set(tokenHash, row);
  }

  passwordHash(username: string): string | null {
    return this.byName.get(nameKey(username))?.password_hash ?? null;
  }

  async incrementHosted(accountId: string, _updatedAt: string): Promise<void> {
    this.hosted.set(accountId, (this.hosted.get(accountId) ?? 0) + 1);
  }

  async remove(accountId: string): Promise<void> {
    let row: MemoryRow | undefined;
    for (const candidate of this.byName.values()) {
      if (candidate.id === accountId) {
        row = candidate;
        break;
      }
    }
    if (!row) return;
    this.byName.delete(nameKey(row.username));
    for (const [key, candidate] of this.byToken) {
      if (candidate.id === accountId) this.byToken.delete(key);
    }
    this.hosted.delete(accountId);
  }
}

export class D1Accounts implements AccountStore {
  constructor(private readonly db: D1Database) {}

  async create(
    id: string,
    username: string,
    passwordHash: string,
    tokenHash: string,
    createdAt: string,
  ): Promise<"ok" | "conflict"> {
    const existing = await this.db
      .prepare(`SELECT id FROM accounts WHERE username = ? COLLATE NOCASE`)
      .bind(username)
      .first<{ id: string }>();
    if (existing) return "conflict";
    try {
      await this.db
        .prepare(
          `INSERT INTO accounts (id, display_name, token_hash, created_at, username, password_hash)
           VALUES (?, ?, ?, ?, ?, ?)`,
        )
        .bind(id, username, tokenHash, createdAt, username, passwordHash)
        .run();
      return "ok";
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      if (message.includes("UNIQUE")) return "conflict";
      throw error;
    }
  }

  async findByTokenHash(tokenHash: string): Promise<Account | null> {
    const row = await this.db
      .prepare(`SELECT id, display_name, username FROM accounts WHERE token_hash = ?`)
      .bind(tokenHash)
      .first<{ id: string; display_name: string; username: string | null }>();
    return row ? publicAccount(row) : null;
  }

  async verify(username: string, passwordHash: string): Promise<Account | null> {
    const row = await this.db
      .prepare(
        `SELECT id, display_name, username, password_hash FROM accounts
         WHERE username = ? COLLATE NOCASE`,
      )
      .bind(username)
      .first<{ id: string; display_name: string; username: string | null; password_hash: string | null }>();
    if (!row?.password_hash || !sameHex(row.password_hash, passwordHash)) return null;
    return publicAccount(row);
  }

  async replaceToken(accountId: string, tokenHash: string): Promise<void> {
    await this.db
      .prepare(`UPDATE accounts SET token_hash = ? WHERE id = ?`)
      .bind(tokenHash, accountId)
      .run();
  }

  async incrementHosted(accountId: string, updatedAt: string): Promise<void> {
    await this.db
      .prepare(
        `INSERT INTO host_stats (account_id, sessions_hosted, updated_at) VALUES (?, 1, ?)
         ON CONFLICT(account_id) DO UPDATE SET
           sessions_hosted = sessions_hosted + 1,
           updated_at = excluded.updated_at`,
      )
      .bind(accountId, updatedAt)
      .run();
  }

  async remove(accountId: string): Promise<void> {
    await this.db.prepare(`DELETE FROM host_stats WHERE account_id = ?`).bind(accountId).run();
    await this.db.prepare(`DELETE FROM accounts WHERE id = ?`).bind(accountId).run();
  }
}

function publicAccount(row: {
  id: string;
  display_name: string;
  username: string | null;
}): Account {
  return {
    id: row.id,
    display_name: row.display_name,
    username: row.username ?? "",
  };
}
