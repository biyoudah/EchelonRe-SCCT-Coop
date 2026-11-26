/** Service limits. Not game constants. See docs/revival/00_costs.md and 06_player_cap_research.md. */

export const HEARTBEAT_SECONDS = 30;
export const TTL_MS = 60_000;
export const MAX_LOBBIES = 32;
export const RATE_LIMIT = 8;
/** Directory calls that present no account token. Per public address, and under the per-account cap. */
export const ANON_RATE_LIMIT = 2;
export const RATE_WINDOW_MS = 60_000;
export const MAX_BODY_BYTES = 4096;
/** Capture text `MaxPlayers=2` and the revival rule. Not an exe compare. */
export const COOP_PLAYERS = 2;
export const VERSUS_PLAYERS_MIN = 2;
/** Abuse cap on a stored integer. Not a confirmed Versus team size. */
export const VERSUS_PLAYERS_MAX = 8;

export class RateGate {
  private readonly hits = new Map<string, number[]>();

  constructor(
    private readonly limit: number,
    private readonly windowMs: number,
  ) {}

  allow(key: string, now: number): boolean {
    const recent = (this.hits.get(key) ?? []).filter((stamp) => now - stamp < this.windowMs);
    if (recent.length >= this.limit) {
      this.hits.set(key, recent);
      this.prune(now);
      return false;
    }
    recent.push(now);
    this.hits.set(key, recent);
    this.prune(now);
    return true;
  }

  private prune(now: number): void {
    if (this.hits.size <= 1024) return;
    for (const [key, stamps] of this.hits) {
      const recent = stamps.filter((stamp) => now - stamp < this.windowMs);
      if (recent.length === 0) this.hits.delete(key);
      else this.hits.set(key, recent);
    }
    if (this.hits.size > 4096) this.hits.clear();
  }
}
