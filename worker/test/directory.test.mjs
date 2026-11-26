import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { test } from "node:test";
import { MemoryAccounts } from "../dist/src/accounts.js";
import { LobbyBook } from "../dist/src/book.js";
import { handleEdge, handleLobby } from "../dist/src/http.js";
import { ANON_RATE_LIMIT, RATE_LIMIT, RateGate } from "../dist/src/limits.js";
import { MemoryLobbyStore } from "../dist/src/store.js";

function lobbyDeps(clientKey = "203.0.113.4") {
  let now = 1_700_000_000_000;
  return {
    book: new LobbyBook(new MemoryLobbyStore()),
    accountRate: new RateGate(100, 60_000),
    anonRate: new RateGate(100, 60_000),
    now: () => now,
    at(next) {
      now = next;
    },
    emulatorEnabled: false,
    clientKey,
  };
}

function createBody(overrides = {}) {
  return {
    host_name: "Sam",
    mode: "coop",
    address: "203.0.113.10",
    port: 9103,
    max_players: 2,
    ...overrides,
  };
}

function call(deps, path, init = {}) {
  return handleLobby(new Request(`https://directory.example${path}`, init), deps);
}

test("coop rejects a third player and versus stores a host-declared size", async () => {
  const deps = lobbyDeps();
  const coop = await call(deps, "/v1/lobbies", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(createBody({ max_players: 3 })),
  });
  assert.equal(coop.status, 400);

  const versus = await call(deps, "/v1/lobbies", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(createBody({ mode: "versus", max_players: 8 })),
  });
  assert.equal(versus.status, 201);
  const tooBig = await call(deps, "/v1/lobbies", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(createBody({ mode: "versus", max_players: 9 })),
  });
  assert.equal(tooBig.status, 400);
});

test("rejects loopback, link-local, and unknown fields", async () => {
  const deps = lobbyDeps();
  for (const address of ["127.0.0.1", "169.254.1.1", "224.0.0.1", "10.0.0.1:9103"]) {
    const response = await call(deps, "/v1/lobbies", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(createBody({ address })),
    });
    assert.equal(response.status, 400, address);
  }
  const extra = await call(deps, "/v1/lobbies", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(createBody({ password: "nope" })),
  });
  assert.equal(extra.status, 400);
  const lan = await call(deps, "/v1/lobbies", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(createBody({ address: "192.168.1.20" })),
  });
  assert.equal(lan.status, 201);
});

test("lists a lobby until the heartbeat expires", async () => {
  const deps = lobbyDeps();
  const created = await call(deps, "/v1/lobbies", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(createBody()),
  });
  assert.equal(created.status, 201);
  const body = await created.json();
  assert.equal(typeof body.token, "string");

  const listed = await call(deps, "/v1/lobbies?mode=coop");
  const listBody = await listed.json();
  assert.equal(listBody.lobbies.length, 1);
  assert.equal(listBody.lobbies[0].token, undefined);

  const beat = await call(deps, `/v1/lobbies/${body.lobby.id}/heartbeat`, {
    method: "POST",
    headers: { authorization: `Bearer ${body.token}` },
  });
  assert.equal(beat.status, 200);

  const wrong = await call(deps, `/v1/lobbies/${body.lobby.id}/heartbeat`, {
    method: "POST",
    headers: { authorization: `Bearer ${"a".repeat(64)}` },
  });
  assert.equal(wrong.status, 403);

  deps.at(deps.now() + 60_001);
  const stale = await (await call(deps, "/v1/lobbies")).json();
  assert.equal(stale.lobbies.length, 0);
});

test("a guest publishes the punched endpoint on the lobby", async () => {
  const deps = lobbyDeps();
  const created = await call(deps, "/v1/lobbies", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(createBody()),
  });
  const body = await created.json();
  const peer = await call(deps, `/v1/lobbies/${body.lobby.id}/peer`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ address: "198.51.100.20", port: 41000 }),
  });
  assert.equal(peer.status, 200);
  const listed = await (await call(deps, "/v1/lobbies?mode=coop")).json();
  assert.equal(listed.lobbies[0].peer_address, "198.51.100.20");
  assert.equal(listed.lobbies[0].peer_port, 41000);
});

