/**
 * ONVIF Profile S control routes
 * -------------------------------------------------------------------------
 * /api/onvif/cameras/:id/capabilities    GET
 * /api/onvif/cameras/:id/profiles        GET    (list + cache in DB)
 * /api/onvif/cameras/:id/profiles/:token/stream-uri    GET
 * /api/onvif/cameras/:id/profiles/:token/activate       POST  (select active stream)
 *
 * /api/onvif/cameras/:id/ptz/status      GET
 * /api/onvif/cameras/:id/ptz/move        POST   { mode: continuous|relative|absolute, x, y, zoom, speed, timeout }
 * /api/onvif/cameras/:id/ptz/stop        POST
 * /api/onvif/cameras/:id/ptz/presets     GET / POST (create)
 * /api/onvif/cameras/:id/ptz/presets/:token/goto    POST
 * /api/onvif/cameras/:id/ptz/presets/:token         DELETE
 * /api/onvif/cameras/:id/ptz/home        POST   { action: goto|set }
 *
 * /api/onvif/cameras/:id/image           GET / PUT  (brightness/contrast/saturation/sharpness/...)
 * /api/onvif/cameras/:id/audio           GET / PUT
 */

const express = require('express');
const { query, queryOne } = require('../db');
const { authenticate, requireRole } = require('../middleware/auth');
const { broadcast } = require('../websocket');
const onvif = require('../services/onvifClient');

const router = express.Router();

function handleOnvifError(res, err, cameraId) {
  if (cameraId) onvif.invalidate(Number(cameraId));
  const msg = err?.message || String(err);
  const isTimeout = /timeout|ECONNREFUSED|EHOSTUNREACH/i.test(msg);
  res.status(isTimeout ? 504 : 502).json({ error: msg, camera_id: cameraId ?? null });
}

/* ------------------------------------------------------------------ */
/* Capabilities                                                        */
/* ------------------------------------------------------------------ */

router.get('/cameras/:id/capabilities', authenticate, async (req, res) => {
  try {
    const caps = await onvif.getCapabilities(req.params.id);
    await query(
      `INSERT INTO camera_onvif_capabilities
         (camera_id, supports_media, supports_ptz, supports_imaging, supports_audio,
          supports_events, profile_s_conformant, device_service_url, media_service_url,
          ptz_service_url, imaging_service_url, last_checked_at)
       VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,NOW())
       ON CONFLICT (camera_id) DO UPDATE SET
         supports_media=$2, supports_ptz=$3, supports_imaging=$4, supports_audio=$5,
         supports_events=$6, profile_s_conformant=$7, device_service_url=$8,
         media_service_url=$9, ptz_service_url=$10, imaging_service_url=$11,
         last_checked_at=NOW()`,
      [
        req.params.id, caps.supports_media, caps.supports_ptz, caps.supports_imaging,
        caps.supports_audio ?? false, caps.supports_events ?? false,
        !!(caps.supports_media && caps.supports_ptz), caps.device_service_url,
        caps.media_service_url, caps.ptz_service_url, caps.imaging_service_url,
      ]
    );
    res.json(caps);
  } catch (err) { handleOnvifError(res, err, req.params.id); }
});

/* ------------------------------------------------------------------ */
/* Media profiles / multi-stream selection                             */
/* ------------------------------------------------------------------ */

// GET /cameras/:id/profiles — fetch live from camera, cache in DB, return list
router.get('/cameras/:id/profiles', authenticate, async (req, res) => {
  try {
    const cameraId = req.params.id;
    const profiles = await onvif.getProfiles(cameraId);

    const active = await queryOne(
      `SELECT profile_token FROM camera_media_profiles WHERE camera_id=$1 AND is_active_stream=TRUE`,
      [cameraId]
    );

    for (const prof of profiles) {
      let streamUri = null, snapshotUri = null;
      try { streamUri = await onvif.getStreamUri(cameraId, prof.token); } catch {}
      try { snapshotUri = await onvif.getSnapshotUri(cameraId, prof.token); } catch {}

      await query(
        `INSERT INTO camera_media_profiles
           (camera_id, profile_token, profile_name, encoding, resolution_width,
            resolution_height, fps, bitrate_kbps, gov_length, quality, stream_uri,
            snapshot_uri, video_source_token, audio_encoder_token, last_synced_at)
         VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,NOW())
         ON CONFLICT (camera_id, profile_token) DO UPDATE SET
           profile_name=$3, encoding=$4, resolution_width=$5, resolution_height=$6,
           fps=$7, bitrate_kbps=$8, gov_length=$9, quality=$10, stream_uri=$11,
           snapshot_uri=$12, video_source_token=$13, audio_encoder_token=$14,
           last_synced_at=NOW()`,
        [
          cameraId, prof.token, prof.name, prof.video_encoder?.encoding,
          prof.video_encoder?.resolution?.width, prof.video_encoder?.resolution?.height,
          prof.video_encoder?.frame_rate_limit, prof.video_encoder?.bitrate_limit,
          prof.video_encoder?.gov_length, prof.video_encoder?.quality,
          streamUri, snapshotUri, prof.video_source_token, prof.audio_encoder?.token,
        ]
      );
    }

    // If nothing is marked active yet, default to the first (usually highest-res) profile
    if (!active && profiles.length) {
      await query(
        `UPDATE camera_media_profiles SET is_active_stream=TRUE
         WHERE camera_id=$1 AND profile_token=$2`,
        [cameraId, profiles[0].token]
      );
    }

    const rows = await query(
      `SELECT * FROM camera_media_profiles WHERE camera_id=$1 ORDER BY resolution_width DESC NULLS LAST`,
      [cameraId]
    );
    res.json({ profiles: rows.rows });
  } catch (err) { handleOnvifError(res, err, req.params.id); }
});

