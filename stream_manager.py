#!/usr/bin/env python3
"""
stream_manager.py — ffmpeg-based live view + recording, one thread per
camera per purpose ("dual stream" — LIVE profile pushed to MediaMTX,
REC profile recorded to disk).

WHY THIS EXISTS (context for future maintainers):
Today's extensive testing (packet captures, side-by-side comparisons)
proved that GStreamer's rtspsrc has real reliability problems against
this specific camera fleet — inconsistent SETUP failures, and in one
proven case (packet-captured) a camera that accepted PLAY with 200 OK
and then sent zero RTP data. ffmpeg's RTSP client, tested side by side
against the exact same URLs and credentials, worked cleanly every single
time. Rather than keep fighting rtspsrc, this replaces mnvrd's own
camera connections with ffmpeg subprocesses.

ARCHITECTURE:
  - LIVE profile  -> ffmpeg -c copy -f rtsp  -> pushed directly into
    MediaMTX as an RTSP publisher (MediaMTX already accepts publishers;
    this replaces the custom GstRTSPServer relay in rtsp_relay.c, which
    is no longer needed). No re-encode, lowest latency/CPU, but no
    burned-in watermark on live view (trade-off — recordings keep it).
  - REC profile   -> ffmpeg with a drawtext watermark (date/time; GPS/
    speed placeholder until hardware exists) -> segmented MP4 files
    written directly to rec_output_dir, flat, camera-id-prefixed.

Each camera gets two supervisor threads (one per purpose), each running
its own restart-with-backoff loop around an ffmpeg subprocess — matching
the same 5s/10s/20s/30s-cap backoff pattern used elsewhere in this
project, so a struggling camera doesn't get hammered with reconnects.

mnvrd itself should have native_streaming_enabled=false in mnvr.conf
when this is running, so its own streamer_module.c/recorder.c don't
also try to connect to the same cameras (mnvrd's AI/DB/API/health
functions keep running either way — only the direct camera RTSP
connections are disabled).
"""
import os
import sys
import time
import shlex
import signal
import logging
import subprocess
import threading
from pathlib import Path

import psycopg2
import psycopg2.extras

# ── Config (env-overridable, same pattern as mediamtx-sync.py) ─────────────
DB_HOST     = os.environ.get("DB_HOST", "localhost")
DB_PORT     = int(os.environ.get("DB_PORT", "5432"))
DB_NAME     = os.environ.get("DB_NAME", "mnvr")
DB_USER     = os.environ.get("DB_USER", "mnvr")
DB_PASSWORD = os.environ.get("DB_PASSWORD", "mnvr")

MEDIAMTX_RTSP_HOST = os.environ.get("MEDIAMTX_RTSP_HOST", "127.0.0.1")
MEDIAMTX_RTSP_PORT = os.environ.get("MEDIAMTX_RTSP_PORT", "8554")

RECORDINGS_BASE = os.environ.get("RECORDINGS_PATH", "/storage/recordings")
SEGMENT_SECONDS = int(os.environ.get("SEGMENT_SECONDS", "120"))

CAMERA_REFRESH_INTERVAL = 30  # seconds between checking the DB for camera changes

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [STREAM-MGR] %(levelname)s %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger("stream_manager")

_shutdown = threading.Event()


def _handle_signal(signum, frame):
    log.info("Shutdown signal received, stopping all camera threads...")
    _shutdown.set()


signal.signal(signal.SIGTERM, _handle_signal)
signal.signal(signal.SIGINT, _handle_signal)


# ── DB ───────────────────────────────────────────────────────────────────
def get_db():
    for attempt in range(30):
        try:
            return psycopg2.connect(
                host=DB_HOST, port=DB_PORT, dbname=DB_NAME,
                user=DB_USER, password=DB_PASSWORD,
            )
        except Exception as e:
            log.warning(f"DB connect failed ({e}), retrying in 2s...")
            time.sleep(2)
    raise RuntimeError("Could not connect to PostgreSQL after 60s")


