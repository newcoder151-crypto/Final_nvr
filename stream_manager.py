#!/usr/bin/env python3
"""
stream_manager.py — ffmpeg-based live view + recording, working from a
camera feed that MediaMTX itself pulls (never from the camera directly).

WHY THIS EXISTS (context for future maintainers):
Testing (packet captures, side-by-side comparisons) proved that
GStreamer's rtspsrc has real reliability problems against this specific
camera fleet — inconsistent SETUP failures, and in one proven case
(packet-captured) a camera that accepted PLAY with 200 OK and then sent
zero RTP data. ffmpeg's RTSP client, tested side by side against the
exact same URLs and credentials, worked cleanly every single time.
Rather than keep fighting rtspsrc, this replaces mnvrd's own camera
connections with ffmpeg subprocesses — but see the architecture history
below for how "which process actually talks to the camera" evolved.

ARCHITECTURE (2 stages per camera, this process — plus MediaMTX itself):
  0. MediaMTX pulls directly from each camera (mediamtx-sync.py adds a
     cam_<id>_raw path per camera via MediaMTX's own API, source =
     rec_rtsp_url — the profile proven reliable across this fleet). This
     is the ONLY thing that ever opens an RTSP connection to a physical
     camera, using MediaMTX's own mature, purpose-built RTSP client and
     reconnect logic rather than anything hand-rolled in this file.
  1. WATERMARK (run_watermark): reads cam_<id>_raw — a local, already-
     stable MediaMTX path, never the camera itself — burns in the
     date/time watermark (GPS/speed placeholder until hardware exists)
     once, encodes once, republishes to cam_<id>: the path the
     frontend's WebRTC/HLS players actually watch.
  2. REC RELAY (run_rec_relay): reads cam_<id> — the SAME final
     watermarked path LIVE viewers see — and writes segmented MP4 files
     via a plain -c copy (no second encode needed). Fully independent
     process from run_watermark(); each only depends on MediaMTX being
     up, not on the other still running, so one failing doesn't touch
     the other. Reading the same path LIVE uses (rather than a separate
     raw feed) also guarantees recordings and live view show identical
     content, watermark included.

ARCHITECTURE HISTORY (why it took several tries to land here):
  - v1: two totally separate ffmpeg processes, each opening its OWN
    connection to the camera (LIVE from rtsp_url via -c copy with no
    watermark, REC from rec_rtsp_url via transcode with one). This
    fleet's firmware appears to cap concurrent RTSP sessions per camera,
    so these two connections regularly fought over that budget — the
    repeated "REC connects fine, LIVE gets Connection refused" pattern.
  - v2: merged into ONE ffmpeg process reading the camera once and
    fanning out to two encoded outputs directly. Fixed the connection
    contention, but ffmpeg opens every output's header during startup
    before processing any frames — so a transient failure on the LIVE-
    publish side made the WHOLE process fail to start, taking a
    perfectly fine recording down with it too.
  - v3: split into ingest (the one real camera connection, done by THIS
    script) + two independent local relays. Fixed the fault-isolation
    problem, but the ingest process was still a hand-rolled ffmpeg
    subprocess doing the actual camera reconnect logic — which is
    exactly the kind of thing that kept surfacing new edge cases
    (dump_extra's own H.264 parser crashing one relay, 404s from
    relays starting before ingest had published, etc).
  - v4 (this version): stopped hand-rolling the camera connection
    entirely and let MediaMTX do it — that's what it's actually built
    and tested for. This script now only ever touches already-stable
    local MediaMTX paths, never a physical camera.

Each stage gets its own supervisor thread running a restart-with-backoff
loop around its ffmpeg subprocess — matching the same 5s/10s/20s/30s-cap
backoff pattern used elsewhere in this project — plus a separate
recording-indexer thread (see index_recordings() below). That's 3
threads per camera now (watermark, rec-relay, indexer).

mnvrd itself should have native_streaming_enabled=false in mnvr.conf
when this is running, so its own streamer_module.c/recorder.c don't
also try to connect to the same cameras (mnvrd's AI/DB/API/health
functions keep running either way — only the direct camera RTSP
connections are disabled).
"""
import os
import sys
import re
import json
import time
import shlex
import signal
import logging
import subprocess
import threading
from pathlib import Path
from datetime import datetime, timedelta

import psycopg2
import psycopg2.extras
import requests

