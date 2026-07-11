# mNVR - Mobile Network Video Recorder

**Version:** 1.0.0 | **Platform:** Linux (Ubuntu 22.04+ / embedded ARM/x86) | **Language:** C11
**Database:** PostgreSQL 14+  | **Build status:** 0 errors 0 warnings (gcc -Wall -Wextra)

---

## Quick Start

### 1. Install dependencies

```bash
sudo apt install \
  libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-libav \
  libpq-dev \
  libmicrohttpd-dev \
  postgresql

# libmicrohttpd-dev is optional - enables the REST API.
# Without it the system still records, runs HLS, and runs all other modules.
```

### 2. Set up PostgreSQL

```bash
# Create database, user, and apply schema
sudo -u postgres psql << 'SQL'
CREATE USER mnvr WITH PASSWORD 'mnvr';
CREATE DATABASE mnvr OWNER mnvr;
GRANT ALL PRIVILEGES ON DATABASE mnvr TO mnvr;
SQL

# Apply the schema
psql -h localhost -U mnvr -d mnvr -f database_pg.sql
```

### 3. Configure

```bash
sudo mkdir -p /etc/mnvr /storage/recordings /storage/hls /var/log/mnvr
sudo cp config/mnvr.conf.example /etc/mnvr/mnvr.conf
# Edit /etc/mnvr/mnvr.conf:
#   db_path = host=localhost port=5432 dbname=mnvr user=mnvr password=mnvr sslmode=disable
```

### 4. Build

```bash
make
# Prints:  REST API: ENABLED  (libmicrohttpd found)
# Binary:  build/mnvrd  (108K)
```

### 5. Run

```bash
# Direct run
./build/mnvrd \
  -c /etc/mnvr/mnvr.conf \
  -s database_pg.sql \
  -d "host=localhost port=5432 dbname=mnvr user=mnvr password=mnvr sslmode=disable"

# Or as a system service
sudo make install
sudo systemctl enable --now mnvrd
```

### Command-line options

```
  -c <path>   Path to mnvr.conf               (default: /etc/mnvr/mnvr.conf)
  -s <path>   Path to database_pg.sql schema  (default: /etc/mnvr/database.sch)
  -d <str>    PostgreSQL conninfo string       (default: host=localhost dbname=mnvr user=mnvr)
  -l <level>  Log level: 0=trace 1=debug 2=info 3=warn 4=error 5=fatal
  -h          Show help
```

---

## Project Structure

```
mnvr/
├── Makefile                              Top-level build
├── database_pg.sql                       PostgreSQL schema (50+ tables, triggers, views)
├── README.md
├── config/
│   └── mnvr.conf.example                 Configuration template
├── deploy/
│   └── mnvrd.service                     systemd unit file
└── src/
    ├── include/
    │   └── mnvr_system.h                 AppContext, shared types, LOG_* macros
    ├── core/
    │   └── main.c                        Orchestrator: module wiring, signal handlers
    ├── modules/
    │   ├── logger/     logger.h/.c       Async ring-buffer logger, file rotation
    │   ├── config/     config_module.h/.c  INI file + PostgreSQL system_config overlay
    │   ├── onvif/      onvif_module.h/.c   WS-Discovery + ONVIF SOAP (stream URI, PTZ)
    │   ├── recorder/
    │   │   ├── recorder_module.h/.c      Adapter wrapping the original GStreamer module
    │   │   └── original_gstreamer_module/  Original standalone recorder (compiled as-is)
    │   ├── hls/        hls_module.h/.c   MP4->HLS segmenter (.ts + sliding m3u8 playlist)
    │   ├── streamer/   streamer_module.h/.c  RTSP re-stream + decoded frame tap for AI
    │   ├── ai/         ai_module.h/.c    Motion detection (live) + face/RDAS stubs
    │   ├── health/     health_module.h/.c  CPU/RAM/disk/camera watchdog + storage cleanup
    │   └── api/        api_module.h/.c   REST API (libmicrohttpd, optional) + HLS serve
    └── db/
        ├── db_module.h                   PostgreSQL module header (libpq)
        └── db_module.c                   libpq implementation with async write queue
```

---

## Database Migration: SQLite -> PostgreSQL

All SQLite-specific constructs have been converted:

| SQLite | PostgreSQL |
|--------|-----------|
| `INTEGER PRIMARY KEY AUTOINCREMENT` | `SERIAL PRIMARY KEY` |
| `REAL` | `DOUBLE PRECISION` |
| `BLOB` | `BYTEA` |
| `datetime(X, 'unixepoch')` | `to_timestamp(X)` |
| `datetime('now', '-24 hours')` | `NOW() - INTERVAL '24 hours'` |
| `julianday()` arithmetic | `EXTRACT(EPOCH FROM ...)` |
| `last_insert_rowid()` | `LASTVAL()` or `RETURNING id` |
| `strftime('%s','now')` | `EXTRACT(EPOCH FROM NOW())::BIGINT` |
| `instr(str, ch)` | `position(ch IN str)` |
| `CREATE VIEW IF NOT EXISTS` | `CREATE OR REPLACE VIEW` |
| `CREATE TRIGGER IF NOT EXISTS ... BEGIN...END` | `CREATE OR REPLACE FUNCTION` + `CREATE OR REPLACE TRIGGER ... EXECUTE FUNCTION` |
| `PRAGMA journal_mode=WAL` etc. | Removed (configure in `postgresql.conf`) |

