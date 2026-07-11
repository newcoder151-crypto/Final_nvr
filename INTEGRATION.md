# Railway mNVR — Integration & Operations Guide

## Single Command Launch

```bash
# First-time setup (creates DB, applies schema, seeds admin user)
bash start.sh --setup-db

# Normal startup (starts AI + API + Frontend, all services)
bash start.sh

# With mNVR C daemon (needs --build-nvr first)
bash start.sh --with-nvr

# Build the C daemon
bash start.sh --build-nvr

# Build C daemon + start everything
bash start.sh --build-nvr && bash start.sh --with-nvr
```

## Service URLs

| Service     | URL                              |
|-------------|----------------------------------|
| Frontend    | http://localhost:8080            |
| API         | http://localhost:3001/api        |
| API Docs    | http://localhost:3001/api/docs   |
| YOLO AI     | http://localhost:8000            |

Default login: `admin` / `Admin@123`

---

## Operations Commands

```bash
bash start.sh --status      # show which services are running
bash start.sh --stop        # stop all services gracefully
bash start.sh --logs api    # tail API logs
bash start.sh --logs ai     # tail YOLO sidecar logs
bash start.sh --logs frontend # tail frontend logs
```

---

## mNVR Core Daemon (C binary)

### Why it's separate
The `nvr_core` C program (`mnvrd`) is the low-level recorder that talks RTSP, writes MP4 segments, generates HLS, and speaks ONVIF. It runs separately from the Node.js API.

### Build
```bash
# Install dependencies (Ubuntu/Debian)
sudo apt install -y \
  gcc make pkg-config \
  libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-libav \
  libpq-dev \
  libmicrohttpd-dev

# Build (output: nvr_core/build/mnvrd)
bash start.sh --build-nvr
```

### Run manually (after building)
```bash
# start.sh patches the config automatically, but you can also run directly:
cd nvr_core
./build/mnvrd -c config/mnvr.conf
```

### Configure cameras (ONVIF)
Edit `nvr_core/config/mnvr.conf` — uncomment and fill in camera slots:
```ini
onvif_cam_1_ip       = 192.168.1.101
onvif_cam_1_user     = admin
onvif_cam_1_pass     = your_password
onvif_cam_1_type     = INTERIOR
onvif_cam_1_location = Coach S1 Front
```

The daemon will auto-register cameras into the PostgreSQL DB.

---

## Docker Deployment

### Start with Docker
```bash
# Build and start all services (Postgres + API + YOLO + Frontend)
docker compose up -d

# First time — check logs to confirm DB is ready
docker compose logs -f postgres

# Check all services are healthy
docker compose ps

# View logs
docker compose logs -f api
docker compose logs -f yolo
docker compose logs -f frontend
```

### Access
- Frontend: http://localhost:8080
- API Docs: http://localhost:3001/api/docs

### Stop
```bash
docker compose down          # stop containers
docker compose down -v       # stop + delete all data volumes (CAREFUL)
```

### Production Docker Tips
```bash
# Set a strong JWT secret
export JWT_SECRET="your-very-long-random-secret-here"
docker compose up -d

# Scale the API (multiple workers)
docker compose up -d --scale api=2

# Rebuild after code changes
docker compose up -d --build
```

---

## Database

### Apply schema manually
```bash
psql -U mnvr -d mnvr -f server/schema.sql
```

### Seed sample data
```bash
cd server && node seed.js
```

### Reset database
```bash
psql -U postgres -c "DROP DATABASE mnvr;"
psql -U postgres -c "CREATE DATABASE mnvr OWNER mnvr;"
psql -U mnvr -d mnvr -f server/schema.sql
cd server && node seed.js
```

---

## AI Features

The YOLO sidecar (`server/ai/sidecar.py`) starts automatically with `bash start.sh`.

### Railway-specific detections
| Feature | Trigger | Severity |
|---------|---------|----------|
| Crowd density | ≥5 persons in frame | WARNING |
| Person fallen | Horizontal bounding box | CRITICAL |
| Smoke/fire | `fire`/`smoke` class detected | CRITICAL |
| Mobile phone | `cell phone` class | WARNING |
| Animal on track | `cow`/`dog`/`horse` etc. | WARNING |
| Intrusion | Person in restricted zone | CRITICAL |
| Obstacle | Vehicle/debris on track | CRITICAL |

All detections auto-create events in the DB and push via WebSocket.

### AI Page
The AI Analytics page (`/ai/analytics`) automatically discovers all active cameras and runs live AI on each tile. No camera selection needed — toggle AI per tile or enable all cameras at once.

---

## Smart Search

The search page (`/search`) supports natural language:
- `"man in red shirt"` → searches event descriptions and tags
- `"crowd near door yesterday"` → CROWD_DENSITY + DOOR camera + last 24h
- `"smoke passenger area morning"` → SMOKE events + INTERIOR + 06:00-12:00
- `"camera 3 intrusion"` → INTRUSION events from camera_id=3
- `"animal obstacle front camera"` → ANIMAL_DETECTION + DRIVER_CAB

---

## Storage Layout

```
storage/
├── recordings/
│   ├── cam_1/
│   │   ├── recording_1234567890.mp4
│   │   └── recording_1234567890.jpg    ← thumbnail
│   └── cam_2/
└── hls/
    ├── cam_1/
    │   ├── stream.m3u8
    │   ├── seg00001.ts
    │   └── ...
    └── cam_2/
```

Videos are served at:
```
GET /api/streaming/recordings/:id/stream?token=<JWT>   ← byte-range MP4
GET /api/streaming/hls/:camId/stream.m3u8?token=<JWT>  ← HLS live
GET /api/streaming/recordings/:id/download?token=<JWT>  ← download
```
