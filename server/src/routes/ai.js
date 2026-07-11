const express = require('express');
const multer = require('multer');
const axios = require('axios');
const FormData = require('form-data');
const { query, queryOne } = require('../db');
const { authenticate } = require('../middleware/auth');
const { broadcast } = require('../websocket');

const router = express.Router();
const upload = multer({ storage: multer.memoryStorage(), limits: { fileSize: 25 * 1024 * 1024 } });
const YOLO_URL = () => process.env.YOLO_SIDECAR_URL || 'http://localhost:8000';

// Helper: proxy frame to YOLO sidecar
async function callYolo(imageBuffer, filename, mimetype, conf, model) {
  const fd = new FormData();
  fd.append('image', imageBuffer, { filename: filename || 'frame.jpg', contentType: mimetype || 'image/jpeg' });
  if (conf)  fd.append('conf', String(conf));
  if (model) fd.append('model', String(model));
  const r = await axios.post(`${YOLO_URL()}/detect`, fd, {
    headers: fd.getHeaders(),
    maxBodyLength: Infinity,
    timeout: 30_000,
  });
  return r.data;
}

// POST /api/ai/detect — generic YOLO detection (proxy to Python sidecar)
router.post('/detect', authenticate, upload.single('image'), async (req, res) => {
  if (!req.file) return res.status(400).json({ error: 'image file required' });
  try {
    const data = await callYolo(req.file.buffer, req.file.originalname, req.file.mimetype, req.body.conf, req.body.model);
    res.json(data);
  } catch (err) {
    res.status(err.response?.status || 502).json({
      error: 'YOLO sidecar error', detail: err.response?.data || err.message,
      hint: `Is the Python sidecar running at ${YOLO_URL()}? Run: cd server/ai && uvicorn sidecar:app --port 8000`,
    });
  }
});

// POST /api/ai/people-count — unique person detection + density
router.post('/people-count', authenticate, upload.single('image'), async (req, res) => {
  if (!req.file) return res.status(400).json({ error: 'image file required' });
  try {
    const data = await callYolo(req.file.buffer, req.file.originalname, req.file.mimetype, req.body.conf || 0.4, req.body.model || 'yolov8n.pt');
    const people = data.detections.filter(d => d.label === 'person');
    const count = people.length;
    const [W, H] = data.image_size;
    const frameArea = W * H;
    const personArea = people.reduce((sum, d) => sum + (d.bbox[2]-d.bbox[0]) * (d.bbox[3]-d.bbox[1]), 0);
    const density = frameArea > 0 ? Math.min(100, Math.round((personArea / frameArea) * 300)) : 0;
    const level = density > 60 ? 'HIGH' : density > 30 ? 'MEDIUM' : 'LOW';

    // Auto-create event for high density
    if (level === 'HIGH' && req.body.camera_id) {
      await query(
        `INSERT INTO events(event_type, title, severity, camera_id, description, event_data, occurred_at)
         VALUES('CROWD_DENSITY','High crowd density detected','WARNING',$1,$2,$3,NOW())`,
        [req.body.camera_id, `${count} persons detected, density ${density}%`,
         JSON.stringify({ count, density, level })]);
      broadcast({ type: 'event.new', data: { event_type: 'CROWD_DENSITY', severity: 'WARNING', camera_id: req.body.camera_id, count, density } });
    }

    res.json({ ...data, people_count: count, density_percent: density, density_level: level, people_detections: people });
  } catch (err) {
    res.status(err.response?.status || 502).json({ error: 'YOLO sidecar error', detail: err.message });
  }
});

