const express = require('express');
const bcrypt = require('bcryptjs');
const { query, queryOne } = require('../db');
const { generateToken, authenticate, authLimiter } = require('../middleware/auth');

const router = express.Router();

// POST /api/auth/login
router.post('/login', authLimiter, async (req, res) => {
  try {
    const { username, password } = req.body;
    if (!username || !password) return res.status(400).json({ error: 'username and password required' });

    const user = await queryOne(`SELECT * FROM users WHERE username=$1`, [username]);
    if (!user || !user.is_active) return res.status(401).json({ error: 'Invalid credentials' });
    if (user.is_locked) return res.status(403).json({ error: 'Account locked — contact administrator' });

    const valid = await bcrypt.compare(password, user.password_hash);
    if (!valid) {
      await query(
        `UPDATE users SET failed_login_attempts=failed_login_attempts+1, last_failed_login=NOW(),
         is_locked=CASE WHEN failed_login_attempts+1>=5 THEN 1 ELSE 0 END WHERE user_id=$1`,
        [user.user_id]
      );
      return res.status(401).json({ error: 'Invalid credentials' });
    }

    await query(`UPDATE users SET failed_login_attempts=0, last_login_at=NOW(), last_login_ip=$2 WHERE user_id=$1`,
      [user.user_id, req.ip]);

    const token = generateToken(user);
    res.json({
      token,
      user: { user_id: user.user_id, username: user.username, full_name: user.full_name, email: user.email, role: user.role }
    });
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// POST /api/auth/register
router.post('/register', authLimiter, async (req, res) => {
  try {
    const { username, password, full_name, email } = req.body;
    if (!username || !password || !full_name)
      return res.status(400).json({ error: 'username, password, full_name required' });

    const existing = await queryOne('SELECT user_id FROM users WHERE username=$1', [username]);
    if (existing) return res.status(409).json({ error: 'Username already taken' });

    const hash = await bcrypt.hash(password, 12);
    const user = await queryOne(
      `INSERT INTO users(username, password_hash, full_name, email, role)
       VALUES($1,$2,$3,$4,'VIEWER') RETURNING user_id, username, full_name, email, role`,
      [username, hash, full_name, email || null]
    );
    res.status(201).json({ token: generateToken(user), user });
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// GET /api/auth/me
router.get('/me', authenticate, async (req, res) => {
  try {
    const user = await queryOne(
      `SELECT user_id, username, full_name, email, phone, role, last_login_at, created_at FROM users WHERE user_id=$1`,
      [req.user.sub]
    );
    if (!user) return res.status(404).json({ error: 'User not found' });
    res.json(user);
  } catch (err) { res.status(500).json({ error: err.message }); }
});

// POST /api/auth/refresh
router.post('/refresh', authenticate, async (req, res) => {
  try {
    const user = await queryOne('SELECT user_id, username, full_name, email, role FROM users WHERE user_id=$1', [req.user.sub]);
    if (!user) return res.status(404).json({ error: 'Not found' });
    res.json({ token: generateToken(user) });
  } catch (err) { res.status(500).json({ error: err.message }); }
});

module.exports = router;