# ── Config (env-overridable, same pattern as mediamtx-sync.py) ─────────────
DB_HOST     = os.environ.get("DB_HOST", "localhost")
DB_PORT     = int(os.environ.get("DB_PORT", "5432"))
DB_NAME     = os.environ.get("DB_NAME", "mnvr")
DB_USER     = os.environ.get("DB_USER", "mnvr")
DB_PASSWORD = os.environ.get("DB_PASSWORD", "mnvr")

MEDIAMTX_RTSP_HOST = os.environ.get("MEDIAMTX_RTSP_HOST", "127.0.0.1")
MEDIAMTX_RTSP_PORT = os.environ.get("MEDIAMTX_RTSP_PORT", "8554")
MEDIAMTX_API = os.environ.get("MEDIAMTX_API", "http://localhost:9997")

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
    """Only camera_id/name/rec_output_dir are needed here now — this
    process never connects to a physical camera itself anymore (see the
    module docstring), so it has no need for RTSP URLs or credentials at
    all. Those live in mediamtx-sync.py, which is the thing that actually
    talks to cameras now."""
    with conn.cursor(cursor_factory=psycopg2.extras.DictCursor) as cur:
        cur.execute("""
            SELECT camera_id, camera_name, rec_output_dir
            FROM cameras
            WHERE status = 'ACTIVE'
              AND rtsp_url IS NOT NULL AND rtsp_url != ''
            ORDER BY camera_id
        """)
        return cur.fetchall()


# ── Backoff (matches the 5s/10s/20s/30s-cap pattern used in the C code) ────
def backoff_seconds(retry_count: int) -> int:
    return min(5 * (2 ** min(retry_count, 3)), 30)


# ── Shared supervisor loop: restart-with-backoff around one ffmpeg cmd ─────
# ── Live status tracking, for report_health() below ─────────────────────────
# Nothing else in this project writes to camera_health anymore. mnvrd used
# to (via POST /api/cameras/:id/health), but it's disabled — see the
# module docstring. Without this, the table just sits there with stale or
# empty data forever, and the frontend's online/recording badges (which
# read the latest camera_health row via GET /api/cameras) end up
# completely disconnected from whether ffmpeg/MediaMTX are actually
# streaming — a camera can be recording live footage right now and still
# show "offline" in the UI, because nothing ever told the DB otherwise.
_status_lock = threading.Lock()
_camera_status: dict[int, dict[str, bool]] = {}


def _set_stage_alive(camera_id: int, stage_key: str, alive: bool):
    with _status_lock:
        _camera_status.setdefault(camera_id, {})[stage_key] = alive


# ── Shared supervisor loop: restart-with-backoff around one ffmpeg cmd ─────
def _run_supervised(camera_name: str, stage: str, cmd: list[str],
                     camera_id: int | None = None, stage_key: str | None = None):
    """Runs `cmd` in a restart-with-backoff loop until shutdown. Shared by
    both stages (watermark/rec-relay) so each is a genuinely independent
    process/thread — one stage crashing or backing off never blocks or
    kills the other, unlike having them share one ffmpeg invocation (see
    the module docstring's "v2" note for why that mattered).

    If camera_id/stage_key are given, also updates _camera_status so
    report_health() can tell the DB (and therefore the UI) what's
    actually running right now, instead of the UI reading stale data."""
    retry_count = 0
    while not _shutdown.is_set():
        log.info(f"[{camera_name}] {stage} starting")
        start = time.monotonic()
        try:
            proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.PIPE, text=True)
            marked_alive = False
            while proc.poll() is None:
                if _shutdown.is_set():
                    proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                    if camera_id is not None and stage_key:
                        _set_stage_alive(camera_id, stage_key, False)
                    return
                # Only mark "alive" once it's survived a few seconds —
                # avoids a crash-loop briefly flashing "online" every retry.
                if not marked_alive and time.monotonic() - start > 3:
                    if camera_id is not None and stage_key:
                        _set_stage_alive(camera_id, stage_key, True)
                    marked_alive = True
                time.sleep(1)
            if camera_id is not None and stage_key:
                _set_stage_alive(camera_id, stage_key, False)
            stderr_tail = ""
            if proc.stderr:
                stderr_tail = proc.stderr.read()[-400:]
            elapsed = time.monotonic() - start
            log.warning(f"[{camera_name}] {stage} exited (code={proc.returncode}, "
                        f"ran {elapsed:.0f}s) {stderr_tail.strip()!r}")
        except FileNotFoundError:
            log.error(f"ffmpeg not found on PATH — cannot start {stage}")
            return
        except Exception as e:
            log.error(f"[{camera_name}] {stage} error: {e}")
            elapsed = 0

        retry_count = 0 if elapsed >= 15 else retry_count + 1
        delay = backoff_seconds(retry_count)
        log.info(f"[{camera_name}] {stage} retrying in {delay}s")
        _shutdown.wait(delay)


