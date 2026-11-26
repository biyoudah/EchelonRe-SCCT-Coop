-- Online Co-op auth, email verification, rate limiting, friends, and room directory.
-- Keeps existing /v1/* accounts and host_stats columns intact.

ALTER TABLE accounts ADD COLUMN email TEXT;
ALTER TABLE accounts ADD COLUMN email_verified INTEGER NOT NULL DEFAULT 0;
ALTER TABLE accounts ADD COLUMN pw_salt TEXT;
ALTER TABLE accounts ADD COLUMN last_seen TEXT;
ALTER TABLE accounts ADD COLUMN active_room TEXT;
ALTER TABLE accounts ADD COLUMN friendly_fire INTEGER NOT NULL DEFAULT 0;

CREATE UNIQUE INDEX IF NOT EXISTS accounts_email_nocase
  ON accounts (email COLLATE NOCASE)
  WHERE email IS NOT NULL;

CREATE TABLE IF NOT EXISTS email_verifications (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  account_id TEXT NOT NULL,
  code_salt TEXT NOT NULL,
  code_hash TEXT NOT NULL,
  attempts INTEGER NOT NULL DEFAULT 0,
  expires_at TEXT NOT NULL,
  created_at TEXT NOT NULL,
  FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_email_verifications_account
  ON email_verifications (account_id);

CREATE TABLE IF NOT EXISTS auth_attempts (
  key TEXT PRIMARY KEY,
  failures INTEGER NOT NULL DEFAULT 0,
  locked_until TEXT NOT NULL DEFAULT '',
  updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS friendships (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id TEXT NOT NULL,
  friend_user_id TEXT NOT NULL,
  status TEXT NOT NULL,
  created_at TEXT NOT NULL,
  FOREIGN KEY (user_id) REFERENCES accounts(id) ON DELETE CASCADE,
  FOREIGN KEY (friend_user_id) REFERENCES accounts(id) ON DELETE CASCADE,
  UNIQUE (user_id, friend_user_id)
);

CREATE TABLE IF NOT EXISTS coop_rooms (
  code TEXT PRIMARY KEY,
  host_id TEXT NOT NULL,
  host_name TEXT NOT NULL,
  friendly_fire INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  FOREIGN KEY (host_id) REFERENCES accounts(id) ON DELETE CASCADE
);
