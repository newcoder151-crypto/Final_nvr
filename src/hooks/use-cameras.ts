import { useQuery, useMutation, useQueryClient } from "@tanstack/react-query";
import { apiGet, apiPost, apiPut, apiDelete } from "@/lib/api";

export interface Camera {
  camera_id: number; camera_name: string; camera_type: string;
  ip_address: string; rtsp_url: string; rtsp_port: number;
  status: string; location_description: string | null; physical_position: string | null;
  resolution_width: number; resolution_height: number; target_fps: number;
  video_codec: string; audio_supported: number; ptz_supported: number;
  manufacturer: string | null; model: string | null;
  hls_playlist_url: string | null; hls_output_dir: string | null; rec_output_dir: string | null;
  added_at: string; updated_at: string;
  // from health join
  is_online: number | null; is_recording: number | null;
  frame_rate_actual: number | null; bitrate_kbps: number | null; last_error: string | null;
}

interface CamerasResponse { cameras: Camera[]; total: number; }

export function useCameras(params?: { status?: string; camera_type?: string }) {
  const qs = new URLSearchParams(params as Record<string,string> || {}).toString();
  return useQuery<Camera[]>({
    queryKey: ["cameras", params],
    queryFn: async () => {
      const data = await apiGet<CamerasResponse>(`/api/cameras${qs ? '?' + qs : ''}`);
      return data.cameras;
    },
    refetchInterval: 15000,
  });
}

export function useCamera(id: number | undefined) {
  return useQuery<Camera>({
    queryKey: ["cameras", id],
    enabled: !!id,
    queryFn: () => apiGet<Camera>(`/api/cameras/${id}`),
  });
}

export function useCreateCamera() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (camera: Partial<Camera>) => apiPost<Camera>("/api/cameras", camera),
    onSuccess: () => qc.invalidateQueries({ queryKey: ["cameras"] }),
  });
}

export function useUpdateCamera() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: ({ camera_id, ...updates }: Partial<Camera> & { camera_id: number }) =>
      apiPut<Camera>(`/api/cameras/${camera_id}`, updates),
    onSuccess: () => qc.invalidateQueries({ queryKey: ["cameras"] }),
  });
}

export function useDeleteCamera() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (camera_id: number) => apiDelete(`/api/cameras/${camera_id}`),
    onSuccess: () => qc.invalidateQueries({ queryKey: ["cameras"] }),
  });
}
