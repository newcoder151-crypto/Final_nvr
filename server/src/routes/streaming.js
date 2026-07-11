const express = require("express");
const path = require("path");
const fs = require("fs");
const { queryOne } = require("../db");
const { authenticate } = require("../middleware/auth");
const router = express.Router();

const RECORDINGS_PATH = process.env.RECORDINGS_PATH || "/storage/recordings";
const HLS_PATH = process.env.HLS_PATH || "/storage/hls";

function resolveFile(rec) {
  const candidates = [
    rec.file_path && path.isAbsolute(rec.file_path) ? rec.file_path : null,
    rec.file_path ? path.join(RECORDINGS_PATH, rec.file_path) : null,
    rec.file_name && rec.camera_id
      ? path.join(RECORDINGS_PATH, `cam_${rec.camera_id}`, rec.file_name)
      : null,
    rec.file_name ? path.join(RECORDINGS_PATH, rec.file_name) : null,
  ].filter(Boolean);
  for (const p of candidates) {
    try {
      if (fs.existsSync(p)) return p;
    } catch {}
  }
  return null;
}

// MP4 byte-range stream
router.get("/recordings/:id/stream", authenticate, async (req, res) => {
  try {
    const rec = await queryOne(
      `SELECT r.recording_id, r.file_path, r.file_name, r.camera_id, c.camera_name
       FROM recordings r LEFT JOIN cameras c ON r.camera_id=c.camera_id WHERE r.recording_id=$1`,
      [req.params.id],
    );
    if (!rec) return res.status(404).json({ error: "Recording not found" });

    const fp = resolveFile(rec);
    if (!fp)
      return res.status(404).json({
        error: "Video file not on disk yet",
        note: "Recorder generates files every 60s — wait for the next segment",
        recordings_path: RECORDINGS_PATH,
      });

    const stat = fs.statSync(fp);
    const size = stat.size;
    const range = req.headers.range;

    if (range) {
      const [s, e] = range.replace(/bytes=/, "").split("-");
      const start = parseInt(s, 10);
      const end = e
        ? parseInt(e, 10)
        : Math.min(start + 10 * 1024 * 1024 - 1, size - 1);
      res.writeHead(206, {
        "Content-Range": `bytes ${start}-${end}/${size}`,
        "Accept-Ranges": "bytes",
        "Content-Length": end - start + 1,
        "Content-Type": "video/mp4",
        "Cache-Control": "no-cache",
      });
      fs.createReadStream(fp, { start, end }).pipe(res);
    } else {
      res.writeHead(200, {
        "Content-Length": size,
        "Content-Type": "video/mp4",
        "Accept-Ranges": "bytes",
        "Cache-Control": "no-cache",
      });
      fs.createReadStream(fp).pipe(res);
    }
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// Download
router.get("/recordings/:id/download", authenticate, async (req, res) => {
  try {
    const rec = await queryOne(
      "SELECT * FROM recordings WHERE recording_id=$1",
      [req.params.id],
    );
    if (!rec) return res.status(404).json({ error: "Not found" });
    const fp = resolveFile(rec);
    if (!fp) return res.status(404).json({ error: "File not on disk" });
    res.download(fp, rec.file_name);
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// HLS live playlist
router.get("/hls/:cameraId/stream.m3u8", authenticate, async (req, res) => {
  try {
    const cid = req.params.cameraId;
    const cam = await queryOne(
      "SELECT hls_output_dir FROM cameras WHERE camera_id=$1",
      [cid],
    );
    const candidates = [
      cam?.hls_output_dir ? path.join(cam.hls_output_dir, "stream.m3u8") : null,
      path.join(HLS_PATH, `cam_${cid}`, "stream.m3u8"),
      path.join(HLS_PATH, String(cid), "stream.m3u8"),
    ].filter(Boolean);

    for (const p of candidates) {
      if (fs.existsSync(p)) {
        res.setHeader("Content-Type", "application/vnd.apple.mpegurl");
        res.setHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        return res.sendFile(path.resolve(p));
      }
    }
    // Not ready yet — recorder is still on first segment
    res.status(404).json({
      error: "HLS playlist not yet available",
      note: "Recorder is starting — first segment generates in up to 60s",
      checked: candidates,
    });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// HLS .ts segments
router.get("/hls/:cameraId/:segment", authenticate, (req, res) => {
  try {
    const { cameraId, segment } = req.params;
    if (!segment.match(/\.(ts|m3u8)$/)) return res.status(400).end();
    const dirs = [
      path.join(HLS_PATH, `cam_${cameraId}`),
      path.join(HLS_PATH, cameraId),
    ];
    for (const dir of dirs) {
      const p = path.join(dir, segment);
      if (fs.existsSync(p)) {
        res.setHeader(
          "Content-Type",
          segment.endsWith(".m3u8")
            ? "application/vnd.apple.mpegurl"
            : "video/MP2T",
        );
        res.setHeader("Cache-Control", "no-cache");
        return res.sendFile(path.resolve(p));
      }
    }
    res.status(404).end();
  } catch {
    res.status(404).end();
  }
});

// UDP stream info (for mNVR core)
router.get("/udp/:cameraId/info", authenticate, (req, res) => {
  const cid = parseInt(req.params.cameraId);
  const udp_port = 5000 + cid * 2;
  res.json({
    camera_id: cid,
    udp_url: `udp://127.0.0.1:${udp_port}`,
    rtp_port: udp_port,
    protocol: "RTP/H264",
    note: "mNVR core streamer_module.c formula: 5000 + camera_id * 2",
  });
});

// Debug: list files on disk
router.get("/list", authenticate, (req, res) => {
  const files = [];
  const walk = (dir, depth) => {
    if (depth > 3 || !fs.existsSync(dir)) return;
    fs.readdirSync(dir).forEach((f) => {
      const full = path.join(dir, f);
      try {
        const stat = fs.statSync(full);
        if (stat.isDirectory()) walk(full, depth + 1);
        else files.push({ path: full, size: stat.size, mtime: stat.mtime });
      } catch {}
    });
  };
  walk(RECORDINGS_PATH, 0);
  res.json({
    recordings_path: RECORDINGS_PATH,
    hls_path: HLS_PATH,
    count: files.length,
    files: files.slice(0, 200),
  });
});

module.exports = router;