// GET /cameras/:id/profiles/:token/stream-uri — re-fetch a fresh, possibly time-limited, RTSP URI
router.get('/cameras/:id/profiles/:token/stream-uri', authenticate, async (req, res) => {
  try {
    const uri = await onvif.getStreamUri(req.params.id, req.params.token);
    res.json({ uri });
  } catch (err) { handleOnvifError(res, err, req.params.id); }
});

// POST /cameras/:id/profiles/:token/activate — mark this profile as the one
// the live view / recorder should pull (NVR core re-reads rtsp_url on reload)
router.post('/cameras/:id/profiles/:token/activate', authenticate, requireRole('ADMIN'), async (req, res) => {
  try {
    const { id: cameraId, token } = req.params;
    const prof = await queryOne(
      `SELECT * FROM camera_media_profiles WHERE camera_id=$1 AND profile_token=$2`,
      [cameraId, token]
    );
    if (!prof) return res.status(404).json({ error: 'Profile not found — call GET .../profiles first to sync' });

    await query(`UPDATE camera_media_profiles SET is_active_stream=FALSE WHERE camera_id=$1`, [cameraId]);
    await query(
      `UPDATE camera_media_profiles SET is_active_stream=TRUE WHERE camera_id=$1 AND profile_token=$2`,
      [cameraId, token]
    );

    // Refresh the stream URI and push it into cameras.rtsp_url so the NVR
    // core picks up the newly selected stream on its next config reload.
    let uri = prof.stream_uri;
    try { uri = await onvif.getStreamUri(cameraId, token); } catch {}
    if (uri) {
      await query(`UPDATE cameras SET rtsp_url=$1, updated_at=NOW() WHERE camera_id=$2`, [uri, cameraId]);
    }

    broadcast?.({ type: 'camera.stream_profile_changed', data: { camera_id: Number(cameraId), profile_token: token, stream_uri: uri } });
    res.json({ activated: token, stream_uri: uri, note: 'NVR core will use this stream on next config reload (POST /api/nvr/reload)' });
  } catch (err) { handleOnvifError(res, err, req.params.id); }
});

/* ------------------------------------------------------------------ */
/* PTZ                                                                  */
/* ------------------------------------------------------------------ */

router.get('/cameras/:id/ptz/status', authenticate, async (req, res) => {
  try {
    const status = await onvif.ptzGetStatus(req.params.id);
    await query(
      `INSERT INTO camera_ptz_status (camera_id, pan, tilt, zoom, move_status, updated_at)
       VALUES ($1,$2,$3,$4,$5,NOW())
       ON CONFLICT (camera_id) DO UPDATE SET
         pan=$2, tilt=$3, zoom=$4, move_status=$5, updated_at=NOW()`,
      [req.params.id, status.pan, status.tilt, status.zoom, status.move_status || 'IDLE']
    );
    res.json(status);
  } catch (err) { handleOnvifError(res, err, req.params.id); }
});

// POST /cameras/:id/ptz/move  { mode, x, y, zoom, speed, timeout }
router.post('/cameras/:id/ptz/move', authenticate, requireRole('OPERATOR', 'ADMIN'), async (req, res) => {
  try {
    const { mode = 'continuous', x = 0, y = 0, zoom = 0, speed, timeout } = req.body;
    const cameraId = req.params.id;

    let result;
    if (mode === 'continuous') result = await onvif.ptzContinuousMove(cameraId, { x, y, zoom, timeout });
    else if (mode === 'relative') result = await onvif.ptzRelativeMove(cameraId, { x, y, zoom, speed });
    else if (mode === 'absolute') result = await onvif.ptzAbsoluteMove(cameraId, { x, y, zoom, speed });
    else return res.status(400).json({ error: 'mode must be continuous, relative, or absolute' });

    res.json({ moved: true, mode, x, y, zoom });
  } catch (err) { handleOnvifError(res, err, req.params.id); }
});

