# Railway mNVR — Quick Start

## Single command (bare metal)

```bash
# First time only
bash start.sh --setup-db

# Every run
bash start.sh
```

## Single command (Docker)

```bash
docker compose up
```

---

## What starts

| Step | Service | Port | Purpose |
|------|---------|------|---------|
| 1 | PostgreSQL | 5432 | Database |
| 2 | YOLO AI | 8000 | Object detection sidecar |
| 3 | MediaMTX | 8554/8889/8888/9997 | RTSP → WebRTC/HLS transcoder |
| 4 | MTX Sync | — | Reads cameras from DB → creates MediaMTX paths |
| 5 | Node.js API | 3001 | REST API + WebSocket |
| 6 | Recorder | — | Records RTSP/UDP → MP4 + HLS |
| 7 | Frontend | 8080 | React UI |

---

## Access

| URL | What |
|-----|------|
| http://localhost:8080 | Web UI (login: admin / Admin@123) |
| http://localhost:3001/api/docs | Swagger API docs |
| http://localhost:8889/cam_1/whep | Camera 1 WebRTC stream |
| http://localhost:8888/cam_1/index.m3u8 | Camera 1 HLS stream |
| rtsp://localhost:8554/cam_1 | Camera 1 RTSP re-stream |
| http://localhost:9997/v3/paths/list | MediaMTX path list |

---

## How MediaMTX integration works

```
Camera RTSP (192.168.1.x:554)
        │  UDP ingest
        ▼
MediaMTX (:8554)
  ├── /cam_1  ← reads from rtsp://admin:bel123456@192.168.1.100:554/stream1
  ├── /cam_2
  └── /cam_N
        │
  ┌─────┴──────────────────────────┐
  │                                │
WebRTC WHEP (:8889)          HLS (:8888)
/cam_N/whep                  /cam_N/index.m3u8
  │                                │
Browser (Camera Grid)        Browser (fallback)
~200ms latency               ~2s latency

mediamtx-sync.py polls DB every 15s
→ creates/removes paths automatically
→ new cameras appear in 15s, deleted cameras cleaned up
```

---

## Camera stream URLs per camera_id

```
camera_id = 1  →  cam_1
  WebRTC:  http://localhost:8889/cam_1/whep
  HLS:     http://localhost:8888/cam_1/index.m3u8
  RTSP:    rtsp://localhost:8554/cam_1

camera_id = 5  →  cam_5
  WebRTC:  http://localhost:8889/cam_5/whep
  HLS:     http://localhost:8888/cam_5/index.m3u8
```

---

## CLI commands

```bash
bash start.sh --status          # check all services
bash start.sh --stop            # stop everything
bash start.sh --logs mediamtx   # MediaMTX logs
bash start.sh --logs mtxsync    # sync service logs
bash start.sh --logs api        # API logs
bash start.sh --logs ai         # YOLO logs
bash start.sh --logs recorder   # recorder logs
bash start.sh --build-nvr       # compile C daemon
bash start.sh --with-nvr        # start with C daemon
bash start.sh --skip-ai         # start without YOLO
```

---

## Docker tips

```bash
docker compose up -d                    # background
docker compose logs -f mediamtx        # MediaMTX logs
docker compose logs -f mtxsync         # sync logs
docker compose logs -f recorder        # recorder logs
docker compose ps                      # health of all containers
docker compose down                    # stop
docker compose down -v                 # stop + wipe volumes
```

### Port 5432 conflict fix
```bash
# Option A (default): DB uses port 15432 on host
psql -h localhost -p 15432 -U mnvr -d mnvr

# Option B: remove ports block from postgres in docker-compose.yml
```

---

## Adding a new camera

1. Go to **Camera Grid** → **Add Camera**
2. Enter IP, RTSP URL, username, password
3. Wait ~15 seconds — MediaMTX sync creates the stream path automatically
4. Live WebRTC tile appears in the grid

---

## AI detection

- **Camera Grid tiles**: hover → click AI button for per-tile overlay
- **Video Player**: click **Enable AI** → select mode (Object Detection / People Count / Intrusion)
- Results shown in the right-hand panel in real-time
- YOLO runs at ~500ms inference on CPU, ~50ms on GPU

---

## mNVR C Daemon

```bash
# Build
bash start.sh --build-nvr

# Start with daemon
bash start.sh --with-nvr

# Manual (after build)
cd nvr_core && ./build/mnvrd -c config/mnvr.conf

# UDP streams from daemon: udp://127.0.0.1:<5000 + camera_id*2>
# camera_id=1 → udp://127.0.0.1:5002
# camera_id=2 → udp://127.0.0.1:5004
```
