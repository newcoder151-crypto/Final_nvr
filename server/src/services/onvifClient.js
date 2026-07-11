/**
 * ONVIF Profile S client service
 * -------------------------------------------------------------------------
 * Wraps the `onvif` npm package with:
 *   - Promise-based helpers (the package is callback-based)
 *   - A short-lived connection cache per camera (avoid re-handshaking
 *     on every single request within a burst, e.g. PTZ joystick drag)
 *   - DB-driven credential lookup from cameras_config_details / cameras
 *
 * This module talks to cameras directly over ONVIF SOAP — it does NOT
 * go through the NVR C core. The C core's own onvif_module.c performs
 * discovery + recording-relevant queries; this module is for the
 * interactive control surface (PTZ, image settings, stream selection,
 * audio settings) driven by the web UI.
 */

const { Cam } = require('onvif');
const { query, queryOne } = require('../db');

/** camera_id -> { cam: Cam instance, lastUsed: epoch ms } */
const connectionCache = new Map();
const CACHE_TTL_MS = 5 * 60 * 1000; // 5 minutes idle timeout

function pruneCache() {
  const now = Date.now();
  for (const [id, entry] of connectionCache) {
    if (now - entry.lastUsed > CACHE_TTL_MS) connectionCache.delete(id);
  }
}
setInterval(pruneCache, 60 * 1000).unref?.();

/**
 * Resolve ONVIF connection parameters for a camera: prefer
 * cameras_config_details (onvif_username/onvif_password/onvif_port),
 * fall back to the cameras table's generic username/password.
 * Matches the same join convention used in routes/settings.js.
 */
async function resolveOnvifTarget(cameraId) {
  const cam = await queryOne(
    `SELECT c.camera_id, c.ip_address, c.username AS cam_username,
            c.password_hash AS cam_password,
            cd.onvif_port, cd.onvif_username, cd.onvif_password
     FROM cameras c
     LEFT JOIN cameras_config_details cd ON host(cd.ip_address) = c.ip_address
     WHERE c.camera_id = $1`,
    [cameraId]
  );
  if (!cam) throw new Error(`Camera ${cameraId} not found`);

  return {
    cameraId,
    hostname: cam.ip_address,
    port: cam.onvif_port || 80,
    username: cam.onvif_username || cam.cam_username || 'admin',
    password: cam.onvif_password || cam.cam_password || '',
  };
}

/** Get (or create + cache) a connected Cam instance for a camera. */
function getCam(target) {
  return new Promise((resolve, reject) => {
    const cached = connectionCache.get(target.cameraId);
    if (cached) {
      cached.lastUsed = Date.now();
      return resolve(cached.cam);
    }
    const cam = new Cam(
      {
        hostname: target.hostname,
        username: target.username,
        password: target.password,
        port: target.port,
        timeout: 8000,
      },
      (err) => {
        if (err) return reject(new Error(`ONVIF connect failed (${target.hostname}:${target.port}): ${err.message}`));
        connectionCache.set(target.cameraId, { cam, lastUsed: Date.now() });
        resolve(cam);
      }
    );
  });
}

async function connectCamera(cameraId) {
  const target = await resolveOnvifTarget(cameraId);
  const cam = await getCam(target);
  return cam;
}

/** Drop a cached connection (e.g. after a connection error) so the next
 * call re-handshakes instead of reusing a dead session. */
function invalidate(cameraId) {
  connectionCache.delete(cameraId);
}

/* ---------------------------------------------------------------------- */
/* Promise wrappers around the callback-based onvif package               */
/* ---------------------------------------------------------------------- */

function p(fn, cam, ...args) {
  return new Promise((resolve, reject) => {
    fn.call(cam, ...args, (err, result) => (err ? reject(err) : resolve(result)));
  });
}

async function getCapabilities(cameraId) {
  const cam = await connectCamera(cameraId);
  const caps = await p(cam.getCapabilities, cam);
  return {
    supports_media: !!caps.media,
    supports_ptz: !!caps.PTZ,
    supports_imaging: !!caps.imaging,
    supports_events: !!caps.events,
    device_service_url: cam.deviceServiceAddress?.href || cam.deviceServiceAddress,
    media_service_url: cam.mediaServiceAddress?.href || cam.mediaServiceAddress,
    ptz_service_url: cam.ptzServiceAddress?.href || cam.ptzServiceAddress,
    imaging_service_url: cam.imagingServiceAddress?.href || cam.imagingServiceAddress,
  };
}

async function getProfiles(cameraId) {
  const cam = await connectCamera(cameraId);
  const profiles = await p(cam.getProfiles, cam);
  return profiles.map((prof) => ({
    token: prof.$ ? prof.$.token : prof.token,
    name: prof.name,
    video_encoder: prof.videoEncoderConfiguration
      ? {
          token: prof.videoEncoderConfiguration.$ ? prof.videoEncoderConfiguration.$.token : undefined,
          encoding: prof.videoEncoderConfiguration.encoding,
          resolution: prof.videoEncoderConfiguration.resolution,
          quality: prof.videoEncoderConfiguration.quality,
          frame_rate_limit: prof.videoEncoderConfiguration.rateControl?.frameRateLimit,
          bitrate_limit: prof.videoEncoderConfiguration.rateControl?.bitrateLimit,
          gov_length: prof.videoEncoderConfiguration.h264?.govLength,
        }
      : null,
    audio_encoder: prof.audioEncoderConfiguration
      ? {
          token: prof.audioEncoderConfiguration.$ ? prof.audioEncoderConfiguration.$.token : undefined,
          encoding: prof.audioEncoderConfiguration.encoding,
          bitrate: prof.audioEncoderConfiguration.bitrate,
          sample_rate: prof.audioEncoderConfiguration.sampleRate,
        }
      : null,
    ptz_configuration: prof.PTZConfiguration || null,
    video_source_token: prof.videoSourceConfiguration?.sourceToken,
  }));
}

