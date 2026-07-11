#!/usr/bin/env python3
"""
Railway mNVR — Recorder Service
- One thread per camera, strictly enforced
- RTSP with embedded credentials
- UDP from mNVR core (udp://127.0.0.1:<5000 + camera_id*2>)
- NO test pattern — just logs when stream unavailable
- 30-day FIFO retention
"""
import os, sys, time, logging, subprocess, threading, requests
from datetime import datetime, timezone, timedelta
from pathlib import Path
from urllib.parse import urlparse, urlunparse

# ── Config ─────────────────────────────────────────────────────────────────────
API_URL         = os.environ.get("API_URL",             "http://localhost:3001")
API_USER        = os.environ.get("API_USER",            "admin")
API_PASS        = os.environ.get("API_PASS",            "Admin@123")
STORAGE_PATH    = Path(os.environ.get("STORAGE_PATH",   "./storage"))
RECORDINGS_PATH = Path(os.environ.get("RECORDINGS_PATH", str(STORAGE_PATH / "recordings")))
HLS_PATH        = Path(os.environ.get("HLS_PATH",        str(STORAGE_PATH / "hls")))
SEGMENT_SECS    = int(os.environ.get("SEGMENT_SECONDS",  "60"))
HLS_SEG_SECS    = int(os.environ.get("HLS_SEGMENT_SECS", "4"))
POLL_SECS       = int(os.environ.get("POLL_CAMERAS_SECS","30"))
RETENTION_DAYS  = int(os.environ.get("RETENTION_DAYS",   "30"))

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [RECORDER] %(levelname)s %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
    stream=sys.stdout,
)
log = logging.getLogger("recorder")

# ── Auth ───────────────────────────────────────────────────────────────────────
_token = None
_token_exp = 0
_token_lock = threading.Lock()

def get_token():
    global _token, _token_exp
    with _token_lock:
        if _token and time.time() < _token_exp:
            return _token
        for attempt in range(120):
            try:
                r = requests.post(
                    f"{API_URL}/api/auth/login",
                    json={"username": API_USER, "password": API_PASS},
                    timeout=10,
                )
                if r.status_code == 200:
                    _token     = r.json()["token"]
                    _token_exp = time.time() + 3600 * 20
                    log.info(f"✓ Authenticated ({API_URL})")
                    return _token
                log.warning(f"Auth {attempt+1}: HTTP {r.status_code}")
            except Exception as e:
                log.warning(f"Auth {attempt+1}/120: {e}")
            time.sleep(5)
        return None

def _headers():
    t = get_token()
    return {"Authorization": f"Bearer {t}"} if t else {}

def api_get(path):
    try:
        r = requests.get(f"{API_URL}{path}", headers=_headers(), timeout=10)
        return r.json() if r.ok else None
    except Exception as e:
        log.debug(f"api_get {path}: {e}"); return None

def api_post(path, data):
    try:
        r = requests.post(f"{API_URL}{path}", json=data, headers=_headers(), timeout=15)
        return r.json() if r.ok else None
    except Exception as e:
        log.debug(f"api_post {path}: {e}"); return None

def api_put(path, data):
    try:
        r = requests.put(f"{API_URL}{path}", json=data, headers=_headers(), timeout=15)
        return r.json() if r.ok else None
    except Exception as e:
        log.debug(f"api_put {path}: {e}"); return None

# ── ffmpeg ─────────────────────────────────────────────────────────────────────
def ffmpeg_ok():
    try:
        subprocess.run(["ffmpeg", "-version"], capture_output=True, check=True)
        return True
    except:
        return False

def inject_creds(rtsp_url: str, user: str | None, passwd: str | None) -> str:
    if not user or not passwd: return rtsp_url
    p = urlparse(rtsp_url)
    if p.username: return rtsp_url
    netloc = f"{user}:{passwd}@{p.hostname}"
    if p.port: netloc += f":{p.port}"
    return urlunparse(p._replace(netloc=netloc))