### C code migration (db_module.c + config_module.c)

| SQLite API | PostgreSQL libpq API |
|-----------|---------------------|
| `#include <sqlite3.h>` | `#include <libpq-fe.h>` |
| `sqlite3_open_v2(path, &db, ...)` | `PQconnectdb(conninfo)` |
| `sqlite3_exec(db, sql, cb, ud, &err)` | `PQexec(conn, sql)` + `PQresultStatus()` |
| `SQLITE_OK` | `PGRES_COMMAND_OK` / `PGRES_TUPLES_OK` |
| `sqlite3_stmt` / `sqlite3_prepare_v2` | `PGresult` / `PQexec` |
| `sqlite3_step` / `sqlite3_column_text` | `PQntuples` / `PQgetvalue` |
| `sqlite3_last_insert_rowid` | `RETURNING id` clause in INSERT |
| `PRAGMA journal_mode=WAL` | Handled by PostgreSQL server |
| `PRAGMA wal_checkpoint(FULL)` | Not needed (auto-managed) |
| `sqlite3_close` | `PQfinish` |

---

## PostgreSQL conninfo string

The `-d` option and `db_path` in `mnvr.conf` now accept a **libpq connection string**:

```bash
# TCP connection
host=localhost port=5432 dbname=mnvr user=mnvr password=secret sslmode=disable

# Unix socket (faster, same machine)
host=/var/run/postgresql dbname=mnvr user=mnvr

# With SSL
host=10.0.0.5 port=5432 dbname=mnvr user=mnvr password=secret sslmode=require

# Using environment variables (alternative - no -d needed)
export PGHOST=localhost PGPORT=5432 PGDATABASE=mnvr PGUSER=mnvr PGPASSWORD=secret
```

---

## Thread Map

| Thread | Loop | Module | Interval |
|--------|------|--------|----------|
| `logger_drain_thread` | `while(running)` | LoggerModule | condvar |
| `db_writer_thread` | `while(running)` | DbModule | condvar - async PG writes |
| `discovery_thread_fn` | `while(running)` | OnvifModule | 60 s |
| `health_poll_thread` | `while(running)` | HealthModule | 10 s |
| `recorder_thread` x N | `g_main_loop_run()` | RecorderModule | GLib event loop |
| `hls_worker_thread` x N | `while(running)` | HlsModule | condvar |
| `streamer_thread_fn` x N | `g_main_loop_run()` | StreamerModule | GLib event loop |
| `ai_worker_thread` x N | `while(running)` | AiModule | condvar |
| MHD thread pool | poll() | ApiModule | network I/O |
| `api_heartbeat_thread` | `while(running)` | ApiModule | 5 s |
| **main thread** | `pthread_cond_timedwait` | - | 30 s |

---

## REST API Endpoints (port 8443)

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/v1/system/status` | CPU%, RAM%, disk%, device info |
| GET | `/api/v1/cameras` | Camera list with state and stream URLs |
| GET | `/api/v1/recordings` | Latest 200 recordings from PostgreSQL |
| GET | `/api/v1/events` | Latest 200 AI events from PostgreSQL |
| GET | `/hls/cam_{id}/stream.m3u8` | Live HLS playlist |
| GET | `/hls/cam_{id}/<seg>.ts` | HLS transport stream segments |

---

## PostgreSQL Recommended Settings (postgresql.conf)

For embedded NVR use on a dedicated machine:

```ini
shared_buffers = 256MB
work_mem = 4MB
synchronous_commit = off       # Safe for NVR - minor data loss on crash acceptable
wal_level = minimal
max_wal_size = 1GB
checkpoint_completion_target = 0.9
log_min_duration_statement = 1000   # Log slow queries >1s
```

---

## Startup / Shutdown Order

```
STARTUP
  1  gst_init()
  2  signal handlers (SIGINT / SIGTERM / SIGHUP)
  3  logger_start()          -> logger_drain_thread
  4  config_load()           (mnvr.conf + PostgreSQL system_config table)
  5  db_module_start()       -> connect PostgreSQL, apply database_pg.sql, db_writer_thread
  6  config_load_cameras()   (SELECT FROM cameras WHERE status='ACTIVE')
  7  onvif_module_start()    -> discovery_thread_fn (WS-Discovery every 60s)
  8  health_module_start()   -> health_poll_thread (every 10s)
  9  recorder_module_start() -> recorder_thread x N (original GStreamer, MP4 segments)
 10  hls_module_start()      -> hls_worker_thread x N (MP4 -> .ts + m3u8)
 11  streamer_module_start() -> streamer_thread_fn x N (RTSP re-stream + AI frame tap)
 12  ai_module_start()       -> ai_worker_thread x N (motion detect, face/RDAS stubs)
 13  api_module_start()      -> MHD HTTP server + heartbeat_thread
 14  Main loop               (30s timedwait: config reload on SIGHUP, shutdown on SIGINT)

SHUTDOWN  (SIGINT / SIGTERM)
  api_stop -> ai_stop -> streamer_stop -> hls_stop -> recorder_stop
  -> onvif_stop -> health_stop -> db_stop (PQfinish) -> logger_flush -> logger_stop
  gst_deinit()
```
