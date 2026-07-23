#!/usr/bin/env bash
# =============================================================================
#  Railway mNVR — Single-Command Launcher (with MediaMTX)
#
#  FIRST TIME:   bash start.sh --setup-db
#  EVERY RUN:    bash start.sh
#  FLAGS:
#    --setup-db   Create DB, apply schema, seed data
#    --build-nvr  Compile C mnvrd daemon
#    --with-nvr   Also launch mnvrd
#    --skip-ai    Skip YOLO sidecar
#    --stop       Stop all services
#    --restart    Stop then start
#    --status     Show health of all services
#    --logs <svc> Tail logs: api|ai|recorder|frontend|nvr|mediamtx|mtxsync
#    --clean      Wipe storage/recordings and storage/hls
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
LOG_DIR="$ROOT/logs"
PID_DIR="$ROOT/.pids"
STORAGE="/storage"
mkdir -p "$LOG_DIR" "$PID_DIR" "$STORAGE/recordings" "$STORAGE/hls"
# Define NVR paths as absolute vars immediately — before any set -u reference
NVR_BIN="$ROOT/nvr_core/build/mnvrd"
NVR_CONF="$ROOT/nvr_core/config/mnvr.conf"
NVR_DIR="$ROOT/nvr_core"

# ── Arg parsing ───────────────────────────────────────────────────────────────
DO_SETUP=false; DO_BUILD_NVR=false; DO_WITH_NVR=false; DO_STOP=false
DO_STATUS=false; DO_RESTART=false; DO_CLEAN=false; SKIP_AI=false; TAIL_SVC=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --setup|--setup-db|-s)  DO_SETUP=true ;;
    --build-nvr|--build|-b) DO_BUILD_NVR=true ;;
    --with-nvr|--nvr|-n)    DO_WITH_NVR=true ;;
    --skip-ai|--no-ai)      SKIP_AI=true ;;
    --stop|--kill)          DO_STOP=true ;;
    --status|--health)      DO_STATUS=true ;;
    --restart|-r)           DO_RESTART=true ;;
    --clean)                DO_CLEAN=true ;;
    --logs|-l)              shift; TAIL_SVC="${1:-api}" ;;
    --help|-h|help)
      echo ""
      echo "  Railway mNVR — Usage:"
      echo "  bash start.sh                 Normal start"
      echo "  bash start.sh --setup-db      First-time DB setup"
      echo "  bash start.sh --build-nvr     Compile C daemon"
      echo "  bash start.sh --with-nvr      Start with C daemon"
      echo "  bash start.sh --skip-ai       Start without YOLO"
      echo "  bash start.sh --stop          Stop all services"
      echo "  bash start.sh --status        Show service health"
      echo "  bash start.sh --logs api      Tail a log"
      echo "  bash start.sh --logs mediamtx Tail MediaMTX log"
      echo "  bash start.sh --clean         Wipe recordings + HLS"
      echo ""
      exit 0 ;;
    *) echo "Unknown flag: '$1' — use --help"; exit 1 ;;
  esac
  shift
done

# ── Colors + helpers ──────────────────────────────────────────────────────────
G='\033[0;32m'; Y='\033[1;33m'; R='\033[0;31m'; B='\033[0;34m'
C='\033[0;36m'; W='\033[1;37m'; NC='\033[0m'; BOLD='\033[1m'
ok()   { echo -e "${G}  ✓${NC} $*"; }
warn() { echo -e "${Y}  ⚠${NC} $*"; }
err()  { echo -e "${R}  ✗${NC} $*" >&2; }
hdr()  { echo -e "\n${B}${BOLD}━━━ $* ━━━${NC}"; }
info() { echo -e "    $*"; }

