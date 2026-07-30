const express = require("express");
const path = require("path");
const fs = require("fs");
const { queryOne } = require("../db");
const { authenticate, requireRole } = require("../middleware/auth");
const router = express.Router();

const RECORDINGS_PATH = path.resolve(process.env.RECORDINGS_PATH || "/storage/recordings");
const HLS_PATH = path.resolve(process.env.HLS_PATH || "/storage/hls");

// A bare filename component: no path separators, no "..", no leading dot.
// Anything else is rejected outright rather than "sanitized" (OWASP A01/A03:
// reject-by-default is far safer than trying to strip traversal sequences).
const SAFE_SEGMENT_RE = /^[A-Za-z0-9._-]+\.(ts|m3u8)$/;
const SAFE_ID_RE = /^\d+$/;

/**
 * Resolve `candidate` under `root` and verify the resolved, real path is
 * actually contained within `root`. This is the authoritative check —
 * regexes on the input are a first filter, but symlinks or unusual path
 * segments could still escape a naive path.join(), so we always confirm
 * containment against the resolved real path before touching the file.
 * OWASP A01 (Broken Access Control) / path traversal.
 */
function safeResolve(root, candidate) {
  const resolved = path.resolve(root, candidate);
  if (resolved !== root && !resolved.startsWith(root + path.sep)) return null;
  return resolved;
}

function existsWithin(root, candidate) {
  const resolved = safeResolve(root, candidate);
  if (!resolved) return null;
  try {
    // Resolve symlinks too, and re-check containment on the real path.
    const real = fs.realpathSync(resolved);
    const realRoot = fs.realpathSync(root);
    if (real !== realRoot && !real.startsWith(realRoot + path.sep)) return null;
    return fs.existsSync(real) ? real : null;
  } catch {
    return null;
  }
}

function resolveRecordingFile(rec) {
  const candidates = [];
  // An absolute file_path from the DB is only ever trusted if it was itself
  // written by the recorder process under RECORDINGS_PATH — we still route
  // it through safeResolve/existsWithin so a compromised or malformed DB
  // row can't be used to read arbitrary files.
  if (rec.file_path) {
    if (path.isAbsolute(rec.file_path)) {
      const rel = path.relative(RECORDINGS_PATH, rec.file_path);
      candidates.push(rel);
    } else {
      candidates.push(rec.file_path);
    }
  }
  if (rec.file_name && rec.camera_id && SAFE_ID_RE.test(String(rec.camera_id))) {
    candidates.push(path.join(`cam_${rec.camera_id}`, rec.file_name));
  }
  if (rec.file_name) candidates.push(rec.file_name);

  for (const c of candidates) {
    const found = existsWithin(RECORDINGS_PATH, c);
    if (found) return found;
  }
  return null;
}

