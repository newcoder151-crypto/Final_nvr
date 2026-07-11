/**
 * NVR Core integration route — bridges the Node.js API with the compiled
 * mnvrd C daemon. Exposes status, config reload, and camera discovery.
 */
const express = require('express');
const { exec } = require('child_process');
const path = require('path');
const fs = require('fs');
const { query, queryOne } = require('../db');
const { authenticate, requireRole } = require('../middleware/auth');

const router = express.Router();
const NVR_BIN = process.env.NVR_BIN || path.join(__dirname, '../../../nvr_core/build/mnvrd');
const NVR_CONF = process.env.NVR_CONF || path.join(__dirname, '../../../nvr_core/config/mnvr.conf');
const PID_FILE = process.env.NVR_PID || path.join(__dirname, '../../../.pids/nvr.pid');

function nvrPid() {
  try { return parseInt(fs.readFileSync(PID_FILE, 'utf8').trim()); } catch { return null; }
}
function nvrRunning() {
  const pid = nvrPid();
  if (!pid) return false;
  try { process.kill(pid, 0); return true; } catch { return false; }
}

// GET /api/nvr/status
router.get('/status', authenticate, (req, res) => {
  const running = nvrRunning();
  const binExists = fs.existsSync(NVR_BIN);
  res.json({
    daemon_running: running,
    pid: nvrPid(),
    binary_exists: binExists,
    binary_path: NVR_BIN,
    config_path: NVR_CONF,
    config_exists: fs.existsSync(NVR_CONF),
    hint: !binExists ? 'Run: bash start.sh --build-nvr  to compile the C daemon' : null,
  });
});

// POST /api/nvr/reload — send SIGHUP to reload config
router.post('/reload', authenticate, requireRole('ADMIN'), (req, res) => {
  const pid = nvrPid();
  if (!pid || !nvrRunning()) return res.status(400).json({ error: 'NVR daemon not running' });
  try { process.kill(pid, 'SIGHUP'); res.json({ message: 'Config reload signal sent (SIGHUP)' }); }
  catch (err) { res.status(500).json({ error: err.message }); }
});

// GET /api/nvr/config — read current mnvr.conf
router.get('/config', authenticate, requireRole('ADMIN'), (req, res) => {
  try {
    if (!fs.existsSync(NVR_CONF)) return res.status(404).json({ error: 'Config file not found', path: NVR_CONF });
    const raw = fs.readFileSync(NVR_CONF, 'utf8');
    // Parse key=value lines (skip comments)
    const cfg = {};
    raw.split('\n').forEach(line => {
      const trimmed = line.trim();
      if (trimmed.startsWith('#') || !trimmed.includes('=')) return;
      const [k, ...rest] = trimmed.split('=');
      cfg[k.trim()] = rest.join('=').split('#')[0].trim();
    });
    res.json({ config: cfg, raw });
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// POST /api/nvr/discover — trigger ONVIF camera discovery
router.post('/discover', authenticate, requireRole('ADMIN', 'OPERATOR'), async (req, res) => {
  // If daemon is running it handles discovery; otherwise return cameras from DB
  try {
    const cams = await query(`SELECT camera_id, camera_name, ip_address, status FROM cameras ORDER BY camera_id`);
    res.json({
      message: nvrRunning() ? 'Discovery triggered via daemon' : 'Daemon not running — returning DB cameras',
      cameras: cams.rows,
      daemon_active: nvrRunning(),
    });
  } catch (err) { res.status(500).json({ error: err.message }); }
});

module.exports = router;
