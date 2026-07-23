import { useState, useEffect, useRef, useCallback } from "react";
import {
  Plus,
  RefreshCw,
  Video,
  VideoOff,
  Wifi,
  WifiOff,
  Network,
  Bot,
  Maximize2,
  Radio,
  AlertTriangle,
  Grid2x2,
  Grid3x3,
  LayoutGrid,
  Square,
  ChevronLeft,
  ChevronRight,
  X,
  Settings2,
} from "lucide-react";
import { AppLayout } from "@/components/layout/AppLayout";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";
import { Card, CardContent } from "@/components/ui/card";
import {
  Dialog,
  DialogContent,
  DialogTitle,
} from "@/components/ui/dialog";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import { useCameras, useCreateCamera, type Camera } from "@/hooks/use-cameras";
import { CameraControlPanel } from "@/components/camera/CameraControlPanel";
import { useAuth } from "@/contexts/AuthContext";
import { useToast } from "@/hooks/use-toast";
import { apiGet, detectObjects, type Detection } from "@/lib/api";
import { useQuery } from "@tanstack/react-query";

const MTX_WEB =
  (import.meta.env.VITE_MEDIAMTX_WEB as string | undefined) ??
  "http://localhost:8889";
const MTX_HLS =
  (import.meta.env.VITE_MEDIAMTX_HLS as string | undefined) ??
  "http://localhost:8888";

const LAYOUTS = [
  { key: "1x1", label: "1×1", cols: 1, perPage: 1, icon: Square },
  { key: "2x2", label: "2×2", cols: 2, perPage: 4, icon: Grid2x2 },
  { key: "3x3", label: "3×3", cols: 3, perPage: 9, icon: Grid3x3 },
  { key: "4x4", label: "4×4", cols: 4, perPage: 16, icon: LayoutGrid },
] as const;

// ── AI overlay ────────────────────────────────────────────────────────────────
function useAiOverlay(
  videoRef: React.RefObject<HTMLVideoElement>,
  enabled: boolean,
  model?: string,
) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  useEffect(() => {
    if (!enabled) {
      const ctx = canvasRef.current?.getContext("2d");
      if (ctx && canvasRef.current)
        ctx.clearRect(0, 0, canvasRef.current.width, canvasRef.current.height);
      return;
    }
    const id = setInterval(async () => {
      const v = videoRef.current;
      const c = canvasRef.current;
      if (!v || !c || v.paused || v.videoWidth === 0) return;
      const tmp = document.createElement("canvas");
      tmp.width = v.videoWidth;
      tmp.height = v.videoHeight;
      tmp.getContext("2d")?.drawImage(v, 0, 0);
      tmp.toBlob(
        async (blob) => {
          if (!blob) return;
          try {
            const res = await detectObjects(blob, { conf: 0.35, model });
            c.width = tmp.width;
            c.height = tmp.height;
            const ctx = c.getContext("2d")!;
            ctx.clearRect(0, 0, c.width, c.height);
            res.detections.forEach((d: Detection) => {
              const [x1, y1, x2, y2] = d.bbox;
              const col = d.label === "person" ? "#00ff88" : "#ff8800";
              ctx.strokeStyle = col;
              ctx.lineWidth = 2;
              ctx.strokeRect(x1, y1, x2 - x1, y2 - y1);
              const lbl = `${d.label} ${(d.confidence * 100).toFixed(0)}%`;
              ctx.font = "bold 11px system-ui";
              const tw = ctx.measureText(lbl).width + 6;
              ctx.fillStyle = col + "cc";
              ctx.fillRect(x1, y1 - 16, tw, 16);
              ctx.fillStyle = "#000";
              ctx.fillText(lbl, x1 + 3, y1 - 3);
            });
          } catch {}
        },
        "image/jpeg",
        0.6,
      );
    }, 3000);
    return () => clearInterval(id);
  }, [enabled, videoRef, model]);
  return canvasRef;
}