test("authenticated directory calls are counted per account", async () => {
  assert.equal(RATE_LIMIT, 8);
  assert.equal(ANON_RATE_LIMIT, 2);
  assert.ok(ANON_RATE_LIMIT < RATE_LIMIT);

  const lobby = lobbyDeps("203.0.113.4");
  lobby.accountRate = new RateGate(RATE_LIMIT, 60_000);
  lobby.anonRate = new RateGate(ANON_RATE_LIMIT, 60_000);
  const { edge } = accountEdge(lobby);

  for (let i = 0; i < ANON_RATE_LIMIT; i += 1) {
    const open = await call(lobby, "/v1/lobbies");
    assert.equal(open.status, 200);
  }
  const anonBlocked = await call(lobby, "/v1/lobbies");
  assert.equal(anonBlocked.status, 429);
  assert.equal(anonBlocked.headers.get("retry-after"), "60");
  lobby.clientKey = "198.51.100.8";
  const otherAddress = await call(lobby, "/v1/lobbies");
  assert.equal(otherAddress.status, 200);
  lobby.clientKey = "203.0.113.4";
  const sameAddress = await call(lobby, "/v1/lobbies");
  assert.equal(sameAddress.status, 429);

  const samResponse = await handleEdge(
    new Request("https://directory.example/v1/accounts", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: accountBody("Sam", accountFixture),
    }),
    edge,
  );
  const sam = await samResponse.json();
  const maxResponse = await handleEdge(
    new Request("https://directory.example/v1/accounts", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: accountBody("Max", "other-pass"),
    }),
    edge,
  );
  const max = await maxResponse.json();

  for (let i = 0; i < RATE_LIMIT; i += 1) {
    const listed = await handleEdge(
      new Request("https://directory.example/v1/lobbies?mode=coop", {
        headers: { "x-account-token": sam.token },
      }),
      edge,
    );
    assert.equal(listed.status, 200);
  }
  const blocked = await handleEdge(
    new Request("https://directory.example/v1/lobbies?mode=coop", {
      headers: { "x-account-token": sam.token },
    }),
    edge,
  );
  assert.equal(blocked.status, 429);
  lobby.clientKey = "198.51.100.50";
  const sameAccount = await call(lobby, "/v1/lobbies?mode=coop", {
    headers: { "x-scct-account-id": sam.id },
  });
  assert.equal(sameAccount.status, 429);
  lobby.clientKey = "203.0.113.4";
  const otherAccount = await handleEdge(
    new Request("https://directory.example/v1/lobbies?mode=coop", {
      headers: { "x-account-token": max.token },
    }),
    edge,
  );
  assert.equal(otherAccount.status, 200);

  const junk = await handleEdge(
    new Request("https://directory.example/v1/lobbies?mode=coop", {
      headers: { "x-account-token": "zz" },
    }),
    edge,
  );
  assert.equal(junk.status, 429);
});

test("health reports the emulator flag and does not enable it", async () => {
  const off = await handleLobby(new Request("https://directory.example/v1/health"), {
    ...lobbyDeps(),
    emulatorEnabled: false,
  });
  assert.deepEqual(await off.json(), { ok: true, emulator: "off" });
  const blocked = await handleLobby(new Request("https://directory.example/v1/health"), {
    ...lobbyDeps(),
    emulatorEnabled: true,
  });
  assert.deepEqual(await blocked.json(), { ok: true, emulator: "blocked", reason: "header unknown" });
});

const accountFixture = "s3cret-pass";

function accountEdge(lobby) {
  const accounts = new MemoryAccounts();
  let forwarded = 0;
  return {
    accounts,
    forwarded: () => forwarded,
    edge: {
      accounts,
      rate: new RateGate(8, 60_000),
      now: lobby.now,
      emulatorEnabled: false,
      clientKey: "203.0.113.4",
      forwardLobby: (request) => {
        forwarded += 1;
        assert.equal(request.headers.get("x-account-token"), null);
        return handleLobby(request, lobby);
      },
    },
  };
}

function accountBody(username, password) {
  return JSON.stringify({ username, password });
}

