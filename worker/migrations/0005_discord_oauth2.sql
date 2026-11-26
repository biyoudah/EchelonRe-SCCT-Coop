-- Discord OAuth2 account linking. A single Discord account can only be linked to
-- one SCCT account, and vice versa. Both columns are nullable for email-only users.

ALTER TABLE accounts ADD COLUMN discord_id TEXT;
ALTER TABLE accounts ADD COLUMN discord_username TEXT;

CREATE UNIQUE INDEX IF NOT EXISTS accounts_discord_id_unique
  ON accounts (discord_id)
  WHERE discord_id IS NOT NULL;

-- Temporary tokens for Discord OAuth2 link flow. The worker stores a short-lived
-- link_token keyed by OAuth2 state, then the launcher redeems it.
CREATE TABLE IF NOT EXISTS discord_link_nonces (
  state TEXT PRIMARY KEY,
  link_token_hash TEXT NOT NULL,
  discord_id TEXT NOT NULL,
  discord_username TEXT NOT NULL,
  expires_at TEXT NOT NULL,
  created_at TEXT NOT NULL
);