# ── PID helpers ───────────────────────────────────────────────────────────────
pid_write() { echo "$2" > "$PID_DIR/$1.pid"; }
pid_read()  { cat "$PID_DIR/$1.pid" 2>/dev/null || true; }
pid_alive() { local p; p=$(pid_read "$1"); [[ -n "$p" ]] && kill -0 "$p" 2>/dev/null; }
pid_kill()  {
  local p; p=$(pid_read "$1")
  if [[ -n "$p" ]] && kill -0 "$p" 2>/dev/null; then
    kill "$p" 2>/dev/null || true
    # Wait up to 10s for a clean SIGTERM shutdown before escalating —
    # mnvrd especially can take a moment if a thread is mid-retry on a
    # camera connection. Verify it's actually gone; don't just fire the
    # signal and assume.
    for _ in $(seq 1 20); do
      kill -0 "$p" 2>/dev/null || break
      sleep 0.5
    done
    if kill -0 "$p" 2>/dev/null; then
      warn "$1 (PID $p) didn't exit after SIGTERM — forcing with SIGKILL"
      kill -9 "$p" 2>/dev/null || true
      sleep 0.5
    fi
    if kill -0 "$p" 2>/dev/null; then
      err "$1 (PID $p) still alive after SIGKILL — this shouldn't happen"
    else
      ok "Stopped $1 (PID $p)"
    fi
  fi
  rm -f "$PID_DIR/$1.pid"
}
kill_all() {
  for s in frontend api ai recorder nvr mediamtx mtxsync streammgr; do pid_kill "$s"; done
  pkill -f "node src/index.js"       2>/dev/null || true
  pkill -f "uvicorn sidecar"         2>/dev/null || true
  pkill -f "recorder.py"             2>/dev/null || true
  pkill -f "mediamtx-sync.py"        2>/dev/null || true
  pkill -f "stream_manager.py"       2>/dev/null || true
  pkill -f "mediamtx"                2>/dev/null || true
  # Safety net for mnvrd specifically — every other service already has
  # one above. Without this, a stale/wrong PID file (or a SIGTERM that
  # didn't land in time before pid_kill's own escalation ran) could leave
  # an orphaned mnvrd running, invisible to --status, competing with the
  # next one for the same camera connections. Verified this actually
  # happened: three overlapping mnvrd processes were found running
  # simultaneously after repeated --stop/--restart cycles.
  if pgrep -f "nvr_core/build/mnvrd" >/dev/null 2>&1; then
    warn "Found leftover mnvrd process(es) — force-killing"
    pkill -9 -f "nvr_core/build/mnvrd" 2>/dev/null || true
    sleep 0.5
  fi
}

# ── --logs ────────────────────────────────────────────────────────────────────
if [[ -n "$TAIL_SVC" ]]; then
  logf="$LOG_DIR/${TAIL_SVC}.log"
  if [[ -f "$logf" ]]; then
    echo -e "${C}Tailing $logf — Ctrl+C to stop${NC}"
    exec tail -f "$logf"
  else
    err "No log: $logf"
    info "Available: $(ls "$LOG_DIR"/*.log 2>/dev/null | xargs -n1 basename | tr '\n' ' ')"
    exit 1
  fi
fi

# ── --stop ────────────────────────────────────────────────────────────────────
if $DO_STOP || $DO_RESTART; then
  hdr "Stopping Railway mNVR"
  kill_all; sleep 1; ok "All services stopped"
  $DO_STOP && exit 0
fi

# ── --clean ───────────────────────────────────────────────────────────────────
if $DO_CLEAN; then
  warn "Cleaning storage..."
  rm -rf "$STORAGE/recordings" "$STORAGE/hls"
  mkdir -p "$STORAGE/recordings" "$STORAGE/hls"
  ok "Storage cleaned"
fi