router.post('/cameras/:id/ptz/stop', authenticate, requireRole('OPERATOR', 'ADMIN'), async (req, res) => {
  try {
    const { panTilt = true, zoom = true } = req.body || {};
    await onvif.ptzStop(req.params.id, { panTilt, zoom });
    res.json({ stopped: true });
  } catch (err) { handleOnvifError(res, err, req.params.id); }
});

router.post('/cameras/:id/ptz/home', authenticate, requireRole('OPERATOR', 'ADMIN'), async (req, res) => {
  try {
    const { action = 'goto', speed } = req.body || {};
    if (action === 'set') await onvif.ptzSetHome(req.params.id);
    else await onvif.ptzGotoHome(req.params.id, speed);
    res.json({ action });
  } catch (err) { handleOnvifError(res, err, req.params.id); }
});

// Presets — synced with DB so the UI has names even when the camera is briefly offline
router.get('/cameras/:id/ptz/presets', authenticate, async (req, res) => {
  try {
    const cameraId = req.params.id;
    let presets = [];
    try {
      presets = await onvif.ptzGetPresets(cameraId);
      for (const pr of presets) {
        await query(
          `INSERT INTO camera_ptz_presets (camera_id, preset_token, preset_name, pan, tilt, zoom, updated_at)
           VALUES ($1,$2,$3,$4,$5,$6,NOW())
           ON CONFLICT (camera_id, preset_token) DO UPDATE SET
             preset_name=$3, pan=$4, tilt=$5, zoom=$6, updated_at=NOW()`,
          [cameraId, pr.token, pr.name, pr.position?.panTilt?.x, pr.position?.panTilt?.y, pr.position?.zoom?.x]
        );
      }
    } catch (liveErr) {
      // Camera unreachable — fall back to last-known presets from DB
    }
    const rows = await query(`SELECT * FROM camera_ptz_presets WHERE camera_id=$1 ORDER BY preset_name`, [cameraId]);
    res.json({ presets: rows.rows, live: presets.length > 0 });
  } catch (err) { handleOnvifError(res, err, req.params.id); }
});

router.post('/cameras/:id/ptz/presets', authenticate, requireRole('OPERATOR', 'ADMIN'), async (req, res) => {
  try {
    const { name } = req.body;
    if (!name) return res.status(400).json({ error: 'name is required' });
    const result = await onvif.ptzSetPreset(req.params.id, name);
    const token = result?.presetToken || result;
    res.status(201).json({ token, name });
  } catch (err) { handleOnvifError(res, err, req.params.id); }
});

router.post('/cameras/:id/ptz/presets/:token/goto', authenticate, requireRole('OPERATOR', 'ADMIN'), async (req, res) => {
  try {
    const { speed } = req.body || {};
    await onvif.ptzGotoPreset(req.params.id, req.params.token, speed);
    res.json({ moved_to: req.params.token });
  } catch (err) { handleOnvifError(res, err, req.params.id); }
});

router.delete('/cameras/:id/ptz/presets/:token', authenticate, requireRole('ADMIN'), async (req, res) => {
  try {
    await onvif.ptzRemovePreset(req.params.id, req.params.token);
    await query(`DELETE FROM camera_ptz_presets WHERE camera_id=$1 AND preset_token=$2`, [req.params.id, req.params.token]);
    res.json({ deleted: req.params.token });
  } catch (err) { handleOnvifError(res, err, req.params.id); }
});

/* ------------------------------------------------------------------ */
/* Imaging settings (brightness / contrast / saturation / sharpness)  */
/* ------------------------------------------------------------------ */

router.get('/cameras/:id/image', authenticate, async (req, res) => {
  try {
    const cameraId = req.params.id;
    const live = await onvif.getImagingSettings(cameraId);

    const row = await queryOne(
      `INSERT INTO camera_image_settings
         (camera_id, video_source_token, brightness, contrast, color_saturation,
          sharpness, ir_cut_filter, wide_dynamic_range, exposure_mode, last_synced_at)
       VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,NOW())
       ON CONFLICT (camera_id) DO UPDATE SET
         video_source_token=$2, brightness=$3, contrast=$4, color_saturation=$5,
         sharpness=$6, ir_cut_filter=$7, wide_dynamic_range=$8, exposure_mode=$9,
         last_synced_at=NOW()
       RETURNING *`,
      [
        cameraId, live.token,
        live.brightness, live.contrast, live.colorSaturation, live.sharpness,
        live.irCutFilter || 'AUTO',
        live.wideDynamicRange?.mode === 'ON',
        live.exposure?.mode || 'AUTO',
      ]
    );
    res.json(row);
  } catch (err) { handleOnvifError(res, err, req.params.id); }
});