# Date/time watermark (top-left), applied once at ingest so REC and LIVE
# always match automatically instead of needing to be kept in sync by
# hand. Deliberately using ffmpeg's default %{localtime} expansion rather
# than a custom strftime format string — a custom format requires colons
# to be escaped through three separate layers (shell, ffmpeg arg parsing,
# drawtext's own filter-option parser), which is fragile and, when tested,
# produced a "%{localtime} requires at most 1 arguments" warning even
# after escaping. The default format is a perfectly readable
# "YYYY-MM-DD HH:MM:SS" already. GPS/speed intentionally omitted here
# until real hardware exists — add a second drawtext with a file-based
# text source once a GPS reader is available, e.g.:
#   drawtext=textfile=/run/mnvr/gps_overlay.txt:reload=1:...
WATERMARK = (
    "drawtext=text='%{localtime}'"
    ":fontcolor=white:fontsize=22:box=1:boxcolor=black@0.5:boxborderw=4"
    ":x=10:y=10"
)


# ── Stage 1: watermark — reads MediaMTX's own camera pull, republishes live ─
def run_watermark(camera_id: int, camera_name: str):
    """Reads cam_<id>_raw — a path MediaMTX itself pulls directly from the
    camera (see mediamtx-sync.py; that's now the ONLY thing that ever
    opens an RTSP connection to the physical camera). Burns in the
    watermark, encodes once, republishes to cam_<id> — the path the
    frontend's WebRTC/HLS players actually watch.

    This process never touches the camera itself, only MediaMTX's own
    local, already-stable pull of it — so camera flakiness becomes
    MediaMTX's problem to reconnect through (which it's built and tested
    for), not this process's."""
    src = f"rtsp://{MEDIAMTX_RTSP_HOST}:{MEDIAMTX_RTSP_PORT}/cam_{camera_id}_raw"
    target = f"rtsp://{MEDIAMTX_RTSP_HOST}:{MEDIAMTX_RTSP_PORT}/cam_{camera_id}"
    cmd = [
        "ffmpeg", "-nostdin", "-loglevel", "warning",
        "-rtsp_transport", "tcp",
        "-analyzeduration", "10M",
        "-probesize", "10M",
        "-i", src,
        "-vf", WATERMARK,
        # -bf 0 + baseline profile: MediaMTX's WebRTC (WHEP) closes the
        # session outright ("WebRTC doesn't support H264 streams with
        # B-frames") for any H264 stream containing B-frames. libx264's
        # "veryfast" preset emits B-frames by default, which is invisible
        # for recording (MP4/-c copy tolerates B-frames fine) but silently
        # breaks the WHEP path that live view depends on — exactly the
        # "REC works, LIVE doesn't" split reported. -pix_fmt yuv420p is
        # also required: WebRTC/browsers only decode 4:2:0.
        "-c:v", "libx264", "-preset", "veryfast", "-b:v", "2048k",
        "-profile:v", "baseline", "-bf", "0", "-pix_fmt", "yuv420p",
        "-g", "50", "-keyint_min", "50", "-sc_threshold", "0",
        "-force_key_frames", "expr:gte(t,n_forced*2)",
        # Opus, not AAC: MediaMTX's WebRTC only supports Opus/G711/LPCM
        # for audio and drops any other audio codec from the WHEP output
        # (silently, no error) — AAC would still show video over WebRTC
        # but live view would always be muted. Opus keeps audio in BOTH
        # live (WebRTC) and recordings (still muxes into MP4 fine).
        "-c:a", "libopus", "-b:a", "64k", "-async", "50",
        "-bsf:v", "dump_extra=freq=keyframe",
        "-f", "rtsp", "-rtsp_transport", "tcp",
        target,
    ]
    _run_supervised(camera_name, "WATERMARK", cmd, camera_id, "watermark")


