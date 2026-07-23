/**
 * Central API client.
 * In Docker: VITE_API_URL="" so all /api/* calls are proxied by nginx.
 * In dev:    VITE_API_URL="http://localhost:3001"
 */

export const API_BASE: string =
  (import.meta.env.VITE_API_URL as string | undefined) ?? '';

export const tokenStore = {
  get:   (): string | null => localStorage.getItem('nvr_token'),
  set:   (t: string)       => localStorage.setItem('nvr_token', t),
  clear: ()                => localStorage.removeItem('nvr_token'),
};

async function apiFetch(path: string, opts: RequestInit = {}): Promise<Response> {
  const token = tokenStore.get();
  const headers: Record<string, string> = { ...(opts.headers as Record<string, string>) };
  if (token) headers['Authorization'] = `Bearer ${token}`;
  if (!(opts.body instanceof FormData)) headers['Content-Type'] = 'application/json';
  const res = await fetch(`${API_BASE}${path}`, { ...opts, headers });
  if (res.status === 401) { tokenStore.clear(); window.location.href = '/login'; }
  return res;
}

async function parseOrThrow<T>(res: Response): Promise<T> {
  const body = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error((body as any).error || `HTTP ${res.status}`);
  return body as T;
}

export const apiGet    = <T>(p: string)                  => apiFetch(p)                                       .then(r => parseOrThrow<T>(r));
export const apiPost   = <T>(p: string, b: unknown)      => apiFetch(p, { method: 'POST', body: JSON.stringify(b) }).then(r => parseOrThrow<T>(r));
export const apiPut    = <T>(p: string, b: unknown)      => apiFetch(p, { method: 'PUT',  body: JSON.stringify(b) }).then(r => parseOrThrow<T>(r));
export const apiDelete = <T>(p: string)                  => apiFetch(p, { method: 'DELETE' })                 .then(r => parseOrThrow<T>(r));
export const apiPostForm = <T>(p: string, fd: FormData)  => apiFetch(p, { method: 'POST', body: fd, headers: {} }).then(r => parseOrThrow<T>(r));

// ── Streaming URL helpers ──────────────────────────────────────────────────────
const tok = () => encodeURIComponent(tokenStore.get() ?? '');
export const getStreamUrl   = (id: number | string) => `${API_BASE}/api/streaming/recordings/${id}/stream?token=${tok()}`;
export const getDownloadUrl = (id: number | string) => `${API_BASE}/api/streaming/recordings/${id}/download?token=${tok()}`;
export const getHlsUrl      = (cameraId: number | string) => `${API_BASE}/api/streaming/hls/${cameraId}/stream.m3u8?token=${tok()}`;

// ── AI detection ──────────────────────────────────────────────────────────────
export type Detection   = { label: string; confidence: number; bbox: [number, number, number, number] };
export type DetectResult = {
  detections: Detection[]; inference_ms: number; image_size: [number, number]; model: string;
  people_count?: number; density_percent?: number; density_level?: string;
  intrusion_detected?: boolean; intruder_count?: number; summary?: Record<string, number>;
};

export async function detectObjects(blob: Blob, opts?: { conf?: number; model?: string; endpoint?: string }): Promise<DetectResult> {
  const fd = new FormData();
  fd.append('image', blob, 'frame.jpg');
  if (opts?.conf)  fd.append('conf',  String(opts.conf));
  if (opts?.model) fd.append('model', opts.model);
  return apiPostForm<DetectResult>(opts?.endpoint ?? '/api/ai/detect', fd);
}

// Backward compat aliases
export const API_SERVER_URL = API_BASE;
export const getRecordingStreamUrl   = getStreamUrl;
export const getRecordingDownloadUrl = getDownloadUrl;
