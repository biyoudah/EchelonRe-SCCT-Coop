-- Username login for the revival directory. The password column stores SHA-256
-- hex only. Old display-name rows keep a null username and cannot log in.

ALTER TABLE accounts ADD COLUMN username TEXT;
ALTER TABLE accounts ADD COLUMN password_hash TEXT;

CREATE UNIQUE INDEX IF NOT EXISTS accounts_username_nocase
  ON accounts (username COLLATE NOCASE);
