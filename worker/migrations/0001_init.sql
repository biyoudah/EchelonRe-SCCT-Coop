-- Accounts and a host counter. Live lobbies are in the Durable Object, not here.
-- Heartbeats must not write this database. See docs/revival/00_costs.md.

CREATE TABLE IF NOT EXISTS accounts (
  id TEXT PRIMARY KEY,
  display_name TEXT NOT NULL,
  token_hash TEXT NOT NULL UNIQUE,
  created_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS host_stats (
  account_id TEXT PRIMARY KEY,
  sessions_hosted INTEGER NOT NULL DEFAULT 0,
  updated_at TEXT NOT NULL,
  FOREIGN KEY (account_id) REFERENCES accounts(id)
);