// ── Single camera tile ────────────────────────────────────────────────────────
interface TileProps {
  cam: Camera & {
    mediamtx?: {
      path_name: string;
      is_live: boolean;
      webrtc_url: string;
      hls_url: string;
    };
  };
  compact: boolean;
  onExpand: (cam: TileProps["cam"]) => void;
}

function CameraTile({ cam, compact, onExpand }: TileProps) {
  const videoRef = useRef<HTMLVideoElement>(null);
  const pcRef = useRef<RTCPeerConnection | null>(null);
  const hlsRef = useRef<any>(null);
  const [connState, setConnState] = useState<
    "connecting" | "connected" | "failed" | "idle"
  >("idle");
  const [mode, setMode] = useState<"webrtc" | "hls" | "none">("none");
  const [aiEnabled, setAiEnabled] = useState(false);
  const [retry, setRetry] = useState(0);
  const canvasRef = useAiOverlay(videoRef, aiEnabled, cam.ai_model);

  const whepUrl =
    cam.mediamtx?.webrtc_url ?? `${MTX_WEB}/cam_${cam.camera_id}/whep`;
  const hlsUrl =
    cam.mediamtx?.hls_url ?? `${MTX_HLS}/cam_${cam.camera_id}/index.m3u8`;

  const startHls = useCallback(() => {
    const video = videoRef.current;
    if (!video) return;
    setMode("hls");
    setConnState("connecting");
    if (video.canPlayType("application/vnd.apple.mpegurl")) {
      video.src = hlsUrl;
      video.load();
      video
        .play()
        .then(() => setConnState("connected"))
        .catch(() => setConnState("failed"));
      return;
    }
    import("hls.js")
      .then(({ default: Hls }) => {
        if (!Hls.isSupported() || !videoRef.current) return;
        hlsRef.current?.destroy();
        const hls = new Hls({
          enableWorker: false,
          lowLatencyMode: true,
          backBufferLength: 10,
        });
        hlsRef.current = hls;
        hls.loadSource(hlsUrl);
        hls.attachMedia(videoRef.current);
        hls.on(Hls.Events.MANIFEST_PARSED, () => {
          videoRef.current
            ?.play()
            .then(() => setConnState("connected"))
            .catch(() => {});
        });
        hls.on(Hls.Events.ERROR, (_: any, data: any) => {
          if (data.fatal) {
            setConnState("failed");
            setTimeout(() => setRetry((r) => r + 1), 4000);
          }
        });
      })
      .catch(() => setConnState("failed"));
  }, [hlsUrl]);

  const startWebRTC = useCallback(async () => {
    if (!videoRef.current) return;
    setConnState("connecting");
    pcRef.current?.close();
    pcRef.current = null;
    const pc = new RTCPeerConnection({
      iceServers: [],
      bundlePolicy: "max-bundle",
    });
    pcRef.current = pc;
    pc.ontrack = (ev) => {
      if (videoRef.current) {
        videoRef.current.srcObject = ev.streams[0];
        videoRef.current.play().catch(() => {});
        setConnState("connected");
        setMode("webrtc");
      }
    };
    pc.oniceconnectionstatechange = () => {
      if (
        ["failed", "disconnected", "closed"].includes(pc.iceConnectionState)
      ) {
        setConnState("failed");
        setMode("none");
        setTimeout(() => setRetry((r) => r + 1), 3000);
      }
    };
    pc.addTransceiver("video", { direction: "recvonly" });
    pc.addTransceiver("audio", { direction: "recvonly" });
    try {
      const offer = await pc.createOffer();
      await pc.setLocalDescription(offer);
      const resp = await fetch(whepUrl, {
        method: "POST",
        headers: { "Content-Type": "application/sdp" },
        body: offer.sdp,
      });
      if (!resp.ok) throw new Error(`WHEP ${resp.status}`);
      await pc.setRemoteDescription({ type: "answer", sdp: await resp.text() });
    } catch {
      pc.close();
      pcRef.current = null;
      startHls();
    }
  }, [whepUrl, startHls]);

  useEffect(() => {
    if (cam.status !== "ACTIVE") {
      setMode("none");
      setConnState("idle");
      return;
    }
    startWebRTC();
    return () => {
      pcRef.current?.close();
      pcRef.current = null;
      hlsRef.current?.destroy();
      hlsRef.current = null;
      if (videoRef.current) {
        videoRef.current.srcObject = null;
        videoRef.current.src = "";
      }
    };
  }, [cam.camera_id, retry]);

  return (
    <div
      className="relative bg-black rounded-lg overflow-hidden group"
      style={{ aspectRatio: "16/9" }}
    >
      <video
        ref={videoRef}
        className="w-full h-full object-cover"
        muted
        playsInline
        autoPlay
      />
      {aiEnabled && (
        <canvas
          ref={canvasRef}
          className="absolute inset-0 w-full h-full pointer-events-none"
        />
      )}

      {/* No signal overlay */}
      {connState !== "connected" && (
        <div className="absolute inset-0 flex flex-col items-center justify-center bg-black/80">
          {connState === "connecting" ? (
            <>
              <div className="w-5 h-5 border-2 border-primary border-t-transparent rounded-full animate-spin mb-1" />
              <p className="text-[10px] text-white/60">Connecting…</p>
            </>
          ) : (
            <>
              <VideoOff className="h-5 w-5 text-white/30 mb-1" />
              <p className="text-[10px] text-white/40">
                {cam.status !== "ACTIVE" ? cam.status : "No signal"}
              </p>
            </>
          )}
        </div>
      )}

      {/* Top-left: status */}
      <div className="absolute top-1.5 left-1.5 flex gap-1">
        {connState === "connected" && (
          <span className="text-[9px] px-1.5 py-0.5 rounded bg-green-500/80 text-white font-medium">
            LIVE {mode.toUpperCase()}
          </span>
        )}
        {cam.is_recording === 1 && (
          <span className="flex items-center gap-0.5 text-[9px] px-1.5 py-0.5 rounded bg-red-500/80 text-white font-medium animate-pulse">
            ● REC
          </span>
        )}
      </div>

      {/* Hover controls — AI button kept well away from top-right edge */}
      <div className="absolute top-1.5 right-1.5 flex gap-1 opacity-0 group-hover:opacity-100 transition-opacity">
        <button
          className={`w-6 h-6 rounded flex items-center justify-center transition-colors text-white ${aiEnabled ? "bg-green-500/80" : "bg-black/60 hover:bg-black/80"}`}
          onClick={() => setAiEnabled((e) => !e)}
          title="AI Overlay"
        >
          <Bot className="h-3.5 w-3.5" />
        </button>
        <button
          className="w-6 h-6 rounded bg-black/60 hover:bg-black/80 flex items-center justify-center text-white"
          onClick={() => onExpand(cam)}
          title="Expand"
        >
          <Maximize2 className="h-3.5 w-3.5" />
        </button>
      </div>

      {/* Bottom info */}
      {!compact && (
        <div className="absolute bottom-0 left-0 right-0 bg-gradient-to-t from-black/80 to-transparent p-2">
          <p className="text-xs font-semibold text-white leading-tight truncate">
            {cam.camera_name}
          </p>
          <div className="flex items-center gap-1.5 mt-0.5">
            <Network className="h-2.5 w-2.5 text-white/50 shrink-0" />
            <span className="text-[10px] font-mono text-white/60">
              {cam.ip_address || "—"}
            </span>
            {cam.is_online === 1 ? (
              <Wifi className="h-2.5 w-2.5 text-green-400 ml-auto" />
            ) : (
              <WifiOff className="h-2.5 w-2.5 text-white/30 ml-auto" />
            )}
          </div>
        </div>
      )}
      {compact && (
        <div className="absolute bottom-0 left-0 right-0 bg-gradient-to-t from-black/70 to-transparent px-1.5 pb-1">
          <p className="text-[10px] font-semibold text-white truncate">
            {cam.camera_name}
          </p>
        </div>
      )}
    </div>
  );
}

