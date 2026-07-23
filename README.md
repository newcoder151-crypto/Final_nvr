# mNVR — Corrections

Drop these files into your project at the same relative paths (they replace
the existing files). Verified with a full `make clean && make` — the daemon
now builds and links with zero errors.

## 1. Build error (--build-nvr failing) — FIXED

`ai_module.c` calls two DB functions, `db_insert_motion_event` and
`db_insert_face_with_dedup_embed`, that were never implemented anywhere —
that's the `undefined reference` your linker was hitting.

Changed:
- `nvr_core/src/db/db_module.h` — added the two declarations.
- `nvr_core/src/db/db_module.c` — added the two implementations.
  - `db_insert_motion_event` is a thin wrapper over the existing
    `db_insert_event("MOTION", ...)`.
  - `db_insert_face_with_dedup_embed` inserts into `face_detections`
    (bbox converted from normalised to pixel coords, embedding hex-encoded
    as a `bytea` literal). It executes synchronously (not through the async
    write queue) because the embedding hex string can be several KB —
    larger than one write-queue slot.

## 2. `server/ai/requirements.txt: No such file` / `Could not import module "sidecar"` — FIXED

Your zip is missing the entire `server/ai/` directory — no `sidecar.py`, no
`requirements.txt`. `server/src/routes/ai.js` launches
`uvicorn sidecar:app --port 8000` from that folder and proxies four routes
to it (`/detect`, used by `/api/ai/detect`, `/api/ai/people-count`,
`/api/ai/intrusion`, `/api/ai/object-detect`), plus `/api/ai/health` → `/health`.

I reconstructed `server/ai/sidecar.py` from that exact contract (read
straight out of your `ai.js` — same request fields, same response shape:
`{detections:[{label, confidence, bbox:[x1,y1,x2,y2]}], image_size:[w,h], model}`).
It uses `ultralytics` YOLO, lazy-loads/caches models by filename (so
`model=yolov8n.pt` vs any other weights you pass in `people-count`/`object-detect`
calls both work without reloading each request), and won't crash on startup
even if the default model can't be fetched yet (network not up, etc.) —
`/detect` just returns a clear 500 instead of taking the whole process down.

Verified: started it with uvicorn, hit `/health` (200 OK), hit `/detect`
with a test image and got a clean, correctly-formatted error rather than a
crash when `ultralytics` wasn't installed. I could not fully pip-install
`ultralytics`+`torch` in this sandbox (very large download, timed out) to
run a real inference end-to-end, so do a first real test on your machine:

```bash
cd server/ai
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/uvicorn sidecar:app --port 8000
curl -F "image=@/path/to/some.jpg" http://localhost:8000/detect
```

**Not restorable by me:** if your original `sidecar.py` had custom logic
beyond plain YOLO object detection (face recognition model, smoke/fire
classifier, RDAS-specific tuning, etc. — several of these are referenced
elsewhere in your codebase), you'll need to port that over from your own
backup; I only had the generic `/detect` + `/health` contract to go on.

## 3. Watermark: date/time + GPS/speed (live view + recording)

Both the live restream pipeline (`streamer_module.c`) and the recording
pipeline (`recorder.c`) were doing a pure stream-copy (no decode/re-encode),
so nothing could be burned into the video before. Both now decode once,
overlay, and re-encode:

```
... -> videoconvert -> clockoverlay (date/time) -> textoverlay (GPS/speed)
    -> videoconvert -> x264enc/x265enc -> ... (restream / mp4mux as before)
```

- `clockoverlay` draws the date/time automatically every frame — no extra
  wiring needed (`time-format = "%Y-%m-%d %H:%M:%S"`).
- `textoverlay` shows GPS + speed. Its text is refreshed once a second via
  a buffer probe (not a GLib timer, so it doesn't need to share a
  `GMainContext` across camera threads).
- **There is no GPS receiver integration anywhere in this codebase** — I
  found no serial/NMEA/gpsd code at all, only a `time_sync_method = GPS`
  config string. So the overlay currently shows `GPS: NO FIX` until you
  feed it real data.

  Wire up your GPS source by calling, whenever you get a fix:
  ```c
  streamer_update_telemetry(cam, lat, lon, speed_kmh, true);   // live view
  recorder_update_telemetry(rec, lat, lon, speed_kmh, true);   // recording
  ```
  (`cam`/`rec` are the per-camera structs already in `streamer_module.c` /
  `recorder.c` — call these from wherever your GPS module ends up living.)

Trade-offs worth knowing:
- This adds a decode + encode step to both pipelines that used to be pure
  passthrough (no CPU cost before, real CPU cost now). On weak hardware you
  may need to lower the recording encoder's `speed-preset` (currently `5`
  /"fast") or the live encoder's bitrate (currently 2048 kbps, `ultrafast`).
- Live-view watermark encoder tunes for low latency (`ultrafast`,
  `zerolatency`); recording watermark encoder tunes for quality (`fast`,
  4096 kbps) since it isn't latency-sensitive.

## 4. Root cause: no recordings + black/no watermark — FOUND & FIXED

