# YOLO Sidecar (Python)

A tiny FastAPI service that runs Ultralytics YOLOv8 and returns detections.
The Node server proxies `/api/ai/detect` → this sidecar.

## Setup (one-time)

```bash
cd server/ai
python3 -m venv .venv
source .venv/bin/activate          # Windows: .venv\Scripts\activate
pip install -r requirements.txt
```

## Run

```bash
uvicorn sidecar:app --host 0.0.0.0 --port 8000 --reload
```

First request downloads `yolov8n.pt` (~6 MB) automatically. To pre-download
a heavier model, set `YOLO_MODEL=yolov8s.pt` (or `yolov8m.pt`, `yolov8l.pt`,
`yolov8x.pt`) before starting.

## Test directly

```bash
curl -F image=@/path/to/frame.jpg -F conf=0.4 http://localhost:8000/detect
```

## Test via Node proxy (requires JWT)

```bash
TOKEN="<your-supabase-jwt>"
curl -H "Authorization: Bearer $TOKEN" \
     -F image=@frame.jpg \
     http://localhost:8080/api/ai/detect
```

## Configuration (env vars on the Node server)

- `YOLO_SIDECAR_URL` — defaults to `http://localhost:8000`.
- `YOLO_MODEL` — model file the sidecar loads by default.

## Use cases wired across the project

- **Live camera grid** — sample 1 frame/sec from each `<video>`, POST to
  `/api/ai/detect`, draw boxes on overlay. (Recommended cap: 4 cameras at a time.)
- **Recorded playback (Video Player page)** — click "AI Motion Zoom" to scan
  the current frame and auto-zoom to the highest-confidence detection.
- **Search / archived footage** — extract a thumbnail per recording, run
  detection, store labels in `recordings.event_data` for semantic filtering.
- **Event triggers** — when motion detection (existing) flags a segment,
  call YOLO once to classify (`person`, `bag`, `fall`, etc.) and write to
  `events.event_subtype`.

## GPU note

If the host has CUDA, `ultralytics` auto-uses GPU. On CPU, expect
~50–80 ms/frame for `yolov8n` at 640×480.