# ── --status ──────────────────────────────────────────────────────────────────
if $DO_STATUS; then
  hdr "Railway mNVR — Service Status"
  for svc in api ai recorder frontend nvr mediamtx mtxsync streammgr; do
    if pid_alive "$svc"; then
      printf "  ${G}●${NC} %-12s PID %s\n" "$svc" "$(pid_read $svc)"
    else
      printf "  ${R}○${NC} %-12s stopped\n" "$svc"
    fi
  done
  echo ""
  check_http() {
    curl -sf --max-time 3 "$1" >/dev/null 2>&1 \
      && printf "  ${G}✓${NC} %-46s ${G}UP${NC}\n" "$2" \
      || printf "  ${R}✗${NC} %-46s ${R}DOWN${NC}\n" "$2"
  }
  check_http "http://localhost:${PORT:-3001}/api/health" "API       http://localhost:${PORT:-3001}/api"
  check_http "http://localhost:8000/health"              "YOLO AI   http://localhost:8000"
  check_http "http://localhost:9997/v3/paths/list"       "MediaMTX  http://localhost:9997"
  check_http "http://localhost:8889"                     "MTX WebRTC http://localhost:8889"
  check_http "http://localhost:8888"                     "MTX HLS   http://localhost:8888"
  check_http "http://localhost:8080"                     "Frontend  http://localhost:8080"
  exit 0
fi

# Kill stale processes
kill_all 2>/dev/null || true; sleep 0.3

# ── Banner ────────────────────────────────────────────────────────────────────
echo ""
echo -e "${C}${BOLD}"
echo "  ╔══════════════════════════════════════════════════════════╗"
echo "  ║   Railway mNVR — AI Network Video Recorder              ║"
echo "  ║   BEL · Indian Railways · EN 50155 · MediaMTX WebRTC   ║"
echo "  ╚══════════════════════════════════════════════════════════╝"
echo -e "${NC}"

# =============================================================================
# STEP 1 — Environment
# =============================================================================
hdr "1/10  Environment"
ENV_FILE="$ROOT/server/.env"
if [[ ! -f "$ENV_FILE" ]]; then
  warn "server/.env not found — creating defaults"
  cat > "$ENV_FILE" << 'ENVEOF'
DB_HOST=localhost
DB_PORT=5432
DB_NAME=mnvr
DB_USER=mnvr
DB_PASSWORD=mnvr
JWT_SECRET=railway-nvr-super-secret-change-in-production
CORS_ORIGIN=http://localhost:8080
YOLO_SIDECAR_URL=http://localhost:8000
STORAGE_PATH=/storage
RECORDINGS_PATH=/storage/recordings
HLS_PATH=/storage/hls
PORT=3001
NODE_ENV=development
MEDIAMTX_API=http://localhost:9997
MEDIAMTX_WEB=http://localhost:8889
MEDIAMTX_HLS=http://localhost:8888
MEDIAMTX_RTSP=localhost:8554
ENVEOF
fi
set -a; source "$ENV_FILE"; set +a

# Ensure MediaMTX vars exist in env file (add if missing)
grep -q "MEDIAMTX_API" "$ENV_FILE" || cat >> "$ENV_FILE" << 'ADDENV'
MEDIAMTX_API=http://localhost:9997
MEDIAMTX_WEB=http://localhost:8889
MEDIAMTX_HLS=http://localhost:8888
MEDIAMTX_RTSP=localhost:8554
ADDENV

[[ ! -f "$ROOT/.env" ]] && echo "VITE_API_URL=http://localhost:${PORT:-3001}" > "$ROOT/.env"

# Add MediaMTX URLs to frontend env
{
  grep -q "VITE_MEDIAMTX_WEB" "$ROOT/.env" || echo "VITE_MEDIAMTX_WEB=http://localhost:8889"
  grep -q "VITE_MEDIAMTX_HLS" "$ROOT/.env" || echo "VITE_MEDIAMTX_HLS=http://localhost:8888"
} 2>/dev/null || true

ok "Environment loaded — API :${PORT:-3001}"

# =============================================================================
# STEP 2 — Prerequisites
# =============================================================================
hdr "2/10  Prerequisites"
MISSING=()
for cmd in node npm python3 ffmpeg psql curl; do
  command -v "$cmd" &>/dev/null && ok "$cmd" || { err "$cmd not found"; MISSING+=("$cmd"); }
