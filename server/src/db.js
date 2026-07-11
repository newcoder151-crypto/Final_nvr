require('dotenv').config();
const { Pool } = require('pg');

const pool = new Pool({
  host:     process.env.DB_HOST     || 'localhost',
  port:     parseInt(process.env.DB_PORT || '5432'),
  database: process.env.DB_NAME     || 'mnvr',
  user:     process.env.DB_USER     || 'mnvr',
  password: process.env.DB_PASSWORD || 'mnvrpass',
  max: 20,
  idleTimeoutMillis: 30000,
  connectionTimeoutMillis: 5000,
});

pool.on('error', (err) => console.error('[DB] Pool error:', err.message));

async function query(text, params) {
  try { return await pool.query(text, params); }
  catch (err) {
    console.error('[DB] Query error:', err.message, '\nSQL:', text.slice(0, 200));
    throw err;
  }
}

async function queryOne(text, params) {
  const res = await query(text, params);
  return res.rows[0] || null;
}

async function testConnection() {
  try {
    const r = await query('SELECT current_database(), version()');
    console.log(`[DB] Connected to: ${r.rows[0].current_database}`);
    return true;
  } catch (e) {
    console.error('[DB] Connection failed:', e.message);
    return false;
  }
}

module.exports = { pool, query, queryOne, testConnection };
