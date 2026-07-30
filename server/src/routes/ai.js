const express = require('express');
const multer = require('multer');
const axios = require('axios');
const FormData = require('form-data');
const { query } = require('../db');
const { authenticate } = require('../middleware/auth');
const { broadcast } = require('../websocket');

const router = express.Router();

// OWASP A08 (Software & Data Integrity) / A04 (Insecure Design): only accept
// image content-types, and only via multer's in-memory buffer (never written
// to disk under a client-controlled name, so there's no path-traversal
// surface on upload).
const ALLOWED_IMAGE_TYPES = new Set(['image/jpeg', 'image/png', 'image/webp', 'image/bmp']);
const upload = multer({
  storage: multer.memoryStorage(),
  limits: { fileSize: 25 * 1024 * 1024, files: 1 },
  fileFilter: (req, file, cb) => {
    if (!ALLOWED_IMAGE_TYPES.has(file.mimetype)) {
      return cb(new Error('Only JPEG, PNG, WEBP, or BMP images are accepted'));
    }
    cb(null, true);
  },
});

const YOLO_URL = () => process.env.YOLO_SIDECAR_URL || 'http://localhost:8000';

// Curated list of Ultralytics YOLO weights known to work with sidecar.py's
// `YOLO(name)` loader. Defined once, above its first use, so both the model
// whitelist check and the /models endpoint share a single source of truth.
const AVAILABLE_AI_MODELS = [
  { value: 'yolov8n.pt', label: 'YOLOv8 Nano',    note: 'Fastest, lowest accuracy — good for low-power/edge cameras' },
  { value: 'yolov8s.pt', label: 'YOLOv8 Small',   note: 'Balanced speed/accuracy' },
  { value: 'yolov8m.pt', label: 'YOLOv8 Medium',  note: 'Higher accuracy, more CPU/GPU load' },
  { value: 'yolov8l.pt', label: 'YOLOv8 Large',   note: 'High accuracy, GPU recommended' },
  { value: 'yolov8x.pt', label: 'YOLOv8 X-Large', note: 'Highest accuracy, GPU required for real-time use' },
  { value: 'yolo11n.pt', label: 'YOLO11 Nano',    note: 'Newer architecture, fastest variant' },
  { value: 'yolo11s.pt', label: 'YOLO11 Small',   note: 'Newer architecture, balanced' },
];
const ALLOWED_MODEL_NAMES = new Set(AVAILABLE_AI_MODELS.map((m) => m.value));

// OWASP A03/A08: `model` used to be forwarded to the Python sidecar's
// `YOLO(name)` loader completely unchecked. Ultralytics treats that string
// as either a known weight name (auto-downloaded from the hub) OR a local
// file path — an attacker-controlled value here could make the sidecar load
// an arbitrary path on its filesystem, or trigger unbounded outbound
// downloads. Pin it to the same curated whitelist the UI offers.
function safeModel(requested, fallback) {
  if (requested && ALLOWED_MODEL_NAMES.has(String(requested))) return String(requested);
  return fallback;
}

function safeConfidence(requested, fallback) {
  const n = parseFloat(requested);
  return Number.isFinite(n) && n >= 0 && n <= 1 ? n : fallback;
}

const CAMERA_ID_RE = /^\d+$/;
function safeCameraId(id) {
  return CAMERA_ID_RE.test(String(id)) ? String(id) : null;
}

// Helper: proxy frame to YOLO sidecar
async function callYolo(imageBuffer, filename, mimetype, conf, model) {
  const fd = new FormData();
  fd.append('image', imageBuffer, { filename: 'frame.jpg', contentType: mimetype });
  if (conf !== undefined) fd.append('conf', String(conf));
  if (model) fd.append('model', String(model));
  const r = await axios.post(`${YOLO_URL()}/detect`, fd, {
    headers: fd.getHeaders(),
    maxBodyLength: 30 * 1024 * 1024,
    timeout: 30_000,
  });
  return r.data;
}

// Multer errors (oversized file, bad mimetype) land in Express's error
// handler unless we catch them per-route — wrap the upload middleware so we
// return a clean 400 instead of a generic 500.
function uploadImage(req, res, next) {
  upload.single('image')(req, res, (err) => {
    if (err) return res.status(400).json({ error: err.message });
    next();
  });
}