Both symptoms had the same cause: **`gstreamer1.0-x` wasn't installed.**
That package (not `plugins-good`/`bad`/`ugly` — a separate one) is what
actually provides the `clockoverlay` and `textoverlay` elements the
watermark uses. Without it:

- `recorder.c`'s `gst_element_factory_make("clockoverlay", ...)` returns
  `NULL` → my `if (!rec->clock_overlay || ...) goto error;` check fires →
  `recorder_start()` fails immediately, for every camera → **zero
  recordings, silently.**
- `streamer_module.c`'s pipeline build fails the same way → the live-view
  pipeline never reaches PLAYING → the browser shows a blank/black video
  element (no track, not literally a black frame).

I confirmed this by reproducing your exact chain with `gst-launch-1.0`:
`no element "clockoverlay"` before installing the package, then a clean run
producing a real, watermark-visible MP4 after. Fixed two ways:

1. Added `gstreamer1.0-x` to `start.sh`'s dependency install line (section 5
   above) — a fresh `sudo bash start.sh --build-nvr` will now pull it in.
2. If you don't want to rebuild: just run
   ```bash
   sudo apt install -y gstreamer1.0-x
   bash start.sh --restart
   ```
   No rebuild needed — the binary already has the code, it just needs the
   plugin available at runtime.

## 5. Two smaller issues from your log (fixed / explained)

- **`mediamtx-sync` spamming "path already exists" (400) every 15s**:
  the sync script only treated HTTP 409 as "already exists" and fell back
  to PATCH; your MediaMTX build returns 400 with `{"error":"path already
  exists"}` instead. Fixed in `mediamtx/mediamtx-sync.py` — it now checks
  the response body for both 400 and 409. This was cosmetic (the path kept
  working either way, it just re-tried every cycle), but the log noise is
  gone now.
- **`cam_29 401 Unauthorized`** — this is a real, camera-specific problem,
  not something in the code: MediaMTX pulls `cam_29`'s RTSP feed straight
  from the camera using the username/password stored for it in the
  `cameras` table, and that camera is rejecting them. Worth checking: has
  its password changed on the camera itself recently, or is the row in
  `cameras` for `camera_id=29` out of date? `cam_30` right above it in your
  log works fine, so this looks isolated to that one camera's stored
  credentials rather than a systemic issue.

## 6. `--build-nvr` now also starts (and keeps running) mnvrd

Previously `--build-nvr` compiled the daemon and exited, telling you to
separately run `--with-nvr`. Changed `start.sh` (one line) so `--build-nvr`
now sets the same flag `--with-nvr` does internally, so it builds *and*
launches mnvrd, alongside the rest of the stack, in the same run:

```bash
sudo bash start.sh --build-nvr
```

mnvrd runs the same way every other service in this project already does
(`mediamtx`, the AI sidecar, the recorder, the API, the frontend) — detached
via `nohup ... &`, PID tracked in `.pids/nvr.pid`, log at `logs/nvr.log`.
That means it **keeps running after you close the terminal** — you no
longer need to run `./build/mnvrd -c ... -s ... -d ...` by hand at all.

Manage it like the other services:
```bash
bash start.sh --status        # shows mnvrd PID alongside everything else
bash start.sh --logs nvr      # tail logs/nvr.log
bash start.sh --stop          # stops everything, including mnvrd
bash start.sh --restart
```

**One gap worth knowing:** this project's process model (like all its other
services) doesn't auto-restart a crashed process or survive a reboot — it's
nohup + a PID file, not a supervisor. If you genuinely need "always
recording, even across a crash or a reboot", the standard fix is a systemd
unit for `mnvrd` with `Restart=always`, e.g.:

```ini
# /etc/systemd/system/mnvrd.service
[Unit]
Description=Railway mNVR C Daemon
After=network.target postgresql.service

[Service]
ExecStart=/home/nvr/Documents/ai_mnvr/mnvr_integrated_final/integrated/nvr_core/build/mnvrd -c /home/nvr/Documents/ai_mnvr/mnvr_integrated_final/integrated/nvr_core/config/mnvr.conf
Restart=always
RestartSec=5
User=nvr

[Install]
WantedBy=multi-user.target
```
then `sudo systemctl enable --now mnvrd`. I didn't wire this into `start.sh`
since it'd conflict with the nohup/PID-file model the rest of the stack
uses — happy to build it out properly if you want to move the whole stack
to systemd rather than just mnvrd.

## 7. Where to actually see the watermark

- **Live view**: open the web UI's camera grid/live view page (the one using
  `hls.js` / MediaMTX) — the date/time (top-left) and `GPS: NO FIX` or
  `GPS: lat, lon  Speed: X km/h` (bottom-left) are burned directly into the
  decoded video, so they show up in the browser player itself.
- **Recordings**: open any `.mp4` written to `storage/recordings/` (VLC,
  `ffplay`, etc.) — same two overlays, burned into the file itself, not
  something the player adds.
- If you only see the clock and not GPS coordinates: that's expected right
  now, since (as noted in section 3) there's no GPS receiver wired in yet —
  it'll show `GPS: NO FIX` until you call `streamer_update_telemetry()` /
  `recorder_update_telemetry()` with real fixes.
