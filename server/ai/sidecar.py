"""
sidecar.py — YOLO detection microservice for the Railway mNVR AI pipeline.

Contract (matches server/src/routes/ai.js exactly):
    POST /detect   multipart form: image=<file>, conf=<float, optional>, model=<str, optional>
                   -> { "detections": [ {label, confidence, bbox:[x1,y1,x2,y2]}, ... ],
                        "image_size": [width, height], "model": "<model file used>" }
    GET  /health   -> { "status": "ok", "device": "cpu"|"cuda", "default_model": "..." }

Run with:  uvicorn sidecar:app --host 0.0.0.0 --port 8000
(this is exactly what start.sh / ai.js already invoke)
"""

import io
import logging
import threading

from fastapi import FastAPI, File, Form, UploadFile, HTTPException
from PIL import Image

logging.basicConfig(level=logging.INFO)
log = logging.getLogger("sidecar")

app = FastAPI(title="mNVR YOLO Sidecar")

DEFAULT_MODEL = "yolov8n.pt"

# Loaded ultralytics YOLO models, keyed by weights filename. Loaded lazily on
# first request and cached — avoids paying the load cost (and the ~500MB
# first-run download start.sh warns about) more than once per model.
_models = {}
_models_lock = threading.Lock()
_device = "cpu"


def _get_model(name: str):
    name = name or DEFAULT_MODEL
    with _models_lock:
        if name not in _models:
            from ultralytics import YOLO  # imported lazily so /health works before it's installed/loaded
            log.info("Loading YOLO model: %s", name)
            _models[name] = YOLO(name)
        return _models[name]


@app.on_event("startup")
def _warm_up():
    global _device
    try:
        import torch
        _device = "cuda" if torch.cuda.is_available() else "cpu"
    except Exception:
        _device = "cpu"
    try:
        _get_model(DEFAULT_MODEL)
    except Exception as e:
        # Don't crash the process if the default weights can't be fetched at
        # startup (e.g. no network yet) — /detect will retry the load and
        # surface a clear error instead of the whole sidecar dying.
        log.warning("Could not pre-load default model %s: %s", DEFAULT_MODEL, e)


@app.get("/health")
def health():
    return {
        "status": "ok",
        "device": _device,
        "default_model": DEFAULT_MODEL,
        "loaded_models": list(_models.keys()),
    }


@app.post("/detect")
async def detect(
    image: UploadFile = File(...),
    conf: float = Form(0.35),
    model: str = Form(None),
):
    model_name = model or DEFAULT_MODEL

    try:
        raw = await image.read()
        pil_img = Image.open(io.BytesIO(raw)).convert("RGB")
    except Exception as e:
        raise HTTPException(status_code=400, detail=f"Invalid image: {e}")

    try:
        yolo = _get_model(model_name)
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Could not load model '{model_name}': {e}")

    try:
        results = yolo.predict(pil_img, conf=conf, verbose=False)
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Inference failed: {e}")

    detections = []
    if results:
        r = results[0]
        names = r.names
        for box in r.boxes:
            x1, y1, x2, y2 = [float(v) for v in box.xyxy[0].tolist()]
            detections.append({
                "label": names[int(box.cls[0])],
                "confidence": float(box.conf[0]),
                "bbox": [x1, y1, x2, y2],
            })

    w, h = pil_img.size
    return {
        "detections": detections,
        "image_size": [w, h],
        "model": model_name,
    }