done
if [[ ${#MISSING[@]} -gt 0 ]]; then
  echo ""
  err "Missing: ${MISSING[*]}"
  echo -e "${Y}  Fix:${NC}  sudo apt install -y nodejs npm python3 python3-pip python3-venv ffmpeg postgresql-client curl"
  exit 1
fi

# =============================================================================
# STEP 3 — Build C daemon (--build-nvr)
# =============================================================================
if $DO_BUILD_NVR; then
  hdr "3/10  Building mNVR C Daemon"
  sudo apt-get install -y -q gcc make pkg-config \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly \
    gstreamer1.0-x libgstrtspserver-1.0-dev gstreamer1.0-rtsp \
    gstreamer1.0-libav libpq-dev libmicrohttpd-dev 2>/dev/null || true
  cd "$NVR_DIR" && make clean 2>/dev/null || true && make -j"$(nproc)" 2>&1 | tee "$LOG_DIR/nvr_build.log"
  cd "$ROOT"
  [[ -f "$NVR_DIR/build/mnvrd" ]] && { chmod +x "$NVR_DIR/build/mnvrd"; ok "Build → nvr_core/build/mnvrd"; } \
    || { err "Build FAILED — see logs/nvr_build.log"; exit 1; }
  # --build-nvr now always launches mnvrd too (along with the rest of the
  # stack below) — no more separate "bash start.sh --with-nvr" step needed.
  DO_WITH_NVR=true
else
  hdr "3/10  C Daemon Build  [skipped — use --build-nvr]"
fi

# =============================================================================
# STEP 4 — Database
# =============================================================================
hdr "4/10  Database"
DB_HOST="${DB_HOST:-localhost}"; DB_PORT="${DB_PORT:-5432}"
DB_NAME="${DB_NAME:-mnvr}";     DB_USER="${DB_USER:-mnvr}"; DB_PASSWORD="${DB_PASSWORD:-mnvr}"

if $DO_SETUP; then
  warn "Creating user '${DB_USER}' and database '${DB_NAME}'..."
  _admin() {
    (sudo -u postgres psql -c "$1" 2>/dev/null) ||
    (PGPASSWORD="$DB_PASSWORD" psql -h "$DB_HOST" -p "$DB_PORT" -U postgres -c "$1" 2>/dev/null) || true
  }
  _admin "CREATE USER ${DB_USER} WITH PASSWORD '${DB_PASSWORD}';"
  _admin "CREATE DATABASE ${DB_NAME} OWNER ${DB_USER};"
  _admin "GRANT ALL PRIVILEGES ON DATABASE ${DB_NAME} TO ${DB_USER};"
  PGPASSWORD="$DB_PASSWORD" psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -d "$DB_NAME" \
    -f "$ROOT/server/schema.sql" && ok "Schema applied"
  (cd "$ROOT/server" && [[ ! -d node_modules ]] && npm install --silent)
  (cd "$ROOT/server" && node seed.js) && ok "Seeded"
fi

warn "Checking PostgreSQL..."
for i in $(seq 1 15); do
  PGPASSWORD="$DB_PASSWORD" psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -d "$DB_NAME" \
    -c "SELECT 1" >/dev/null 2>&1 && { ok "PostgreSQL ready"; break; }
  [[ $i -eq 15 ]] && { err "Cannot connect to PostgreSQL — run: bash start.sh --setup-db"; exit 1; }
  printf "\r    Waiting (%d/15)..." "$i"; sleep 2
done; echo ""

# =============================================================================
# STEP 5 — npm dependencies
# =============================================================================
hdr "5/10  Node.js Packages"
[[ ! -d "$ROOT/server/node_modules" ]] && { warn "Installing backend packages..."; (cd "$ROOT/server" && npm install --silent); } && ok "Backend ready" || ok "Backend ready"
[[ ! -d "$ROOT/node_modules" ]]        && { warn "Installing frontend packages..."; (cd "$ROOT" && npm install --silent); }        && ok "Frontend ready" || ok "Frontend ready"

# =============================================================================
# STEP 6 — Python venv
# =============================================================================
hdr "6/10  Python / AI Packages"
AI_DIR="$ROOT/server/ai"
VENV="$AI_DIR/.venv"
[[ ! -d "$VENV" ]] && { warn "Creating Python venv..."; python3 -m venv "$VENV"; }
"$VENV/bin/pip" install --upgrade pip --quiet 2>/dev/null
warn "Installing AI packages (ultralytics ~500MB first run):"
info "──────────────────────────────────────────────────────"
"$VENV/bin/pip" install -r "$AI_DIR/requirements.txt" --progress-bar on 2>&1 \
  | grep -E "^Collecting|^Downloading|^Installing|^Successfully|already satisfied|ERROR" | sed 's/^/    /'

# Install mediamtx-sync dependencies
warn "Installing mediamtx-sync packages..."
"$VENV/bin/pip" install -r "$ROOT/mediamtx/requirements.txt" -q
info "──────────────────────────────────────────────────────"
ok "Python packages ready"

# =============================================================================
# STEP 7 — Download & start MediaMTX
# =============================================================================
hdr "7/10  MediaMTX (WebRTC / RTSP server)"

MTX_DIR="$ROOT/mediamtx"
MTX_BIN="$MTX_DIR/mediamtx"
MTX_VER="v1.9.3"

# Detect OS/arch
ARCH=$(uname -m)
OS=$(uname -s | tr '[:upper:]' '[:lower:]')
case "$ARCH" in
  x86_64)  ARCH_TAG="amd64" ;;
  aarch64) ARCH_TAG="arm64v8" ;;
  armv7*)  ARCH_TAG="armv7" ;;
  *)       ARCH_TAG="amd64" ;;
