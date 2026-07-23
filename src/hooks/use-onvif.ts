import { useQuery, useMutation, useQueryClient } from "@tanstack/react-query";
import { apiGet, apiPost, apiPut, apiDelete } from "@/lib/api";

/* ---------------------------------------------------------------------- */
/* Types                                                                   */
/* ---------------------------------------------------------------------- */

export interface OnvifCapabilities {
  supports_media: boolean;
  supports_ptz: boolean;
  supports_imaging: boolean;
  supports_audio?: boolean;
  supports_events?: boolean;
  device_service_url?: string;
  media_service_url?: string;
  ptz_service_url?: string;
  imaging_service_url?: string;
}

export interface MediaProfile {
  id: number;
  camera_id: number;
  profile_token: string;
  profile_name: string | null;
  encoding: string | null;
  resolution_width: number | null;
  resolution_height: number | null;
  fps: number | null;
  bitrate_kbps: number | null;
  gov_length: number | null;
  quality: number | null;
  stream_uri: string | null;
  snapshot_uri: string | null;
  is_active_stream: boolean;
  video_source_token: string | null;
  audio_encoder_token: string | null;
}

export interface PtzStatus {
  pan: number | null;
  tilt: number | null;
  zoom: number | null;
  move_status: string | null;
}

export interface PtzPreset {
  id: number;
  camera_id: number;
  preset_token: string;
  preset_name: string;
  pan: number | null;
  tilt: number | null;
  zoom: number | null;
  is_home: boolean;
}

export interface ImageSettings {
  camera_id: number;
  brightness: number | null;
  contrast: number | null;
  color_saturation: number | null;
  sharpness: number | null;
  ir_cut_filter: string | null;
  wide_dynamic_range: boolean | null;
  exposure_mode: string | null;
  brightness_min: number; brightness_max: number;
  contrast_min: number; contrast_max: number;
  saturation_min: number; saturation_max: number;
  sharpness_min: number; sharpness_max: number;
}

export interface AudioSettings {
  camera_id: number;
  audio_encoder_token: string | null;
  encoding: string | null;
  bitrate_kbps: number | null;
  sample_rate_khz: number | null;
  input_gain_db: number | null;
  is_enabled: boolean | null;
}

/* ---------------------------------------------------------------------- */
/* Capabilities                                                            */
/* ---------------------------------------------------------------------- */

export function useOnvifCapabilities(cameraId: number | undefined) {
  return useQuery<OnvifCapabilities>({
    queryKey: ["onvif-capabilities", cameraId],
    enabled: !!cameraId,
    queryFn: () => apiGet(`/api/onvif/cameras/${cameraId}/capabilities`),
    retry: false,
    staleTime: 5 * 60 * 1000,
  });
}

/* ---------------------------------------------------------------------- */
/* Media profiles / multi-stream selection                                 */
/* ---------------------------------------------------------------------- */

export function useMediaProfiles(cameraId: number | undefined) {
  return useQuery<{ profiles: MediaProfile[] }>({
    queryKey: ["onvif-profiles", cameraId],
    enabled: !!cameraId,
    queryFn: () => apiGet(`/api/onvif/cameras/${cameraId}/profiles`),
    retry: false,
  });
}

export function useActivateStreamProfile(cameraId: number | undefined) {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (token: string) =>
      apiPost(`/api/onvif/cameras/${cameraId}/profiles/${token}/activate`, {}),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ["onvif-profiles", cameraId] });
      qc.invalidateQueries({ queryKey: ["cameras"] });
    },
  });
}

/* ---------------------------------------------------------------------- */
/* PTZ                                                                      */
/* ---------------------------------------------------------------------- */

export function usePtzStatus(cameraId: number | undefined, enabled = true) {
  return useQuery<PtzStatus>({
    queryKey: ["ptz-status", cameraId],
    enabled: !!cameraId && enabled,
    queryFn: () => apiGet(`/api/onvif/cameras/${cameraId}/ptz/status`),
    refetchInterval: enabled ? 2000 : false,
    retry: false,
  });
}

export function usePtzMove(cameraId: number | undefined) {
  return useMutation({
    mutationFn: (body: { mode: "continuous" | "relative" | "absolute"; x?: number; y?: number; zoom?: number; speed?: number; timeout?: number }) =>
      apiPost(`/api/onvif/cameras/${cameraId}/ptz/move`, body),
  });
}

export function usePtzStop(cameraId: number | undefined) {
  return useMutation({
    mutationFn: () => apiPost(`/api/onvif/cameras/${cameraId}/ptz/stop`, {}),
  });
}

export function usePtzHome(cameraId: number | undefined) {
  return useMutation({
    mutationFn: (action: "goto" | "set" = "goto") =>
      apiPost(`/api/onvif/cameras/${cameraId}/ptz/home`, { action }),
  });
}

export function usePtzPresets(cameraId: number | undefined) {
  return useQuery<{ presets: PtzPreset[]; live: boolean }>({
    queryKey: ["ptz-presets", cameraId],
    enabled: !!cameraId,
    queryFn: () => apiGet(`/api/onvif/cameras/${cameraId}/ptz/presets`),
    retry: false,
  });
}

export function useCreatePtzPreset(cameraId: number | undefined) {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (name: string) => apiPost(`/api/onvif/cameras/${cameraId}/ptz/presets`, { name }),
    onSuccess: () => qc.invalidateQueries({ queryKey: ["ptz-presets", cameraId] }),
  });
}

export function useGotoPtzPreset(cameraId: number | undefined) {
  return useMutation({
    mutationFn: (token: string) => apiPost(`/api/onvif/cameras/${cameraId}/ptz/presets/${token}/goto`, {}),
  });
}

export function useDeletePtzPreset(cameraId: number | undefined) {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (token: string) => apiDelete(`/api/onvif/cameras/${cameraId}/ptz/presets/${token}`),
    onSuccess: () => qc.invalidateQueries({ queryKey: ["ptz-presets", cameraId] }),
  });
}

/* ---------------------------------------------------------------------- */
/* Image settings                                                           */
/* ---------------------------------------------------------------------- */

export function useImageSettings(cameraId: number | undefined) {
  return useQuery<ImageSettings>({
    queryKey: ["image-settings", cameraId],
    enabled: !!cameraId,
    queryFn: () => apiGet(`/api/onvif/cameras/${cameraId}/image`),
    retry: false,
  });
}

export function useUpdateImageSettings(cameraId: number | undefined) {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: Partial<{ brightness: number; contrast: number; saturation: number; sharpness: number; ir_cut_filter: string; wide_dynamic_range: boolean; exposure_mode: string }>) =>
      apiPut(`/api/onvif/cameras/${cameraId}/image`, body),
    onSuccess: (data) => qc.setQueryData(["image-settings", cameraId], data),
  });
}

/* ---------------------------------------------------------------------- */
/* Audio settings                                                           */
/* ---------------------------------------------------------------------- */

export function useAudioSettings(cameraId: number | undefined) {
  return useQuery<AudioSettings>({
    queryKey: ["audio-settings", cameraId],
    enabled: !!cameraId,
    queryFn: () => apiGet(`/api/onvif/cameras/${cameraId}/audio`),
    retry: false,
  });
}

export function useUpdateAudioSettings(cameraId: number | undefined) {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: Partial<{ encoding: string; bitrate_kbps: number; sample_rate_khz: number; is_enabled: boolean }>) =>
      apiPut(`/api/onvif/cameras/${cameraId}/audio`, body),
    onSuccess: (data) => qc.setQueryData(["audio-settings", cameraId], data),
  });
}