# ── Stage 2: REC relay — reads the final watermarked path, segments to disk ─
def run_rec_relay(camera_id: int, camera_name: str, output_dir: str):
    """Reads cam_<id> — the SAME final watermarked path the frontend
    watches live — and writes segmented MP4 files. Plain -c copy, no
    re-encode needed. Reading the same path LIVE viewers see (rather than
    the raw pre-watermark feed) guarantees recordings and live view are
    provably showing identical content, and this process is fully
    independent from run_watermark(): if one dies, the other is
    unaffected, since neither depends on the other still running — both
    only depend on MediaMTX, which is already up either way."""
    Path(output_dir).mkdir(parents=True, exist_ok=True)
    pattern = os.path.join(output_dir, f"cam_{camera_id}_%Y%m%d_%H%M%S.mp4")
    src = f"rtsp://{MEDIAMTX_RTSP_HOST}:{MEDIAMTX_RTSP_PORT}/cam_{camera_id}"
    cmd = [
        "ffmpeg", "-nostdin", "-loglevel", "warning",
        "-rtsp_transport", "tcp",
        "-i", src,
        "-c", "copy",
        "-f", "segment", "-segment_time", str(SEGMENT_SECONDS),
        "-segment_format", "mp4", "-reset_timestamps", "1", "-strftime", "1",
        pattern,
    ]
    _run_supervised(camera_name, "REC-RELAY", cmd, camera_id, "rec")


# ── Recording indexer ───────────────────────────────────────────────────────
# recorder/recorder.py used to be the thing that called POST /api/recordings
# for every finished segment, but it's disabled in start.sh now that this
# script owns recording. Without something doing that, files land correctly
# in rec_output_dir but the `recordings` table (which the video player reads
# via GET /api/recordings) never gets a row for them, so nothing shows up in
# the UI even though the files are genuinely there. This closes that gap by
# writing directly to Postgres (no dependency on the API/auth being up).
_SEGMENT_RE = re.compile(r"^cam_(\d+)_(\d{8})_(\d{6})\.mp4$")


def _ffprobe(path: str) -> dict | None:
    try:
        out = subprocess.run(
            ["ffprobe", "-v", "error", "-print_format", "json",
             "-show_format", "-show_streams", path],
            capture_output=True, text=True, timeout=20,
        )
        return json.loads(out.stdout) if out.returncode == 0 and out.stdout else None
    except Exception:
        return None


def _video_codec_label(codec_name: str) -> str:
    return {"h264": "H.264", "hevc": "H.265", "h265": "H.265"}.get(
        (codec_name or "").lower(), (codec_name or "H.264").upper())


def _upsert_recording(conn, camera_id: int, path: Path):
    m = _SEGMENT_RE.match(path.name)
    if not m:
        return  # not one of our segment files (e.g. a leftover/unexpected file)
    try:
        start_ts = datetime.strptime(f"{m.group(2)}{m.group(3)}", "%Y%m%d%H%M%S")
    except ValueError:
        return

    probe = _ffprobe(str(path))
    if not probe:
        return  # file may still be mid-write/corrupt; will retry next poll

    fmt = probe.get("format", {})
    streams = probe.get("streams", [])
    v = next((s for s in streams if s.get("codec_type") == "video"), {})
    has_audio = any(s.get("codec_type") == "audio" for s in streams)

    try:
        duration = float(fmt.get("duration") or 0)
    except (TypeError, ValueError):
        duration = 0.0
    try:
        num, den = (v.get("r_frame_rate") or "0/1").split("/")
        fps = round(float(num) / float(den), 2) if float(den) else None
    except Exception:
        fps = None

    try:
        file_size = int(fmt.get("size") or path.stat().st_size)
    except Exception:
        file_size = path.stat().st_size

    with conn.cursor() as cur:
        cur.execute(
            """
            INSERT INTO recordings(
                camera_id, file_path, file_name, file_size_bytes, duration_seconds,
                start_timestamp, end_timestamp, video_codec, resolution_width,
                resolution_height, fps_actual, has_audio, recording_mode, status)
            VALUES(%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,'CONTINUOUS','COMPLETED')
            ON CONFLICT (file_path) DO UPDATE SET
                file_size_bytes=EXCLUDED.file_size_bytes,
                duration_seconds=EXCLUDED.duration_seconds,
                end_timestamp=EXCLUDED.end_timestamp,
                status='COMPLETED',
                updated_at=NOW()
            """,
            (camera_id, str(path), path.name, file_size, int(duration),
             start_ts, start_ts + timedelta(seconds=duration),
             _video_codec_label(v.get("codec_name")),
             v.get("width"), v.get("height"), fps, int(has_audio)),
        )
    conn.commit()


