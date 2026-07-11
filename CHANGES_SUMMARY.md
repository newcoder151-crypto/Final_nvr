# mNVR — ONVIF Profile S + Timeline Integration (merged into your latest build)

This package takes the codebase you uploaded (which already had your Settings
page rebuild, the NVR core fixes, and the `start.sh`/port fixes from earlier
sessions) and merges in the two pieces of work that were missing from it:
**ONVIF Profile S controls** and the **timeline date-picker / red event
markers**. Nothing from your uploaded version was removed or reverted.

## Why you couldn't see ONVIF settings / streams / image controls

Two separate things were going on:

1. **The ONVIF Profile S backend+frontend code had never been merged into
   this version of the app.** The "ONVIF Discovery Settings" section you
   *could* see in Settings only configures camera **discovery** (multicast
   probing) — it was never meant to expose per-camera multi-stream selection,
   PTZ, image, or audio controls. Those needed a whole new route, service,
   hook, and UI component, which is what's added below.

2. **The ONVIF Discovery section itself defaulted to collapsed** on page
   load (`onvif: false` in the section-open state), so even the discovery
   settings that did exist were easy to miss. Fixed — it (and the camera
   credentials section) now default to **open**.

## What was added

### Database (`nvr_core/mnvr_schema.sql`, `server/schema.sql`, both updated
identically + standalone `server/migrations/001_onvif_profile_s.sql` for
applying to an already-running DB)
- `camera_media_profiles` — cached ONVIF stream profiles per camera
  (MainStream/SubStream/etc.), each with resolution/fps/bitrate/codec and a
  `is_active_stream` flag
- `camera_ptz_presets`, `camera_ptz_status` — preset storage + live position
- `camera_image_settings` — brightness/contrast/saturation/sharpness + IR/WDR/
  exposure, with min/max ranges for slider bounds
- `camera_audio_settings` — encoding/bitrate/sample-rate/gain
- `camera_onvif_capabilities` — per-camera capability snapshot (drives which
  UI tabs are enabled)

### Backend
- `server/src/services/onvifClient.js` — new ONVIF SOAP client (using the
  `onvif` npm package, added to `server/package.json`), with a 5-minute
  connection cache and credential resolution that follows the exact same
  `cameras_config_details` join pattern your `routes/settings.js` already
  uses
- `server/src/routes/onvif.js` — new routes registered at `/api/onvif`:
  capabilities, multi-stream profile list + activation, full PTZ (move/stop/
  home/presets), image settings, audio settings
- `server/src/index.js` — registered the new route

### Frontend
- `src/hooks/use-onvif.ts` — React Query hooks for every endpoint above
- `src/components/camera/CameraControlPanel.tsx` — new tabbed panel:
  **Streams** (pick which ONVIF profile feeds the live view), **PTZ**
  (joystick + presets), **Image** (brightness/contrast/saturation/sharpness
  sliders, IR cut filter, WDR, exposure mode), **Audio** (encoding/bitrate/
  sample rate)
- **`src/pages/CameraGrid.tsx`** — this is the page you actually use to view
  live cameras, and where the controls are now wired in: expand any camera
  tile, then click the new **"Controls"** button next to the AI toggle in
  the expanded view's header. The panel appears below the video.
- `src/components/camera/CameraZoomDialog.tsx` — also wired with the same
  panel (this component exists in your codebase but isn't currently routed
  to anywhere; wired anyway so it's ready if you use it later)
- `src/pages/SettingsPage.tsx`:
  - ONVIF Discovery and Camera Credentials sections now **default to open**
  - Added a note inside the ONVIF Discovery card clarifying that per-camera
    stream/PTZ/image/audio controls live in Camera Grid, with a direct
    "Open Camera Grid" button

### Timeline scrubber (`src/pages/VideoPlayer.tsx`)
- **Date picker** — the date label in the timeline header is now a button
  that opens a calendar popover; click any date to jump straight to it
- **Red event markers** — events for the visible day are drawn as red ticks
  directly on the scrubber at their exact timestamp; clicking one jumps to
  the recording covering that moment and seeks to the exact second

## Where to find the controls now

1. Go to **Camera Grid** (`/cameras`)
2. Click the expand icon on any camera tile (top-right on hover)
3. In the expanded view, click **"Controls"** (next to the AI button)
4. Four tabs appear: **Streams**, **PTZ**, **Image**, **Audio**
   - PTZ and Audio tabs auto-disable if the camera doesn't advertise that
     ONVIF service (checked live via `/api/onvif/cameras/:id/capabilities`)

## Before running

- `cd server && npm install` — pulls in the new `onvif` package
- If you already have a running Postgres instance, run
  `server/migrations/001_onvif_profile_s.sql` once against it (it's
  idempotent — safe to run even if some tables already exist)
- A fresh `nvr_core/mnvr_schema.sql` / `server/schema.sql` already includes
  everything, so a clean install needs no extra step

## Everything from your uploaded version is preserved

- Settings page (system config, health, recording, HLS, ONVIF discovery,
  camera credentials) — untouched except the two default-open fixes above
- NVR core fixes (credential separation, retry loops, port moves) — untouched
- `start.sh` fixes (absolute `NVR_BIN`/`NVR_CONF`/`STORAGE` paths) — untouched
- `settings.js` route — untouched
