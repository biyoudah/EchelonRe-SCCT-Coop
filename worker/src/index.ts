import { D1Accounts } from "./accounts.js";
import type { Env } from "./env.js";
import { handleEdge, type EdgeDeps } from "./http.js";
import { RATE_LIMIT, RATE_WINDOW_MS, RateGate } from "./limits.js";
import { LobbyDirectory } from "./lobby-do.js";
import { handleOnlineV2 } from "./online.js";

export { LobbyDirectory };
export type { Env };

const edgeRate = new RateGate(RATE_LIMIT, RATE_WINDOW_MS);

const JSON_ERROR = {
  "content-type": "application/json; charset=utf-8",
  "cache-control": "no-store",
};

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    try {
      const url = new URL(request.url);
      const clientKey = request.headers.get("CF-Connecting-IP") ?? "unknown";
      if (url.pathname.startsWith("/v2/")) {
        return await handleOnlineV2(request, env, clientKey, Date.now());
      }
      const deps: EdgeDeps = {
        accounts: new D1Accounts(env.DB),
        rate: edgeRate,
        now: () => Date.now(),
        emulatorEnabled: env.EMULATOR_ENABLED === "1",
        clientKey,
        forwardLobby: (incoming) => {
          const stub = env.LOBBY.get(env.LOBBY.idFromName("directory"));
          return stub.fetch(incoming);
        },
      };
      return await handleEdge(request, deps);
    } catch {
      return new Response(JSON.stringify({ error: "internal", detail: "failed" }), {
        status: 500,
        headers: JSON_ERROR,
      });
    }
  },
};