esac
MTX_TARBALL="mediamtx_${MTX_VER}_${OS}_${ARCH_TAG}.tar.gz"
MTX_URL="https://github.com/bluenviron/mediamtx/releases/download/${MTX_VER}/${MTX_TARBALL}"

if [[ ! -f "$MTX_BIN" ]]; then
  warn "Downloading MediaMTX ${MTX_VER} (${OS}/${ARCH_TAG})..."
  info "URL: $MTX_URL"
  if curl -L --progress-bar -o "$MTX_DIR/$MTX_TARBALL" "$MTX_URL"; then
    tar -xzf "$MTX_DIR/$MTX_TARBALL" -C "$MTX_DIR" mediamtx 2>/dev/null || \
    tar -xzf "$MTX_DIR/$MTX_TARBALL" -C "$MTX_DIR" 2>/dev/null
    chmod +x "$MTX_BIN"
    rm -f "$MTX_DIR/$MTX_TARBALL"
    ok "MediaMTX downloaded → $MTX_BIN"
  else
    err "Download failed. Download manually from:"
    err "  $MTX_URL"
    err "  Extract 'mediamtx' binary to: $MTX_DIR/"
    warn "Continuing without MediaMTX — WebRTC streams unavailable"
  fi
fi

if [[ -f "$MTX_BIN" ]]; then
  warn "Starting MediaMTX (RTSP :8554, WebRTC :8889, HLS :8888, API :9997)..."
  nohup "$MTX_BIN" "$MTX_DIR/mediamtx.yml" > "$LOG_DIR/mediamtx.log" 2>&1 &
  MTX_PID=$!
  pid_write "mediamtx" "$MTX_PID"

  # Wait for MediaMTX API to be ready
  for i in $(seq 1 20); do
    curl -sf --max-time 2 "http://localhost:9997/v3/paths/list" >/dev/null 2>&1 && { ok "MediaMTX ready ✓  (${i}s)"; break; }
    if ! kill -0 "$MTX_PID" 2>/dev/null; then
      err "MediaMTX crashed — last 10 lines of logs/mediamtx.log:"
      tail -10 "$LOG_DIR/mediamtx.log" | sed 's/^/    /'
      break
    fi
    [[ $i -eq 20 ]] && warn "MediaMTX taking longer — check logs/mediamtx.log"
    sleep 1
  done

  # Start mediamtx-sync (reads cameras from DB → creates MediaMTX paths)
  warn "Starting mediamtx-sync (camera → stream path sync every 15s)..."
  DB_HOST="$DB_HOST" DB_PORT="$DB_PORT" DB_NAME="$DB_NAME" \
  DB_USER="$DB_USER" DB_PASSWORD="$DB_PASSWORD" \
  MEDIAMTX_API="http://localhost:9997" \
  SYNC_INTERVAL_SECS="15" \
  nohup "$VENV/bin/python" "$MTX_DIR/mediamtx-sync.py" > "$LOG_DIR/mtxsync.log" 2>&1 &
  pid_write "mtxsync" "$!"
  ok "mediamtx-sync PID $(pid_read mtxsync)  (logs/mtxsync.log)"
