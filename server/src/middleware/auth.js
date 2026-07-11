const jwt = require('jsonwebtoken');
const rateLimit = require('express-rate-limit');

const JWT_SECRET = process.env.JWT_SECRET || 'railway-nvr-secret-change-in-production';

function authenticate(req, res, next) {
  let token = null;
  const h = req.headers.authorization;
  if (h && h.startsWith('Bearer ')) token = h.slice(7);
  else if (req.query && req.query.token) token = String(req.query.token);
  if (!token) return res.status(401).json({ error: 'Authorization token required' });
  try {
    req.user = jwt.verify(token, JWT_SECRET);
    next();
  } catch {
    return res.status(401).json({ error: 'Invalid or expired token' });
  }
}

function requireRole(...roles) {
  return (req, res, next) => {
    if (!req.user) return res.status(401).json({ error: 'Not authenticated' });
    if (!roles.map(r => r.toUpperCase()).includes(req.user.role))
      return res.status(403).json({ error: `Requires role: ${roles.join(' or ')}` });
    next();
  };
}

function generateToken(user) {
  return jwt.sign(
    { sub: user.user_id, username: user.username, role: user.role, full_name: user.full_name },
    JWT_SECRET, { expiresIn: '24h' }
  );
}

const authLimiter = rateLimit({ windowMs: 15 * 60 * 1000, max: 30, message: { error: 'Too many attempts, try again later' } });

module.exports = { authenticate, requireRole, generateToken, authLimiter, JWT_SECRET };