def fetch_active_cameras(conn):
    """Returns cameras with credentials properly separated (never embedded
    in the URL — matches the fix applied in main.c/config_module.c for the
    same reason: embedding a password containing '@' corrupts the URL)."""
    with conn.cursor(cursor_factory=psycopg2.extras.DictCursor) as cur:
        cur.execute("""
            SELECT c.camera_id, c.camera_name, c.rtsp_url, c.rec_rtsp_url,
                   c.rec_output_dir,
                   COALESCE(cd.rtsp_username, c.username) AS rtsp_username,
                   cd.rtsp_password
            FROM cameras c
            LEFT JOIN cameras_config_details cd
                   ON host(cd.ip_address) = c.ip_address
            WHERE c.status = 'ACTIVE'
              AND c.rtsp_url IS NOT NULL AND c.rtsp_url != ''
            ORDER BY c.camera_id
        """)
        return cur.fetchall()


def build_authed_url(raw_url: str, user: str | None, passwd: str | None) -> str:
    """Insert credentials via urlparse/urlunparse (correct percent-encoding),
    never raw string concatenation — that's exactly the bug that caused the
    credential-doubling issue fixed earlier today in main.c."""
    if not user or not passwd:
        return raw_url
    from urllib.parse import urlparse, urlunparse, quote
    p = urlparse(raw_url)
    if p.username:  # already has credentials embedded
        return raw_url
    netloc = f"{quote(user, safe='')}:{quote(passwd, safe='')}@{p.hostname}"
    if p.port:
        netloc += f":{p.port}"
    return urlunparse((p.scheme, netloc, p.path, p.params, p.query, p.fragment))


# ── Backoff (matches the 5s/10s/20s/30s-cap pattern used in the C code) ────
def backoff_seconds(retry_count: int) -> int:
    return min(5 * (2 ** min(retry_count, 3)), 30)


# ── Live view: ffmpeg -c copy, pushed straight into MediaMTX as publisher ──
def run_live(camera_id: int, camera_name: str, url: str):
    target = f"rtsp://{MEDIAMTX_RTSP_HOST}:{MEDIAMTX_RTSP_PORT}/cam_{camera_id}"
    retry_count = 0
    while not _shutdown.is_set():
        cmd = [
            "ffmpeg", "-nostdin", "-loglevel", "warning",
            "-rtsp_transport", "tcp",
            "-i", url,
            "-c", "copy",
            "-f", "rtsp",
            target,
        ]
        log.info(f"[{camera_name}] LIVE starting -> {target}")
        start = time.monotonic()
        try:
            proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.PIPE, text=True)
            while proc.poll() is None:
                if _shutdown.is_set():
                    proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                    return
                time.sleep(1)
            stderr_tail = ""
            if proc.stderr:
                stderr_tail = proc.stderr.read()[-400:]
            elapsed = time.monotonic() - start
            log.warning(f"[{camera_name}] LIVE exited (code={proc.returncode}, "
                        f"ran {elapsed:.0f}s) {stderr_tail.strip()!r}")
        except FileNotFoundError:
            log.error("ffmpeg not found on PATH — cannot start LIVE stream")
            return
        except Exception as e:
            log.error(f"[{camera_name}] LIVE error: {e}")
            elapsed = 0

        retry_count = 0 if elapsed >= 15 else retry_count + 1
        delay = backoff_seconds(retry_count)
        log.info(f"[{camera_name}] LIVE retrying in {delay}s")
        _shutdown.wait(delay)


