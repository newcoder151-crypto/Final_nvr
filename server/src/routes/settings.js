/**
 * Settings routes — manages the new DB-driven settings tables
 *
 * Tables (all created by mnvr_schema.sql):
 *   health_monitor_settings    — single-row, health thresholds
 *   recording_settings         — single-row, recording params
 *   hls_settings               — single-row, HLS params
 *   onvif_settings             — single-row, ONVIF discovery params
 *   cameras_config_details     — per-camera ONVIF/RTSP credentials
 */

const express = require('express');
const { query, queryOne } = require('../db');
const { authenticate, requireRole } = require('../middleware/auth');
const router = express.Router();

/* ------------------------------------------------------------------ */
/* Health Monitor Settings                                             */
/* ------------------------------------------------------------------ */

// GET /api/settings/health
router.get('/health', authenticate, async (req, res) => {
  try {
    const row = await queryOne(`SELECT * FROM health_monitor_settings ORDER BY id LIMIT 1`);
    res.json(row || {});
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// PUT /api/settings/health
router.put('/health', authenticate, requireRole('ADMIN'), async (req, res) => {
  try {
    const {
      poll_interval_sec, cpu_warn_threshold, mem_warn_threshold, disk_warn_threshold,
      cpu_critical_threshold, mem_critical_threshold, disk_critical_threshold, enable_alerts,
    } = req.body;

    const row = await queryOne(`
      UPDATE health_monitor_settings SET
        poll_interval_sec       = COALESCE($1, poll_interval_sec),
        cpu_warn_threshold      = COALESCE($2, cpu_warn_threshold),
        mem_warn_threshold      = COALESCE($3, mem_warn_threshold),
        disk_warn_threshold     = COALESCE($4, disk_warn_threshold),
        cpu_critical_threshold  = COALESCE($5, cpu_critical_threshold),
        mem_critical_threshold  = COALESCE($6, mem_critical_threshold),
        disk_critical_threshold = COALESCE($7, disk_critical_threshold),
        enable_alerts           = COALESCE($8, enable_alerts),
        updated_at              = NOW()
      WHERE id = (SELECT id FROM health_monitor_settings ORDER BY id LIMIT 1)
      RETURNING *`,
      [poll_interval_sec, cpu_warn_threshold, mem_warn_threshold, disk_warn_threshold,
       cpu_critical_threshold, mem_critical_threshold, disk_critical_threshold, enable_alerts]
    );
    res.json(row);
  } catch (err) { res.status(500).json({ error: err.message }); }
});

/* ------------------------------------------------------------------ */
/* Recording Settings                                                  */
/* ------------------------------------------------------------------ */

// GET /api/settings/recording
router.get('/recording', authenticate, async (req, res) => {
  try {
    const row = await queryOne(`SELECT * FROM recording_settings ORDER BY id LIMIT 1`);
    res.json(row || {});
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// PUT /api/settings/recording
router.put('/recording', authenticate, requireRole('ADMIN'), async (req, res) => {
  try {
    const {
      storage_path, recording_retention_days, segment_duration_sec,
      segment_max_size_mb, max_storage_gb, enable_audio, enable_watermark,
    } = req.body;

    const row = await queryOne(`
      UPDATE recording_settings SET
        storage_path             = COALESCE($1, storage_path),
        recording_retention_days = COALESCE($2, recording_retention_days),
        segment_duration_sec     = COALESCE($3, segment_duration_sec),
        segment_max_size_mb      = COALESCE($4, segment_max_size_mb),
        max_storage_gb           = COALESCE($5, max_storage_gb),
        enable_audio             = COALESCE($6, enable_audio),
        enable_watermark         = COALESCE($7, enable_watermark),
        updated_at               = NOW()
      WHERE id = (SELECT id FROM recording_settings ORDER BY id LIMIT 1)
      RETURNING *`,
      [storage_path, recording_retention_days, segment_duration_sec,
       segment_max_size_mb, max_storage_gb, enable_audio, enable_watermark]
    );
    res.json(row);
  } catch (err) { res.status(500).json({ error: err.message }); }
});

/* ------------------------------------------------------------------ */
/* HLS Settings                                                        */
/* ------------------------------------------------------------------ */

// GET /api/settings/hls
router.get('/hls', authenticate, async (req, res) => {
  try {
    const row = await queryOne(`SELECT * FROM hls_settings ORDER BY id LIMIT 1`);
    res.json(row || {});
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// PUT /api/settings/hls
router.put('/hls', authenticate, requireRole('ADMIN'), async (req, res) => {
  try {
    const { hls_base, hls_segment_sec, hls_window_size, hls_delete_old_segments } = req.body;

    const row = await queryOne(`
      UPDATE hls_settings SET
        hls_base               = COALESCE($1, hls_base),
        hls_segment_sec        = COALESCE($2, hls_segment_sec),
        hls_window_size        = COALESCE($3, hls_window_size),
        hls_delete_old_segments= COALESCE($4, hls_delete_old_segments),
        updated_at             = NOW()
      WHERE id = (SELECT id FROM hls_settings ORDER BY id LIMIT 1)
      RETURNING *`,
      [hls_base, hls_segment_sec, hls_window_size, hls_delete_old_segments]
    );
    res.json(row);
  } catch (err) { res.status(500).json({ error: err.message }); }
});

/* ------------------------------------------------------------------ */
/* ONVIF Global Settings                                               */
/* ------------------------------------------------------------------ */

// GET /api/settings/onvif
router.get('/onvif', authenticate, async (req, res) => {
  try {
    const row = await queryOne(`SELECT * FROM onvif_settings ORDER BY id LIMIT 1`);
    res.json(row || {});
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// PUT /api/settings/onvif
router.put('/onvif', authenticate, requireRole('ADMIN'), async (req, res) => {
  try {
    const { multicast_ip, multicast_port, discovery_interval_sec, probe_timeout_ms, enable_discovery } = req.body;

    const row = await queryOne(`
      UPDATE onvif_settings SET
        multicast_ip           = COALESCE($1, multicast_ip),
        multicast_port         = COALESCE($2, multicast_port),
        discovery_interval_sec = COALESCE($3, discovery_interval_sec),
        probe_timeout_ms       = COALESCE($4, probe_timeout_ms),
        enable_discovery       = COALESCE($5, enable_discovery),
        updated_at             = NOW()
      WHERE id = (SELECT id FROM onvif_settings ORDER BY id LIMIT 1)
      RETURNING *`,
      [multicast_ip, multicast_port, discovery_interval_sec, probe_timeout_ms, enable_discovery]
    );
    res.json(row);
  } catch (err) { res.status(500).json({ error: err.message }); }
});

/* ------------------------------------------------------------------ */
/* Camera Config Details (per-camera credentials)                     */
/* ------------------------------------------------------------------ */

// GET /api/settings/cameras
router.get('/cameras', authenticate, async (req, res) => {
  try {
    const rows = await query(`
      SELECT id, camera_slot, name, location, camera_type,
             host(ip_address) AS ip_address, onvif_port,
             onvif_username,
             rtsp_username,
             manufacturer, model, is_active, created_at, updated_at
      FROM cameras_config_details
      ORDER BY camera_slot`);
    // passwords intentionally omitted from list
    res.json(rows.rows);
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// GET /api/settings/cameras/:slot
router.get('/cameras/:slot', authenticate, async (req, res) => {
  try {
    const row = await queryOne(`
      SELECT id, camera_slot, name, location, camera_type,
             host(ip_address) AS ip_address, onvif_port,
             onvif_username,
             rtsp_username,
             manufacturer, model, is_active, created_at, updated_at
      FROM cameras_config_details WHERE camera_slot=$1`,
      [req.params.slot]
    );
    if (!row) return res.status(404).json({ error: 'Camera slot not found' });
    res.json(row);
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// POST /api/settings/cameras — add a camera slot
router.post('/cameras', authenticate, requireRole('ADMIN'), async (req, res) => {
  try {
    const {
      camera_slot, name, location, camera_type, ip_address, onvif_port,
      onvif_username, onvif_password, rtsp_username, rtsp_password,
      manufacturer, model, is_active,
    } = req.body;

    if (!camera_slot || !ip_address || !name) {
      return res.status(400).json({ error: 'camera_slot, name, and ip_address are required' });
    }

    const row = await queryOne(`
      INSERT INTO cameras_config_details
        (camera_slot, name, location, camera_type, ip_address, onvif_port,
         onvif_username, onvif_password, rtsp_username, rtsp_password,
         manufacturer, model, is_active)
      VALUES ($1,$2,$3,$4,$5::inet,$6,$7,$8,$9,$10,$11,$12,COALESCE($13,true))
      ON CONFLICT (camera_slot) DO UPDATE SET
        name=EXCLUDED.name, location=EXCLUDED.location,
        camera_type=EXCLUDED.camera_type, ip_address=EXCLUDED.ip_address,
        onvif_port=EXCLUDED.onvif_port,
        onvif_username=EXCLUDED.onvif_username,
        onvif_password=EXCLUDED.onvif_password,
        rtsp_username=EXCLUDED.rtsp_username,
        rtsp_password=EXCLUDED.rtsp_password,
        manufacturer=EXCLUDED.manufacturer, model=EXCLUDED.model,
        is_active=EXCLUDED.is_active, updated_at=NOW()
      RETURNING id, camera_slot, name, location, camera_type,
                host(ip_address) AS ip_address, onvif_port,
                onvif_username, rtsp_username,
                manufacturer, model, is_active, created_at, updated_at`,
      [camera_slot, name, location, camera_type, ip_address,
       onvif_port || 80, onvif_username, onvif_password,
       rtsp_username, rtsp_password, manufacturer, model, is_active]
    );
    res.status(201).json(row);
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// PUT /api/settings/cameras/:slot — update a camera slot
router.put('/cameras/:slot', authenticate, requireRole('ADMIN'), async (req, res) => {
  try {
    const {
      name, location, camera_type, ip_address, onvif_port,
      onvif_username, onvif_password, rtsp_username, rtsp_password,
      manufacturer, model, is_active,
    } = req.body;

    const row = await queryOne(`
      UPDATE cameras_config_details SET
        name           = COALESCE($1, name),
        location       = COALESCE($2, location),
        camera_type    = COALESCE($3, camera_type),
        ip_address     = COALESCE($4::inet, ip_address),
        onvif_port     = COALESCE($5, onvif_port),
        onvif_username = COALESCE($6, onvif_username),
        onvif_password = COALESCE($7, onvif_password),
        rtsp_username  = COALESCE($8, rtsp_username),
        rtsp_password  = COALESCE($9, rtsp_password),
        manufacturer   = COALESCE($10, manufacturer),
        model          = COALESCE($11, model),
        is_active      = COALESCE($12, is_active),
        updated_at     = NOW()
      WHERE camera_slot = $13
      RETURNING id, camera_slot, name, location, camera_type,
                host(ip_address) AS ip_address, onvif_port,
                onvif_username, rtsp_username,
                manufacturer, model, is_active, created_at, updated_at`,
      [name, location, camera_type, ip_address, onvif_port,
       onvif_username, onvif_password, rtsp_username, rtsp_password,
       manufacturer, model, is_active, req.params.slot]
    );
    if (!row) return res.status(404).json({ error: 'Camera slot not found' });
    res.json(row);
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// DELETE /api/settings/cameras/:slot
router.delete('/cameras/:slot', authenticate, requireRole('ADMIN'), async (req, res) => {
  try {
    const row = await queryOne(
      `DELETE FROM cameras_config_details WHERE camera_slot=$1 RETURNING camera_slot`,
      [req.params.slot]
    );
    if (!row) return res.status(404).json({ error: 'Camera slot not found' });
    res.json({ deleted: true, camera_slot: row.camera_slot });
  } catch (err) { res.status(500).json({ error: err.message }); }
});

module.exports = router;