else
  warn "MediaMTX binary not found — WebRTC live streams unavailable"
  warn "Cameras will still record via recorder service"
fi

# =============================================================================
# STEP 8 — YOLO AI Sidecar
# =============================================================================
if ! $SKIP_AI; then
  hdr "8/10  YOLO AI Sidecar"
  warn "Starting on :8000..."
  cd "$AI_DIR"
  nohup "$VENV/bin/python" -m uvicorn sidecar:app \
    --host 0.0.0.0 --port 8000 --workers 1 \
    > "$LOG_DIR/ai.log" 2>&1 &
  AI_PID=$!; pid_write "ai" "$AI_PID"; cd "$ROOT"
  ok "YOLO sidecar PID $AI_PID  (logs/ai.log)"

  warn "Waiting for YOLO (model loads in seconds)..."
  info "──────────────────────────────────────────────────────"
  READY=false
  for i in $(seq 1 90); do
    LAST=$(tail -1 "$LOG_DIR/ai.log" 2>/dev/null | tr -d '\r' || true)
    [[ -n "$LAST" ]] && printf "\r    [%2ds] %-72s" "$i" "${LAST:0:72}"
    if curl -sf --max-time 2 http://localhost:8000/health >/dev/null 2>&1; then
      echo ""; info "──────────────────────────────────────────────────────"
      ok "YOLO ready ✓  (${i}s)"; READY=true; break
    fi
    if ! kill -0 "$AI_PID" 2>/dev/null; then
      echo ""; err "YOLO crashed:"; tail -10 "$LOG_DIR/ai.log" | sed 's/^/    /'; exit 1
    fi
    sleep 1
  done
  $READY || { echo ""; warn "YOLO not ready after 90s — AI will come online shortly"; }
else
  hdr "8/10  YOLO AI Sidecar  [SKIPPED]"
fi

# =============================================================================
# STEP 9 — API + Recorder + C daemon
# =============================================================================
hdr "9/10  API Server + Recorder"

# API
warn "Starting Node.js API on :${PORT:-3001}..."
cd "$ROOT/server"
nohup node src/index.js > "$LOG_DIR/api.log" 2>&1 &
API_PID=$!; pid_write "api" "$API_PID"; cd "$ROOT"
ok "API PID $API_PID  (logs/api.log)"

warn "Waiting for API..."
for i in $(seq 1 30); do
  LAST=$(tail -1 "$LOG_DIR/api.log" 2>/dev/null | tr -d '\r' || true)
  [[ -n "$LAST" ]] && printf "\r    [%2ds] %-72s" "$i" "${LAST:0:72}"
  if curl -sf --max-time 2 "http://localhost:${PORT:-3001}/api/health" >/dev/null 2>&1; then
    echo ""; ok "API ready ✓  (${i}s)"; break
  fi
  if ! kill -0 "$API_PID" 2>/dev/null; then
    echo ""; err "API crashed:"; tail -10 "$LOG_DIR/api.log" | sed 's/^/    /'; exit 1
  fi
  [[ $i -eq 30 ]] && { echo ""; err "API not responding — check logs/api.log"; exit 1; }
  sleep 1
done