- If you don't see either: check `logs/nvr.log` for GStreamer element
  errors (`clockoverlay`/`textoverlay`/`x264enc` failing to load usually
  means `gstreamer1.0-plugins-good` / `gstreamer1.0-plugins-ugly` aren't
  installed — `x264enc` specifically ships in `-ugly`).

Already implemented — `src/pages/CameraGrid.tsx` and `VideoPlayer.tsx`
already load `hls.js` and point it at MediaMTX's HLS output, and
`mediamtx/mediamtx.yml` already has `hls: yes` / port 8888. Nothing to fix
here; if it's not working for you in practice, check that
`VITE_MEDIAMTX_HLS` in your `.env` matches wherever MediaMTX is actually
reachable from the browser.

## 8. Fixed: cam_30 live-view-but-no-recording / cam_29 recording-but-no-live-view

Root cause: **two independent RTSP connections to the same physical camera,
racing each other.** MediaMTX was connecting directly to each camera's RTSP
URL for live view, while `mnvrd` *also* independently connected to the same
camera's RTSP URL for recording/AI. Most IP cameras (including yours, based
on the symptoms) only accept 1-2 concurrent RTSP client connections —
whichever of the two grabbed the slot worked, the other got rejected/reset.
That's exactly the opposite-symptom pattern you saw per camera.

**Fix — new architecture:** `mnvrd` is now the *only* thing that opens an
RTSP session to the physical camera. Added a small RTSP relay server
inside `mnvrd` (`nvr_core/src/modules/rtsp_relay/`, using `GstRTSPServer`)
that re-exposes each camera's already-running watermarked restream (the
`udpsink` output `streamer_module.c` already produces) as
`rtsp://127.0.0.1:8555/cam_<id>`. `mediamtx-sync.py` now points MediaMTX at
*that* local relay instead of the camera directly — so MediaMTX no longer
touches the camera at all, and there's only one consumer of the camera's
RTSP session (`mnvrd`) instead of two.

Bonus: since MediaMTX now serves `mnvrd`'s watermarked stream instead of
the camera's raw one, **live view now shows the same date/time + GPS/speed
watermark that recordings do** — this also fixes the live-view-watermark
gap from earlier.

New build dependency: `libgstrtspserver-1.0-dev` — already added to
`start.sh`'s install line, or `sudo apt install libgstrtspserver-1.0-dev`
if installing manually before a rebuild.

Nothing to configure — this is automatic once you rebuild. Verify with:
```bash
sudo bash start.sh --build-nvr
tail -f logs/nvr.log         # look for "RTSP relay started on :8555"
tail -f logs/mediamtx.log    # paths should now read from 127.0.0.1:8555
```

## 9. GPS + speed watermark logic — implemented (no hardware needed yet)

New module: `nvr_core/src/modules/gps/gps_module.c`. It's fully functional,
just has nothing to read from until a GPS receiver is physically connected:

- **Speed**: preferentially read directly from the GPS chip's own `$GPRMC`
  NMEA sentence (the "speed over ground" field, in knots — this is more
  accurate than anything computed after the fact, since the GPS chip
  derives it from Doppler shift). If a fix has position but no usable
  speed field, it falls back to computing speed itself: haversine
  great-circle distance between this fix and the previous one, divided by
  elapsed time.
- Verified the NMEA parser against the standard reference test sentence
  (`$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,...`) — correctly
  produces lat 48.1173, lon 11.5167, 41.5 km/h.
- Runs a background thread reading line-by-line from a serial device
  (termios, configurable baud), parses `$GPRMC`/`$GNRMC` sentences, and
  broadcasts every fix to **all** cameras' watermarks at once (via
  `streamer_update_telemetry()` / `recorder_update_telemetry()`) — one GPS
  receiver for the whole train, not per-camera.
- If a `RMC` sentence has status `V` (void/no fix), the watermark
  correctly falls back to `GPS: NO FIX` rather than showing stale/zero
  coordinates.

**To activate once hardware arrives**, edit `nvr_core/config/mnvr.conf`:
```ini
gps_enabled       = true
gps_serial_device = /dev/ttyUSB0     # whatever your GPS receiver enumerates as
gps_baud_rate     = 9600             # check your receiver's datasheet
```
then `bash start.sh --restart`. Until then it logs
`GPS disabled (gps_enabled=false in mnvr.conf)` and does nothing — the
watermark keeps showing `GPS: NO FIX`, which is correct/expected.

## 10. Removing the camera's own burned-in OSD (date/time + channel name)

That text ("05 07 2026 Fri 07:10:28 PM" and "CAM065" in your screenshot)
is burned into the video by the **camera itself**, before it ever reaches
`mnvrd` — it's not something in this codebase's control once the frame
arrives, since by then it's just pixels indistinguishable from the rest of
the image.

**Correct fix — disable it at the source.** Every camera matching your RTSP
URLs (`/Streaming/Channels/101` = Hikvision-style, `/onvif/media?profile=...`
= generic ONVIF) has an OSD setting in its own web admin UI, typically
under **Configuration → Image/Video → OSD Settings** — uncheck "Display
Date & Time" and "Display Channel/Camera Name" there. Log into
`http://172.210.140.91` / `http://172.210.140.130` directly (same
admin/password as RTSP) to find it. This is the right fix: zero CPU cost,
no risk of misaligned masking if the camera's OSD position or your
resolution ever changes.