def probe_udp(port: int) -> bool:
    """Check if mNVR core is streaming on UDP port (quick 3s probe)."""
    cmd = ["ffprobe", "-v", "quiet",
           "-protocol_whitelist", "udp,rtp",
           "-i", f"udp://127.0.0.1:{port}?timeout=3000000",
           "-show_entries", "stream=codec_type", "-of", "csv=p=0"]
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=4)
        return r.returncode == 0
    except:
        return False

def probe_rtsp(url: str) -> bool:
    """Check if RTSP stream is reachable (quick 4s probe)."""
    cmd = ["ffprobe", "-v", "quiet", "-rtsp_transport", "tcp",
           "-i", url,
           "-show_entries", "stream=codec_type", "-of", "csv=p=0",
           "-timeout", "4000000"]
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=5)
        return r.returncode == 0
    except:
        return False

def record_from_url(camera_id: int, source_url: str, protocol: str,
                    out_dir: Path, secs: int) -> Path | None:
    """Generic ffmpeg record from any URL (RTSP or UDP) into MP4."""
    ts    = int(datetime.now(timezone.utc).timestamp())
    fname = f"cam{camera_id}_{ts}.mp4"
    out   = out_dir / fname
    out_dir.mkdir(parents=True, exist_ok=True)

    if protocol == "udp":
        cmd = ["ffmpeg", "-y",
               "-protocol_whitelist", "udp,rtp",
               "-i", source_url,
               "-c", "copy", "-t", str(secs),
               "-movflags", "+faststart", str(out)]
    else:
        cmd = ["ffmpeg", "-y",
               "-rtsp_transport", "tcp", "-timeout", "5000000",
               "-i", source_url,
               "-c", "copy", "-t", str(secs),
               "-movflags", "+faststart", str(out)]

    log.info(f"[CAM {camera_id}] Recording {protocol.upper()} → {fname}")
    try:
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=secs + 45)
        if res.returncode == 0 and out.exists() and out.stat().st_size > 50_000:
            log.info(f"[CAM {camera_id}] ✓ {fname}  ({out.stat().st_size // 1024} KB)")
            return out
        log.warning(f"[CAM {camera_id}] ffmpeg rc={res.returncode}: {res.stderr[-150:]}")
    except subprocess.TimeoutExpired:
        log.warning(f"[CAM {camera_id}] ffmpeg timeout after {secs+45}s")
    except Exception as e:
        log.warning(f"[CAM {camera_id}] ffmpeg error: {e}")

    if out.exists(): out.unlink(missing_ok=True)
    return None

def generate_hls(mp4: Path, hls_dir: Path):
    hls_dir.mkdir(parents=True, exist_ok=True)
    cmd = ["ffmpeg", "-y", "-i", str(mp4),
           "-c", "copy", "-f", "hls",
           "-hls_time", str(HLS_SEG_SECS),
           "-hls_list_size", "10",
           "-hls_flags", "delete_segments+append_list",
           "-hls_segment_filename", str(hls_dir / "seg%05d.ts"),
           str(hls_dir / "stream.m3u8")]
    try:
        subprocess.run(cmd, capture_output=True, timeout=60)
    except Exception as e:
        log.warning(f"[HLS] {e}")

def register_recording(camera_id: int, mp4: Path, started: datetime, ended: datetime):
    size = mp4.stat().st_size if mp4.exists() else 0
    dur  = int((ended - started).total_seconds())
    res  = api_post("/api/recordings", {
        "camera_id":        camera_id,
        "file_path":        str(mp4),
        "file_name":        mp4.name,
        "file_size_bytes":  size,
        "duration_seconds": dur,
        "start_timestamp":  started.isoformat(),
        "end_timestamp":    ended.isoformat(),
        "video_codec":      "H.264",
        "resolution_width": 1280, "resolution_height": 720,
        "fps_actual":       25.0, "has_audio": 1,
        "recording_mode":   "CONTINUOUS", "status": "COMPLETED",
    })
    if res and res.get("recording_id"):
        log.info(f"[CAM {camera_id}] DB registered → recording_id={res['recording_id']}")
    else:
        log.warning(f"[CAM {camera_id}] DB register failed: {res}")