test("a lobby create without an account token is rejected", async () => {
  const lobby = lobbyDeps();
  const { accounts, edge, forwarded } = accountEdge(lobby);
  const account = await handleEdge(
    new Request("https://directory.example/v1/accounts", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: accountBody("Sam", accountFixture),
    }),
    edge,
  );
  assert.equal(account.status, 201);
  const created = await account.json();
  assert.equal(created.username, "Sam");
  assert.equal(created.password, undefined);
  assert.equal(created.password_hash, undefined);
  const digest = createHash("sha256").update(accountFixture).digest("hex");
  assert.equal(accounts.passwordHash("Sam"), digest);
  assert.notEqual(accounts.passwordHash("Sam"), accountFixture);

  const missing = await handleEdge(
    new Request("https://directory.example/v1/lobbies", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(createBody()),
    }),
    edge,
  );
  assert.equal(missing.status, 401);
  assert.equal(forwarded(), 0);

  const bad = await handleEdge(
    new Request("https://directory.example/v1/lobbies", {
      method: "POST",
      headers: {
        "content-type": "application/json",
        "x-account-token": "zz",
      },
      body: JSON.stringify(createBody()),
    }),
    edge,
  );
  assert.equal(bad.status, 401);
  assert.equal(forwarded(), 0);

  const hosted = await handleEdge(
    new Request("https://directory.example/v1/lobbies", {
      method: "POST",
      headers: {
        "content-type": "application/json",
        "x-account-token": created.token,
      },
      body: JSON.stringify(createBody()),
    }),
    edge,
  );
  assert.equal(hosted.status, 201);
  assert.equal(forwarded(), 1);
  assert.equal(accounts.hosted.size, 1);
});

test("usernames are unique and an empty name is rejected", async () => {
  const lobby = lobbyDeps();
  const { edge } = accountEdge(lobby);
  const empty = await handleEdge(
    new Request("https://directory.example/v1/accounts", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: accountBody("", accountFixture),
    }),
    edge,
  );
  assert.equal(empty.status, 400);

  const first = await handleEdge(
    new Request("https://directory.example/v1/accounts", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: accountBody("Sam", accountFixture),
    }),
    edge,
  );
  assert.equal(first.status, 201);
  const again = await handleEdge(
    new Request("https://directory.example/v1/accounts", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: accountBody("sam", "other-pass"),
    }),
    edge,
  );
  assert.equal(again.status, 409);
  assert.equal((await again.json()).error, "conflict");
});

test("login checks the stored hash and replaces the account token", async () => {
  const lobby = lobbyDeps();
  const { accounts, edge, forwarded } = accountEdge(lobby);
  const created = await handleEdge(
    new Request("https://directory.example/v1/accounts", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: accountBody("Sam", accountFixture),
    }),
    edge,
  );
  const firstToken = (await created.json()).token;
  const wrong = await handleEdge(
    new Request("https://directory.example/v1/accounts/login", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: accountBody("Sam", "other-pass"),
    }),
    edge,
  );
  assert.equal(wrong.status, 401);

  const unknown = await handleEdge(
    new Request("https://directory.example/v1/accounts/login", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: accountBody("Nobody", accountFixture),
    }),
    edge,
  );
  assert.equal(unknown.status, 401);

  const login = await handleEdge(
    new Request("https://directory.example/v1/accounts/login", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: accountBody("Sam", accountFixture),
    }),
    edge,
  );
  assert.equal(login.status, 200);
  const body = await login.json();
  assert.equal(body.username, "Sam");
  assert.equal(body.password, undefined);
  assert.notEqual(body.token, firstToken);
  assert.equal(accounts.passwordHash("Sam"), createHash("sha256").update(accountFixture).digest("hex"));

  const stale = await handleEdge(
    new Request("https://directory.example/v1/lobbies", {
      method: "POST",
      headers: {
        "content-type": "application/json",
        "x-account-token": firstToken,
      },
      body: JSON.stringify(createBody()),
    }),
    edge,
  );
  assert.equal(stale.status, 401);
  assert.equal(forwarded(), 0);

  const hosted = await handleEdge(
    new Request("https://directory.example/v1/lobbies", {
      method: "POST",
      headers: {
        "content-type": "application/json",
        "x-account-token": body.token,
      },
      body: JSON.stringify(createBody({ host_name: "host" })),
    }),
    edge,
  );
  assert.equal(hosted.status, 201);
  assert.equal((await hosted.json()).lobby.host_name, "Sam");
  assert.equal(forwarded(), 1);
});

