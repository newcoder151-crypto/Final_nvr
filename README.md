# 🚂 Railway NVR — Full-Stack mNVR System

AI-powered Mobile Network Video Recorder for Indian Railways.  
**Zero Supabase. Pure Node.js + PostgreSQL (mNVR schema) + React + YOLO.**

---

## Quick Start (Development)

### Prerequisites

- Node.js 20+
- PostgreSQL 14+ running locally
- Python 3.9+ (for AI sidecar, optional)

### 1. Database Setup

```bash
# Create user & database (run as postgres superuser)
psql -U postgres -c "CREATE USER mnvr WITH PASSWORD 'mnvrpass';"
psql -U postgres -c "CREATE DATABASE mnvr OWNER mnvr;"

# Apply mNVR schema
psql -U mnvr -d mnvr -f server/schema.sql
```

### 2. One-Command Start

```bash
# Start everything (API + frontend)
bash start.sh

# With AI sidecar
bash start.sh --with-ai

# First time: also creates DB + applies schema
bash start.sh --setup-db --with-ai
```

### 3. Access

| Service    | URL                            |
| ---------- | ------------------------------ |
| Frontend   | http://localhost:8080          |
| API        | http://localhost:3001/api      |
| API Docs   | http://localhost:3001/api/docs |
| AI Sidecar | http://localhost:8000          |

**Default login:** `admin` / `Admin@123` ← change on first login!

---

## Docker (Production)

```bash
# Start everything with Docker Compose
docker compose up -d

# View logs
docker compose logs -f api

# Stop
docker compose down
```

---

## Architecture

```
Browser (React + Vite :8080)
         │ REST + WebSocket
         ▼
Node.js API (:3001)
  ├── /api/auth        JWT auth (bcrypt + pg users table)
  ├── /api/cameras     Camera CRUD + health metrics
  ├── /api/recordings  Recording metadata + list
  ├── /api/streaming   MP4 byte-range + HLS m3u8 serving
  ├── /api/events      Alerts, ack, batch-ack
  ├── /api/users       User management (ADMIN only)
  ├── /api/config      System config + dashboard stats
  └── /api/ai          Proxy to Python YOLO sidecar
         │
         ├── PostgreSQL (mNVR schema — 50+ tables)
         │
         └── Python FastAPI YOLO Sidecar (:8000)
               ├── /detect          Generic detection
               ├── /people-count    Crowd density
               └── /intrusion       Zone breach
```

---

## AI Features

| Feature             | API Endpoint                | Description                       |
| ------------------- | --------------------------- | --------------------------------- |
| Object Detection    | `POST /api/ai/detect`       | Detect any COCO object in a frame |
| People Count        | `POST /api/ai/people-count` | Count people + density %          |
| Crowd Density       | `POST /api/ai/people-count` | LOW/MEDIUM/HIGH with auto event   |
| Intrusion Detection | `POST /api/ai/intrusion`    | Zone breach detection, auto event |
| Live AI Overlay     | VideoPlayer → Enable AI     | Real-time overlay on video        |
| Analytics           | `GET /api/ai/analytics`     | Historical event stats            |

---

## Video Playback

Recordings are served with **byte-range support** for seeking:

```
GET /api/streaming/recordings/:id/stream?token=<JWT>
```

HLS live streams:

```
GET /api/streaming/hls/:cameraId/stream.m3u8?token=<JWT>
```

Place MP4 files in `storage/recordings/` and they will be served automatically.  
HLS segments go in `storage/hls/cam_<id>/`.

---

## Storage Layout

```
storage/
├── recordings/
│   ├── cam_1/
│   │   ├── recording_001.mp4
│   │   └── recording_001.jpg  ← thumbnail
│   └── cam_2/
└── hls/
    ├── cam_1/
    │   ├── stream.m3u8
    │   └── seg_00001.ts
    └── cam_2/
```

---

## Environment Variables

### server/.env

| Key                | Default                 | Description         |
| ------------------ | ----------------------- | ------------------- |
| `DB_HOST`          | `localhost`             | PostgreSQL host     |
| `DB_NAME`          | `mnvr`                  | Database name       |
| `DB_USER`          | `mnvr`                  | DB user             |
| `DB_PASSWORD`      | `mnvrpass`              | DB password         |
| `JWT_SECRET`       | _(must change)_         | JWT signing secret  |
| `PORT`             | `3001`                  | API server port     |
| `YOLO_SIDECAR_URL` | `http://localhost:8000` | Python sidecar      |
| `RECORDINGS_PATH`  | `./storage/recordings`  | MP4 storage         |
| `HLS_PATH`         | `./storage/hls`         | HLS segment storage |

### .env (frontend)

| Key            | Default                 | Description |
| -------------- | ----------------------- | ----------- |
| `VITE_API_URL` | `http://localhost:3001` | Backend URL |

---

## Roles

| Role          | Permissions                          |
| ------------- | ------------------------------------ |
| `ADMIN`       | Full access — users, cameras, config |
| `OPERATOR`    | Create events, update cameras        |
| `MAINTENANCE` | Read + system config                 |
| `VIEWER`      | Read-only                            |

=======

# complete