def enforce_retention(camera_id: int, rec_dir: Path):
    """Delete MP4s older than RETENTION_DAYS (30-day FIFO)."""
    cutoff = datetime.now(timezone.utc) - timedelta(days=RETENTION_DAYS)
    removed = 0
    for f in sorted(rec_dir.glob("*.mp4")):
        try:
            mtime = datetime.fromtimestamp(f.stat().st_mtime, tz=timezone.utc)
            if mtime < cutoff:
                f.unlink()
                removed += 1
                log.info(f"[CAM {camera_id}] FIFO removed {f.name}")
        except:
            pass
    if removed:
        log.info(f"[CAM {camera_id}] FIFO: {removed} segment(s) deleted (>{RETENTION_DAYS}d old)")

# ── Per-camera loop (one thread, strictly one) ─────────────────────────────────
_thread_lock = threading.Lock()
_active: dict[int, threading.Thread] = {}

def camera_loop(cam: dict):
    camera_id   = cam["camera_id"]
    camera_name = cam.get("camera_name") or f"CAM-{camera_id}"
    rtsp_url    = cam.get("rtsp_url") or ""
    ip_address  = cam.get("ip_address") or ""
    udp_port    = 5000 + camera_id * 2          # streamer_module.c formula

    rec_dir = RECORDINGS_PATH / f"cam_{camera_id}"
    hls_dir = HLS_PATH        / f"cam_{camera_id}"
    rec_dir.mkdir(parents=True, exist_ok=True)
    hls_dir.mkdir(parents=True, exist_ok=True)

    # Fetch password from API (stored in password_hash field for RTSP)
    detail   = api_get(f"/api/cameras/{camera_id}")
    cam_user = detail.get("username")    if detail else cam.get("username")
    cam_pass = detail.get("password_hash") if detail else None
    authed_rtsp = inject_creds(rtsp_url, cam_user, cam_pass) if rtsp_url.startswith("rtsp://") else ""

    # Register storage paths so streaming API can find files
    api_put(f"/api/cameras/{camera_id}", {
        "rec_output_dir":   str(rec_dir),
        "hls_output_dir":   str(hls_dir),
        "hls_playlist_url": f"/api/streaming/hls/{camera_id}/stream.m3u8",
    })

    log.info(f"[CAM {camera_id}] Thread started: {camera_name} | IP:{ip_address} | UDP:{udp_port}")

    # Determine source priority: UDP (mNVR core) → RTSP
    source_url:  str | None = None
    source_proto: str       = "none"

    def detect_source():
        nonlocal source_url, source_proto
        # 1. UDP from mNVR core streamer
        if probe_udp(udp_port):
            source_url   = f"udp://127.0.0.1:{udp_port}"
            source_proto = "udp"
            log.info(f"[CAM {camera_id}] ✓ mNVR UDP stream on port {udp_port}")
            return
        # 2. RTSP
        if authed_rtsp and probe_rtsp(authed_rtsp):
            source_url   = authed_rtsp
            source_proto = "rtsp"
            log.info(f"[CAM {camera_id}] ✓ RTSP stream reachable")
            return
        # 3. Nothing
        source_url   = None
        source_proto = "none"
        log.warning(
            f"[CAM {camera_id}] No stream available — "
            f"UDP port {udp_port} unreachable, RTSP unreachable. "
            f"Waiting for mNVR core or camera to come online."
        )

    detect_source()

    seg_count  = 0
    no_src_log = 0   # throttle "no source" log messages

    while True:
        started = datetime.now(timezone.utc)

        # ── FIFO check every 100 segments ──────────────────────────────────
        seg_count += 1
        if seg_count % 100 == 0:
            enforce_retention(camera_id, rec_dir)

        # ── Re-probe source every 5 segments if currently none ─────────────
        if source_proto == "none" and seg_count % 5 == 0:
            detect_source()

        if source_proto == "none":
            # Log once every 5 checks, not every segment
            no_src_log += 1
            if no_src_log % 5 == 1:
                log.warning(
                    f"[CAM {camera_id}] Still no stream — "
                    f"skipping segment (check camera IP:{ip_address} / mNVR core)"
                )
            time.sleep(SEGMENT_SECS)  # wait a full segment period before retrying
            continue

        # ── Record ──────────────────────────────────────────────────────────
        try:
            mp4 = record_from_url(camera_id, source_url, source_proto, rec_dir, SEGMENT_SECS)

            if mp4 and mp4.exists():
                ended = datetime.now(timezone.utc)
                generate_hls(mp4, hls_dir)
                register_recording(camera_id, mp4, started, ended)
                api_post(f"/api/cameras/{camera_id}/health", {
                    "is_online": 1, "is_recording": 1,
                    "frame_rate_actual": 25.0, "bitrate_kbps": 2048, "error_count": 0,
                })
                no_src_log = 0
            else:
                # Recording failed — source may have dropped
                log.warning(f"[CAM {camera_id}] Segment failed — re-probing source next cycle")
                api_post(f"/api/cameras/{camera_id}/health", {
                    "is_online": 0, "is_recording": 0, "error_count": 1,
                    "last_error": f"{source_proto.upper()} stream recording failed",
                })
                source_proto = "none"   # force re-probe
                source_url   = None

        except Exception as e:
            log.error(f"[CAM {camera_id}] Unexpected: {e}")
            source_proto = "none"
            time.sleep(10)

