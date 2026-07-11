# Railway NVR API Server

Node.js Express backend for the Railway Mobile NVR system. See the main [README.md](../README.md) for full API documentation.

## Quick Start

```bash
cd server
npm install
cp .env.example .env   # Edit with your Supabase credentials
npm run dev             # Starts on port 8080
```

## Environment Variables

| Variable | Required | Description |
|----------|----------|-------------|
| `PORT` | No | Server port (default: 8080) |
| `JWT_SECRET` | Yes | Secret key for JWT signing |
| `SUPABASE_URL` | Yes | Your Supabase project URL |
| `SUPABASE_ANON_KEY` | Yes | Supabase anon/public key |
| `SUPABASE_SERVICE_ROLE_KEY` | Yes | Supabase service role key |
| `CORS_ORIGIN` | No | Allowed CORS origin (default: `*`) |
| `NODE_ENV` | No | `development` or `production` |

## API Endpoints Summary

| Method | Endpoint | Auth | Description |
|--------|----------|------|-------------|
| GET | `/api/health` | No | Health check |
| GET | `/api/docs` | No | Swagger UI |
| POST | `/api/auth/register` | No | Register user |
| POST | `/api/auth/login` | No | Login |
| POST | `/api/auth/refresh` | JWT | Refresh token |
| GET | `/api/auth/me` | JWT | Current user |
| GET | `/api/cameras` | JWT | List cameras |
| POST | `/api/cameras` | Admin | Create camera |
| GET | `/api/cameras/:id` | JWT | Get camera |
| PUT | `/api/cameras/:id` | Admin | Update camera |
| DELETE | `/api/cameras/:id` | Admin | Delete camera |
| GET | `/api/events` | JWT | Search events |
| POST | `/api/events` | Operator+ | Create event |
| GET | `/api/events/:id` | JWT | Get event |
| PUT | `/api/events/:id/acknowledge` | JWT | Acknowledge event |
| PUT | `/api/events/batch/acknowledge` | JWT | Bulk acknowledge |
| GET | `/api/recordings` | JWT | Search recordings |
| GET | `/api/recordings/:id` | JWT | Get recording |
| GET | `/api/streaming/recordings/:id/stream` | JWT | Stream video |
| GET | `/api/streaming/recordings/:id/thumbnail` | JWT | Get thumbnail |
| GET | `/api/streaming/recordings/:id/download` | JWT | Download file |
| GET | `/api/users` | Admin | List users |
| POST | `/api/users` | Admin | Create user |
| GET | `/api/users/:id` | Admin | Get user |
| PUT | `/api/users/:id` | Admin | Update user |
| DELETE | `/api/users/:id` | Admin | Delete user |
| GET | `/api/config` | JWT | List config |
| GET | `/api/config/status` | JWT | System status |
| GET | `/api/config/:key` | JWT | Get config |
| PUT | `/api/config/:key` | Admin | Update config |
| WS | `/ws` | JWT | WebSocket events |

**Total: 31 endpoints + 1 WebSocket**

## WebSocket Events

Connect: `ws://localhost:8080/ws?token=YOUR_JWT`

| Event | Description |
|-------|-------------|
| `camera.created` | New camera added |
| `camera.status` | Camera status changed |
| `camera.deleted` | Camera removed |
| `event.new` | New alert/incident |
| `event.acknowledged` | Event acknowledged |
| `system.alert` | System health warning |