test("hosting and heartbeat carry the logged-in account", async () => {
  const lobby = lobbyDeps("203.0.113.9");
  const accounts = new MemoryAccounts();
  const forwarded = [];
  const edge = {
    accounts,
    rate: new RateGate(8, 60_000),
    now: lobby.now,
    emulatorEnabled: false,
    clientKey: "203.0.113.9",
    forwardLobby: (request) => {
      forwarded.push({
        path: new URL(request.url).pathname,
        account: request.headers.get("x-scct-account-id"),
        name: request.headers.get("x-scct-account-name"),
        token: request.headers.get("x-account-token"),
      });
      return handleLobby(request, lobby);
    },
  };
  const created = await handleEdge(
    new Request("https://directory.example/v1/accounts", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: accountBody("Sam", accountFixture),
    }),
    edge,
  );
  const sam = await created.json();
  const other = await handleEdge(
    new Request("https://directory.example/v1/accounts", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: accountBody("Max", "other-pass"),
    }),
    edge,
  );
  const max = await other.json();

  const hosted = await handleEdge(
    new Request("https://directory.example/v1/lobbies", {
      method: "POST",
      headers: {
        "content-type": "application/json",
        "x-account-token": sam.token,
        "x-scct-account-id": "0123456789abcdef",
        "x-scct-account-name": "Max",
      },
      body: JSON.stringify(createBody({ host_name: "host" })),
    }),
    edge,
  );
  assert.equal(hosted.status, 201);
  const lobbyBody = await hosted.json();
  assert.equal(lobbyBody.lobby.host_name, "Sam");
  assert.equal(forwarded[0].token, null);
  assert.equal(forwarded[0].name, "Sam");
  assert.equal(forwarded[0].account, sam.id);

  const beatMissing = await handleEdge(
    new Request(`https://directory.example/v1/lobbies/${lobbyBody.lobby.id}/heartbeat`, {
      method: "POST",
      headers: { authorization: `Bearer ${lobbyBody.token}` },
    }),
    edge,
  );
  assert.equal(beatMissing.status, 401);

  const beat = await handleEdge(
    new Request(`https://directory.example/v1/lobbies/${lobbyBody.lobby.id}/heartbeat`, {
      method: "POST",
      headers: {
        authorization: `Bearer ${lobbyBody.token}`,
        "x-account-token": sam.token,
      },
    }),
    edge,
  );
  assert.equal(beat.status, 200);
  assert.equal((await beat.json()).lobby.host_name, "Sam");
  assert.equal(forwarded.at(-1).account, sam.id);
  assert.equal(forwarded.at(-1).token, null);

  const intruder = await handleEdge(
    new Request(`https://directory.example/v1/lobbies/${lobbyBody.lobby.id}/heartbeat`, {
      method: "POST",
      headers: {
        authorization: `Bearer ${lobbyBody.token}`,
        "x-account-token": max.token,
      },
    }),
    edge,
  );
  assert.equal(intruder.status, 403);

  const listed = await handleEdge(
    new Request("https://directory.example/v1/lobbies?mode=coop", {
      headers: { "x-account-token": sam.token },
    }),
    edge,
  );
  assert.equal(listed.status, 200);
  assert.equal(forwarded.at(-1).account, sam.id);
  assert.equal(forwarded.at(-1).token, null);
  const listBody = await listed.json();
  assert.equal(listBody.lobbies.some((row) => row.id === lobbyBody.lobby.id && row.host_name === "Sam"), true);

  const peer = await handleEdge(
    new Request(`https://directory.example/v1/lobbies/${lobbyBody.lobby.id}/peer`, {
      method: "POST",
      headers: {
        "content-type": "application/json",
        "x-account-token": sam.token,
      },
      body: JSON.stringify({ address: "198.51.100.20", port: 41000 }),
    }),
    edge,
  );
  assert.equal(peer.status, 200);
  assert.equal(forwarded.at(-1).account, sam.id);
  assert.equal(forwarded.at(-1).token, null);

  const removed = await handleEdge(
    new Request("https://directory.example/v1/accounts", {
      method: "DELETE",
      headers: { "x-account-token": sam.token },
    }),
    edge,
  );
  assert.equal(removed.status, 204);
  const again = await handleEdge(
    new Request("https://directory.example/v1/accounts/login", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: accountBody("Sam", accountFixture),
    }),
    edge,
  );
  assert.equal(again.status, 401);
});
