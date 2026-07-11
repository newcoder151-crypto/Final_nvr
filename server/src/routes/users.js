const express = require('express');
const bcrypt = require('bcryptjs');
const { query, queryOne } = require('../db');
const { authenticate, requireRole } = require('../middleware/auth');
const router = express.Router();

// GET /api/users
router.get('/', authenticate, requireRole('ADMIN'), async (req, res) => {
  try {
    const rows = await query(
      `SELECT user_id, username, full_name, email, phone, role, is_active, is_locked,
              failed_login_attempts, created_at, last_login_at, created_by
       FROM users ORDER BY created_at DESC`);
    res.json({ users: rows.rows, total: rows.rowCount });
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// GET /api/users/:id
router.get('/:id', authenticate, requireRole('ADMIN'), async (req, res) => {
  try {
    const user = await queryOne(
      `SELECT user_id, username, full_name, email, phone, role, is_active, is_locked,
              created_at, last_login_at FROM users WHERE user_id=$1`, [req.params.id]);
    if (!user) return res.status(404).json({ error: 'User not found' });
    res.json(user);
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// POST /api/users
router.post('/', authenticate, requireRole('ADMIN'), async (req, res) => {
  try {
    const { username, password, full_name, email, phone, role = 'VIEWER' } = req.body;
    if (!username || !password || !full_name) return res.status(400).json({ error: 'username, password, full_name required' });
    if (await queryOne('SELECT user_id FROM users WHERE username=$1', [username]))
      return res.status(409).json({ error: 'Username already taken' });
    const hash = await bcrypt.hash(password, 12);
    const user = await queryOne(
      `INSERT INTO users(username, password_hash, full_name, email, phone, role, created_by)
       VALUES($1,$2,$3,$4,$5,$6,$7) RETURNING user_id, username, full_name, email, phone, role, created_at`,
      [username, hash, full_name, email || null, phone || null, role.toUpperCase(), req.user.username]);
    res.status(201).json(user);
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// PUT /api/users/:id
router.put('/:id', authenticate, requireRole('ADMIN'), async (req, res) => {
  try {
    const { full_name, email, phone, role, is_active, is_locked } = req.body;
    const user = await queryOne(
      `UPDATE users SET
         full_name=COALESCE($1, full_name), email=COALESCE($2, email),
         phone=COALESCE($3, phone), role=COALESCE($4, role),
         is_active=COALESCE($5, is_active), is_locked=COALESCE($6, is_locked),
         updated_at=NOW()
       WHERE user_id=$7
       RETURNING user_id, username, full_name, email, phone, role, is_active, is_locked`,
      [full_name || null, email || null, phone || null, role ? role.toUpperCase() : null,
       is_active !== undefined ? is_active : null, is_locked !== undefined ? is_locked : null, req.params.id]);
    if (!user) return res.status(404).json({ error: 'User not found' });
    res.json(user);
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// PUT /api/users/:id/password
router.put('/:id/password', authenticate, requireRole('ADMIN'), async (req, res) => {
  try {
    const { password } = req.body;
    if (!password) return res.status(400).json({ error: 'password required' });
    const hash = await bcrypt.hash(password, 12);
    await query('UPDATE users SET password_hash=$1, must_change_password=0, updated_at=NOW() WHERE user_id=$2', [hash, req.params.id]);
    res.json({ message: 'Password updated' });
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// DELETE /api/users/:id (soft-delete — deactivate)
router.delete('/:id', authenticate, requireRole('ADMIN'), async (req, res) => {
  try {
    if (parseInt(req.params.id) === req.user.sub) return res.status(400).json({ error: 'Cannot delete yourself' });
    await query('UPDATE users SET is_active=0, updated_at=NOW() WHERE user_id=$1', [req.params.id]);
    res.json({ message: 'User deactivated' });
  } catch (err) { res.status(500).json({ error: err.message }); }
});

module.exports = router;