# ── Main ────────────────────────────────────────────────────────────────────────
def main():
    log.info("=" * 60)
    log.info("  Railway mNVR — Recorder Service")
    log.info(f"  API     : {API_URL}")
    log.info(f"  Storage : {RECORDINGS_PATH}")
    log.info(f"  Segment : {SEGMENT_SECS}s | Retention: {RETENTION_DAYS}d FIFO")
    log.info(f"  UDP base port: 5000 (5000 + camera_id*2)")
    log.info("=" * 60)

    RECORDINGS_PATH.mkdir(parents=True, exist_ok=True)
    HLS_PATH.mkdir(parents=True, exist_ok=True)

    if not ffmpeg_ok():
        log.error("ffmpeg not found. Install: sudo apt install ffmpeg"); sys.exit(1)

    log.info(f"Waiting for API at {API_URL} ...")
    for i in range(120):
        try:
            r = requests.get(f"{API_URL}/api/health", timeout=5)
            if r.ok:
                log.info("✓ API ready"); break
        except:
            pass
        if i % 6 == 0 and i > 0:
            log.info(f"  ...still waiting ({i*5}s)")
        time.sleep(5)

    if not get_token():
        log.error("Cannot authenticate — exiting"); sys.exit(1)

    log.info("Camera polling started (every 30s)")
    while True:
        data    = api_get("/api/cameras?status=ACTIVE&limit=100")
        cameras = (data or {}).get("cameras", [])

        if not cameras:
            log.warning("No active cameras found — retrying in 30s")
        else:
            for cam in cameras:
                cid = cam["camera_id"]
                with _thread_lock:
                    existing = _active.get(cid)
                    if existing and existing.is_alive():
                        continue  # thread is healthy, skip
                    # Start exactly one thread
                    t = threading.Thread(
                        target=camera_loop,
                        args=(cam,),
                        daemon=True,
                        name=f"cam-{cid}",
                    )
                    t.start()
                    _active[cid] = t
                    log.info(f"Started recorder thread for cam {cid}: {cam.get('camera_name')}")

        time.sleep(POLL_SECS)

if __name__ == "__main__":
    main()
