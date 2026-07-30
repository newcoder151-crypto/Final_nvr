require("dotenv").config();
// Fail fast on missing/weak secrets before anything else touches them.
const { isProduction } = require("./config/security");

const express = require("express");
const cors = require("cors");
const helmet = require("helmet");
const morgan = require("morgan");
const rateLimit = require("express-rate-limit");
const http = require("http");
const path = require("path");
const fs = require("fs");
const { setupWebSocket } = require("./websocket");
const { testConnection } = require("./db");
const { authenticate, requireRole } = require("./middleware/auth");
const swaggerUi = require("swagger-ui-express");

const app = express();
const server = http.createServer(app);
const PORT = parseInt(process.env.PORT || "3001");
const STORAGE_PATH = process.env.STORAGE_PATH || "/storage";

// Trust exactly one reverse-proxy hop (nginx) so req.ip / express-rate-limit
// key on the real client IP instead of the proxy's, without blindly
// trusting an arbitrary chain of X-Forwarded-For values.
app.set("trust proxy", process.env.TRUST_PROXY_HOPS ? parseInt(process.env.TRUST_PROXY_HOPS) : 1);

// ── Security headers (OWASP A05: Security Misconfiguration) ───────────────────
app.use(
  helmet({
    crossOriginResourcePolicy: { policy: "cross-origin" },
    contentSecurityPolicy: {
      directives: {
        defaultSrc: ["'self'"],
        imgSrc: ["'self'", "data:", "blob:"],
        mediaSrc: ["'self'", "blob:"],
        connectSrc: ["'self'"],
        scriptSrc: ["'self'"],
        styleSrc: ["'self'", "'unsafe-inline'"],
        objectSrc: ["'none'"],
        frameAncestors: ["'none'"],
      },
    },
    hsts: { maxAge: 31536000, includeSubDomains: true, preload: false },
    referrerPolicy: { policy: "no-referrer" },
  }),
);

// ── CORS (OWASP A05) ────────────────────────────────────────────────────────
// A wildcard origin ("*") combined with credentials:true is both rejected by
// browsers and, if it weren't, would let ANY site read authenticated
// responses. Require an explicit allow-list in production; fall back to the
// Vite dev server origin only outside production.
const configuredOrigins = (process.env.CORS_ORIGIN || "")
  .split(",")
  .map((o) => o.trim())
  .filter(Boolean);

if (isProduction && configuredOrigins.length === 0) {
  throw new Error(
    "[SECURITY] CORS_ORIGIN must be set to an explicit comma-separated list of allowed origins in production.",
  );
}
const allowedOrigins = configuredOrigins.length
  ? configuredOrigins
  : ["http://localhost:8080", "http://127.0.0.1:8080"];

app.use(
  cors({
    origin(origin, cb) {
      // Allow same-origin / non-browser tools (no Origin header) and any
      // explicitly allow-listed origin; reject everything else.
      if (!origin || allowedOrigins.includes(origin)) return cb(null, true);
      return cb(new Error("Not allowed by CORS"));
    },
    credentials: true,
    methods: ["GET", "POST", "PUT", "DELETE", "OPTIONS", "PATCH"],
    allowedHeaders: ["Content-Type", "Authorization"],
    maxAge: 600,
  }),
);

app.use(morgan(isProduction ? "combined" : "dev"));
app.use(express.json({ limit: "10mb" }));
app.use(express.urlencoded({ extended: true, limit: "10mb" }));

// ── Global rate limiting (OWASP A04 — resource-consumption abuse) ─────────────
app.use(
  "/api",
  rateLimit({
    windowMs: 60 * 1000,
    max: parseInt(process.env.API_RATE_LIMIT || "300"),
    standardHeaders: true,
    legacyHeaders: false,
    message: { error: "Too many requests, slow down" },
  }),
);

// ── Storage dirs ──────────────────────────────────────────────────────────────
const recPath = path.join(STORAGE_PATH, "recordings");
const hlsPath = path.join(STORAGE_PATH, "hls");
[recPath, hlsPath].forEach((p) => {
  if (!fs.existsSync(p)) fs.mkdirSync(p, { recursive: true });
});

// ── Routes ────────────────────────────────────────────────────────────────────
app.use("/api/auth", require("./routes/auth"));
app.use("/api/cameras", require("./routes/cameras"));
app.use("/api/recordings", require("./routes/recordings"));
app.use("/api/events", require("./routes/events"));
app.use("/api/users", require("./routes/users"));
app.use("/api/config", require("./routes/config"));
app.use("/api/streaming", require("./routes/streaming"));
app.use("/api/search", require("./routes/search"));
app.use("/api/ai", require("./routes/ai"));
app.use("/api/settings", require("./routes/settings"));
app.use("/api/nvr", require("./routes/nvr"));
app.use("/api/onvif", require("./routes/onvif"));
app.use("/api/mediamtx", require("./routes/mediamtx"));

// ── Health ────────────────────────────────────────────────────────────────────
// Deliberately minimal — no version/uptime/internal detail for unauthenticated
// callers (OWASP A05: avoid unnecessary information disclosure).
app.get("/api/health", (req, res) => res.json({ status: "ok" }));

// ── Swagger (OWASP A05) ─────────────────────────────────────────────────────
// API documentation reveals the entire attack surface (routes, params,
// schemas). Never expose it unauthenticated in production.
try {
  const swaggerDoc = require("./swagger");
  const docsGate = isProduction ? [authenticate, requireRole("ADMIN")] : [];
  app.use(
    "/api/docs",
    ...docsGate,
    swaggerUi.serve,
    swaggerUi.setup(swaggerDoc, {
      customSiteTitle: "Railway NVR API",
      swaggerOptions: { persistAuthorization: true },
    }),
  );
  app.get("/api/docs.json", ...docsGate, (req, res) => res.json(swaggerDoc));
} catch (e) {
  console.warn("[swagger] Could not load swagger doc:", e.message);
}

// ── 404 ───────────────────────────────────────────────────────────────────────
app.use((req, res) => res.status(404).json({ error: "Not found" }));

// ── Error handler (OWASP A09: no internal detail leaked to clients) ───────────
app.use((err, req, res, _next) => {
  console.error("[ERROR]", err.stack || err.message);
  if (err.message === "Not allowed by CORS") {
    return res.status(403).json({ error: "Origin not allowed" });
  }
  const status = err.status && err.status >= 400 && err.status < 600 ? err.status : 500;
  res.status(status).json({
    error: isProduction && status === 500 ? "Internal server error" : err.message,
  });
});

// ── WebSocket ─────────────────────────────────────────────────────────────────
setupWebSocket(server);

// ── Start ─────────────────────────────────────────────────────────────────────
async function start() {
  const ok = await testConnection();
  if (!ok) {
    console.error("\n❌  Database connection failed.");
    console.error("    1. Ensure PostgreSQL is running");
    console.error("    2. Run:  bash start.sh --setup-db");
    console.error(
      "    3. Check server/.env for DB_HOST, DB_NAME, DB_USER, DB_PASSWORD\n",
    );
    process.exit(1);
  }
  server.listen(PORT, () => {
    console.log(`\n🚂  Railway NVR API  →  http://localhost:${PORT}`);
    console.log(`📖  API Docs         →  http://localhost:${PORT}/api/docs`);
    console.log(`🔌  WebSocket        →  ws://localhost:${PORT}/ws`);
    console.log(`💾  Storage          →  ${STORAGE_PATH}\n`);
  });
}

start();
module.exports = { app, server };
