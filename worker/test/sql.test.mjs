import assert from "node:assert/strict";
import { DatabaseSync } from "node:sqlite";
import test from "node:test";
import { LobbyBook } from "../dist/src/book.js";
import { SqliteLobbyStore } from "../dist/src/store.js";

function open() {
  const db = new DatabaseSync(":memory:");
  return {
    exec(query, ...bindings) {
      const statement = db.prepare(query);
      const text = query.trim().toUpperCase();
      if (text.startsWith("SELECT")) {
        return { toArray: () => statement.all(...bindings) };
      }
      statement.run(...bindings);
      return { toArray: () => [] };
    },
  };
}

test("sqlite store expires a lobby after 60 seconds", () => {
  const store = new SqliteLobbyStore(open());
  store.migrate();
  const book = new LobbyBook(store);
  const now = 1_700_000_000_000;
  const created = book.create(
    {
      host_name: "Sam",
      mode: "coop",
      address: "203.0.113.10",
      port: 9103,
      map: null,
      max_players: 2,
    },
    "0123456789abcdef",
    "a".repeat(64),
    now,
  );
  assert.equal(created.id, "0123456789abcdef");
  assert.equal(book.list("coop", now).length, 1);
  assert.equal(book.list("coop", now + 60_001).length, 0);
  assert.equal(book.heartbeat("0123456789abcdef", "b".repeat(64), now), "missing");
});

test("sqlite heartbeat keeps the account that created the lobby", () => {
  const store = new SqliteLobbyStore(open());
  store.migrate();
  const book = new LobbyBook(store);
  const now = 1_700_000_000_000;
  const id = "fedcba9876543210";
  const created = book.create(
    {
      host_name: "Sam",
      mode: "coop",
      address: "203.0.113.10",
      port: 9103,
      map: null,
      max_players: 2,
      account_id: "0123456789abcdef",
    },
    id,
    "a".repeat(64),
    now,
  );
  assert.equal(created.host_name, "Sam");
  assert.equal(book.heartbeat(id, "a".repeat(64), now, "aaaaaaaaaaaaaaaa"), "forbidden");
  const beat = book.heartbeat(id, "a".repeat(64), now, "0123456789abcdef");
  assert.equal(beat.host_name, "Sam");
});