def index_recordings(camera_id: int, camera_name: str, output_dir: str):
    """Polls output_dir for this camera's segments and upserts finished ones
    into the DB. The newest-by-name file is always skipped — ffmpeg is still
    writing it — everything older is guaranteed closed and safe to probe."""
    conn = get_db()
    indexed: set[str] = set()
    poll_interval = 30
    while not _shutdown.is_set():
        try:
            files = sorted(Path(output_dir).glob(f"cam_{camera_id}_*.mp4"))
            for f in files[:-1]:  # skip the newest (still being written)
                key = str(f)
                if key in indexed:
                    continue
                try:
                    _upsert_recording(conn, camera_id, f)
                    indexed.add(key)
                except Exception as e:
                    log.warning(f"[{camera_name}] recording index failed for {f.name}: {e}")
                    try:
                        conn.rollback()
                    except Exception:
                        pass
        except Exception as e:
            log.warning(f"[{camera_name}] recording indexer error ({e}), reconnecting to DB...")
            try:
                conn.close()
            except Exception:
                pass
            conn = get_db()
        _shutdown.wait(poll_interval)


# ── Health reporting: tell the DB (and therefore the UI) what's real ───────
def _path_ready(path_name: str) -> bool:
    """Ground truth for is_online: does MediaMTX currently have an active,
    ready source for cam_<id>_raw? This comes from MediaMTX itself (which
    is the only thing that actually talks to the camera now — see the
    module docstring), not from anything this script assumes."""
    try:
        r = requests.get(f"{MEDIAMTX_API}/v3/paths/get/{path_name}", timeout=3)
        if r.ok:
            return bool(r.json().get("ready"))
    except Exception:
        pass
    return False


def report_health(all_camera_ids_fn):
    """Periodically writes a real camera_health row per active camera.

    This is the fix for the actual root cause behind cameras showing
    "offline" in the UI while genuinely streaming: nothing has written to
    camera_health since mnvrd (which used to, via
    POST /api/cameras/:id/health) was disabled. GET /api/cameras joins
    the LATEST camera_health row to populate is_online/is_recording for
    the frontend — with nobody writing new rows, that join was returning
    stale-or-empty data forever, completely disconnected from whether
    ffmpeg/MediaMTX were actually streaming. Writing directly to Postgres
    here (same approach as index_recordings() above) avoids needing an
    API auth token inside this script.
    """
    conn = get_db()
    while not _shutdown.is_set():
        try:
            for cid in all_camera_ids_fn():
                is_online = _path_ready(f"cam_{cid}_raw")
                with _status_lock:
                    is_recording = bool(_camera_status.get(cid, {}).get("rec"))
                try:
                    with conn.cursor() as cur:
                        cur.execute(
                            """INSERT INTO camera_health
                               (camera_id, is_online, is_recording, error_count)
                               VALUES (%s, %s, %s, 0)""",
                            (cid, int(is_online), int(is_recording)))
                    conn.commit()
                except Exception as e:
                    log.warning(f"health report failed for camera {cid}: {e}")
                    conn.rollback()
        except Exception as e:
            log.warning(f"report_health error ({e}), reconnecting to DB...")
            try:
                conn.close()
            except Exception:
                pass
            conn = get_db()
        _shutdown.wait(10)


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

    t_health = threading.Thread(target=report_health, args=(lambda: set(started_cameras),),
                                 name="health-reporter", daemon=True)
    t_health.start()
    threads.append(t_health)

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
            out_dir = cam["rec_output_dir"] or RECORDINGS_BASE

            t_watermark = threading.Thread(target=run_watermark, args=(cid, name),
                                            name=f"watermark-{cid}", daemon=True)
            t_rec = threading.Thread(target=run_rec_relay, args=(cid, name, out_dir),
                                      name=f"rec-{cid}", daemon=True)
            t_idx = threading.Thread(target=index_recordings, args=(cid, name, out_dir),
                                      name=f"idx-{cid}", daemon=True)
            t_watermark.start()
            t_rec.start()
            t_idx.start()
            threads.extend([t_watermark, t_rec, t_idx])
            started_cameras.add(cid)
            log.info(f"[{name}] Started WATERMARK + REC-RELAY + "
                     f"recording-indexer threads (camera_id={cid})")

        _shutdown.wait(CAMERA_REFRESH_INTERVAL)

    log.info("Waiting for camera threads to exit...")
    for t in threads:
        t.join(timeout=10)
    log.info("Stream manager stopped.")


if __name__ == "__main__":
    main()