// POST /api/ai/detect — generic YOLO detection (proxy to Python sidecar)
router.post('/detect', authenticate, uploadImage, async (req, res) => {
  if (!req.file) return res.status(400).json({ error: 'image file required' });
  try {
    const data = await callYolo(
      req.file.buffer, req.file.originalname, req.file.mimetype,
      safeConfidence(req.body.conf, undefined), safeModel(req.body.model, undefined),
    );
    res.json(data);
  } catch (err) {
    console.error('[ai:detect]', err.message);
    res.status(err.response?.status || 502).json({
      error: 'AI detection service unavailable',
      hint: `Is the Python sidecar running at ${YOLO_URL()}?`,
    });
  }
});

// POST /api/ai/people-count — unique person detection + density
router.post('/people-count', authenticate, uploadImage, async (req, res) => {
  if (!req.file) return res.status(400).json({ error: 'image file required' });
  try {
    const data = await callYolo(
      req.file.buffer, req.file.originalname, req.file.mimetype,
      safeConfidence(req.body.conf, 0.4), safeModel(req.body.model, 'yolov8n.pt'),
    );
    const people = (data.detections || []).filter(d => d.label === 'person');
    const count = people.length;
    const [W, H] = data.image_size || [0, 0];
    const frameArea = W * H;
    const personArea = people.reduce((sum, d) => sum + (d.bbox[2]-d.bbox[0]) * (d.bbox[3]-d.bbox[1]), 0);
    const density = frameArea > 0 ? Math.min(100, Math.round((personArea / frameArea) * 300)) : 0;
    const level = density > 60 ? 'HIGH' : density > 30 ? 'MEDIUM' : 'LOW';

    const cameraId = safeCameraId(req.body.camera_id);
    if (level === 'HIGH' && cameraId) {
      await query(
        `INSERT INTO events(event_type, title, severity, camera_id, description, event_data, occurred_at)
         VALUES('CROWD_DENSITY','High crowd density detected','WARNING',$1,$2,$3,NOW())`,
        [cameraId, `${count} persons detected, density ${density}%`,
         JSON.stringify({ count, density, level })]);
      broadcast({ type: 'event.new', data: { event_type: 'CROWD_DENSITY', severity: 'WARNING', camera_id: cameraId, count, density } });
    }

    res.json({ ...data, people_count: count, density_percent: density, density_level: level, people_detections: people });
  } catch (err) {
    console.error('[ai:people-count]', err.message);
    res.status(err.response?.status || 502).json({ error: 'AI detection service unavailable' });
  }
});

// POST /api/ai/intrusion — zone intrusion detection
router.post('/intrusion', authenticate, uploadImage, async (req, res) => {
  if (!req.file) return res.status(400).json({ error: 'image file required' });
  try {
    // zone: {x1,y1,x2,y2} as percentages of frame (0-100). Parse defensively
    // — this is client-supplied JSON — and clamp to the valid range so a
    // malformed/hostile payload can't produce NaN/Infinity math downstream.
    let zone = { x1: 0, y1: 0, x2: 100, y2: 100 };
    if (req.body.zone) {
      try {
        const parsed = JSON.parse(req.body.zone);
        const clamp = (v, d) => (Number.isFinite(v) ? Math.min(100, Math.max(0, v)) : d);
        zone = {
          x1: clamp(parsed.x1, 0), y1: clamp(parsed.y1, 0),
          x2: clamp(parsed.x2, 100), y2: clamp(parsed.y2, 100),
        };
      } catch {
        return res.status(400).json({ error: 'zone must be valid JSON: {x1,y1,x2,y2}' });
      }
    }

    const data = await callYolo(
      req.file.buffer, req.file.originalname, req.file.mimetype,
      safeConfidence(req.body.conf, 0.4), safeModel(req.body.model, undefined),
    );
    const [W, H] = data.image_size || [0, 0];
    const zx1 = (zone.x1 / 100) * W, zy1 = (zone.y1 / 100) * H;
    const zx2 = (zone.x2 / 100) * W, zy2 = (zone.y2 / 100) * H;

    const intruders = (data.detections || []).filter(d => {
      const cx = (d.bbox[0] + d.bbox[2]) / 2;
      const cy = (d.bbox[1] + d.bbox[3]) / 2;
      return cx >= zx1 && cx <= zx2 && cy >= zy1 && cy <= zy2;
    });

    const intrusionDetected = intruders.length > 0;
    const cameraId = safeCameraId(req.body.camera_id);
    if (intrusionDetected && cameraId) {
      const title = `Intrusion detected: ${intruders.length} object(s) in restricted zone`;
      const severity = intruders.some(d => d.label === 'person') ? 'CRITICAL' : 'WARNING';
      await query(
        `INSERT INTO events(event_type, title, severity, camera_id, description, event_data, occurred_at)
         VALUES('INTRUSION',$1,$2,$3,$4,$5,NOW())`,
        [title, severity, cameraId, `Zone: ${JSON.stringify(zone)}`,
         JSON.stringify({ zone, intruders })]);
      broadcast({ type: 'event.new', data: { event_type: 'INTRUSION', severity, camera_id: cameraId, count: intruders.length } });
    }

    res.json({ ...data, zone, intrusion_detected: intrusionDetected, intruder_count: intruders.length, intruders });
  } catch (err) {
    console.error('[ai:intrusion]', err.message);
    res.status(err.response?.status || 502).json({ error: 'AI detection service unavailable' });
  }
});

