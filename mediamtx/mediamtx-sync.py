#!/usr/bin/env python3
"""
mediamtx-sync.py — Syncs camera RTSP URLs from PostgreSQL → MediaMTX API.

How it works:
  1. Reads ACTIVE cameras from the DB (rtsp_url, username, password_hash, camera_id)
  2. For each camera, creates a MediaMTX path  cam_<camera_id>
     that pulls the RTSP stream via UDP
  3. Removes paths for cameras that are deleted/inactive
  4. Re-runs every SYNC_INTERVAL_SECS to pick up new cameras automatically
  5. Handles reconnects: if a path's source drops, MediaMTX retries every 2s

Path naming: cam_<camera_id>
  WebRTC URL:  http://localhost:8889/cam_<id>/whep   (browser)
  RTSP URL:    rtsp://localhost:8554/cam_<id>          (VLC / ffplay)
  HLS URL:     http://localhost:8888/cam_<id>/index.m3u8
"""
import os, sys, time, logging, requests, psycopg2, psycopg2.extras
from urllib.parse import urlparse, urlunparse

# ── Config ─────────────────────────────────────────────────────────────────────
MEDIAMTX_API    = os.environ.get("MEDIAMTX_API",    "http://localhost:9997")
DB_HOST         = os.environ.get("DB_HOST",         "localhost")
DB_PORT         = int(os.environ.get("DB_PORT",     "5432"))
DB_NAME         = os.environ.get("DB_NAME",         "mnvr")
DB_USER         = os.environ.get("DB_USER",         "mnvr")
DB_PASSWORD     = os.environ.get("DB_PASSWORD",     "mnvr")
SYNC_INTERVAL   = int(os.environ.get("SYNC_INTERVAL_SECS", "15"))
PATH_PREFIX     = os.environ.get("PATH_PREFIX",     "cam_")

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [MTX-SYNC] %(levelname)s %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
    stream=sys.stdout,
)
log = logging.getLogger("sync")

# ── DB ──────────────────────────────────────────────────────────────────────────
def get_db():
    for attempt in range(30):
        try:
            conn = psycopg2.connect(
                host=DB_HOST, port=DB_PORT, dbname=DB_NAME,
                user=DB_USER, password=DB_PASSWORD,
                connect_timeout=5,
            )
            return conn
        except Exception as e:
            log.warning(f"DB connect attempt {attempt+1}/30: {e}")
            time.sleep(5)
    raise RuntimeError("Cannot connect to PostgreSQL")

def fetch_active_cameras(conn):
    with conn.cursor(cursor_factory=psycopg2.extras.DictCursor) as cur:
        cur.execute("""
            SELECT camera_id, camera_name, rtsp_url, username, password_hash, ip_address
            FROM cameras
            WHERE status = 'ACTIVE'
              AND rtsp_url IS NOT NULL
              AND rtsp_url != ''
            ORDER BY camera_id
        """)
        return cur.fetchall()

# ── RTSP URL with embedded credentials ────────────────────────────────────────
def build_rtsp_url(rtsp_url: str, user: str | None, passwd: str | None) -> str:
    if not user or not passwd:
        return rtsp_url
    p = urlparse(rtsp_url)
    if p.username:          # already has creds
        return rtsp_url
    netloc = f"{user}:{passwd}@{p.hostname}"
    if p.port:
        netloc += f":{p.port}"
    return urlunparse(p._replace(netloc=netloc))

# ── MediaMTX API ───────────────────────────────────────────────────────────────
def mtx_get(path: str) -> dict | None:
    try:
        r = requests.get(f"{MEDIAMTX_API}{path}", timeout=5)
        return r.json() if r.ok else None
    except Exception as e:
        log.debug(f"mtx_get {path}: {e}")
        return None

def mtx_add_path(path_name: str, rtsp_url: str) -> bool:
    """Create or update a MediaMTX path that pulls from an RTSP source."""
    payload = {
        "sourceProtocol": "udp",
        "source": rtsp_url,
        "sourceOnDemand": False,
        "maxReaders": 64,
        "fallback": "",
        "record": False,
    }
    try:
        # Try add first; if exists, update
        r = requests.post(f"{MEDIAMTX_API}/v3/config/paths/add/{path_name}",
                          json=payload, timeout=5)
        if r.status_code in (200, 201):
            log.info(f"[MTX] Added path /{path_name} → {rtsp_url.split('@')[-1]}")
            return True
        # This MediaMTX build reports a duplicate path as 400 with
        # {"error":"path already exists"} rather than 409 — check the body,
        # not just the status code, before falling back to patch.
        already_exists = (
            r.status_code == 409
            or (r.status_code == 400 and "already exists" in r.text.lower())
        )
        if already_exists:
            r2 = requests.patch(f"{MEDIAMTX_API}/v3/config/paths/patch/{path_name}",
                                json=payload, timeout=5)
            if r2.ok:
                log.debug(f"[MTX] Patched path /{path_name}")
                return True
        log.warning(f"[MTX] add/patch failed {r.status_code}: {r.text[:120]}")
        return False
    except Exception as e:
        log.warning(f"[MTX] add_path error: {e}")
        return False

