/**
 * Smart Semantic Search — natural-language queries over recordings + events.
 * Examples:
 *   "man in red shirt"  → searches AI event tags, description, detected labels
 *   "smoke near door"   → event_type SMOKE + camera type DOOR
 *   "crowd yesterday"   → density events in last 24h
 *   "camera 3 morning"  → camera_id=3, 06:00-12:00
 *   "person fallen"     → event_type PERSON_FALLEN
 *   "intrusion"         → INTRUSION events
 */
const express = require('express');
const { query } = require('../db');
const { authenticate } = require('../middleware/auth');
const router = express.Router();

// ── Keyword → DB field mappings ───────────────────────────────────────────────
const EVENT_TYPE_MAP = {
  crowd:       'CROWD_DENSITY', density: 'CROWD_DENSITY', congestion: 'CROWD_DENSITY',
  intrusion:   'INTRUSION', trespass: 'INTRUSION', unauthorized: 'INTRUSION',
  smoke:       'SMOKE', cigarette: 'SMOKE', fire: 'SMOKE', fumes: 'SMOKE',
  fallen:      'PERSON_FALLEN', fall: 'PERSON_FALLEN', leaning: 'PERSON_FALLEN',
  phone:       'PHONE_USE', mobile: 'PHONE_USE', 'mobile phone': 'PHONE_USE',
  stone:       'STONE_PELTING', pelt: 'STONE_PELTING', attack: 'STONE_PELTING',
  debris:      'OBSTACLE', obstacle: 'OBSTACLE', object: 'OBSTACLE',
  animal:      'ANIMAL_DETECTION', cattle: 'ANIMAL_DETECTION',
  pantograph:  'OHE_DEFECT', ohe: 'OHE_DEFECT', arcing: 'OHE_DEFECT',
  absent:      'CREW_ABSENT', absence: 'CREW_ABSENT',
  gesture:     'SIGNAL_GESTURE', signal: 'SIGNAL_GESTURE', acknowledgement: 'SIGNAL_GESTURE',
  emergency:   'EMERGENCY_BRAKE', brake: 'EMERGENCY_BRAKE',
  alarm:       'ALARM', tamper: 'TAMPERING', tampering: 'TAMPERING',
  motion:      'MOTION', movement: 'MOTION',
};

const CAMERA_TYPE_MAP = {
  door: 'DOOR', entrance: 'DOOR', exit: 'DOOR',
  interior: 'INTERIOR', passenger: 'INTERIOR', coach: 'INTERIOR',
  exterior: 'EXTERIOR', outside: 'EXTERIOR', side: 'EXTERIOR',
  driver: 'DRIVER_CAB', cab: 'DRIVER_CAB', front: 'DRIVER_CAB',
};

const COLOR_KEYWORDS = ['red','blue','green','yellow','white','black','orange','purple','grey','gray','pink','brown'];
const CLOTHING_KEYWORDS = ['shirt','jacket','uniform','coat','dress','vest','helmet','cap','hat','trousers','pants'];

// Time-of-day ranges (hours)
const TIME_MAP = {
  morning:   [6,  12], afternoon: [12, 17], evening: [17, 21],
  night:     [21, 6],  midnight:  [0,  4],  dawn:    [4,  7],
};
const RELATIVE_TIME = {
  today:      0, yesterday: 1, 'this week': 7, 'last week': 7,
  'this month': 30, 'last month': 30, recent: 1, latest: 0,
};

function parseQuery(q) {
  const lower = q.toLowerCase().trim();
  const tokens = lower.split(/\s+/);
  const result = {
    eventTypes: [],
    cameraTypes: [],
    colors: [],
    clothing: [],
    timeFilter: null,      // { start, end } ISO strings
    timeOfDay: null,       // { from_hour, to_hour }
    cameraIds: [],
    severities: [],
    freeText: [],
    searchRecordings: false,
    searchEvents: true,
  };

  // Detect event types
  for (const [kw, type] of Object.entries(EVENT_TYPE_MAP)) {
    if (lower.includes(kw)) result.eventTypes.push(type);
  }

  // Detect camera types
  for (const [kw, type] of Object.entries(CAMERA_TYPE_MAP)) {
    if (lower.includes(kw)) result.cameraTypes.push(type);
  }

  // Detect colors (clothing color detection)
  for (const color of COLOR_KEYWORDS) {
    if (lower.includes(color)) result.colors.push(color);
  }

  // Detect clothing items
  for (const item of CLOTHING_KEYWORDS) {
    if (lower.includes(item)) result.clothing.push(item);
  }

  // Detect severity
  if (lower.includes('critical') || lower.includes('emergency')) result.severities.push('CRITICAL','EMERGENCY');
  if (lower.includes('warning')) result.severities.push('WARNING');
  if (lower.includes('error')) result.severities.push('ERROR');

  // Detect camera IDs e.g. "camera 3", "cam 5"
  const camMatch = lower.match(/(?:camera|cam)\s*(\d+)/g) || [];
  camMatch.forEach(m => {
    const num = m.match(/\d+/);
    if (num) result.cameraIds.push(parseInt(num[0]));
  });

  // Detect relative time
  for (const [kw, days] of Object.entries(RELATIVE_TIME)) {
    if (lower.includes(kw)) {
      const start = new Date();
      start.setDate(start.getDate() - days);
      start.setHours(0, 0, 0, 0);
      result.timeFilter = { start: start.toISOString(), end: new Date().toISOString() };
      break;
    }
  }

  // Detect time of day
  for (const [kw, [from, to]] of Object.entries(TIME_MAP)) {
    if (lower.includes(kw)) {
      result.timeOfDay = { from_hour: from, to_hour: to };
      break;
    }
  }

  // Detect if user is looking for recordings
  if (lower.includes('recording') || lower.includes('video') || lower.includes('footage')) {
    result.searchRecordings = true;
    result.searchEvents = false;
  }
  if (lower.includes('event') || lower.includes('alert') || lower.includes('alarm')) {
    result.searchEvents = true;
  }

  // Free text tokens (for full-text search in descriptions)
  const stopWords = new Set(['a','the','in','on','at','of','for','with','and','or','is','was','were','has','have']);
  result.freeText = tokens.filter(t => t.length > 2 && !stopWords.has(t));

  return result;
}

