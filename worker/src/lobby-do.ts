import { DurableObject } from "cloudflare:workers";
import { LobbyBook } from "./book.js";
import type { Env } from "./env.js";
import { handleLobby } from "./http.js";
import { ANON_RATE_LIMIT, RATE_LIMIT, RATE_WINDOW_MS, RateGate } from "./limits.js";
import { SqliteLobbyStore, type SqlDatabase } from "./store.js";

const ALARM_MS = 30_000;

export class LobbyDirectory extends DurableObject<Env> {
  private readonly book: LobbyBook;
  private readonly accountRate = new RateGate(RATE_LIMIT, RATE_WINDOW_MS);
  private readonly anonRate = new RateGate(ANON_RATE_LIMIT, RATE_WINDOW_MS);

  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);
    const store = new SqliteLobbyStore(ctx.storage.sql as unknown as SqlDatabase);
    this.book = new LobbyBook(store);
    void ctx.blockConcurrencyWhile(async () => {
      store.migrate();
    });
  }

  async fetch(request: Request): Promise<Response> {
    const url = new URL(request.url);
    if (url.pathname.startsWith("/v2/relay/")) {
      return this.handleRelaySocket(request, url);
    }

    const clientKey = request.headers.get("x-scct-client") ?? "unknown";
    try {
      const response = await handleLobby(request, {
        book: this.book,
        accountRate: this.accountRate,
        anonRate: this.anonRate,
        now: () => Date.now(),
        emulatorEnabled: this.env.EMULATOR_ENABLED === "1",
        clientKey,
      });
      if (request.method === "POST" && response.status === 201) {
        const existing = await this.ctx.storage.getAlarm();
        if (existing === null) await this.ctx.storage.setAlarm(Date.now() + ALARM_MS);
      }
      return response;
    } catch {
      return new Response(JSON.stringify({ error: "internal", detail: "failed" }), {
        status: 500,
        headers: { "content-type": "application/json; charset=utf-8" },
      });
    }
  }

  private handleRelaySocket(request: Request, url: URL): Response {
    if ((request.headers.get("upgrade") ?? "").toLowerCase() !== "websocket") {
      return new Response("Expected WebSocket", { status: 426 });
    }
    const existing = this.ctx.getWebSockets();
    if (existing.length >= 2) {
      return new Response(JSON.stringify({ ok: false, error: "Room is full (max 2 players)." }), {
        status: 409,
        headers: { "content-type": "application/json; charset=utf-8" },
      });
    }

    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    const uid = url.searchParams.get("uid") ?? "";
    const username = url.searchParams.get("user") ?? "";
    server.serializeAttachment({ uid, username });
    this.ctx.acceptWebSocket(server);

    const peersAfter = existing.length + 1;
    if (peersAfter >= 2) {
      const notice = JSON.stringify({ type: "peer_joined", peers: peersAfter, username });
      for (const peer of existing) {
        try {
          peer.send(notice);
        } catch {
          // Ignore closed socket
        }
      }
      try {
        server.send(JSON.stringify({ type: "peer_joined", peers: peersAfter }));
      } catch {
        // Ignore
      }
    } else {
      try {
        server.send(JSON.stringify({ type: "waiting", peers: 1 }));
      } catch {
        // Ignore
      }
    }

    return new Response(null, { status: 101, webSocket: client });
  }

  async webSocketMessage(ws: WebSocket, message: string | ArrayBuffer): Promise<void> {
    if (typeof message === "string") {
      if (message === "ping") {
        try {
          ws.send("pong");
        } catch {
          // Ignore
        }
      }
      return;
    }
    for (const other of this.ctx.getWebSockets()) {
      if (other !== ws) {
        try {
          other.send(message);
        } catch {
          // Ignore stale peer socket until close fires
        }
      }
    }
  }

  async webSocketClose(ws: WebSocket, _code: number, _reason: string, _wasClean: boolean): Promise<void> {
    this.notifyPeerLeft(ws);
  }

  async webSocketError(ws: WebSocket): Promise<void> {
    this.notifyPeerLeft(ws);
  }

  private notifyPeerLeft(closed: WebSocket): void {
    for (const other of this.ctx.getWebSockets()) {
      if (other !== closed) {
        try {
          other.send(JSON.stringify({ type: "peer_left" }));
        } catch {
          // Ignore
        }
      }
    }
    try {
      closed.close();
    } catch {
      // Ignore already closed
    }
  }

  async alarm(): Promise<void> {
    const now = Date.now();
    this.book.expire(now);
    if (this.book.list(null, now).length > 0) {
      await this.ctx.storage.setAlarm(now + ALARM_MS);
    }
  }
}
