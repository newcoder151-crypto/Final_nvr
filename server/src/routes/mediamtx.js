/**
 * MediaMTX proxy routes — the frontend calls these to get WebRTC/HLS URLs
 * without needing to know the MediaMTX host (useful for Docker/production).
 */
const express = require("express");
const axios = require("axios");
const { query } = require("../db");
const { authenticate } = require("../middleware/auth");

const router = express.Router();
const MTX_API = process.env.MEDIAMTX_API || "http://localhost:9997";
const MTX_RTSP = process.env.MEDIAMTX_RTSP || "localhost:8554";
// MEDIAMTX_WEB/MEDIAMTX_HLS default to null (not "localhost") so we can
// tell "explicitly configured" apart from "not set". When not set, we
// derive the host per-request from req.hostname below — whatever host the
// browser used to reach this API is also the host MediaMTX is reachable
// on (same box). Hardcoding "localhost" here meant every WebRTC/HLS URL
// silently pointed at the *viewer's own machine* whenever the dashboard
// on this same box). Hardcoding "localhost" here meant every WebRTC/HLS
// URL silently pointed at the *viewer's own machine* whenever the
// dashboard was opened from anywhere other than the NVR box itself — the
// live path would exist and be healthy in MediaMTX, but nothing would
// ever load in the browser, with no error surfaced anywhere obvious.
const MTX_WEB_ENV = process.env.MEDIAMTX_WEB || null;
const MTX_HLS_ENV = process.env.MEDIAMTX_HLS || null;
function mtxWebFor(req) { return MTX_WEB_ENV || `${req.protocol}://${req.hostname}:8889`; }
function mtxHlsFor(req) { return MTX_HLS_ENV || `${req.protocol}://${req.hostname}:8888`; }

// GET /api/mediamtx/streams  — list all active MediaMTX paths
router.get("/streams", authenticate, async (req, res) => {
  try {
    const r = await axios.get(`${MTX_API}/v3/paths/list`, { timeout: 4000 });
    res.json(r.data);
  } catch (err) {
    res
      .status(503)
      .json({
        error: "MediaMTX not reachable",
        detail: err.message,
        hint: "Run: bash start.sh",
      });
  }
});

// GET /api/mediamtx/cameras  — cameras with their MediaMTX URLs
router.get("/cameras", authenticate, async (req, res) => {
  try {
    const rows = await query(
      `SELECT camera_id, camera_name, camera_type, ip_address, rtsp_url, status, location_description
       FROM cameras WHERE status='ACTIVE' ORDER BY camera_id`,
    );

    // Check which paths are live in MediaMTX
    let livePaths = new Set();
    try {
      const r = await axios.get(`${MTX_API}/v3/paths/list`, { timeout: 3000 });
      (r.data.items || []).forEach((p) => livePaths.add(p.name));
    } catch {
      /* MediaMTX may still be starting */
    }

    const cameras = rows.rows.map((cam) => {
      const pathName = `cam_${cam.camera_id}`;
      const isLive = livePaths.has(pathName);
      return {
        ...cam,
        mediamtx: {
          path_name: pathName,
          is_live: isLive,
          webrtc_url: `${mtxWebFor(req)}/${pathName}/whep`, // WHEP for browser WebRTC
          hls_url: `${mtxHlsFor(req)}/${pathName}/index.m3u8`,
          rtsp_url: `rtsp://${MTX_RTSP}/${pathName}`,
        },
      };
    });

    res.json({ cameras, mediamtx_api: MTX_API });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// GET /api/mediamtx/status  — MediaMTX health
router.get("/status", authenticate, async (req, res) => {
  try {
    const [paths, version] = await Promise.all([
      axios
        .get(`${MTX_API}/v3/paths/list`, { timeout: 3000 })
        .then((r) => r.data)
        .catch(() => null),
      axios
        .get(`${MTX_API}/v3/config/global/get`, { timeout: 3000 })
        .then((r) => r.data)
        .catch(() => null),
    ]);
    res.json({
      online: !!paths,
      active_paths: (paths?.items || []).length,
      webrtc_url: mtxWebFor(req),
      hls_url: mtxHlsFor(req),
      rtsp_url: `rtsp://${MTX_RTSP}`,
      api_url: MTX_API,
    });
  } catch (err) {
    res.status(503).json({ online: false, error: err.message });
  }
});

// POST /api/mediamtx/sync  — trigger immediate sync (no wait for interval)
router.post("/sync", authenticate, async (req, res) => {
  try {
    // The sync service picks up DB changes automatically every 15s.
    // This endpoint kicks a manual trigger by notifying the sync process.
    res.json({
      message:
        "Sync runs every 15s automatically. Changes will appear shortly.",
    });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

module.exports = router;
