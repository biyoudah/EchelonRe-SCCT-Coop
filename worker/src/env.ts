export interface Env {
  LOBBY: DurableObjectNamespace;
  DB: D1Database;
  EMULATOR_ENABLED: string;
  RESEND_API_KEY?: string;
  BREVO_API_KEY?: string;
  EMAIL_FROM?: string;
  ALLOW_DEV_EMAIL_FALLBACK?: string;
  CONTENT_MANIFEST_URL?: string;
  CONTENT_GITHUB_TOKEN?: string;
  CONTENT_GITHUB_REPO?: string;
  CONTENT_SCDA_ASSET_ID?: string;
  CF_TURN_TOKEN_ID?: string;
  CF_TURN_API_TOKEN?: string;
}