// ── Expanded fullscreen dialog ─────────────────────────────────────────────────
function ExpandedDialog({
  cam,
  open,
  onClose,
}: {
  cam: (Camera & { mediamtx?: any }) | null;
  open: boolean;
  onClose: () => void;
}) {
  const videoRef = useRef<HTMLVideoElement>(null);
  const pcRef = useRef<RTCPeerConnection | null>(null);
  const hlsRef = useRef<any>(null);
  const [aiEnabled, setAiEnabled] = useState(false);
  const [showControls, setShowControls] = useState(false);
  const [connState, setConnState] = useState<"connecting" | "connected" | "failed">("connecting");
  const canvasRef = useAiOverlay(videoRef, aiEnabled, cam?.ai_model);

  // Reset AI/controls when dialog closes/changes
  useEffect(() => {
    if (!open) {
      setAiEnabled(false);
      setShowControls(false);
      setConnState("connecting");
    }
  }, [open]);

  useEffect(() => {
    if (!open || !cam) return;

    const whepUrl =
      cam.mediamtx?.webrtc_url ?? `${MTX_WEB}/cam_${cam.camera_id}/whep`;
    const hlsUrl =
      cam.mediamtx?.hls_url ?? `${MTX_HLS}/cam_${cam.camera_id}/index.m3u8`;

    let cancelled = false;

    const startHlsFallback = () => {
      if (cancelled || !videoRef.current) return;
      const video = videoRef.current;
      if (video.canPlayType("application/vnd.apple.mpegurl")) {
        video.src = hlsUrl;
        video.load();
        video.play().then(() => { if (!cancelled) setConnState("connected"); }).catch(() => { if (!cancelled) setConnState("failed"); });
        return;
      }
      import("hls.js").then(({ default: Hls }) => {
        if (cancelled || !videoRef.current || !Hls.isSupported()) return;
        hlsRef.current?.destroy();
        const hls = new Hls({ enableWorker: false, lowLatencyMode: true });
        hlsRef.current = hls;
        hls.loadSource(hlsUrl);
        hls.attachMedia(videoRef.current);
        hls.on(Hls.Events.MANIFEST_PARSED, () => {
          if (cancelled) return;
          videoRef.current?.play().then(() => { if (!cancelled) setConnState("connected"); }).catch(() => {});
        });
        hls.on(Hls.Events.ERROR, (_: any, data: any) => {
          if (data.fatal && !cancelled) setConnState("failed");
        });
      }).catch(() => { if (!cancelled) setConnState("failed"); });
    };

    const run = async () => {
      if (!videoRef.current) return;
      setConnState("connecting");

      const pc = new RTCPeerConnection({ iceServers: [], bundlePolicy: "max-bundle" });
      pcRef.current = pc;

      pc.ontrack = (ev) => {
        if (cancelled || !videoRef.current) return;
        videoRef.current.srcObject = ev.streams[0];
        videoRef.current.play()
          .then(() => { if (!cancelled) setConnState("connected"); })
          .catch(() => { if (!cancelled) setConnState("connected"); }); // still connected even if autoplay blocked
      };

      pc.oniceconnectionstatechange = () => {
        if (["failed", "disconnected", "closed"].includes(pc.iceConnectionState) && !cancelled) {
          setConnState("failed");
        }
      };

      pc.addTransceiver("video", { direction: "recvonly" });
      pc.addTransceiver("audio", { direction: "recvonly" });

      try {
        const offer = await pc.createOffer();
        await pc.setLocalDescription(offer);
        const resp = await fetch(whepUrl, {
          method: "POST",
          headers: { "Content-Type": "application/sdp" },
          body: offer.sdp,
        });
        if (!resp.ok) throw new Error(`${resp.status}`);
        await pc.setRemoteDescription({ type: "answer", sdp: await resp.text() });
      } catch {
        pc.close();
        pcRef.current = null;
        startHlsFallback();
      }
    };

    // Small delay to ensure video element is mounted in DOM
    const timer = setTimeout(run, 50);

    return () => {
      cancelled = true;
      clearTimeout(timer);
      pcRef.current?.close();
      pcRef.current = null;
      hlsRef.current?.destroy();
      hlsRef.current = null;
      if (videoRef.current) {
        videoRef.current.srcObject = null;
        videoRef.current.src = "";
        videoRef.current.load();
      }
    };
  }, [open, cam?.camera_id]);

  if (!cam) return null;

  return (
    <Dialog open={open} onOpenChange={onClose}>
      <DialogContent className="max-w-5xl bg-black border-border p-0 overflow-hidden max-h-[92vh] overflow-y-auto [&>button]:hidden">
        {/* Hidden title for accessibility */}
        <DialogTitle className="sr-only">{cam.camera_name} — expanded view</DialogTitle>

        {/* Custom header — close button placed with enough gap from content */}
        <div className="flex items-center justify-between px-4 py-2.5 bg-card border-b border-border">
          <div className="min-w-0 flex-1 pr-4">
            <p className="text-sm font-semibold text-foreground truncate">
              {cam.camera_name}
            </p>
            <p className="text-[10px] text-muted-foreground flex items-center gap-1 mt-0.5">
              <Network className="h-2.5 w-2.5 shrink-0" />
              {cam.ip_address || "—"} ·{" "}
              {cam.location_description || cam.camera_type}
            </p>
          </div>
          {/* Controls row: Controls toggle + AI button + close — with explicit gap so they never overlap */}
          <div className="flex items-center gap-3 shrink-0">
            <Button
              size="sm"
              variant={showControls ? "default" : "outline"}
              className="h-7 text-xs gap-1.5"
              onClick={() => setShowControls((s) => !s)}
            >
              <Settings2 className="h-3.5 w-3.5" />
              Controls
            </Button>
            <Button
              size="sm"
              variant={aiEnabled ? "default" : "outline"}
              className="h-7 text-xs gap-1.5"
              onClick={() => setAiEnabled((e) => !e)}
            >
              <Bot className="h-3.5 w-3.5" />
              {aiEnabled ? "AI ON" : "AI"}
            </Button>
            <button
              onClick={onClose}
              className="w-7 h-7 rounded-md flex items-center justify-center text-muted-foreground hover:text-foreground hover:bg-muted transition-colors"
              title="Close"
            >
              <X className="h-4 w-4" />
            </button>
          </div>
        </div>

        {/* Video area */}
        <div className="relative bg-black" style={{ aspectRatio: "16/9" }}>
          <video
            ref={videoRef}
            className="w-full h-full object-contain"
            muted
            playsInline
            autoPlay
          />
          {aiEnabled && (
            <canvas
              ref={canvasRef}
              className="absolute inset-0 w-full h-full pointer-events-none"
            />
          )}

          {/* Connecting / failed overlay */}
          {connState !== "connected" && (
            <div className="absolute inset-0 flex flex-col items-center justify-center bg-black/70">
              {connState === "connecting" ? (
                <>
                  <div className="w-7 h-7 border-2 border-primary border-t-transparent rounded-full animate-spin mb-2" />
                  <p className="text-sm text-white/60">Connecting…</p>
                </>
              ) : (
                <>
                  <VideoOff className="h-8 w-8 text-white/30 mb-2" />
                  <p className="text-sm text-white/50">No signal</p>
                </>
              )}
            </div>
          )}
        </div>

        {/* ONVIF Profile S controls: streams / PTZ / image / audio — toggled via "Controls" button */}
        {showControls && (
          <div className="px-4 py-3 bg-card border-t border-border">
            <CameraControlPanel
              cameraId={cam.camera_id}
              cameraName={cam.camera_name}
              ptzSupported={cam.ptz_supported === 1}
            />
          </div>
        )}

        {/* Footer */}
        <div className="px-4 py-2 bg-card border-t border-border flex gap-4 text-[10px] text-muted-foreground flex-wrap">
          <span>WebRTC: {MTX_WEB}/cam_{cam.camera_id}/whep</span>
          <span>HLS: {MTX_HLS}/cam_{cam.camera_id}/index.m3u8</span>
        </div>
      </DialogContent>
    </Dialog>
  );
}

