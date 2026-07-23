import { useQuery } from "@tanstack/react-query";
import { apiGet } from "@/lib/api";

export interface Recording {
  recording_id: number;
  camera_id: number;
  file_path: string;
  file_name: string;
  file_size_bytes: number | null;
  duration_seconds: number | null;
  start_timestamp: string;
  end_timestamp: string | null;
  video_codec: string;
  resolution_width: number;
  resolution_height: number;
  fps_actual: number | null;
  has_audio: number;
  recording_mode: string;
  status: string;
  hls_playlist_path: string | null;
  gps_latitude: number | null;
  gps_longitude: number | null;
  gps_speed_kmh: number | null;
  created_at: string;
  camera_name: string | null;
  location_description: string | null;
  ip_address: string | null;
  camera_type: string | null;
  cameras?: { camera_name: string | null };
}

interface RecordingsResponse { recordings: Recording[]; total: number; }

export function useRecordings(filters?: {
  camera_id?: number;
  start_date?: string;
  end_date?: string;
  status?: string;
  limit?: number;
  offset?: number;
}) {
  const params = new URLSearchParams();
  if (filters?.camera_id)  params.set("camera_id",  String(filters.camera_id));
  if (filters?.start_date) params.set("start_date", filters.start_date);
  if (filters?.end_date)   params.set("end_date",   filters.end_date);
  if (filters?.status)     params.set("status",     filters.status);
  if (filters?.limit)      params.set("limit",      String(filters.limit));
  if (filters?.offset)     params.set("offset",     String(filters.offset));
  const qs = params.toString();

  return useQuery<Recording[]>({
    queryKey: ["recordings", filters],
    queryFn: async () => {
      const data = await apiGet<RecordingsResponse>(`/api/recordings${qs ? '?' + qs : ''}`);
      return data.recordings.map(r => ({ ...r, cameras: { camera_name: r.camera_name } }));
    },
    refetchInterval: 15000,
  });
}