# ── Recording: ffmpeg with drawtext watermark, segmented MP4 to disk ───────
def run_record(camera_id: int, camera_name: str, url: str, output_dir: str):
    Path(output_dir).mkdir(parents=True, exist_ok=True)
    # Flat, camera-id-prefixed filenames directly in output_dir — matches
    # the existing "no subfolders" convention already established today.
    pattern = os.path.join(output_dir, f"cam_{camera_id}_%Y%m%d_%H%M%S.mp4")

    # Date/time watermark (top-left). Deliberately using ffmpeg's default
    # %{localtime} expansion rather than a custom strftime format string —
    # a custom format requires colons to be escaped through three separate
    # layers (shell, ffmpeg arg parsing, drawtext's own filter-option
    # parser), which is fragile and, when tested, produced a
    # "%{localtime} requires at most 1 arguments" warning even after
    # escaping. The default format is a perfectly readable
    # "YYYY-MM-DD HH:MM:SS" already. GPS/speed intentionally omitted here
    # until real hardware exists — add a second drawtext with a
    # file-based text source once a GPS reader is available, e.g.:
    #   drawtext=textfile=/run/mnvr/gps_overlay.txt:reload=1:...
    watermark = (
        "drawtext=text='%{localtime}'"
        ":fontcolor=white:fontsize=22:box=1:boxcolor=black@0.5:boxborderw=4"
        ":x=10:y=10"
    )

    retry_count = 0
    while not _shutdown.is_set():
        cmd = [
            "ffmpeg", "-nostdin", "-loglevel", "warning",
            "-rtsp_transport", "tcp",
            "-i", url,
            "-vf", watermark,
            "-c:v", "libx264", "-preset", "veryfast", "-b:v", "2048k",
            "-c:a", "aac", "-b:a", "64k",
            "-f", "segment", "-segment_time", str(SEGMENT_SECONDS),
            "-segment_format", "mp4", "-reset_timestamps", "1", "-strftime", "1",
            pattern,
        ]
        log.info(f"[{camera_name}] REC starting -> {pattern}")
        start = time.monotonic()
        try:
            proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.PIPE, text=True)
            while proc.poll() is None:
                if _shutdown.is_set():
                    proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                    return
                time.sleep(1)
            stderr_tail = ""
            if proc.stderr:
                stderr_tail = proc.stderr.read()[-400:]
            elapsed = time.monotonic() - start
            log.warning(f"[{camera_name}] REC exited (code={proc.returncode}, "
                        f"ran {elapsed:.0f}s) {stderr_tail.strip()!r}")
        except FileNotFoundError:
            log.error("ffmpeg not found on PATH — cannot start recording")
            return
        except Exception as e:
            log.error(f"[{camera_name}] REC error: {e}")
            elapsed = 0

        retry_count = 0 if elapsed >= 15 else retry_count + 1
        delay = backoff_seconds(retry_count)
        log.info(f"[{camera_name}] REC retrying in {delay}s")
        _shutdown.wait(delay)


# ── Main: one LIVE + one REC thread per camera, refreshed from the DB ──────
def main():
    log.info("=" * 60)
    log.info("  Stream Manager — ffmpeg-based live view + recording")
    log.info(f"  DB           : {DB_USER}@{DB_HOST}:{DB_PORT}/{DB_NAME}")
    log.info(f"  MediaMTX RTSP: {MEDIAMTX_RTSP_HOST}:{MEDIAMTX_RTSP_PORT}")
    log.info(f"  Recordings   : {RECORDINGS_BASE}")
    log.info("=" * 60)

    started_cameras: set[int] = set()
    threads: list[threading.Thread] = []

    conn = get_db()
    log.info("Connected to PostgreSQL")

    while not _shutdown.is_set():
        try:
            cameras = fetch_active_cameras(conn)
        except Exception as e:
            log.warning(f"Camera fetch failed ({e}), reconnecting to DB...")
            try:
                conn.close()
            except Exception:
                pass
            conn = get_db()
            cameras = fetch_active_cameras(conn)

        for cam in cameras:
            cid = cam["camera_id"]
            if cid in started_cameras:
                continue
            name = cam["camera_name"]
            live_url = build_authed_url(cam["rtsp_url"], cam["rtsp_username"], cam["rtsp_password"])
            rec_url = build_authed_url(
                cam["rec_rtsp_url"] or cam["rtsp_url"],
                cam["rtsp_username"], cam["rtsp_password"])
            out_dir = cam["rec_output_dir"] or RECORDINGS_BASE

            t_live = threading.Thread(target=run_live, args=(cid, name, live_url),
                                       name=f"live-{cid}", daemon=True)
            t_rec = threading.Thread(target=run_record, args=(cid, name, rec_url, out_dir),
                                      name=f"rec-{cid}", daemon=True)
            t_live.start()
            t_rec.start()
            threads.extend([t_live, t_rec])
            started_cameras.add(cid)
            log.info(f"[{name}] Started LIVE + REC threads (camera_id={cid})")

        _shutdown.wait(CAMERA_REFRESH_INTERVAL)

    log.info("Waiting for camera threads to exit...")
    for t in threads:
        t.join(timeout=10)
    log.info("Stream manager stopped.")


if __name__ == "__main__":
    main()