# Recorder
# Recorder
# DISABLED: mnvrd (the C daemon, --build-nvr) already has its own built-in
# recorder (recorder_module.c) that records every camera directly via
# GStreamer, respecting cameras.rec_output_dir as-is. Running this Python
# recorder at the same time duplicated every recording (both writing MP4s
# for the same camera simultaneously), and its camera_loop() unconditionally
# PUTs rec_output_dir=".../cam_<id>" back to the API on every (re)start —
# that's what kept silently overwriting any manual rec_output_dir fix.
# Re-enable only if you want the Python-side recorder INSTEAD of mnvrd's
# built-in one, never both at once.
RECORDER="$ROOT/recorder/recorder.py"
if false && [[ -f "$RECORDER" ]]; then
  warn "Starting recorder (RTSP/UDP -> MP4, 30-day FIFO)..."
  API_URL="http://localhost:${PORT:-3001}" \
  API_USER="admin" API_PASS="Admin@123" \
  STORAGE_PATH="$STORAGE" \
  RECORDINGS_PATH="$STORAGE/recordings" \
  HLS_PATH="$STORAGE/hls" \
  SEGMENT_SECONDS="${SEGMENT_SECONDS:-60}" \
  RETENTION_DAYS="${RETENTION_DAYS:-30}" \
  nohup "$VENV/bin/python" "$RECORDER" > "$LOG_DIR/recorder.log" 2>&1 &
  pid_write "recorder" "$!"
  ok "Recorder PID $(pid_read recorder)  (logs/recorder.log)"
else
  warn "Python recorder disabled (mnvrd's built-in recorder handles this now)"
fi

# C daemon
# DISABLED (camera connections only): today's extensive testing (packet
# captures, side-by-side comparisons) proved GStreamer's rtspsrc has real
# reliability problems against this camera fleet specifically, while
# ffmpeg worked cleanly every single time against the exact same URLs.
# stream_manager.py (started below) now handles live view + recording via
# ffmpeg instead. mnvrd itself is NOT started at all right now, since its
# only other jobs (AI/YOLO detection, health monitoring) aren't worth
# running a second, competing set of camera connections for — re-enable
# by flipping DO_WITH_NVR back on below once/if rtspsrc's reliability
# against this fleet is revisited.
if false && $DO_WITH_NVR; then
  if [[ -f "$NVR_BIN" ]]; then
    sed -i "s|db_path.*=.*|db_path = host=${DB_HOST} port=${DB_PORT} dbname=${DB_NAME} user=${DB_USER} password=${DB_PASSWORD} sslmode=disable|" "$NVR_CONF" 2>/dev/null || true
    sed -i "s|storage_base.*=.*|storage_base = /storage/recordings|" "$NVR_CONF" 2>/dev/null || true
    sed -i "s|hls_base.*=.*|hls_base = /storage/hls|" "$NVR_CONF" 2>/dev/null || true
    nohup "$NVR_BIN" -c "$NVR_CONF" > "$LOG_DIR/nvr.log" 2>&1 &
    pid_write "nvr" "$!"; sleep 1
    pid_alive "nvr" && ok "mnvrd PID $(pid_read nvr)" || { err "mnvrd exited — check logs/nvr.log"; }
  else
    err "mnvrd binary not found — run: bash start.sh --build-nvr"
  fi
else
  warn "mnvrd disabled — using ffmpeg-based stream_manager.py instead (see start.sh comments)"
fi

# ffmpeg-based stream manager — live view (push to MediaMTX) + recording,
# one thread per camera per purpose. Replaces mnvrd's native GStreamer
# camera connections above.
STREAM_MGR="$ROOT/stream_manager.py"
if [[ -f "$STREAM_MGR" ]]; then
  DB_HOST="${DB_HOST}" DB_PORT="${DB_PORT}" DB_NAME="${DB_NAME}" \
  DB_USER="${DB_USER}" DB_PASSWORD="${DB_PASSWORD}" \
  RECORDINGS_PATH="${STORAGE:-/storage}/recordings" \
  SEGMENT_SECONDS="${SEGMENT_SECONDS:-120}" \
  MEDIAMTX_RTSP_HOST="127.0.0.1" MEDIAMTX_RTSP_PORT="8554" \
  nohup "$VENV/bin/python" "$STREAM_MGR" > "$LOG_DIR/stream_manager.log" 2>&1 &
  pid_write "streammgr" "$!"; sleep 1
  pid_alive "streammgr" && ok "stream_manager PID $(pid_read streammgr)  (logs/stream_manager.log)" \
    || err "stream_manager exited — check logs/stream_manager.log"