// ── Main page ─────────────────────────────────────────────────────────────────
const CameraGrid = () => {
  const { isAdmin } = useAuth();
  const { toast } = useToast();
  const { data: dbCameras, isLoading, refetch } = useCameras();
  const createCamera = useCreateCamera();

  const [layout, setLayout] = useState<"1x1" | "2x2" | "3x3" | "4x4">("2x2");
  const [page, setPage] = useState(0);
  const [expanded, setExpanded] = useState<any>(null);
  const [addOpen, setAddOpen] = useState(false);
  const [newCam, setNewCam] = useState({
    camera_name: "",
    camera_type: "INTERIOR",
    ip_address: "",
    rtsp_url: "",
    username: "",
    password_hash: "",
    location_description: "",
  });

  const currentLayout = LAYOUTS.find((l) => l.key === layout)!;
  const perPage = currentLayout.perPage;

  const { data: mtxCameras, refetch: refetchMtx } = useQuery({
    queryKey: ["mediamtx-cameras"],
    queryFn: () =>
      apiGet<{ cameras: any[] }>("/api/mediamtx/cameras").catch(() => ({
        cameras: [],
      })),
    refetchInterval: 10000,
  });
  const { data: mtxStatus } = useQuery({
    queryKey: ["mediamtx-status"],
    queryFn: () =>
      apiGet<any>("/api/mediamtx/status").catch(() => ({ online: false })),
    refetchInterval: 15000,
  });

  const cameras = (dbCameras ?? [])
    .filter((c) => c.status === "ACTIVE")
    .map((cam) => ({
      ...cam,
      mediamtx: mtxCameras?.cameras?.find(
        (c: any) => c.camera_id === cam.camera_id,
      )?.mediamtx,
    }));

  const totalPages = Math.ceil(cameras.length / perPage);
  const pageCams = cameras.slice(page * perPage, (page + 1) * perPage);

  const handleLayout = (key: typeof layout) => {
    setLayout(key);
    setPage(0);
  };

  const colClass: Record<string, string> = {
    "1x1": "grid-cols-1",
    "2x2": "grid-cols-2",
    "3x3": "grid-cols-3",
    "4x4": "grid-cols-4",
  };

  const handleAdd = async (e: React.FormEvent) => {
    e.preventDefault();
    try {
      await createCamera.mutateAsync(newCam);
      toast({ title: "Camera added — stream appears in ~15s" });
      setAddOpen(false);
      setNewCam({
        camera_name: "",
        camera_type: "INTERIOR",
        ip_address: "",
        rtsp_url: "",
        username: "",
        password_hash: "",
        location_description: "",
      });
      setTimeout(() => { refetch(); refetchMtx(); }, 16000);
    } catch (err: any) {
      toast({ title: "Error", description: err.message, variant: "destructive" });
    }
  };

  return (
    <AppLayout>
      <div className="space-y-3">
        {/* Header */}
        <div className="flex items-center justify-between flex-wrap gap-2">
          <div>
            <h1 className="text-2xl font-bold text-foreground">Camera Grid</h1>
            <p className="text-sm text-muted-foreground">
              {cameras.length} active cameras · WebRTC via MediaMTX
            </p>
          </div>
          <div className="flex items-center gap-2 flex-wrap">
            {/* Layout picker */}
            <div className="flex items-center border border-border rounded-md overflow-hidden">
              {LAYOUTS.map(({ key, label, icon: Icon }) => (
                <button
                  key={key}
                  onClick={() => handleLayout(key as typeof layout)}
                  className={`flex items-center gap-1 px-2.5 py-1.5 text-xs font-medium transition-colors ${layout === key ? "bg-primary text-primary-foreground" : "text-muted-foreground hover:text-foreground hover:bg-muted/50"}`}
                >
                  <Icon className="h-3.5 w-3.5" />
                  {label}
                </button>
              ))}
            </div>

            {mtxStatus && (
              <Badge
                variant="outline"
                className={`gap-1 text-xs ${mtxStatus.online ? "text-green-600 dark:text-green-400 border-green-500/30" : "text-red-600 dark:text-red-400 border-red-500/30"}`}
              >
                <Radio className={`h-3 w-3 ${mtxStatus.online ? "animate-pulse" : ""}`} />
                MediaMTX {mtxStatus.online ? `· ${mtxStatus.active_paths} paths` : "offline"}
              </Badge>
            )}

            <Button size="sm" variant="outline" onClick={() => { refetch(); refetchMtx(); }}>
              <RefreshCw className="h-3.5 w-3.5 mr-1" />
              Refresh
            </Button>

            {isAdmin && (
              <>
                <Button size="sm" className="gap-1" onClick={() => setAddOpen(true)}>
                  <Plus className="h-4 w-4" />
                  Add Camera
                </Button>

                {/* Add Camera Dialog */}
                <Dialog open={addOpen} onOpenChange={setAddOpen}>
                  <DialogContent className="bg-card border-border">
                    <DialogTitle>Add Camera</DialogTitle>
                    <form onSubmit={handleAdd} className="space-y-3 mt-2">
                      <div>
                        <Label className="text-xs">Name</Label>
                        <Input className="mt-1" placeholder="CAM-01-INTERIOR" required
                          value={newCam.camera_name}
                          onChange={(e) => setNewCam({ ...newCam, camera_name: e.target.value })} />
                      </div>
                      <div>
                        <Label className="text-xs">Type</Label>
                        <Select value={newCam.camera_type} onValueChange={(v) => setNewCam({ ...newCam, camera_type: v })}>
                          <SelectTrigger className="mt-1"><SelectValue /></SelectTrigger>
                          <SelectContent>
                            {["INTERIOR", "EXTERIOR", "DOOR", "DRIVER_CAB"].map((t) => (
                              <SelectItem key={t} value={t}>{t}</SelectItem>
                            ))}
                          </SelectContent>
                        </Select>
                      </div>
                      <div>
                        <Label className="text-xs">IP Address</Label>
                        <Input className="mt-1" placeholder="192.168.1.101"
                          value={newCam.ip_address}
                          onChange={(e) => setNewCam({ ...newCam, ip_address: e.target.value })} />
                      </div>
                      <div>
                        <Label className="text-xs">RTSP URL</Label>
                        <Input className="mt-1" placeholder="rtsp://192.168.1.101:554/stream1" required
                          value={newCam.rtsp_url}
                          onChange={(e) => setNewCam({ ...newCam, rtsp_url: e.target.value })} />
                      </div>
                      <div className="grid grid-cols-2 gap-2">
                        <div>
                          <Label className="text-xs">Username</Label>
                          <Input className="mt-1" placeholder="admin"
                            value={newCam.username}
                            onChange={(e) => setNewCam({ ...newCam, username: e.target.value })} />
                        </div>
                        <div>
                          <Label className="text-xs">Password</Label>
                          <Input className="mt-1" type="password" placeholder="••••••"
                            value={newCam.password_hash}
                            onChange={(e) => setNewCam({ ...newCam, password_hash: e.target.value })} />
                        </div>
                      </div>
                      <div>
                        <Label className="text-xs">Location</Label>
                        <Input className="mt-1" placeholder="Coach S1 — Front"
                          value={newCam.location_description}
                          onChange={(e) => setNewCam({ ...newCam, location_description: e.target.value })} />
                      </div>
                      <Button type="submit" className="w-full" disabled={createCamera.isPending}>
                        Add Camera
                      </Button>
                      <p className="text-[10px] text-muted-foreground text-center">
                        Credentials stored in DB — nvr_core and MediaMTX read them automatically
                      </p>
                    </form>
                  </DialogContent>
                </Dialog>
              </>
            )}
          </div>
        </div>

        {/* MediaMTX offline warning */}
        {mtxStatus && !mtxStatus.online && (
          <Card className="border-yellow-500/30 bg-yellow-500/5">
            <CardContent className="p-3 flex items-center gap-3">
              <AlertTriangle className="h-4 w-4 text-yellow-600 dark:text-yellow-400 shrink-0" />
              <p className="text-sm text-yellow-700 dark:text-yellow-400">
                MediaMTX offline — WebRTC unavailable. Run{" "}
                <code className="bg-muted px-1 rounded text-xs">bash start.sh</code>
              </p>
            </CardContent>
          </Card>
        )}

        {/* No cameras */}
        {!isLoading && cameras.length === 0 && (
          <Card className="bg-card border-border">
            <CardContent className="py-16 text-center">
              <Video className="h-12 w-12 mx-auto mb-3 text-muted-foreground/30" />
              <p className="text-sm text-foreground">No active cameras</p>
              {isAdmin && (
                <p className="text-xs text-muted-foreground mt-1">Click "Add Camera" above</p>
              )}
            </CardContent>
          </Card>
        )}

        {/* Camera grid */}
        {cameras.length > 0 && (
          <>
            <div className={`grid ${colClass[layout]} gap-2`}>
              {pageCams.map((cam) => (
                <CameraTile
                  key={cam.camera_id}
                  cam={cam}
                  compact={layout === "3x3" || layout === "4x4"}
                  onExpand={setExpanded}
                />
              ))}
              {Array.from({ length: perPage - pageCams.length }).map((_, i) => (
                <div
                  key={`empty-${i}`}
                  className="bg-muted/20 rounded-lg"
                  style={{ aspectRatio: "16/9" }}
                />
              ))}
            </div>

            {totalPages > 1 && (
              <div className="flex items-center justify-between pt-1">
                <Button size="sm" variant="outline" className="h-7 text-xs"
                  disabled={page === 0} onClick={() => setPage((p) => p - 1)}>
                  <ChevronLeft className="h-3.5 w-3.5 mr-1" />Prev
                </Button>
                <div className="flex items-center gap-1">
                  {Array.from({ length: totalPages }).map((_, i) => (
                    <button key={i} onClick={() => setPage(i)}
                      className={`w-2 h-2 rounded-full transition-colors ${i === page ? "bg-primary" : "bg-muted hover:bg-muted-foreground/40"}`}
                    />
                  ))}
                </div>
                <Button size="sm" variant="outline" className="h-7 text-xs"
                  disabled={page === totalPages - 1} onClick={() => setPage((p) => p + 1)}>
                  Next<ChevronRight className="h-3.5 w-3.5 ml-1" />
                </Button>
              </div>
            )}

            <p className="text-center text-[11px] text-muted-foreground">
              Page {page + 1} of {totalPages} · {cameras.length} cameras · {perPage} per page
            </p>
          </>
        )}
      </div>

      <ExpandedDialog
        cam={expanded}
        open={!!expanded}
        onClose={() => setExpanded(null)}
      />
    </AppLayout>
  );
};

export default CameraGrid;