// POST /api/search  — main smart search endpoint
router.post('/', authenticate, async (req, res) => {
  try {
    const { q = '', limit = 50, offset = 0 } = req.body;
    if (!q.trim()) return res.status(400).json({ error: 'Query text required' });

    const parsed = parseQuery(q);
    const results = { query: q, parsed, events: [], recordings: [], total: 0 };

    // ── Event search ──────────────────────────────────────────────────────────
    if (parsed.searchEvents) {
      const conds = [];
      const params = [];

      // Event type match
      if (parsed.eventTypes.length) {
        const placeholders = parsed.eventTypes.map((_, i) => `$${params.length + i + 1}`).join(',');
        params.push(...parsed.eventTypes);
        conds.push(`e.event_type IN (${placeholders})`);
      }

      // Camera type filter (join cameras)
      if (parsed.cameraTypes.length) {
        const placeholders = parsed.cameraTypes.map((_, i) => `$${params.length + i + 1}`).join(',');
        params.push(...parsed.cameraTypes);
        conds.push(`c.camera_type IN (${placeholders})`);
      }

      // Camera ID filter
      if (parsed.cameraIds.length) {
        const placeholders = parsed.cameraIds.map((_, i) => `$${params.length + i + 1}`).join(',');
        params.push(...parsed.cameraIds);
        conds.push(`e.camera_id IN (${placeholders})`);
      }

      // Severity filter
      if (parsed.severities.length) {
        const placeholders = parsed.severities.map((_, i) => `$${params.length + i + 1}`).join(',');
        params.push(...parsed.severities);
        conds.push(`e.severity IN (${placeholders})`);
      }

      // Time range
      if (parsed.timeFilter) {
        params.push(parsed.timeFilter.start); conds.push(`e.occurred_at >= $${params.length}`);
        params.push(parsed.timeFilter.end);   conds.push(`e.occurred_at <= $${params.length}`);
      }

      // Time of day
      if (parsed.timeOfDay) {
        const { from_hour, to_hour } = parsed.timeOfDay;
        if (from_hour < to_hour) {
          params.push(from_hour, to_hour);
          conds.push(`EXTRACT(HOUR FROM e.occurred_at) BETWEEN $${params.length-1} AND $${params.length}`);
        } else {
          params.push(from_hour, to_hour);
          conds.push(`(EXTRACT(HOUR FROM e.occurred_at) >= $${params.length-1} OR EXTRACT(HOUR FROM e.occurred_at) <= $${params.length})`);
        }
      }

      // Free-text search across title + description + event_data
      // Color + clothing detection in description
      const textParts = [];
      if (parsed.colors.length || parsed.clothing.length) {
        const colorSearch = [...parsed.colors, ...parsed.clothing].map((t, i) => {
          params.push(`%${t}%`);
          return `(e.description ILIKE $${params.length} OR e.title ILIKE $${params.length})`;
        });
        textParts.push('(' + colorSearch.join(' OR ') + ')');
      } else if (parsed.freeText.length && !parsed.eventTypes.length) {
        // Generic text search if no event type detected
        const tsTokens = parsed.freeText.slice(0, 5).map(t => {
          params.push(`%${t}%`);
          return `(e.title ILIKE $${params.length} OR e.description ILIKE $${params.length} OR e.event_type ILIKE $${params.length})`;
        });
        textParts.push('(' + tsTokens.join(' OR ') + ')');
      }
      if (textParts.length) conds.push(...textParts);

      const where = conds.length ? 'WHERE ' + conds.join(' AND ') : '';
      const countRes = await query(`SELECT COUNT(*) FROM events e LEFT JOIN cameras c ON e.camera_id=c.camera_id ${where}`, params);
      params.push(parseInt(limit), parseInt(offset));

      const evRows = await query(
        `SELECT e.event_id, e.event_type, e.severity, e.title, e.description,
                e.camera_id, e.status, e.is_acknowledged, e.occurred_at,
                e.snapshot_path, e.video_clip_path, e.coach_location,
                c.camera_name, c.camera_type, c.location_description
         FROM events e
         LEFT JOIN cameras c ON e.camera_id=c.camera_id
         ${where}
         ORDER BY e.occurred_at DESC
         LIMIT $${params.length-1} OFFSET $${params.length}`, params);

      results.events = evRows.rows;
      results.total += parseInt(countRes.rows[0].count);
    }

    // ── Recording search ──────────────────────────────────────────────────────
    if (parsed.searchRecordings) {
      const conds = [];
      const params = [];

      if (parsed.cameraIds.length) {
        const ph = parsed.cameraIds.map((_, i) => `$${i+1}`).join(',');
        params.push(...parsed.cameraIds);
        conds.push(`r.camera_id IN (${ph})`);
      }
      if (parsed.cameraTypes.length) {
        const ph = parsed.cameraTypes.map((_, i) => `$${params.length+i+1}`).join(',');
        params.push(...parsed.cameraTypes);
        conds.push(`c.camera_type IN (${ph})`);
      }
      if (parsed.timeFilter) {
        params.push(parsed.timeFilter.start); conds.push(`r.start_timestamp >= $${params.length}`);
        params.push(parsed.timeFilter.end);   conds.push(`r.start_timestamp <= $${params.length}`);
      }

      const where = conds.length ? 'WHERE ' + conds.join(' AND ') : '';
      params.push(parseInt(limit), parseInt(offset));
      const recRows = await query(
        `SELECT r.recording_id, r.camera_id, r.file_name, r.start_timestamp,
                r.end_timestamp, r.duration_seconds, r.file_size_bytes,
                r.status, r.gps_speed_kmh, c.camera_name, c.camera_type, c.location_description
         FROM recordings r LEFT JOIN cameras c ON r.camera_id=c.camera_id
         ${where} ORDER BY r.start_timestamp DESC
         LIMIT $${params.length-1} OFFSET $${params.length}`, params);
      results.recordings = recRows.rows;
    }

    // ── Suggestions for empty result ──────────────────────────────────────────
    if (results.events.length === 0 && results.recordings.length === 0) {
      results.suggestions = [
        'Try: "crowd near door"',
        'Try: "smoke passenger area"',
        'Try: "intrusion yesterday"',
        'Try: "person fallen exterior camera"',
        'Try: "camera 1 morning"',
        'Try: "mobile phone driver cab"',
      ];
    }

    res.json(results);
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// GET /api/search/suggestions — auto-complete suggestions
router.get('/suggestions', authenticate, async (req, res) => {
  try {
    const { q = '' } = req.query;
    const lower = q.toLowerCase();
    const suggestions = [];

    // Event type suggestions
    for (const kw of Object.keys(EVENT_TYPE_MAP)) {
      if (kw.startsWith(lower) || lower.includes(kw.substring(0, 3))) {
        suggestions.push({ type: 'event', label: kw, query: kw });
      }
    }

    // Camera suggestions from DB
    if (q.length >= 1) {
      const cams = await query(
        `SELECT camera_id, camera_name, location_description FROM cameras WHERE camera_name ILIKE $1 OR location_description ILIKE $1 LIMIT 5`,
        [`%${q}%`]
      );
      cams.rows.forEach(c => suggestions.push({
        type: 'camera', label: c.camera_name, query: `camera ${c.camera_id}`,
        description: c.location_description,
      }));
    }

    // Preset intelligent searches
    const presets = [
      { label: 'Crowd near doors', query: 'crowd near door' },
      { label: 'Smoke / Fire alert', query: 'smoke fire' },
      { label: 'Intrusion alerts', query: 'intrusion' },
      { label: 'Person fallen', query: 'person fallen' },
      { label: 'Stone pelting', query: 'stone pelting' },
      { label: 'Mobile phone use', query: 'mobile phone driver' },
      { label: 'OHE / Pantograph defect', query: 'pantograph arcing ohe' },
      { label: 'Crew absent from seat', query: 'absent crew' },
      { label: 'Emergency brake', query: 'emergency brake' },
      { label: 'Animal on track', query: 'animal obstacle front camera' },
      { label: 'Today\'s critical events', query: 'critical today' },
      { label: 'Night recordings', query: 'recordings night' },
    ].filter(p => !q || p.label.toLowerCase().includes(lower) || p.query.includes(lower));

    suggestions.push(...presets.slice(0, 6));
    res.json({ suggestions: suggestions.slice(0, 12) });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

module.exports = router;
