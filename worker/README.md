# Directory Worker

HTTP lobby list for SCCT Revival. TypeScript, Workers Free, one SQLite Durable Object, one D1 database.

It does not open a socket for the game. It does not store the game. `EMULATOR_ENABLED` defaults to `"0"`. Setting it to `"1"` only changes `GET /v1/health` to `emulator: blocked`. See `docs/revival/05_emulator.md`.

Limits were checked Friday Nov 6, 2026 and written in `docs/revival/00_costs.md`. KV and R2 are not bound. There is no Cron Trigger. Observability is off. The placeholder `database_id` in `wrangler.jsonc` is not an account secret; replace it after `npx wrangler d1 create scct-revival`. Do not put an API token in the repo. Wrangler reads the deploy token from the user environment.

## Requests

Clients should refresh or heartbeat every 30 seconds. The object allows 8 requests per minute per client key. A lobby row expires 60 seconds after the last heartbeat. Coop `max_players` must be 2. Versus accepts 2 through 8 as a service cap, not a game constant (`docs/revival/06_player_cap_research.md`).

The Worker does not connect to the address a host submits.

Contract: `openapi.yaml`.

## Local check

```
npm install
npm test
```

Deploy is a separate step and needs the user's Cloudflare account:

```
npx wrangler d1 create scct-revival
npx wrangler d1 migrations apply scct-revival
npx wrangler secret put RESEND_API_KEY        # or BREVO_API_KEY
npx wrangler secret put EMAIL_FROM            # e.g. "SCCT Co-op <noreply@yourdomain.com>"
npx wrangler secret put CONTENT_GITHUB_TOKEN
npx wrangler secret put CONTENT_GITHUB_REPO   # owner/private-content-repo
npx wrangler secret put CONTENT_SCDA_ASSET_ID # GitHub release asset ID
npx wrangler secret put CF_TURN_TOKEN_ID      # optional Cloudflare Calls TURN token ID
npx wrangler secret put CF_TURN_API_TOKEN     # optional Cloudflare Calls TURN API token
npx wrangler deploy
```

This tree was not deployed.
