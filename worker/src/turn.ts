import type { Env } from "./env.js";

const TURN_TTL_SEC = 3600;
const TURN_QUOTA_WINDOW_MS = 10 * 60 * 1000;
const TURN_QUOTA_MAX_PER_ACCOUNT = 12;

interface QuotaBucket {
  windowStart: number;
  count: number;
}

const accountTurnQuota = new Map<string, QuotaBucket>();

export interface TurnResult {
  status: number;
  body: Record<string, unknown>;
}

function checkAndConsumeTurnQuota(accountId: string, nowMs: number): boolean {
  const cur = accountTurnQuota.get(accountId);
  if (!cur || nowMs - cur.windowStart >= TURN_QUOTA_WINDOW_MS) {
    accountTurnQuota.set(accountId, { windowStart: nowMs, count: 1 });
    return true;
  }
  if (cur.count >= TURN_QUOTA_MAX_PER_ACCOUNT) {
    return false;
  }
  cur.count += 1;
  return true;
}

export async function handleTurnCredentials(accountId: string, env: Env, nowMs: number): Promise<TurnResult> {
  if (!checkAndConsumeTurnQuota(accountId, nowMs)) {
    return {
      status: 429,
      body: { ok: false, error: "TURN credential rate limit exceeded. Try again shortly." },
    };
  }

  const tokenId = (env.CF_TURN_TOKEN_ID ?? "").trim();
  const apiToken = (env.CF_TURN_API_TOKEN ?? "").trim();
  if (!tokenId || !apiToken) {
    return {
      status: 200,
      body: {
        ok: true,
        mode: "stun-only",
        ttl: TURN_TTL_SEC,
        iceServers: [{ urls: ["stun:stun.cloudflare.com:3478"] }],
      },
    };
  }

  try {
    const endpoint = `https://rtc.live.cloudflare.com/v1/turn/keys/${encodeURIComponent(tokenId)}/credentials/generate-ice-servers`;
    const res = await fetch(endpoint, {
      method: "POST",
      headers: {
        Authorization: `Bearer ${apiToken}`,
        "Content-Type": "application/json",
      },
      body: JSON.stringify({ ttl: TURN_TTL_SEC }),
    });
    if (!res.ok) {
      return {
        status: 200,
        body: {
          ok: true,
          mode: "stun-fallback",
          ttl: TURN_TTL_SEC,
          upstream_status: res.status,
          iceServers: [{ urls: ["stun:stun.cloudflare.com:3478"] }],
        },
      };
    }
    const payload = (await res.json()) as { iceServers?: unknown };
    return {
      status: 200,
      body: {
        ok: true,
        mode: "turn",
        ttl: TURN_TTL_SEC,
        iceServers: payload.iceServers ?? [{ urls: ["stun:stun.cloudflare.com:3478"] }],
      },
    };
  } catch {
    return {
      status: 200,
      body: {
        ok: true,
        mode: "stun-fallback",
        ttl: TURN_TTL_SEC,
        iceServers: [{ urls: ["stun:stun.cloudflare.com:3478"] }],
      },
    };
  }
}
