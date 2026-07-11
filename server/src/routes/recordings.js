const express = require('express');
const fs = require('fs');
const { query, queryOne } = require('../db');
const { authenticate, requireRole } = require('../middleware/auth');
const router = express.Router();

// GET /api/recordings
router.get('/', authenticate, async (req, res) => {
  try {
    const { camera_id, start_date, end_date, status, limit = 50, offset = 0 } = req.query;
    const conds = [], params = [];
    if (camera_id)  { params.push(parseInt(camera_id)); conds.push(`r.camera_id=$${params.length}`); }
    if (start_date) { params.push(start_date); conds.push(`r.start_timestamp>=$${params.length}`); }
    if (end_date)   { params.push(end_date);   conds.push(`r.start_timestamp<=$${params.length}`); }
    if (status)     { params.push(status.toUpperCase()); conds.push(`r.status=$${params.length}`); }
    const where = conds.length ? 'WHERE ' + conds.join(' AND ') : '';
    const total = parseInt((await query(`SELECT COUNT(*) FROM recordings r ${where}`, params)).rows[0].count);
    params.push(parseInt(limit), parseInt(offset));
    const rows = await query(
      `SELECT r.recording_id, r.camera_id, r.file_path, r.file_name,
              r.file_size_bytes, r.duration_seconds, r.start_timestamp, r.end_timestamp,
              r.video_codec, r.resolution_width, r.resolution_height, r.fps_actual,
              r.has_audio, r.recording_mode, r.status, r.hls_playlist_path,
              r.gps_latitude, r.gps_longitude, r.gps_speed_kmh, r.created_at,
              c.camera_name, c.location_description, c.camera_type, c.ip_address
       FROM recordings r
       LEFT JOIN cameras c ON r.camera_id=c.camera_id
       ${where}
       ORDER BY r.start_timestamp DESC
       LIMIT $${params.length-1} OFFSET $${params.length}`,
      params);
    res.json({ recordings: rows.rows, total, limit: parseInt(limit), offset: parseInt(offset) });
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// GET /api/recordings/:id
router.get('/:id', authenticate, async (req, res) => {
  try {
    const rec = await queryOne(
      `SELECT r.*, c.camera_name, c.location_description, c.camera_type, c.ip_address
       FROM recordings r LEFT JOIN cameras c ON r.camera_id=c.camera_id
       WHERE r.recording_id=$1`, [req.params.id]);
    if (!rec) return res.status(404).json({ error: 'Recording not found' });
    res.json(rec);
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// POST /api/recordings  — called by recorder service
router.post('/', authenticate, async (req, res) => {
  try {
    const {
      camera_id, file_path, file_name, start_timestamp, end_timestamp,
      file_size_bytes, duration_seconds, video_codec = 'H.264',
      resolution_width = 1280, resolution_height = 720,
      fps_actual, has_audio = 1, recording_mode = 'CONTINUOUS',
      gps_latitude, gps_longitude, gps_speed_kmh,
      gps_altitude, gps_heading, status = 'COMPLETED'
    } = req.body;

    if (!camera_id || !file_path || !file_name || !start_timestamp)
      return res.status(400).json({ error: 'camera_id, file_path, file_name, start_timestamp required' });

    const rec = await queryOne(
      `INSERT INTO recordings(
         camera_id, file_path, file_name, start_timestamp, end_timestamp,
         file_size_bytes, duration_seconds, video_codec, resolution_width, resolution_height,
         fps_actual, has_audio, recording_mode, gps_latitude, gps_longitude,
         gps_speed_kmh, gps_altitude, gps_heading, status)
       VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,$19)
       ON CONFLICT (file_path) DO UPDATE SET
         end_timestamp=EXCLUDED.end_timestamp,
         file_size_bytes=EXCLUDED.file_size_bytes,
         duration_seconds=EXCLUDED.duration_seconds,
         status=EXCLUDED.status,
         updated_at=NOW()
       RETURNING recording_id, camera_id, file_name, start_timestamp, status`,
      [camera_id, file_path, file_name, start_timestamp, end_timestamp || null,
       file_size_bytes || null, duration_seconds || null, video_codec,
       resolution_width, resolution_height, fps_actual || null,
       has_audio, recording_mode, gps_latitude || null, gps_longitude || null,
       gps_speed_kmh || null, gps_altitude || null, gps_heading || null, status]);

    res.status(201).json(rec);
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// PUT /api/recordings/:id
router.put('/:id', authenticate, requireRole('ADMIN', 'OPERATOR'), async (req, res) => {
  try {
    const allowed = ['status','end_timestamp','file_size_bytes','duration_seconds','hls_playlist_path'];
    const fields = [], params = [];
    for (const [k,v] of Object.entries(req.body))
      if (allowed.includes(k)) { params.push(v); fields.push(`${k}=$${params.length}`); }
    if (!fields.length) return res.status(400).json({ error: 'No valid fields' });
    fields.push('updated_at=NOW()');
    params.push(parseInt(req.params.id));
    const rec = await queryOne(
      `UPDATE recordings SET ${fields.join(',')} WHERE recording_id=$${params.length} RETURNING *`, params);
    if (!rec) return res.status(404).json({ error: 'Not found' });
    res.json(rec);
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// DELETE /api/recordings/:id
router.delete('/:id', authenticate, requireRole('ADMIN'), async (req, res) => {
  try {
    const rec = await queryOne('SELECT file_path FROM recordings WHERE recording_id=$1', [req.params.id]);
    if (!rec) return res.status(404).json({ error: 'Not found' });
    await query('DELETE FROM recordings WHERE recording_id=$1', [req.params.id]);
    if (req.query.deleteFile === 'true' && rec.file_path && fs.existsSync(rec.file_path))
      fs.unlinkSync(rec.file_path);
    res.json({ message: 'Deleted' });
  } catch (err) { res.status(500).json({ error: err.message }); }
});

module.exports = router;