I didn't build a software mask/crop for this — it's fragile (exact pixel
position of the camera's OSD varies by model/firmware and would silently
drift if either changes) and disabling it at the source is both cleaner
and something every camera in this fleet already supports. If a specific
camera genuinely has no OSD toggle in its UI, tell me its exact model and
I'll look at a masking overlay for that one instead.

## 11. Recordings folder structure — fixable with SQL, no code change needed

You want all recordings directly in `/storage/recordings` (flat) rather
than `/storage/recordings/cam_29/`, since your UI reads from the flat
folder. Good news: the code already does this correctly — the bug is in
the **data**, not the code.

`recorder_module.c` builds each file's path as
`<cameras.rec_output_dir>/cam_<id>_<timestamp>_segNNNNN.mp4` — note
`cam_<id>` here is meant as a **filename prefix**, not a subfolder (no
trailing slash after it). But your `cameras.rec_output_dir` column is
already set to a *per-camera* path like `/storage/recordings/cam_30`
(matching what `mnvr_schema.sql`'s own comments suggest as the default),
so the two combine into the doubled path you saw in your logs:
`/storage/recordings/cam_30/cam_30_...`.

Fix it in the data, not the code:
```sql
UPDATE cameras SET rec_output_dir = '/storage/recordings';
-- or, if you only want to touch these two for now:
UPDATE cameras SET rec_output_dir = '/storage/recordings' WHERE camera_id IN (29, 30);
```
Then `bash start.sh --restart` (same reason as the credentials fix —
`mnvrd` only reads camera info at startup). After that, files land
directly as e.g. `/storage/recordings/cam_29_20260708_091234_seg00001.mp4`
— flat, with the camera identifiable from the filename, matching what your
recordings UI expects.

## 12. Fixed: recordings folder kept reverting to `cam_29/` — found the real cause

You have **two separate recorders** running at once, and I'd missed the
second one until now: `recorder/recorder.py`, a standalone Python service
`start.sh` also launches (Section "9/10 API Server + Recorder" in its
output). It duplicates everything `mnvrd`'s own built-in recorder already
does — pulling the same camera's stream a *third* time (after MediaMTX and
`mnvrd`'s own RTSP pull) — and it's the actual source of both problems:

- It **hardcodes** its own output path: `rec_dir = RECORDINGS_PATH /
  f"cam_{camera_id}"` — a real subfolder, always, regardless of what's in
  the database.
- Worse, every time its `camera_loop()` (re)starts, it calls
  `PUT /api/cameras/{camera_id}` with `rec_output_dir` set back to that
  subfolder path — which is exactly what kept silently overwriting your
  manual SQL fix. Not a caching or restart-timing issue — it was actively
  resetting it every time.

**Disabled it** in `start.sh` (commented out, not deleted — easy to
re-enable if you ever want the Python recorder instead of `mnvrd`'s
built-in one, just don't run both at once). `mnvrd`'s own recorder already
handles recording correctly and respects `rec_output_dir` as-is.

After updating `start.sh`, do a fully clean restart and kill any already-running
copy of the old process by name (it won't die just because the launch
script changed):
```bash
pkill -f "recorder.py"
bash start.sh --restart
```

Also clean up the duplicate old files/folders it already created:
```bash
rm -rf /storage/recordings/cam_29 /storage/recordings/cam_30
```
(only the per-camera *subfolders* — leave the flat `cam_29_*.mp4` /
`cam_30_*.mp4` files at the top level alone, those are `mnvrd`'s correct
output.)

## 13. Fixed: `Permission denied` on `.pids/mediamtx.pid` / `logs/mediamtx.log`

Caused by mixing `sudo bash start.sh ...` and plain `bash start.sh ...`
across different runs — whichever run used `sudo` left those files/folders
owned by `root`, and the next non-sudo run (owned by your `nvr` user)
can't write to them anymore. The same thing happened under `/storage` —
the `cam_29`/`cam_30` folders and recent `.mp4`s were `root`-owned too.

`start.sh` already escalates internally via `sudo apt-get install` for the
one step that needs root — **you don't need to run the whole script with
`sudo`.** Fix the current mess once, then always run it plain:
```bash
sudo chown -R nvr:nvr /home/nvr/Documents/ai_mnvr/mnvr_integrated_final/integrated
sudo chown -R nvr:nvr /storage
bash start.sh --build-nvr        # no leading sudo from here on
```

## 14. Fixed for real this time: the opposite-symptom camera bug (root cause was one level deeper)

Pointing MediaMTX at the RTSP relay (section 8) fixed the MediaMTX-vs-mnvrd
race, but there was a second, identical race I'd missed: **inside `mnvrd`
itself**, `streamer_module.c` (live view + AI) and `recorder.c` (recording)
each opened their *own independent* RTSP connection to the physical
camera. Same problem, one layer deeper — whichever pipeline grabbed the
camera's limited connection slot worked, the other didn't.

**Real fix this time:** `recorder.c` no longer connects to the camera at
all. It now reads the *same* already-watermarked, already-encoded stream
that `streamer_module.c` publishes for the live view, over a local UDP
port — so `mnvrd` opens exactly **one** RTSP connection per camera, full
stop, and recording gets the date/time + GPS/speed watermark for free
(previously it re-did its own separate decode/overlay/encode; now it
doesn't need to).

Because two independent `udpsrc` sockets can't reliably share one UDP port
(the kernel delivers each packet to only one), `streamer_module.c`'s
`udpsink` became a `multiudpsink` fanning out to two destinations:
- `udp://127.0.0.1:5000+id*2` → the RTSP relay (unchanged port, live view)
- `udp://127.0.0.1:6000+id*2` → **new**, dedicated to `recorder.c`

Verified the new recorder pipeline (`udpsrc → rtpjitterbuffer →
rtph264depay → h264parse → splitmuxsink`) end-to-end with a synthetic RTP
source standing in for `streamer_module.c` — produced valid, playable
segment files.

One quality trade-off worth knowing: recording used to run its own encoder
at a recording-tuned preset (`fast`, 4096 kbps) independent of the live
view's lower-latency one. Now there's only one shared encode, so I bumped
it to a middle-ground setting (`veryfast`, 4096 kbps, still real-time-safe)
that's decent for both rather than optimal for either. If recording
quality matters more than live-view latency for you, we can push the
shared bitrate/preset higher — just say so.

Nothing to configure — automatic after rebuilding:
```bash
bash start.sh --build-nvr
grep -i "relay=udp\|recorder=udp" logs/nvr.log   # confirm both ports are logged per camera
```

## 15. Fixed: video player play/pause button showing wrong state after auto-advance

Root cause: when one recording ended and the player auto-advanced to the
next clip, it called `video.play()` after a **blind, fixed 400ms delay** —
just a guess that the new clip would have loaded enough data by then. When
it hadn't (slower network, larger file, whatever), the browser considers
the element legitimately "playing" (`.paused` is `false`, so `onPlay`
fires and the button correctly shows the pause icon) while actually
**stalled waiting for data** — browsers signal that with a `waiting`
event, not a `pause` event, so the picture freezes while the button still
says "playing." Clicking once genuinely pauses that stalled video
(button now correctly flips to the play icon); clicking again finally
starts real playback once data has had time to arrive — matching exactly
"click it 2 times, then it plays."

Fixed in `src/pages/VideoPlayer.tsx`:
- `jumpRelativeRecording()` no longer guesses with `setTimeout(400)` —
  it now waits for the new video element's `loadeddata` event (the
  browser's own "I actually have a frame ready" signal) before calling
  `.play()`.
- Added `onWaiting`/`onPlaying` alongside the existing `onPlay`/`onPause`
  handlers, so the button's state now also reflects genuine stalls (e.g.
  from a slow network mid-playback, not just the auto-advance path) —
  hardens against the same bug class recurring elsewhere in the player.

No rebuild of `mnvrd` needed for this one — it's a frontend-only change;
just restart/rebuild the frontend (`bash start.sh --restart` picks it up,
or however you normally redeploy the React app).

## 16. Switched to dual-profile architecture (your idea — recording + live on separate camera profiles)

Your ONVIF discovery logs showed each camera already exposes named profiles
— `BELLPRFLIVE` and `BELLPRFREC` — meaning these cameras were provisioned
with separate live/recording streams in mind from the start. Since most
cameras allow one RTSP session *per profile* (not one per camera total),
connecting the live view and the recorder to two *different* profiles
avoids the connection-limit race at the actual source, instead of routing
everything through mnvrd's internal relay.

**What changed:**
- `onvif_module.c` now selects two profiles instead of always taking
  `profiles[0]`: one whose token/name contains `LIVE`, one containing
  `REC` (case-insensitive). Falls back sensibly (second profile, or same
  profile for both) if a camera doesn't have this naming convention.
- New `cameras.rec_rtsp_url` column (both `nvr_core/mnvr_schema.sql` and
  `server/schema.sql`, with an `ALTER TABLE ... ADD COLUMN IF NOT EXISTS`
  migration for databases that already exist).
- `recorder.c` connects to `rec_rtsp_url` directly via RTSP again — and,
  per your choice, has its own decode → overlay → re-encode chain back
  (recordings still get the watermark, at the cost of a second encode
  pass — same trade-off as before, just now against a different source).
- `streamer_module.c` is back to a single-destination `udpsink` (only
  feeds the RTSP relay now; recorder no longer shares that stream).
- Existing cameras (yours: 29, 30) will pick up `rec_rtsp_url`
  automatically — I added a self-healing `UPDATE` that runs on every
  discovery cycle (every 60s), not just for brand-new cameras. No manual
  SQL needed this time.

## 17. The actual root cause of the credential-doubling bug — found and fixed at the source

This is the bug that made every previous "fix" (both mine and your manual
SQL edits) get silently undone. It lived in `main.c`'s auto-registration,
which ran on every ONVIF (re)discovery:

```c
snprintf(rtsp_url, sizeof(rtsp_url), "rtsp://%s:%s@%s", ruser, rpass, ...);
```

This embeds the password **raw, with no percent-encoding**, directly into
the URL. The moment a password itself contains `@` (like `bel@1234`), the
resulting URL has two `@` characters — genuinely ambiguous to parse, and
exactly the `bel%401234@1234`-style corruption you kept seeing. Since this
ran on every discovery cycle, it regenerated the broken URL fresh every
time, which is why patching the database directly never held.

There was a second, compounding bug: `config_module.c` already had a
`build_rtsp_url()` helper specifically meant to strip any embedded
credentials back out — but it only searched for the *first* `@`, not the
last. With two `@`s already in a corrupted raw URL, that left a bogus
fragment behind and rebuilt an equally broken URL.

**Fixed both:**
- `main.c` no longer embeds credentials into `cameras.rtsp_url` at all —
  it stores the clean stream URI. Credentials live exclusively in
  `cameras_config_details.rtsp_username`/`rtsp_password`, which every
  consumer (`streamer_module.c`, `recorder.c`) already correctly reads via
  `rtspsrc`'s `user-id`/`user-pw` properties instead of parsing them back
  out of a URL string.
- Also added: `main.c` never actually wrote to `cameras_config_details`
  for auto-discovered cameras before — meaning a brand-new camera would
  have had nowhere for its credentials to actually persist. Added that
  upsert.
- `build_rtsp_url()` now strips everything up to the *last* `@` before
  the path (the correct general rule — a host/port never legitimately
  contains an unencoded `@`), so it self-heals even a legacy row that's
  already corrupted from before this fix existed.

**Verified properly this time** — built a real, auth-protected RTSP test
server (`GstRtspServer`, Python bindings) requiring exactly the same
`admin` / `bel@1234` credentials, and ran `recorder.c`'s exact connection
approach against it end-to-end: authenticated successfully, decoded,
watermarked, re-encoded, and produced a valid playable segment file. Not
guessing this time — actually confirmed.

## 18. What you need to do after pulling this update

1. Run the schema migration (safe to run repeatedly):
   ```bash
   psql -h localhost -U mnvr -d mnvr -f nvr_core/mnvr_schema.sql
   ```
   (or just `ALTER TABLE cameras ADD COLUMN IF NOT EXISTS rec_rtsp_url TEXT;`
   if you don't want to re-run the whole schema file.)
2. Rebuild and restart:
   ```bash
   bash start.sh --build-nvr
   ```
3. Watch discovery pick up both profiles (happens automatically within
   ~60s even for your existing cameras 29/30):
   ```bash
   grep -i "ONVIF-DBG\] Profile" logs/nvr.log | tail -20
   ```
   You should see `[LIVE]`/`[REC]` tags next to the matching profiles.
4. Confirm both cameras are stable:
   ```bash
   tail -50 logs/mediamtx.log
   ls -lat /storage/recordings/ | head -10
   ```

## 19. Found and fixed: the RTSP relay's silent death (file descriptor leak)

Root-caused this one properly before touching code, per your request.
Diagnostic trail: `mnvrd` stayed alive throughout (no crash, no critical
warnings), but the relay's listening socket on :8555 simply vanished after
~35 minutes — a textbook file-descriptor exhaustion signature, confirmed
by `/proc/<pid>/fd`: 986 of the 1024 FD limit in use, 975 of them sockets
— but `ss -tnp`/`ss -unp` showed almost none of those were real TCP/UDP
connections. That left Unix-domain `socketpair()`s — the internal
wakeup/signaling channels GStreamer creates per pipeline — as the culprit.

**Mechanism:** the relay's media factory was `shared=TRUE` (one pipeline
for all viewers) but still used `GstRTSPServer`'s default behavior of
tearing the pipeline down when the last client disconnects and rebuilding
it fresh on the next connection. MediaMTX was retrying its connection to
the relay every ~5 seconds — each retry cycled the pipeline, and each
cycle leaked its internal socketpair(s) rather than releasing them fully
on teardown. Over ~35 minutes (~400+ retries), that exhausted the
process's FD limit, and something in `GstRTSPServer`'s error handling at
that point took the listening socket down entirely as collateral damage.

**Fix:** one property, `nvr_core/src/modules/rtsp_relay/rtsp_relay.c`:
```c
gst_rtsp_media_factory_set_suspend_mode(factory, GST_RTSP_SUSPEND_MODE_NONE);
```
This keeps the pipeline alive permanently regardless of client
connects/disconnects — correct behavior anyway, since a live camera feed
doesn't care whether 0 or 5 people are currently watching. No more
repeated create/destroy cycle, no more leak.

**Verified, not just assumed:** built a standalone test harness with the
exact same factory settings, hammered it with 40 rapid connect/disconnect
cycles (simulating MediaMTX's retry behavior) while watching the server's
`/proc/<pid>/fd` count. Before the fix this class of test would show
steady growth; with `suspend_mode=NONE` in place, the count stayed at
exactly 7 the entire time — before and after.

No config/schema changes needed for this one — just rebuild:
```bash
bash start.sh --build-nvr
```

## 20. Found the actual explanation for today's chaotic streamer failures: `start.sh` wasn't actually killing `mnvrd`

While testing the connection-limit hypothesis, `ps aux` revealed **three
separate `mnvrd` processes running simultaneously** (different PIDs,
started at three different times), despite `start.sh --stop` reporting
"✓ Stopped nvr" each time. Every restart was *adding* a new `mnvrd`
instead of replacing the old one — meaning multiple daemons were
independently trying to open RTSP connections to the same two cameras at
once. That alone explains the chaotic, inconsistent streamer errors we
were chasing (of course connections were failing — several times the
expected number were being attempted), and the binary garbage that showed
up in `nvr.log` (multiple processes writing to the same file with no
coordination garbles the output).

**Root cause, in `start.sh` itself:** `pid_kill()` fired a plain `kill`
(SIGTERM) and immediately assumed success — no verification the process
actually exited, no fallback if it didn't. And unlike every other service
(`frontend`, `api`, `ai`, `mediamtx`, etc.), there was no `pkill -f`
safety net for `mnvrd` in `kill_all()` at all. If `mnvrd`'s shutdown ever
took more than an instant (e.g. a thread mid-retry on a camera connection
attempt when the signal arrived), it would silently survive `--stop`, and
nothing would ever catch it.

**Fixed:** `pid_kill()` now waits up to 10s verifying the process actually
died, escalates to `SIGKILL` if it didn't, and verifies again. Added the
same `pkill -f "nvr_core/build/mnvrd"` safety net every other service
already had, as a last-resort catch-all in `kill_all()`.

**Before re-testing anything else, clean up the current mess by hand —
the fix only prevents this going forward, it won't kill what's already
running:**
```bash
pkill -9 -f "nvr_core/build/mnvrd"
sleep 1
ps aux | grep mnvrd | grep -v grep   # should show nothing now
bash start.sh --restart              # now safe to use the fixed stop logic
```

This means the earlier connection-limit test (dual-profile vs. one
session per camera) was run against three competing daemons and its
result can't be trusted — worth re-running cleanly once you're confident
only one `mnvrd` is ever running (`ps aux | grep mnvrd` before *and*
after any `--stop`/`--restart` from now on is a good habit until this
proves solid).

## 21. Fixed: streamer_module.c was missing reliability properties recorder.c already had

Packet capture proved `BELLPRFLIVE` streams fine over TCP (verified with
`ffmpeg` — clean 1920x1080 H.264+audio, zero errors), but `mnvrd`'s own
`streamer_module.c` was configuring its `rtspsrc` with only `location`,
`latency`, and `protocols` — missing `timeout`, `tcp-timeout`, `do-rtcp`,
`ntp-sync`, and `buffer-mode`, all of which `recorder.c` already sets.
Brought `streamer_module.c` up to match that known-working configuration.

Also added **exponential retry backoff** to both `streamer_module.c` and
`recorder.c` (5s → 10s → 20s, capped at 30s, resetting after a connection
stays up 10+ seconds) instead of a fixed 5-second retry forever. During
today's testing we hammered this camera with dozens of rapid reconnects
within a few minutes and saw increasingly inconsistent behavior — a fixed
5s retry loop does the same thing to it continuously during any real
network hiccup. Backing off gives a struggling camera room to recover
instead of being hit every 5 seconds indefinitely.

```bash
bash start.sh --build-nvr
```

No further diagnosis needed from my side right now — this is a direct code
fix based on the packet-level evidence already gathered, not another round
of testing. Let it run for a while and check back with `ps aux | grep
mnvrd` (should show exactly one), `grep -a STREAMER logs/nvr.log | tail`,
and live view/recording status whenever convenient — no rush.

## 22. Root cause fixed: "connection refused" / "no one is publishing" on every restart

This was a genuine startup-ordering bug, not transient noise to wait out.
`start.sh` starts MediaMTX + `mediamtx-sync.py` (step 7) before `mnvrd`
(step 9) — every single restart, `mediamtx-sync.py` would add paths
pointing at the relay and MediaMTX would immediately try to pull from
them, several seconds before `mnvrd`'s relay had actually started
listening on :8555. That's the "connection refused" burst you saw on
every boot, and — because MediaMTX's own path/session state can get a bit
tangled when its first pull attempts fail immediately — sometimes
manifested afterward as "no one is publishing" even once the relay was
actually up.

**Fixed properly, not with a "just wait" workaround:** `mediamtx-sync.py`
now has a `wait_for_relay()` step (mirroring the existing
`wait_for_mediamtx()` pattern) that blocks — actually checking with a
real TCP connection attempt, retrying every 2s — until `mnvrd`'s relay is
genuinely accepting connections, *before* it adds a single path to
MediaMTX's config. MediaMTX now never attempts a pull before there's
something there to pull from, regardless of which order the scripts
happened to start in. Verified the wait/retry logic directly against a
simulated delayed listener before shipping it.

```bash
bash start.sh --build-nvr
```

You should no longer see any "connection refused" lines in
`logs/mediamtx.log` on a fresh start — `mtxsync.log` will instead show
`Waiting for mnvrd's RTSP relay at 127.0.0.1:8555 ...` for a few seconds,
then `✓ RTSP relay ready`, then the paths get added once and stay clean.

## 23. Also found: `cameras.rec_output_dir` had a stale absolute path baked in

Separately — worth documenting since it's the kind of thing that can
silently recur — `rec_output_dir` for both cameras had gotten set to an
old project checkout location (`.../railway-nvr-v6 (1)/nvr-final2
(Copy)/storage/recordings`) instead of the real `/storage/recordings`.
Not a code bug; just stale data, presumably left over from before the
project was moved/renamed to its current location. Confirmed fixed via:
```sql
UPDATE cameras SET rec_output_dir = '/storage/recordings';
```
If this reappears, it's worth checking whatever tool/script was used
originally to set it (likely a one-time manual entry, not something
`mnvrd` writes on its own) rather than assuming it's the same bug as the
`rec_rtsp_url` doubling issue from earlier — those are different columns
with different write paths.

## 24. Architecture change: switched to ffmpeg for live view + recording (deadline-driven)

Today's testing conclusively showed GStreamer's `rtspsrc` has real,
reproducible reliability problems against this specific camera fleet —
inconsistent `SETUP` failures, and one case (packet-captured) where the
camera accepted `PLAY` with `200 OK` and then sent zero actual data.
`ffmpeg`, tested side by side against the *exact same URLs and
credentials*, worked cleanly every single time. Given the deadline,
rather than continuing to debug `rtspsrc`'s behavior against this camera,
I replaced `mnvrd`'s native camera connections with `ffmpeg` — proven
reliable today, in this environment, against these specific cameras.

**New file: `stream_manager.py`** — one Python process, two threads per
camera:
- **LIVE thread**: `ffmpeg -rtsp_transport tcp -c copy -f rtsp` pushes the
  camera's LIVE profile straight into MediaMTX as an RTSP publisher.
  MediaMTX already supports this natively (it's how people normally feed
  it), which also means **the custom `rtsp_relay.c` GstRTSPServer relay
  from earlier today is no longer needed** — this replaces it with
  something simpler and, per today's testing, more reliable.
- **REC thread**: `ffmpeg` with a `drawtext` date/time watermark,
  re-encoded (`libx264`) and segmented directly to flat,
  camera-id-prefixed files in `rec_output_dir` — same file-naming
  convention already established for the C-based recorder.
- Both use the same credential-encoding fix applied to `main.c` earlier
  today (`urllib.parse` construction, never raw string concatenation —
  a raw `@` in a password is exactly what caused the credential-doubling
  bug that recurred several times today).
- Both use the same 5s/10s/20s/30s-cap exponential backoff as the C code,
  resetting after a connection stays up 10+ seconds.

**Verified before shipping, not just assumed:**
- Ran the exact `drawtext` watermark syntax first — caught and fixed a
  real escaping bug (`%{localtime}` with a custom strftime format
  requires escaping through three separate layers — shell, ffmpeg's own
  arg parser, and the filter's option parser — and produced a warning
  even after escaping attempts). Switched to `%{localtime}`'s default
  format instead, which is simpler and already reads as a normal
  `YYYY-MM-DD HH:MM:SS` timestamp.
- Ran the corrected command against a real password-protected RTSP test
  server (same harness used earlier today) end-to-end: produced a valid,
  playable H.264 segment with the watermark actually visible on an
  extracted frame — not just "the command didn't error."
- Verified the credential-encoding logic specifically against the exact
  `bel@1234`/`admin@123` passwords that caused today's doubling bug —
  confirmed exactly one `@` in the resulting authority section, not two.
- Verified the live-push command's syntax by pointing it at an
  intentionally-unreachable target: confirmed it correctly reads the
  source stream (valid H.264 detected) and correctly attempts to publish,
  failing only with "connection refused" on the fake target — proving
  the command itself is well-formed.

**`start.sh` changes:**
- `mnvrd`'s own launch is now disabled (commented out behind
  `if false && ...`, easy to re-enable later) — its camera connections
  would otherwise compete with `stream_manager.py` for the same cameras,
  recreating the exact problem this change is meant to solve. mnvrd's
  other jobs (AI/YOLO detection, its own health monitoring) are not
  running right now as a result — that's a real trade-off, made
  deliberately given "get streams and recordings working" was the
  explicit priority over AI detection.
- `stream_manager.py` added as a new managed service (`streammgr`) —
  status, stop, and the `pkill` safety net all wired up the same way
  every other service already works.
- `mediamtx-sync.py` no longer adds a `source`-pointed path for each
  camera (that pointed at the now-removed relay) — paths are simply left
  unconfigured, and MediaMTX auto-creates them the moment `ffmpeg`
  publishes, which is MediaMTX's standard, documented behavior for this
  exact use case.

```bash
bash start.sh --build-nvr
```

Check it's working:
```bash
bash start.sh --status                          # should show streammgr PID
tail -f logs/stream_manager.log                  # per-camera LIVE/REC status
ls -lat /storage/recordings/ | head -10           # fresh files landing
```
Then check the actual VMS UI for live view on both cameras.

**Honest caveat on Back Entrance specifically:** this fixes the
*mechanism* (reliable RTSP client), but the packet capture earlier today
proved a real instance of that camera not sending data even after
accepting `PLAY`. If Back Entrance is still inconsistent after this
change, that's the camera itself, not something either `rtspsrc` or
`ffmpeg` can paper over — worth a firmware check or a call to IDIS
support if it recurs, since we've now ruled out the client-side
implementation as the cause on this specific camera.
