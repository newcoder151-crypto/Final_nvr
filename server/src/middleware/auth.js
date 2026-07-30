const jwt = require('jsonwebtoken');
const rateLimit = require('express-rate-limit');
const { JWT_SECRET, VALID_ROLES } = require('../config/security');

const ISSUER = 'railway-nvr-api';

/**
 * OWASP A07 (Identification & Authentication Failures):
 * - Token is only ever accepted from the Authorization header, never from a
 *   query string. Query-string tokens leak into access logs, proxy logs,
 *   browser history, and Referer headers.
 * - Signature algorithm is pinned (no "alg: none" / algorithm confusion).
 * - Issuer is verified.
 */
function authenticate(req, res, next) {
  const h = req.headers.authorization;
  const token = h && h.startsWith('Bearer ') ? h.slice(7).trim() : null;

  if (!token) return res.status(401).json({ error: 'Authorization token required' });

  try {
    const decoded = jwt.verify(token, JWT_SECRET, {
      algorithms: ['HS256'],
      issuer: ISSUER,
    });
    if (!decoded || !decoded.sub || !VALID_ROLES.includes(decoded.role)) {
      return res.status(401).json({ error: 'Invalid or expired token' });
    }
    req.user = decoded;

    // OWASP A07: `must_change_password` was stored on the user row but
    // never actually enforced anywhere — meaning "force a password change
    // on first login" (used for the shipped default admin account) was a
    // no-op. Enforce it here: block every route except the small set a
    // user needs to actually change their password / sign out.
    const SELF_SERVICE_PATHS = new Set([
      '/api/auth/change-password',
      '/api/auth/me',
      '/api/auth/refresh',
    ]);
    if (decoded.must_change_password && !SELF_SERVICE_PATHS.has(req.baseUrl + req.path)) {
      return res.status(403).json({
        error: 'Password change required before continuing',
        code: 'MUST_CHANGE_PASSWORD',
      });
    }

    next();
  } catch {
    // Never reflect the underlying jwt error (expired vs malformed vs bad
    // signature) back to the client — that distinction only helps an
    // attacker probing for valid-but-expired tokens to replay.
    return res.status(401).json({ error: 'Invalid or expired token' });
  }
}

/**
 * OWASP A01 (Broken Access Control): centralized, fail-closed role check.
 * Every route that mutates state or exposes non-public data must pair
 * `authenticate` with `requireRole(...)`.
 */
function requireRole(...roles) {
  const wanted = roles.map((r) => r.toUpperCase());
  return (req, res, next) => {
    if (!req.user) return res.status(401).json({ error: 'Not authenticated' });
    if (!wanted.includes(req.user.role)) {
      return res.status(403).json({ error: `Requires role: ${roles.join(' or ')}` });
    }
    next();
  };
}

function generateToken(user) {
  return jwt.sign(
    {
      sub: user.user_id,
      username: user.username,
      role: user.role,
      full_name: user.full_name,
      must_change_password: !!user.must_change_password,
    },
    JWT_SECRET,
    { expiresIn: '8h', issuer: ISSUER, algorithm: 'HS256' },
  );
}

// Auth endpoints (login/register) are the highest-value brute-force target.
const authLimiter = rateLimit({
  windowMs: 15 * 60 * 1000,
  max: 10,
  standardHeaders: true,
  legacyHeaders: false,
  message: { error: 'Too many attempts, try again later' },
  // Rate-limit by IP + attempted username so one noisy IP behind NAT doesn't
  // lock out an entire office, while still stopping credential stuffing.
  keyGenerator: (req) => `${req.ip}:${(req.body && req.body.username) || ''}`,
});

module.exports = { authenticate, requireRole, generateToken, authLimiter };