else
  err "stream_manager.py not found at $STREAM_MGR"
fi

# =============================================================================
# STEP 10 — Frontend
# =============================================================================
hdr "10/10  Frontend"
warn "Starting Vite on :8080..."
cd "$ROOT"
nohup npm run dev > "$LOG_DIR/frontend.log" 2>&1 &
FE_PID=$!; pid_write "frontend" "$FE_PID"
ok "Frontend PID $FE_PID  (logs/frontend.log)"

for i in $(seq 1 40); do
  LAST=$(tail -1 "$LOG_DIR/frontend.log" 2>/dev/null | tr -d '\r' || true)
  [[ -n "$LAST" ]] && printf "\r    [%2ds] %-72s" "$i" "${LAST:0:72}"
  if curl -sf --max-time 2 http://localhost:8080 >/dev/null 2>&1; then
    echo ""; ok "Frontend ready ✓  (${i}s)"; break
  fi
  [[ $i -eq 40 ]] && { echo ""; warn "Frontend still loading — visit http://localhost:8080 shortly"; }
  sleep 1
done

# =============================================================================
# DONE
# =============================================================================
echo ""
echo -e "${C}${BOLD}"
echo "  ╔══════════════════════════════════════════════════════════════╗"
echo "  ║          ✅  Railway mNVR System Running                    ║"
echo "  ╠══════════════════════════════════════════════════════════════╣"
printf "  ║  %-60s║\n" "🌐 Frontend    →  http://localhost:8080"
printf "  ║  %-60s║\n" "🔌 API         →  http://localhost:${PORT:-3001}/api"
printf "  ║  %-60s║\n" "📖 API Docs    →  http://localhost:${PORT:-3001}/api/docs"
$SKIP_AI || printf "  ║  %-60s║\n" "🤖 YOLO AI     →  http://localhost:8000/health"
printf "  ║  %-60s║\n" "📡 MediaMTX    →  http://localhost:9997  (API)"
printf "  ║  %-60s║\n" "🎥 WebRTC      →  http://localhost:8889/cam_<id>/whep"
printf "  ║  %-60s║\n" "📺 HLS         →  http://localhost:8888/cam_<id>/index.m3u8"
printf "  ║  %-60s║\n" "📷 RTSP        →  rtsp://localhost:8554/cam_<id>"
echo "  ╠══════════════════════════════════════════════════════════════╣"
printf "  ║  %-60s║\n" "🔑 Login: admin / Admin@123"
echo "  ╠══════════════════════════════════════════════════════════════╣"
printf "  ║  %-60s║\n" "📋 Logs:    bash start.sh --logs mediamtx"
printf "  ║  %-60s║\n" "📋 Logs:    bash start.sh --logs mtxsync"
printf "  ║  %-60s║\n" "📊 Status:  bash start.sh --status"
printf "  ║  %-60s║\n" "🛑 Stop:    Ctrl+C  or  bash start.sh --stop"
echo "  ╚══════════════════════════════════════════════════════════════╝"
echo -e "${NC}"
echo -e "${Y}  All logs streaming — Ctrl+C to stop everything${NC}"
echo ""

cleanup() {
  echo ""
  warn "Shutting down all services..."
  kill_all
  ok "All stopped. Bye."; exit 0
}
trap cleanup INT TERM

tail -f "$LOG_DIR/api.log" \
        "$LOG_DIR/mediamtx.log" \
        "$LOG_DIR/mtxsync.log" \
        "$LOG_DIR/ai.log" \
        "$LOG_DIR/recorder.log" \
        "$LOG_DIR/frontend.log" 2>/dev/null &
wait