def mtx_remove_path(path_name: str) -> bool:
    try:
        r = requests.delete(f"{MEDIAMTX_API}/v3/config/paths/delete/{path_name}", timeout=5)
        if r.ok:
            log.info(f"[MTX] Removed path /{path_name}")
        return r.ok
    except Exception as e:
        log.debug(f"[MTX] remove_path {path_name}: {e}")
        return False

def mtx_list_paths() -> set[str]:
    data = mtx_get("/v3/config/paths/list")
    if not data:
        return set()
    return {item.get("name","") for item in data.get("items", []) if item.get("name","").startswith(PATH_PREFIX)}

def wait_for_mediamtx():
    log.info(f"Waiting for MediaMTX API at {MEDIAMTX_API} ...")
    for i in range(120):
        try:
            r = requests.get(f"{MEDIAMTX_API}/v3/paths/list", timeout=3)
            if r.ok:
                log.info("✓ MediaMTX API ready")
                return
        except Exception:
            pass
        if i % 6 == 0 and i > 0:
            log.info(f"  ...still waiting ({i*2}s)")
        time.sleep(2)
    log.error("MediaMTX API not responding after 240s — continuing anyway")

def wait_for_relay(host: str, port: str):
    """Block until mnvrd's RTSP relay is actually accepting TCP connections.

    start.sh launches MediaMTX/this script (step 7) before mnvrd (step 9),
    so without this wait, MediaMTX would immediately try to pull from the
    relay before it exists — logging a harmless-but-confusing burst of
    "connection refused" on every single restart. Waiting here means paths
    only ever get added once there's actually something to pull from,
    regardless of script step ordering.
    """
    import socket
    log.info(f"Waiting for mnvrd's RTSP relay at {host}:{port} ...")
    for i in range(120):
        try:
            with socket.create_connection((host, int(port)), timeout=2):
                log.info("✓ RTSP relay ready")
                return
        except Exception:
            pass
        if i % 6 == 0 and i > 0:
            log.info(f"  ...still waiting for relay ({i*2}s) — is mnvrd still starting up?")
        time.sleep(2)
    log.warning(f"Relay at {host}:{port} not reachable after 240s — "
                "continuing anyway (mnvrd may have failed to start; check logs/nvr.log)")

# RTSP relay hosted by mnvrd itself (see nvr_core/src/modules/rtsp_relay).
# Pointing MediaMTX here instead of at the camera directly means mnvrd is
# the ONLY thing that ever opens an RTSP session to the physical camera —
# many cameras only accept 1-2 concurrent RTSP clients, and having both
# MediaMTX and mnvrd connect independently caused them to race for that
# slot (one camera's live view would work while its recording failed, and
# vice versa, unpredictably). This also means the live view now shows the
# same watermark (date/time + GPS/speed) that recordings do, since it's
# the same watermarked stream either way.
RTSP_RELAY_HOST = os.environ.get("RTSP_RELAY_HOST", "127.0.0.1")
RTSP_RELAY_PORT = os.environ.get("RTSP_RELAY_PORT", "8555")

# ── Main sync loop ─────────────────────────────────────────────────────────────
def sync_once(conn):
    cameras = fetch_active_cameras(conn)

    # DISABLED: no longer adding "source"-based paths here. ffmpeg
    # (stream_manager.py) now pushes each camera's LIVE profile directly
    # into MediaMTX as an RTSP publisher — MediaMTX auto-creates a path on
    # first publish when no explicit source is configured for it, and
    # rejects a publisher for any path that DOES have one, which is why
    # this had to be turned off rather than left pointing at the old
    # relay (that relay is gone; ffmpeg replaced it — see
    # stream_manager.py's module docstring for the full picture).
    #
    # wanted: dict[str, str] = {}
    # for cam in cameras:
    #     path_name = f"{PATH_PREFIX}{cam['camera_id']}"
    #     rtsp = f"rtsp://{RTSP_RELAY_HOST}:{RTSP_RELAY_PORT}/{path_name}"
    #     wanted[path_name] = rtsp
    # existing = mtx_list_paths()
    # for path_name, rtsp_url in wanted.items():
    #     mtx_add_path(path_name, rtsp_url)
    # for path_name in existing - set(wanted.keys()):
    #     mtx_remove_path(path_name)

    if cameras:
        log.info(f"Sync: {len(cameras)} active camera(s) in DB "
                 f"(paths now managed by ffmpeg publishers, not this script)")

    else:
        log.warning("No active cameras with RTSP URLs found in DB")

def main():
    log.info("=" * 60)
    log.info("  MediaMTX Sync — Railway mNVR")
    log.info(f"  MediaMTX : {MEDIAMTX_API}")
    log.info(f"  DB       : {DB_USER}@{DB_HOST}:{DB_PORT}/{DB_NAME}")
    log.info(f"  Interval : {SYNC_INTERVAL}s")
    log.info("=" * 60)

    wait_for_mediamtx()
    # wait_for_relay() removed — no relay to wait for anymore; ffmpeg
    # (stream_manager.py) pushes directly, MediaMTX just needs to be up.

    conn = get_db()
    log.info("✓ Connected to PostgreSQL")

    while True:
        try:
            conn.poll()         # keep-alive / reconnect if needed
        except Exception:
            try:
                conn.close()
            except Exception:
                pass
            conn = get_db()

        try:
            sync_once(conn)
        except Exception as e:
            log.error(f"sync_once error: {e}")

        time.sleep(SYNC_INTERVAL)

if __name__ == "__main__":
    main()