// POST /api/ai/object-detect — full object detection with class filtering
router.post('/object-detect', authenticate, uploadImage, async (req, res) => {
  if (!req.file) return res.status(400).json({ error: 'image file required' });
  try {
    const data = await callYolo(
      req.file.buffer, req.file.originalname, req.file.mimetype,
      safeConfidence(req.body.conf, 0.35), safeModel(req.body.model, undefined),
    );
    const filterClass = typeof req.body.filter_class === 'string' ? req.body.filter_class.slice(0, 64) : null;
    const filtered = filterClass
      ? (data.detections || []).filter(d => d.label.toLowerCase() === filterClass.toLowerCase())
      : (data.detections || []);
    const summary = filtered.reduce((acc, d) => { acc[d.label] = (acc[d.label] || 0) + 1; return acc; }, {});
    res.json({ ...data, detections: filtered, summary, total_detected: filtered.length });
  } catch (err) {
    console.error('[ai:object-detect]', err.message);
    res.status(err.response?.status || 502).json({ error: 'AI detection service unavailable' });
  }
});

// GET /api/ai/analytics — AI event stats from DB
router.get('/analytics', authenticate, async (req, res) => {
  try {
    const parsedSince = req.query.since ? new Date(req.query.since) : new Date(Date.now() - 24 * 3600 * 1000);
    const since = Number.isNaN(parsedSince.getTime()) ? new Date(Date.now() - 24 * 3600 * 1000) : parsedSince;
    const [byType, bySeverity, byCamera, hourly] = await Promise.all([
      query(`SELECT event_type, COUNT(*) as count FROM events WHERE occurred_at>=$1 GROUP BY event_type ORDER BY count DESC`, [since]),
      query(`SELECT severity, COUNT(*) as count FROM events WHERE occurred_at>=$1 GROUP BY severity`, [since]),
      query(`SELECT e.camera_id, c.camera_name, COUNT(*) as count FROM events e LEFT JOIN cameras c ON e.camera_id=c.camera_id WHERE e.occurred_at>=$1 GROUP BY e.camera_id, c.camera_name ORDER BY count DESC LIMIT 10`, [since]),
      query(`SELECT date_trunc('hour', occurred_at) as hour, severity, COUNT(*) as count FROM events WHERE occurred_at>=$1 GROUP BY hour, severity ORDER BY hour`, [since]),
    ]);
    res.json({
      period_start: since.toISOString(),
      by_type: byType.rows,
      by_severity: bySeverity.rows,
      by_camera: byCamera.rows,
      hourly: hourly.rows,
    });
  } catch (err) {
    console.error('[ai:analytics]', err);
    res.status(500).json({ error: 'Failed to load analytics' });
  }
});

// GET /api/ai/models — models available for the per-camera picker
router.get('/models', authenticate, async (req, res) => {
  let loaded = [];
  let device = null;
  try {
    const r = await axios.get(`${YOLO_URL()}/health`, { timeout: 3000 });
    loaded = r.data?.loaded_models || [];
    device = r.data?.device || null;
  } catch {
    // Sidecar may be down/starting — still return the curated list so the
    // picker in the camera controls isn't blocked by that.
  }
  res.json({ models: AVAILABLE_AI_MODELS, loaded_models: loaded, device });
});

// GET /api/ai/health — sidecar health check
router.get('/health', authenticate, async (req, res) => {
  try {
    const r = await axios.get(`${YOLO_URL()}/health`, { timeout: 3000 });
    res.json({ sidecar: 'up', ...r.data });
  } catch (err) {
    console.error('[ai:health]', err.message);
    res.status(503).json({ sidecar: 'down' });
  }
});

module.exports = router;
