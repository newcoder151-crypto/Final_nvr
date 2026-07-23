const express = require('express');
const { query, queryOne } = require('../db');
const { authenticate, requireRole } = require('../middleware/auth');
const { broadcast } = require('../websocket');
const router = express.Router();

router.get('/', authenticate, async (req, res) => {
  try {
    const { status, camera_type, location, limit = 100, offset = 0 } = req.query;
    const conds = [], params = [];
    if (status)      { params.push(status.toUpperCase()); conds.push(`c.status=$${params.length}`); }
    if (camera_type) { params.push(camera_type.toUpperCase()); conds.push(`c.camera_type=$${params.length}`); }
    if (location)    { params.push(`%${location}%`); conds.push(`c.location_description ILIKE $${params.length}`); }
    const where = conds.length ? 'WHERE ' + conds.join(' AND ') : '';
    const total = parseInt((await query(`SELECT COUNT(*) FROM cameras c ${where}`, params)).rows[0].count);
    params.push(parseInt(limit), parseInt(offset));
    const rows = await query(
      `SELECT c.*,
              ch.is_online, ch.is_recording, ch.frame_rate_actual,
              ch.bitrate_kbps, ch.error_count, ch.last_error,
              ch.timestamp AS health_ts
       FROM cameras c
       LEFT JOIN LATERAL (
         SELECT * FROM camera_health
         WHERE camera_id=c.camera_id
         ORDER BY timestamp DESC LIMIT 1
       ) ch ON true
       ${where}
       ORDER BY c.camera_name
       LIMIT $${params.length-1} OFFSET $${params.length}`, params);
    res.json({ cameras: rows.rows, total, limit: parseInt(limit), offset: parseInt(offset) });
  } catch (err) { res.status(500).json({ error: err.message }); }
});

router.get('/:id', authenticate, async (req, res) => {
  try {
    const cam = await queryOne(
      `SELECT c.*,
              ch.is_online, ch.is_recording, ch.frame_rate_actual, ch.bitrate_kbps, ch.last_error
       FROM cameras c
       LEFT JOIN LATERAL (
         SELECT * FROM camera_health WHERE camera_id=c.camera_id ORDER BY timestamp DESC LIMIT 1
       ) ch ON true
       WHERE c.camera_id=$1`, [req.params.id]);
    if (!cam) return res.status(404).json({ error: 'Camera not found' });
    res.json(cam);
  } catch (err) { res.status(500).json({ error: err.message }); }
});

router.post('/', authenticate, requireRole('ADMIN'), async (req, res) => {
  try {
    const { camera_name, camera_type='INTERIOR', ip_address='', rtsp_url='',
            rtsp_port=554, username, password_hash, manufacturer, model,
            resolution_width=1920, resolution_height=1080, target_fps=25,
            video_codec='H.265', location_description, physical_position,
            ptz_supported=0, audio_supported=0,
            ai_model='yolov8n.pt', ai_confidence_threshold=0.35, ai_detection_enabled=true } = req.body;
    if (!camera_name) return res.status(400).json({ error: 'camera_name required' });
    const cam = await queryOne(
      `INSERT INTO cameras(camera_name,camera_type,ip_address,rtsp_url,rtsp_port,
         username,password_hash,manufacturer,model,resolution_width,resolution_height,
         target_fps,video_codec,location_description,physical_position,
         ptz_supported,audio_supported,ai_model,ai_confidence_threshold,ai_detection_enabled)
       VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,$19,$20)
       RETURNING *`,
      [camera_name,camera_type.toUpperCase(),ip_address,rtsp_url,rtsp_port,
       username||null,password_hash||null,manufacturer||null,model||null,
       resolution_width,resolution_height,target_fps,video_codec,
       location_description||null,physical_position||null,ptz_supported,audio_supported,
       ai_model,ai_confidence_threshold,ai_detection_enabled]);
    broadcast({ type: 'camera.created', data: cam });
    res.status(201).json(cam);
  } catch (err) { res.status(500).json({ error: err.message }); }
});

router.put('/:id', authenticate, requireRole('ADMIN','OPERATOR'), async (req, res) => {
  try {
    const allowed = ['camera_name','camera_type','ip_address','rtsp_url','rtsp_port',
                     'location_description','physical_position','status','target_fps',
                     'video_codec','resolution_width','resolution_height',
                     'ptz_supported','audio_supported','manufacturer','model',
                     'hls_playlist_url','hls_output_dir','rec_output_dir',
                     'ai_model','ai_confidence_threshold','ai_detection_enabled'];
    const fields = [], params = [];
    for (const [k,v] of Object.entries(req.body))
      if (allowed.includes(k)) { params.push(v); fields.push(`${k}=$${params.length}`); }
    if (!fields.length) return res.status(400).json({ error: 'No valid fields' });
    fields.push('updated_at=NOW()');
    params.push(parseInt(req.params.id));
    const cam = await queryOne(
      `UPDATE cameras SET ${fields.join(',')} WHERE camera_id=$${params.length} RETURNING *`, params);
    if (!cam) return res.status(404).json({ error: 'Camera not found' });
    broadcast({ type: 'camera.updated', data: cam });
    res.json(cam);
  } catch (err) { res.status(500).json({ error: err.message }); }
});

router.delete('/:id', authenticate, requireRole('ADMIN'), async (req, res) => {
  try {
    const r = await query('DELETE FROM cameras WHERE camera_id=$1 RETURNING camera_id', [req.params.id]);
    if (!r.rows.length) return res.status(404).json({ error: 'Camera not found' });
    broadcast({ type: 'camera.deleted', data: { camera_id: parseInt(req.params.id) } });
    res.json({ message: 'Deleted', camera_id: parseInt(req.params.id) });
  } catch (err) { res.status(500).json({ error: err.message }); }
});

router.get('/:id/health', authenticate, async (req, res) => {
  try {
    const rows = await query(
      `SELECT * FROM camera_health WHERE camera_id=$1 ORDER BY timestamp DESC LIMIT 50`,
      [req.params.id]);
    res.json({ health: rows.rows });
  } catch (err) { res.status(500).json({ error: err.message }); }
});

router.post('/:id/health', authenticate, async (req, res) => {
  try {
    const { is_online=1, is_recording=0, frame_rate_actual, bitrate_kbps, error_count=0, last_error } = req.body;
    await query(
      `INSERT INTO camera_health(camera_id,is_online,is_recording,frame_rate_actual,bitrate_kbps,error_count,last_error)
       VALUES($1,$2,$3,$4,$5,$6,$7)`,
      [req.params.id,is_online,is_recording,frame_rate_actual||null,bitrate_kbps||null,error_count,last_error||null]);
    broadcast({ type:'camera.health', data:{ camera_id:parseInt(req.params.id), is_online, is_recording }});
    res.json({ message: 'Health recorded' });
  } catch (err) { res.status(500).json({ error: err.message }); }
});

module.exports = router;