// POST /api/ai/intrusion — zone intrusion detection
router.post('/intrusion', authenticate, upload.single('image'), async (req, res) => {
  if (!req.file) return res.status(400).json({ error: 'image file required' });
  try {
    // zone: {x1,y1,x2,y2} as percentages of frame (0-100)
    const zone = req.body.zone ? JSON.parse(req.body.zone) : { x1: 0, y1: 0, x2: 100, y2: 100 };
    const data = await callYolo(req.file.buffer, req.file.originalname, req.file.mimetype, req.body.conf || 0.4, req.body.model);
    const [W, H] = data.image_size;
    const zx1 = (zone.x1 / 100) * W, zy1 = (zone.y1 / 100) * H;
    const zx2 = (zone.x2 / 100) * W, zy2 = (zone.y2 / 100) * H;

    const intruders = data.detections.filter(d => {
      const cx = (d.bbox[0] + d.bbox[2]) / 2;
      const cy = (d.bbox[1] + d.bbox[3]) / 2;
      return cx >= zx1 && cx <= zx2 && cy >= zy1 && cy <= zy2;
    });

    const intrusionDetected = intruders.length > 0;
    if (intrusionDetected && req.body.camera_id) {
      const title = `Intrusion detected: ${intruders.length} object(s) in restricted zone`;
      const severity = intruders.some(d => d.label === 'person') ? 'CRITICAL' : 'WARNING';
      await query(
        `INSERT INTO events(event_type, title, severity, camera_id, description, event_data, occurred_at)
         VALUES('INTRUSION',$1,$2,$3,$4,$5,NOW())`,
        [title, severity, req.body.camera_id, `Zone: ${JSON.stringify(zone)}`,
         JSON.stringify({ zone, intruders })]);
      broadcast({ type: 'event.new', data: { event_type: 'INTRUSION', severity, camera_id: req.body.camera_id, count: intruders.length } });
    }

    res.json({ ...data, zone, intrusion_detected: intrusionDetected, intruder_count: intruders.length, intruders });
  } catch (err) {
    res.status(err.response?.status || 502).json({ error: 'YOLO sidecar error', detail: err.message });
  }
});

// POST /api/ai/object-detect — full object detection with class filtering
router.post('/object-detect', authenticate, upload.single('image'), async (req, res) => {
  if (!req.file) return res.status(400).json({ error: 'image file required' });
  try {
    const data = await callYolo(req.file.buffer, req.file.originalname, req.file.mimetype, req.body.conf || 0.35, req.body.model);
    const filterClass = req.body.filter_class;
    const filtered = filterClass
      ? data.detections.filter(d => d.label.toLowerCase() === filterClass.toLowerCase())
      : data.detections;
    const summary = filtered.reduce((acc, d) => { acc[d.label] = (acc[d.label] || 0) + 1; return acc; }, {});
    res.json({ ...data, detections: filtered, summary, total_detected: filtered.length });
  } catch (err) {
    res.status(err.response?.status || 502).json({ error: 'YOLO sidecar error', detail: err.message });
  }
});

// GET /api/ai/analytics — AI event stats from DB
router.get('/analytics', authenticate, async (req, res) => {
  try {
    const since = req.query.since || new Date(Date.now() - 24 * 3600 * 1000).toISOString();
    const [byType, bySeverity, byCamera, hourly] = await Promise.all([
      query(`SELECT event_type, COUNT(*) as count FROM events WHERE occurred_at>=$1 GROUP BY event_type ORDER BY count DESC`, [since]),
      query(`SELECT severity, COUNT(*) as count FROM events WHERE occurred_at>=$1 GROUP BY severity`, [since]),
      query(`SELECT e.camera_id, c.camera_name, COUNT(*) as count FROM events e LEFT JOIN cameras c ON e.camera_id=c.camera_id WHERE e.occurred_at>=$1 GROUP BY e.camera_id, c.camera_name ORDER BY count DESC LIMIT 10`, [since]),
      query(`SELECT date_trunc('hour', occurred_at) as hour, severity, COUNT(*) as count FROM events WHERE occurred_at>=$1 GROUP BY hour, severity ORDER BY hour`, [since]),
    ]);
    res.json({
      period_start: since,
      by_type: byType.rows,
      by_severity: bySeverity.rows,
      by_camera: byCamera.rows,
      hourly: hourly.rows,
    });
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// GET /api/ai/health — sidecar health check
router.get('/health', authenticate, async (req, res) => {
  try {
    const r = await axios.get(`${YOLO_URL()}/health`, { timeout: 3000 });
    res.json({ sidecar: 'up', url: YOLO_URL(), ...r.data });
  } catch (err) {
    res.status(503).json({ sidecar: 'down', url: YOLO_URL(), error: err.message,
      hint: 'Run: cd server/ai && pip install -r requirements.txt && uvicorn sidecar:app --port 8000' });
  }
});

module.exports = router;