router.put('/cameras/:id/image', authenticate, requireRole('OPERATOR', 'ADMIN'), async (req, res) => {
  try {
    const cameraId = req.params.id;
    const { brightness, contrast, saturation, sharpness, ir_cut_filter, wide_dynamic_range, exposure_mode } = req.body;

    const settings = {};
    if (brightness !== undefined) settings.brightness = brightness;
    if (contrast !== undefined) settings.contrast = contrast;
    if (saturation !== undefined) settings.colorSaturation = saturation;
    if (sharpness !== undefined) settings.sharpness = sharpness;
    if (ir_cut_filter !== undefined) settings.irCutFilter = ir_cut_filter;
    if (wide_dynamic_range !== undefined) settings.wideDynamicRange = { mode: wide_dynamic_range ? 'ON' : 'OFF' };
    if (exposure_mode !== undefined) settings.exposure = { mode: exposure_mode };

    await onvif.setImagingSettings(cameraId, settings);

    const row = await queryOne(
      `UPDATE camera_image_settings SET
         brightness        = COALESCE($2, brightness),
         contrast          = COALESCE($3, contrast),
         color_saturation  = COALESCE($4, color_saturation),
         sharpness         = COALESCE($5, sharpness),
         ir_cut_filter     = COALESCE($6, ir_cut_filter),
         wide_dynamic_range= COALESCE($7, wide_dynamic_range),
         exposure_mode     = COALESCE($8, exposure_mode),
         updated_at = NOW()
       WHERE camera_id=$1
       RETURNING *`,
      [cameraId, brightness, contrast, saturation, sharpness, ir_cut_filter, wide_dynamic_range, exposure_mode]
    );
    res.json(row);
  } catch (err) { handleOnvifError(res, err, req.params.id); }
});

/* ------------------------------------------------------------------ */
/* Audio settings                                                       */
/* ------------------------------------------------------------------ */

router.get('/cameras/:id/audio', authenticate, async (req, res) => {
  try {
    const cameraId = req.params.id;
    const configs = await onvif.getAudioEncoderConfigurations(cameraId).catch(() => []);
    const cfg = Array.isArray(configs) ? configs[0] : configs;

    const row = await queryOne(
      `INSERT INTO camera_audio_settings
         (camera_id, audio_encoder_token, encoding, bitrate_kbps, sample_rate_khz, last_synced_at)
       VALUES ($1,$2,$3,$4,$5,NOW())
       ON CONFLICT (camera_id) DO UPDATE SET
         audio_encoder_token=$2, encoding=$3, bitrate_kbps=$4, sample_rate_khz=$5,
         last_synced_at=NOW()
       RETURNING *`,
      [
        cameraId,
        cfg?.$ ? cfg.$.token : cfg?.token,
        cfg?.encoding,
        cfg?.bitrate,
        cfg?.sampleRate,
      ]
    );
    res.json(row);
  } catch (err) { handleOnvifError(res, err, req.params.id); }
});

router.put('/cameras/:id/audio', authenticate, requireRole('OPERATOR', 'ADMIN'), async (req, res) => {
  try {
    const cameraId = req.params.id;
    const { encoding, bitrate_kbps, sample_rate_khz, is_enabled } = req.body;

    const existing = await queryOne(`SELECT * FROM camera_audio_settings WHERE camera_id=$1`, [cameraId]);
    if (existing?.audio_encoder_token) {
      await onvif.setAudioEncoderConfiguration(cameraId, {
        token: existing.audio_encoder_token,
        encoding: encoding || existing.encoding,
        bitrate: bitrate_kbps || existing.bitrate_kbps,
        sampleRate: sample_rate_khz || existing.sample_rate_khz,
      }).catch((e) => { throw e; });
    }

    const row = await queryOne(
      `UPDATE camera_audio_settings SET
         encoding        = COALESCE($2, encoding),
         bitrate_kbps    = COALESCE($3, bitrate_kbps),
         sample_rate_khz = COALESCE($4, sample_rate_khz),
         is_enabled      = COALESCE($5, is_enabled),
         updated_at = NOW()
       WHERE camera_id=$1
       RETURNING *`,
      [cameraId, encoding, bitrate_kbps, sample_rate_khz, is_enabled]
    );
    res.json(row);
  } catch (err) { handleOnvifError(res, err, req.params.id); }
});

module.exports = router;