async function getStreamUri(cameraId, profileToken, protocol = 'RTSP') {
  const cam = await connectCamera(cameraId);
  const result = await p(cam.getStreamUri, cam, { protocol, profileToken });
  return result.uri;
}

async function getSnapshotUri(cameraId, profileToken) {
  const cam = await connectCamera(cameraId);
  const result = await p(cam.getSnapshotUri, cam, { profileToken });
  return result.uri;
}

async function getVideoEncoderConfigurations(cameraId) {
  const cam = await connectCamera(cameraId);
  return p(cam.getVideoEncoderConfigurations, cam);
}

async function setVideoEncoderConfiguration(cameraId, config) {
  const cam = await connectCamera(cameraId);
  return p(cam.setVideoEncoderConfiguration, cam, config);
}

/* ---- PTZ ---- */

async function ptzContinuousMove(cameraId, { x = 0, y = 0, zoom = 0, timeout } = {}) {
  const cam = await connectCamera(cameraId);
  return p(cam.continuousMove, cam, { x, y, zoom, timeout });
}

async function ptzRelativeMove(cameraId, { x = 0, y = 0, zoom = 0, speed } = {}) {
  const cam = await connectCamera(cameraId);
  return p(cam.relativeMove, cam, { x, y, zoom, speed });
}

async function ptzAbsoluteMove(cameraId, { x = 0, y = 0, zoom = 0, speed } = {}) {
  const cam = await connectCamera(cameraId);
  return p(cam.absoluteMove, cam, { x, y, zoom, speed });
}

async function ptzStop(cameraId, { panTilt = true, zoom = true } = {}) {
  const cam = await connectCamera(cameraId);
  return p(cam.stop, cam, { panTilt, zoom });
}

async function ptzGetStatus(cameraId) {
  const cam = await connectCamera(cameraId);
  const status = await p(cam.getStatus, cam, {});
  return {
    pan: status.position?.panTilt?.x,
    tilt: status.position?.panTilt?.y,
    zoom: status.position?.zoom?.x,
    move_status: status.moveStatus?.panTilt || status.moveStatus,
  };
}

async function ptzGetPresets(cameraId) {
  const cam = await connectCamera(cameraId);
  const presets = await p(cam.getPresets, cam, {});
  // onvif lib stores `cam.presets` as a token->preset map after this call
  return Object.entries(cam.presets || {}).map(([token, preset]) => ({
    token,
    name: preset.name || token,
    position: preset.ptzPosition || null,
  }));
}

async function ptzGotoPreset(cameraId, presetToken, speed) {
  const cam = await connectCamera(cameraId);
  return p(cam.gotoPreset, cam, { preset: presetToken, speed });
}

async function ptzSetPreset(cameraId, presetName, presetToken) {
  const cam = await connectCamera(cameraId);
  const result = await p(cam.setPreset, cam, { presetName, presetToken });
  return result; // contains the new/updated PresetToken
}

async function ptzRemovePreset(cameraId, presetToken) {
  const cam = await connectCamera(cameraId);
  return p(cam.removePreset, cam, { preset: presetToken });
}

async function ptzGotoHome(cameraId, speed) {
  const cam = await connectCamera(cameraId);
  return p(cam.gotoHomePosition, cam, { speed });
}

async function ptzSetHome(cameraId) {
  const cam = await connectCamera(cameraId);
  return p(cam.setHomePosition, cam, {});
}

async function ptzGetConfigurationOptions(cameraId, configurationToken) {
  const cam = await connectCamera(cameraId);
  return p(cam.getConfigurationOptions, cam, configurationToken);
}

/* ---- Imaging ---- */

async function getImagingSettings(cameraId) {
  const cam = await connectCamera(cameraId);
  return p(cam.getImagingSettings, cam, {});
}

async function setImagingSettings(cameraId, settings) {
  const cam = await connectCamera(cameraId);
  return p(cam.setImagingSettings, cam, settings);
}

async function getImagingOptions(cameraId) {
  const cam = await connectCamera(cameraId);
  // Not all devices implement this; callers should handle rejection gracefully
  return p(cam.getVideoSourceOptions ? cam.getVideoSourceOptions : cam.getOptions, cam, {});
}

/* ---- Audio ---- */

async function getAudioSources(cameraId) {
  const cam = await connectCamera(cameraId);
  return p(cam.getAudioSources, cam);
}

async function getAudioEncoderConfigurations(cameraId) {
  const cam = await connectCamera(cameraId);
  return p(cam.getAudioEncoderConfigurations, cam);
}

async function setAudioEncoderConfiguration(cameraId, config) {
  const cam = await connectCamera(cameraId);
  return p(cam.setAudioEncoderConfiguration, cam, config);
}

module.exports = {
  resolveOnvifTarget,
  connectCamera,
  invalidate,
  getCapabilities,
  getProfiles,
  getStreamUri,
  getSnapshotUri,
  getVideoEncoderConfigurations,
  setVideoEncoderConfiguration,
  ptzContinuousMove,
  ptzRelativeMove,
  ptzAbsoluteMove,
  ptzStop,
  ptzGetStatus,
  ptzGetPresets,
  ptzGotoPreset,
  ptzSetPreset,
  ptzRemovePreset,
  ptzGotoHome,
  ptzSetHome,
  ptzGetConfigurationOptions,
  getImagingSettings,
  setImagingSettings,
  getImagingOptions,
  getAudioSources,
  getAudioEncoderConfigurations,
  setAudioEncoderConfiguration,
};