// MP4 byte-range stream
router.get("/recordings/:id/stream", authenticate, async (req, res) => {
  try {
    if (!SAFE_ID_RE.test(req.params.id)) return res.status(400).json({ error: "Invalid recording id" });
    const rec = await queryOne(
      `SELECT r.recording_id, r.file_path, r.file_name, r.camera_id, c.camera_name
       FROM recordings r LEFT JOIN cameras c ON r.camera_id=c.camera_id WHERE r.recording_id=$1`,
      [req.params.id],
    );
    if (!rec) return res.status(404).json({ error: "Recording not found" });

    const fp = resolveRecordingFile(rec);
    if (!fp) {
      return res.status(404).json({
        error: "Video file not on disk yet",
        note: "Recorder generates files every 60s — wait for the next segment",
      });
    }

    const stat = fs.statSync(fp);
    const size = stat.size;
    const range = req.headers.range;

    if (range) {
      const match = /^bytes=(\d+)-(\d+)?$/.exec(range);
      if (!match) return res.status(416).end();
      const start = parseInt(match[1], 10);
      const end = match[2] ? parseInt(match[2], 10) : Math.min(start + 10 * 1024 * 1024 - 1, size - 1);
      if (start >= size || end >= size || start > end) {
        res.setHeader("Content-Range", `bytes */${size}`);
        return res.status(416).end();
      }
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
    console.error("[streaming:stream]", err);
    res.status(500).json({ error: "Failed to stream recording" });
  }
});

// Download
router.get("/recordings/:id/download", authenticate, async (req, res) => {
  try {
    if (!SAFE_ID_RE.test(req.params.id)) return res.status(400).json({ error: "Invalid recording id" });
    const rec = await queryOne(
      "SELECT recording_id, file_path, file_name, camera_id FROM recordings WHERE recording_id=$1",
      [req.params.id],
    );
    if (!rec) return res.status(404).json({ error: "Not found" });
    const fp = resolveRecordingFile(rec);
    if (!fp) return res.status(404).json({ error: "File not on disk" });
    // Force a safe download filename — never reflect the DB's file_name
    // verbatim into a Content-Disposition header without sanitizing it.
    const safeName = path.basename(fp);
    res.download(fp, safeName);
  } catch (err) {
    console.error("[streaming:download]", err);
    res.status(500).json({ error: "Failed to download recording" });
  }
});

// HLS live playlist
router.get("/hls/:cameraId/stream.m3u8", authenticate, async (req, res) => {
  try {
    const cid = req.params.cameraId;
    if (!SAFE_ID_RE.test(cid)) return res.status(400).json({ error: "Invalid camera id" });

    const found =
      existsWithin(HLS_PATH, path.join(`cam_${cid}`, "stream.m3u8")) ||
      existsWithin(HLS_PATH, path.join(cid, "stream.m3u8"));

    if (!found) {
      return res.status(404).json({
        error: "HLS playlist not yet available",
        note: "Recorder is starting — first segment generates in up to 60s",
      });
    }
    res.setHeader("Content-Type", "application/vnd.apple.mpegurl");
    res.setHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    return res.sendFile(found);
  } catch (err) {
    console.error("[streaming:hls-playlist]", err);
    res.status(500).json({ error: "Failed to load playlist" });
  }
});

// HLS .ts segments
router.get("/hls/:cameraId/:segment", authenticate, (req, res) => {
  try {
    const { cameraId, segment } = req.params;
    if (!SAFE_ID_RE.test(cameraId)) return res.status(400).end();
    // Strict allow-list: bare filename, no separators, no "..", correct
    // extension. This is the primary fix for the path-traversal issue that
    // previously only checked the *suffix* of the segment name.
    if (!SAFE_SEGMENT_RE.test(segment)) return res.status(400).end();

    const found =
      existsWithin(HLS_PATH, path.join(`cam_${cameraId}`, segment)) ||
      existsWithin(HLS_PATH, path.join(cameraId, segment));

    if (!found) return res.status(404).end();

    res.setHeader(
      "Content-Type",
      segment.endsWith(".m3u8") ? "application/vnd.apple.mpegurl" : "video/MP2T",
    );
    res.setHeader("Cache-Control", "no-cache");
    return res.sendFile(found);
  } catch {
    res.status(404).end();
  }
});

// UDP stream info (for mNVR core)
router.get("/udp/:cameraId/info", authenticate, (req, res) => {
  if (!SAFE_ID_RE.test(req.params.cameraId)) return res.status(400).json({ error: "Invalid camera id" });
  const cid = parseInt(req.params.cameraId, 10);
  const udp_port = 5000 + cid * 2;
  res.json({
    camera_id: cid,
    udp_url: `udp://127.0.0.1:${udp_port}`,
    rtp_port: udp_port,
    protocol: "RTP/H264",
    note: "mNVR core streamer_module.c formula: 5000 + camera_id * 2",
  });
});

// Debug: list files on disk.
// OWASP A01: this enumerates the entire recordings filesystem tree, which is
// operationally useful but not something a VIEWER-role account should see.
router.get("/list", authenticate, requireRole("ADMIN"), (req, res) => {
  const files = [];
  const walk = (dir, depth) => {
    if (depth > 3 || !fs.existsSync(dir)) return;
    fs.readdirSync(dir).forEach((f) => {
      const full = path.join(dir, f);
      try {
        const stat = fs.statSync(full);
        if (stat.isDirectory()) walk(full, depth + 1);
        else files.push({ path: path.relative(RECORDINGS_PATH, full), size: stat.size, mtime: stat.mtime });
      } catch {}
    });
  };
  walk(RECORDINGS_PATH, 0);
  res.json({ count: files.length, files: files.slice(0, 200) });
});

module.exports = router;
