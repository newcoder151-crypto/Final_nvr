require("dotenv").config();
const express = require("express");
const cors = require("cors");
const helmet = require("helmet");
const morgan = require("morgan");
const http = require("http");
const path = require("path");
const fs = require("fs");
const { setupWebSocket } = require("./websocket");
const { testConnection } = require("./db");
const swaggerUi = require("swagger-ui-express");

const app = express();
const server = http.createServer(app);
const PORT = parseInt(process.env.PORT || "3001");
const STORAGE_PATH = process.env.STORAGE_PATH || "/storage";

// ── Middleware ────────────────────────────────────────────────────────────────
app.use(helmet({ crossOriginResourcePolicy: { policy: "cross-origin" } }));
app.use(
  cors({
    origin: process.env.CORS_ORIGIN || "*",
    credentials: true,
    methods: ["GET", "POST", "PUT", "DELETE", "OPTIONS", "PATCH"],
    allowedHeaders: ["Content-Type", "Authorization"],
  }),
);
app.use(morgan("dev"));
app.use(express.json({ limit: "10mb" }));
app.use(express.urlencoded({ extended: true }));

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
app.get("/api/health", (req, res) =>
  res.json({
    status: "ok",
    version: "2.0.0",
    service: "railway-nvr-api",
    uptime: process.uptime(),
    timestamp: new Date().toISOString(),
  }),
);

// ── Swagger ───────────────────────────────────────────────────────────────────
try {
  const swaggerDoc = require("./swagger");
  app.use(
    "/api/docs",
    swaggerUi.serve,
    swaggerUi.setup(swaggerDoc, {
      customSiteTitle: "Railway NVR API",
      swaggerOptions: { persistAuthorization: true },
    }),
  );
  app.get("/api/docs.json", (req, res) => res.json(swaggerDoc));
} catch (e) {
  console.warn("[swagger] Could not load swagger doc:", e.message);
}

// ── 404 / Error ───────────────────────────────────────────────────────────────
app.use((req, res) =>
  res.status(404).json({ error: "Not found", path: req.path }),
);
app.use((err, req, res, _next) => {
  console.error("[ERROR]", err.message);
  res.status(err.status || 500).json({ error: err.message });
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
