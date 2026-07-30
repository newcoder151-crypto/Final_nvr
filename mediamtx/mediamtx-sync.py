#!/usr/bin/env python3
"""
mediamtx-sync.py — Syncs camera RTSP URLs from PostgreSQL → MediaMTX API.

How it works:
  1. Reads ACTIVE cameras from the DB (rec_rtsp_url — the profile proven
     reliable across this fleet — plus credentials from
     cameras_config_details, same join stream_manager.py uses)
  2. For each camera, creates a MediaMTX path cam_<camera_id>_raw that
     pulls directly from the camera over TCP, using MediaMTX's own
     mature RTSP client and reconnect logic. This is the ONLY thing that
     ever opens an RTSP connection to a physical camera — this fleet's
     firmware appears to cap concurrent RTSP sessions per camera, and
     letting multiple independent home-grown ffmpeg processes each open
     their own connection caused unpredictable "one purpose connects,
     the other gets refused" failures. One real connection, managed by
     the tool actually built for that job, removes that entirely.
  3. stream_manager.py then reads ONLY these local cam_<id>_raw paths
     (never the camera directly) to burn in the watermark and produce
     the final cam_<id> path (live view) and the recorded segments.
  4. Removes paths for cameras that are deleted/inactive
  5. Re-runs every SYNC_INTERVAL_SECS to pick up new cameras automatically

Path naming:
  cam_<id>_raw  — local-only, MediaMTX pulling straight from the camera
  cam_<id>      — final, watermarked, what the frontend/WebRTC/HLS use
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
        "sourceProtocol": "tcp",
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


def build_rtsp_url_with_creds(rtsp_url: str, user: str | None, passwd: str | None) -> str:
    """Same percent-encoding approach as stream_manager.py's build_authed_url
    — never raw string concatenation, that's what caused the credential-
    doubling bug for passwords containing '@' fixed earlier in this project."""
    if not user or not passwd:
        return rtsp_url
    from urllib.parse import quote
    p = urlparse(rtsp_url)
    if p.username:
        return rtsp_url
    netloc = f"{quote(user, safe='')}:{quote(passwd, safe='')}@{p.hostname}"
    if p.port:
        netloc += f":{p.port}"
    return urlunparse((p.scheme, netloc, p.path, p.params, p.query, p.fragment))


def fetch_active_cameras_full(conn):
    """Like fetch_active_cameras() but also pulls rec_rtsp_url and the
    cameras_config_details-joined credentials — same query shape as
    stream_manager.py's fetch_active_cameras(), kept in sync deliberately."""
    with conn.cursor(cursor_factory=psycopg2.extras.DictCursor) as cur:
        cur.execute("""
            SELECT c.camera_id, c.camera_name, c.rtsp_url, c.rec_rtsp_url,
                   COALESCE(cd.rtsp_username, c.username) AS rtsp_username,
                   COALESCE(cd.rtsp_password, c.password_hash) AS rtsp_password
            FROM cameras c
            LEFT JOIN cameras_config_details cd
                   ON host(cd.ip_address) = c.ip_address
            WHERE c.status = 'ACTIVE'
              AND c.rtsp_url IS NOT NULL AND c.rtsp_url != ''
            ORDER BY c.camera_id
        """)
        return cur.fetchall()


# ── Main sync loop ─────────────────────────────────────────────────────────────
def sync_once(conn):
    cameras = fetch_active_cameras_full(conn)

    # MediaMTX pulls directly from each camera's reliable profile
    # (rec_rtsp_url — proven reliable across this fleet in extensive
    # testing) into a "_raw" path, using MediaMTX's OWN mature RTSP client
    # and reconnect logic rather than a hand-rolled one. stream_manager.py
    # then only ever touches these local, already-stable paths — it never
    # opens its own connection to a physical camera at all anymore. This
    # is also why only ONE thing (MediaMTX) ever opens an RTSP session to
    # a given camera, avoiding the concurrent-session contention some of
    # this fleet's firmware appears to enforce (previously, having
    # multiple independent ffmpeg processes each connect to the same
    # camera caused "one purpose connects fine, the other gets refused"
    # unpredictably).
    wanted: dict[str, str] = {}
    for cam in cameras:
        path_name = f"{PATH_PREFIX}{cam['camera_id']}_raw"
        url = build_rtsp_url_with_creds(
            cam["rec_rtsp_url"] or cam["rtsp_url"],
            cam["rtsp_username"], cam["rtsp_password"])
        wanted[path_name] = url

    existing = mtx_list_paths()
    existing_raw = {p for p in existing if p.endswith("_raw")}
    for path_name, rtsp_url in wanted.items():
        mtx_add_path(path_name, rtsp_url)
    for path_name in existing_raw - set(wanted.keys()):
        mtx_remove_path(path_name)

    if cameras:
        log.info(f"Sync: {len(cameras)} active camera(s) — "
                 f"{len(wanted)} raw source path(s) managed in MediaMTX")
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
