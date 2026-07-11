// import { useState, useRef, useEffect, useCallback } from "react";
// import {
//   Activity, Users, ShieldAlert, Bot, Flame, Eye, BrainCircuit,
//   AlertTriangle, Zap, Camera, TrendingUp, Clock, CheckCircle2,
//   XCircle, RefreshCw, Maximize2, Radio
// } from "lucide-react";
// import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
// import { Badge } from "@/components/ui/badge";
// import { Button } from "@/components/ui/button";
// import { Progress } from "@/components/ui/progress";
// import { AppLayout } from "@/components/layout/AppLayout";
// import { useQuery, useQueryClient } from "@tanstack/react-query";
// import { apiGet, API_BASE, tokenStore, apiPost, type Detection } from "@/lib/api";
// import { useToast } from "@/hooks/use-toast";
// import { useEvents } from "@/hooks/use-events";
// import { useCameras } from "@/hooks/use-cameras";
// import { formatDistanceToNow } from "date-fns";

// // ── Railway-specific AI event definitions ────────────────────────────────────
// const RAILWAY_AI_EVENTS = [
//   { key: "CROWD_DENSITY",    icon: Users,       label: "Crowd / Congestion",     color: "text-orange-400",  bg: "bg-orange-400/10",  severity: "WARNING"  },
//   { key: "INTRUSION",        icon: ShieldAlert, label: "Intrusion / Trespass",   color: "text-red-400",     bg: "bg-red-400/10",     severity: "CRITICAL" },
//   { key: "SMOKE",            icon: Flame,       label: "Smoke / Fire / Fumes",   color: "text-red-500",     bg: "bg-red-500/10",     severity: "CRITICAL" },
//   { key: "PERSON_FALLEN",    icon: AlertTriangle,label:"Person Fallen / Leaning",color: "text-red-400",     bg: "bg-red-400/10",     severity: "CRITICAL" },
//   { key: "PHONE_USE",        icon: Radio,       label: "Mobile Phone Use (Crew)",color: "text-yellow-400",  bg: "bg-yellow-400/10",  severity: "WARNING"  },
//   { key: "STONE_PELTING",    icon: Zap,         label: "Stone Pelting Alert",    color: "text-orange-500",  bg: "bg-orange-500/10",  severity: "CRITICAL" },
//   { key: "OBSTACLE",         icon: Eye,         label: "Track Obstacle",         color: "text-purple-400",  bg: "bg-purple-400/10",  severity: "CRITICAL" },
//   { key: "OHE_DEFECT",       icon: Zap,         label: "OHE / Pantograph Defect",color:"text-yellow-500",  bg: "bg-yellow-500/10",  severity: "ERROR"    },
//   { key: "CREW_ABSENT",      icon: Users,       label: "Crew Absent from Seat",  color: "text-blue-400",    bg: "bg-blue-400/10",    severity: "WARNING"  },
//   { key: "SIGNAL_GESTURE",   icon: Activity,    label: "Signal / Gesture",       color: "text-green-400",   bg: "bg-green-400/10",   severity: "INFO"     },
//   { key: "EMERGENCY_BRAKE",  icon: AlertTriangle,label:"Emergency Brake Use",   color: "text-red-500",     bg: "bg-red-500/10",     severity: "CRITICAL" },
//   { key: "ANIMAL_DETECTION", icon: Eye,         label: "Animal on Track",        color: "text-purple-400",  bg: "bg-purple-400/10",  severity: "WARNING"  },
// ];

// // ── Per-camera live AI tile ──────────────────────────────────────────────────
// function CameraAiTile({ camera }: { camera: any }) {
//   const { toast } = useToast();
//   const videoRef = useRef<HTMLVideoElement>(null);
//   const canvasRef = useRef<HTMLCanvasElement>(null);
//   const intervalRef = useRef<ReturnType<typeof setInterval> | null>(null);
//   const hlsRef = useRef<any>(null);
//   const qc = useQueryClient();

//   const [aiOn, setAiOn] = useState(false);
//   const [detections, setDetections] = useState<Detection[]>([]);
//   const [alerts, setAlerts] = useState<Array<{label:string;color:string;time:Date}>>([]);
//   const [inferMs, setInferMs] = useState<number|null>(null);
//   const [density, setDensity] = useState<number>(0);
//   const [densityLevel, setDensityLevel] = useState<string>("LOW");
//   const [frameCount, setFrameCount] = useState(0);

//   // Start HLS stream
//   useEffect(() => {
//     const video = videoRef.current;
//     if (!video) return;
//     const token = tokenStore.get() ?? "";
//     const url = `${API_BASE}/api/streaming/hls/${camera.camera_id}/stream.m3u8?token=${encodeURIComponent(token)}`;
//     let cancelled = false;

//     if (video.canPlayType("application/vnd.apple.mpegurl")) {
//       video.src = url; video.load(); video.play().catch(() => {});
//       return () => { video.src = ""; };
//     }
//     import("hls.js").then(({ default: Hls }) => {
//       if (cancelled || !videoRef.current) return;
//       if (!Hls.isSupported()) return;
//       const hls = new Hls({ enableWorker: false, lowLatencyMode: true, backBufferLength: 30 });
//       hlsRef.current = hls;
//       hls.loadSource(url);
//       hls.attachMedia(videoRef.current);
//       hls.on(Hls.Events.MANIFEST_PARSED, () => { videoRef.current?.play().catch(() => {}); });
//     }).catch(() => {});
//     return () => { cancelled = true; hlsRef.current?.destroy(); };
//   }, [camera.camera_id]);

//   // Railway-specific label → event type mapping
//   const classifyDetection = useCallback((label: string, count: number): string | null => {
//     if (label === "person" && count >= 5) return "CROWD_DENSITY";
//     if (label === "fire" || label === "smoke") return "SMOKE";
//     if (label === "cell phone") return "PHONE_USE";
//     if (["cow","horse","sheep","dog","cat","bird"].includes(label)) return "ANIMAL_DETECTION";
//     if (["car","truck","bus","motorcycle"].includes(label)) return "OBSTACLE";
//     return null;
//   }, []);

//   // Run AI on current video frame
//   const runAi = useCallback(async () => {
//     const video = videoRef.current;
//     const canvas = canvasRef.current;
//     if (!video || !canvas || video.paused || video.videoWidth === 0) return;

//     const tmp = document.createElement("canvas");
//     tmp.width = video.videoWidth || 640;
//     tmp.height = video.videoHeight || 360;
//     tmp.getContext("2d")?.drawImage(video, 0, 0);

//     tmp.toBlob(async (blob) => {
//       if (!blob) return;
//       try {
//         const fd = new FormData();
//         fd.append("image", blob, "frame.jpg");
//         fd.append("conf", "0.35");
//         fd.append("camera_id", String(camera.camera_id));
//         const token = tokenStore.get() ?? "";
//         const res = await fetch(`${API_BASE}/api/ai/people-count`, {
//           method: "POST", body: fd,
//           headers: { Authorization: `Bearer ${token}` },
//         });
//         const data = await res.json();
//         if (!data.detections) return;

//         setDetections(data.detections);
//         setInferMs(data.inference_ms);
//         setFrameCount(c => c + 1);
//         if (data.density_percent !== undefined) {
//           setDensity(data.density_percent);
//           setDensityLevel(data.density_level || "LOW");
//         }

//         // Classify detections into railway alerts
//         const labelCounts: Record<string, number> = {};
//         data.detections.forEach((d: Detection) => { labelCounts[d.label] = (labelCounts[d.label] || 0) + 1; });

//         const newAlerts: Array<{label:string;color:string;time:Date}> = [];
//         for (const [label, count] of Object.entries(labelCounts)) {
//           const evType = classifyDetection(label, count);
//           if (evType) {
//             const evDef = RAILWAY_AI_EVENTS.find(e => e.key === evType);
//             newAlerts.push({ label: evDef?.label || evType, color: evDef?.color || "text-orange-400", time: new Date() });
//           }
//         }
//         if (newAlerts.length > 0) {
//           setAlerts(prev => [...newAlerts, ...prev].slice(0, 5));
//           qc.invalidateQueries({ queryKey: ["events"] });
//         }

//         // Draw overlay
//         canvas.width = tmp.width; canvas.height = tmp.height;
//         const ctx = canvas.getContext("2d")!;
//         ctx.clearRect(0, 0, canvas.width, canvas.height);
//         data.detections.forEach((d: Detection) => {
//           const [x1, y1, x2, y2] = d.bbox;
//           const isPerson = d.label === "person";
//           const isAlert  = ["fire","smoke","cell phone"].includes(d.label);
//           ctx.strokeStyle = isAlert ? "#ff0000" : isPerson ? "#00ff88" : "#ff8800";
//           ctx.lineWidth = 2;
//           ctx.strokeRect(x1, y1, x2 - x1, y2 - y1);
//           const lbl = `${d.label} ${(d.confidence * 100).toFixed(0)}%`;
//           ctx.font = "bold 11px system-ui";
//           const tw = ctx.measureText(lbl).width + 6;
//           ctx.fillStyle = isAlert ? "#ff000099" : isPerson ? "#00ff8899" : "#ff880099";
//           ctx.fillRect(x1, y1 - 16, tw, 16);
//           ctx.fillStyle = "#fff";
//           ctx.fillText(lbl, x1 + 3, y1 - 3);
//         });

//         // Density bar
//         if (data.people_count > 0) {
//           const barH = 6;
//           const barW = (data.density_percent / 100) * canvas.width;
//           const col = data.density_level === "HIGH" ? "#ef4444" : data.density_level === "MEDIUM" ? "#f59e0b" : "#22c55e";
//           ctx.fillStyle = col + "bb";
//           ctx.fillRect(0, canvas.height - barH, barW, barH);
//           ctx.fillStyle = "#fff";
//           ctx.font = "bold 12px system-ui";
//           ctx.fillText(`${data.people_count} people · ${data.density_level}`, 6, canvas.height - barH - 4);
//         }
//       } catch {}
//     }, "image/jpeg", 0.7);
//   }, [camera.camera_id, classifyDetection, qc]);

//   useEffect(() => {
//     if (aiOn) {
//       intervalRef.current = setInterval(runAi, 2500);
//     } else {
//       if (intervalRef.current) clearInterval(intervalRef.current);
//       setDetections([]); setAlerts([]);
//       if (canvasRef.current) {
//         const ctx = canvasRef.current.getContext("2d");
//         ctx?.clearRect(0, 0, canvasRef.current.width, canvasRef.current.height);
//       }
//     }
//     return () => { if (intervalRef.current) clearInterval(intervalRef.current); };
//   }, [aiOn, runAi]);

//   const peopleCount = detections.filter(d => d.label === "person").length;
//   const densityColor = densityLevel === "HIGH" ? "text-red-400" : densityLevel === "MEDIUM" ? "text-yellow-400" : "text-green-400";

//   return (
//     <Card className="bg-card border-border overflow-hidden group">
//       {/* Video */}
//       <div className="relative aspect-video bg-black">
//         <video ref={videoRef} className="w-full h-full object-cover" muted playsInline />
//         {aiOn && (
//           <canvas ref={canvasRef} className="absolute inset-0 w-full h-full pointer-events-none" style={{ objectFit: "cover" }} />
//         )}
//         {/* Status bar */}
//         <div className="absolute top-1.5 left-1.5 right-1.5 flex items-center justify-between">
//           <div className="flex gap-1">
//             <Badge className={`text-[9px] py-0 px-1.5 ${camera.status === "ACTIVE" ? "bg-green-500/80" : "bg-gray-500/80"} text-white border-0`}>
//               {camera.status}
//             </Badge>
//             {camera.is_recording === 1 && (
//               <Badge className="text-[9px] py-0 px-1.5 bg-red-500/80 text-white border-0 animate-pulse">REC</Badge>
//             )}
//           </div>
//           {aiOn && inferMs && (
//             <Badge className="text-[9px] py-0 px-1.5 bg-black/60 text-green-400 border-0">{inferMs}ms</Badge>
//           )}
//         </div>
//         {/* People count overlay */}
//         {aiOn && peopleCount > 0 && (
//           <div className="absolute bottom-1.5 left-1.5 flex gap-1">
//             <Badge className={`text-[9px] py-0 px-1.5 bg-black/70 border-0 ${densityColor}`}>
//               {peopleCount} 👥 · {densityLevel}
//             </Badge>
//           </div>
//         )}
//         {/* AI toggle */}
//         <div className="absolute bottom-1.5 right-1.5 opacity-0 group-hover:opacity-100 transition-opacity">
//           <Button
//             size="sm"
//             variant={aiOn ? "default" : "secondary"}
//             className="h-6 text-[10px] px-2 gap-1"
//             onClick={() => setAiOn(!aiOn)}
//           >
//             <Bot className="h-3 w-3" />
//             {aiOn ? "AI ON" : "AI OFF"}
//           </Button>
//         </div>
//       </div>

//       {/* Card body */}
//       <CardContent className="p-2 space-y-1.5">
//         <div className="flex items-center justify-between">
//           <div className="min-w-0">
//             <p className="text-xs font-semibold text-foreground truncate">{camera.camera_name}</p>
//             <p className="text-[10px] text-muted-foreground truncate">{camera.location_description || camera.camera_type}</p>
//           </div>
//           <div className="shrink-0 flex items-center gap-1 ml-1">
//             {camera.is_online ? <div className="w-1.5 h-1.5 rounded-full bg-green-400" /> : <div className="w-1.5 h-1.5 rounded-full bg-gray-400" />}
//             {aiOn && <BrainCircuit className="h-3 w-3 text-ai-glow animate-pulse" />}
//           </div>
//         </div>

//         {aiOn && (
//           <>
//             {density > 0 && (
//               <div className="space-y-0.5">
//                 <div className="flex justify-between text-[10px]">
//                   <span className="text-muted-foreground">Density</span>
//                   <span className={densityColor}>{density}%</span>
//                 </div>
//                 <Progress value={density} className="h-1" />
//               </div>
//             )}
//             {alerts.length > 0 && (
//               <div className="space-y-0.5">
//                 {alerts.slice(0, 2).map((a, i) => (
//                   <div key={i} className={`text-[9px] flex items-center gap-1 ${a.color}`}>
//                     <AlertTriangle className="h-2.5 w-2.5 shrink-0" />
//                     <span className="truncate">{a.label}</span>
//                   </div>
//                 ))}
//               </div>
//             )}
//             <p className="text-[9px] text-muted-foreground">Frames: {frameCount} · {detections.length} objects</p>
//           </>
//         )}
//       </CardContent>
//     </Card>
//   );
// }

// // ── Main AiAnalytics page ────────────────────────────────────────────────────
// const AiAnalytics = () => {
//   const { toast } = useToast();
//   const [globalAi, setGlobalAi] = useState(false);
//   const [aiSidecarUp, setAiSidecarUp] = useState<boolean | null>(null);

//   const { data: cameras, isLoading: camsLoading } = useCameras();
//   const { data: events } = useEvents({ limit: 500 });
//   const { data: analytics, refetch: refetchAnalytics } = useQuery({
//     queryKey: ["ai-analytics"],
//     queryFn: () => apiGet<any>("/api/ai/analytics"),
//     refetchInterval: 30000,
//   });

//   // Check sidecar health on mount
//   useEffect(() => {
//     apiGet<any>("/api/ai/health")
//       .then(d => setAiSidecarUp(d.sidecar === "up"))
//       .catch(() => setAiSidecarUp(false));
//   }, []);

//   const activeCameras = cameras?.filter(c => c.status === "ACTIVE") ?? [];
//   const totalEvents = events?.length ?? 0;

//   // Count by railway event type
//   const countByType = (type: string) => events?.filter(e => e.event_type === type).length ?? 0;

//   return (
//     <AppLayout>
//       <div className="space-y-5">
//         {/* Header */}
//         <div className="flex items-start justify-between">
//           <div>
//             <h1 className="text-2xl font-bold text-foreground flex items-center gap-2">
//               <BrainCircuit className="h-6 w-6 text-ai-glow" />
//               AI Surveillance Analytics
//             </h1>
//             <p className="text-sm text-muted-foreground mt-0.5">
//               Real-time YOLO inference across all cameras — EN 50155 compliant mNVR
//             </p>
//           </div>
//           <div className="flex items-center gap-2">
//             {aiSidecarUp === true  && <Badge className="bg-green-500/20 text-green-400 border-green-500/30 gap-1"><CheckCircle2 className="h-3 w-3"/>AI Online</Badge>}
//             {aiSidecarUp === false && <Badge className="bg-red-500/20 text-red-400 border-red-500/30 gap-1"><XCircle className="h-3 w-3"/>AI Offline</Badge>}
//             {aiSidecarUp === null  && <Badge variant="secondary" className="gap-1"><RefreshCw className="h-3 w-3 animate-spin"/>Checking…</Badge>}
//             <Button
//               variant={globalAi ? "default" : "outline"}
//               size="sm"
//               className="gap-1.5"
//               onClick={() => setGlobalAi(!globalAi)}
//               disabled={aiSidecarUp === false}
//             >
//               <Bot className="h-4 w-4" />
//               {globalAi ? "Disable All AI" : "Enable All AI"}
//             </Button>
//           </div>
//         </div>

//         {/* AI Sidecar offline warning */}
//         {aiSidecarUp === false && (
//           <Card className="border-destructive/40 bg-destructive/5">
//             <CardContent className="p-3 flex items-center gap-3">
//               <XCircle className="h-5 w-5 text-destructive shrink-0" />
//               <div>
//                 <p className="text-sm font-medium text-destructive">YOLO AI sidecar is not running</p>
//                 <p className="text-xs text-muted-foreground mt-0.5">
//                   Start it with: <code className="bg-muted px-1 rounded">bash start.sh</code> — it starts automatically with the system.
//                 </p>
//               </div>
//             </CardContent>
//           </Card>
//         )}

//         {/* Railway AI Capabilities strip */}
//         <div className="grid grid-cols-3 sm:grid-cols-4 lg:grid-cols-6 gap-2">
//           {RAILWAY_AI_EVENTS.slice(0, 12).map(ev => {
//             const count = countByType(ev.key);
//             const Icon = ev.icon;
//             return (
//               <Card key={ev.key} className={`border-border ${count > 0 ? ev.bg : "bg-card"} transition-colors`}>
//                 <CardContent className="p-2 text-center">
//                   <Icon className={`h-4 w-4 mx-auto mb-1 ${count > 0 ? ev.color : "text-muted-foreground"}`} />
//                   <p className={`text-sm font-bold ${count > 0 ? ev.color : "text-muted-foreground"}`}>{count}</p>
//                   <p className="text-[9px] text-muted-foreground leading-tight line-clamp-2">{ev.label}</p>
//                 </CardContent>
//               </Card>
//             );
//           })}
//         </div>

//         {/* Summary stats */}
//         <div className="grid grid-cols-2 lg:grid-cols-4 gap-3">
//           {[
//             { label: "Active Cameras", val: activeCameras.length, icon: Camera, color: "text-blue-400" },
//             { label: "Events (24h)", val: totalEvents, icon: Activity, color: "text-info" },
//             { label: "Critical Alerts", val: events?.filter(e => e.severity === "CRITICAL" || e.severity === "EMERGENCY").length ?? 0, icon: AlertTriangle, color: "text-destructive" },
//             { label: "Crowd Events", val: countByType("CROWD_DENSITY"), icon: Users, color: "text-warning" },
//           ].map(({ label, val, icon: Icon, color }) => (
//             <Card key={label} className="bg-card border-border">
//               <CardContent className="p-4 flex items-center gap-3">
//                 <div className={`w-9 h-9 rounded-lg ${color} bg-current/10 flex items-center justify-center shrink-0`}>
//                   <Icon className={`h-4 w-4 ${color}`} />
//                 </div>
//                 <div>
//                   <p className="text-xl font-bold text-foreground">{val}</p>
//                   <p className="text-xs text-muted-foreground">{label}</p>
//                 </div>
//               </CardContent>
//             </Card>
//           ))}
//         </div>

//         {/* Camera grid with per-tile AI */}
//         <div>
//           <div className="flex items-center justify-between mb-3">
//             <h2 className="text-sm font-semibold text-foreground">
//               Live AI Monitoring — All Cameras ({activeCameras.length} active)
//             </h2>
//             <p className="text-xs text-muted-foreground">
//               {globalAi ? "AI running on all tiles • updates every 2.5s" : "Hover a tile and click AI to enable per-camera"}
//             </p>
//           </div>

//           {camsLoading && (
//             <div className="grid grid-cols-2 md:grid-cols-3 xl:grid-cols-4 gap-3">
//               {Array.from({ length: 6 }).map((_, i) => (
//                 <div key={i} className="aspect-video bg-muted/30 rounded-lg animate-pulse" />
//               ))}
//             </div>
//           )}

//           {!camsLoading && activeCameras.length === 0 && (
//             <Card className="bg-card border-border">
//               <CardContent className="py-12 text-center">
//                 <Camera className="h-10 w-10 mx-auto mb-2 text-muted-foreground/40" />
//                 <p className="text-sm text-muted-foreground">No active cameras found</p>
//                 <p className="text-xs text-muted-foreground mt-1">Add cameras via the Camera Grid page</p>
//               </CardContent>
//             </Card>
//           )}

//           <div className="grid grid-cols-2 md:grid-cols-3 xl:grid-cols-4 gap-3">
//             {activeCameras.map(cam => (
//               <div key={cam.camera_id}>
//                 <CameraAiTileWrapper camera={cam} globalAi={globalAi} />
//               </div>
//             ))}
//           </div>
//         </div>

//         {/* Recent AI events */}
//         <div className="grid lg:grid-cols-2 gap-4">
//           <Card className="bg-card border-border">
//             <CardHeader className="pb-2">
//               <CardTitle className="text-sm flex items-center gap-2">
//                 <Clock className="h-4 w-4 text-muted-foreground" />
//                 Recent Railway AI Alerts
//               </CardTitle>
//             </CardHeader>
//             <CardContent className="p-0">
//               <div className="divide-y divide-border max-h-64 overflow-y-auto">
//                 {events?.filter(e => RAILWAY_AI_EVENTS.some(r => r.key === e.event_type)).slice(0, 20).map(ev => {
//                   const def = RAILWAY_AI_EVENTS.find(r => r.key === ev.event_type);
//                   const Icon = def?.icon || Activity;
//                   return (
//                     <div key={ev.event_id} className="flex items-center gap-2.5 px-4 py-2.5">
//                       <Icon className={`h-4 w-4 shrink-0 ${def?.color || "text-muted-foreground"}`} />
//                       <div className="flex-1 min-w-0">
//                         <p className="text-xs font-medium text-foreground truncate">{ev.title}</p>
//                         <p className="text-[10px] text-muted-foreground">{ev.camera_name} · {formatDistanceToNow(new Date(ev.occurred_at), { addSuffix: true })}</p>
//                       </div>
//                       <Badge variant="outline" className={`text-[9px] shrink-0 ${
//                         ev.severity === "CRITICAL" || ev.severity === "EMERGENCY" ? "border-red-500/40 text-red-400" :
//                         ev.severity === "WARNING" ? "border-yellow-500/40 text-yellow-400" : ""
//                       }`}>
//                         {ev.severity}
//                       </Badge>
//                     </div>
//                   );
//                 })}
//                 {!events?.some(e => RAILWAY_AI_EVENTS.some(r => r.key === e.event_type)) && (
//                   <p className="text-sm text-muted-foreground text-center py-8">No AI events yet — enable AI on cameras above</p>
//                 )}
//               </div>
//             </CardContent>
//           </Card>

//           <Card className="bg-card border-border">
//             <CardHeader className="pb-2">
//               <CardTitle className="text-sm flex items-center gap-2">
//                 <TrendingUp className="h-4 w-4 text-muted-foreground" />
//                 Detection Statistics (24h)
//               </CardTitle>
//             </CardHeader>
//             <CardContent className="space-y-2.5">
//               {RAILWAY_AI_EVENTS.slice(0, 8).map(ev => {
//                 const count = countByType(ev.key);
//                 const max = Math.max(1, ...RAILWAY_AI_EVENTS.map(e => countByType(e.key)));
//                 const pct = Math.round((count / max) * 100);
//                 const Icon = ev.icon;
//                 return (
//                   <div key={ev.key} className="flex items-center gap-2">
//                     <Icon className={`h-3.5 w-3.5 shrink-0 ${ev.color}`} />
//                     <div className="flex-1 min-w-0">
//                       <div className="flex justify-between text-[10px] mb-0.5">
//                         <span className="text-foreground truncate">{ev.label}</span>
//                         <span className={ev.color}>{count}</span>
//                       </div>
//                       <div className="h-1 bg-muted rounded-full overflow-hidden">
//                         <div className={`h-full rounded-full transition-all duration-500 ${
//                           ev.severity === "CRITICAL" ? "bg-red-500" : ev.severity === "WARNING" ? "bg-yellow-500" : "bg-blue-500"
//                         }`} style={{ width: `${pct}%` }} />
//                       </div>
//                     </div>
//                   </div>
//                 );
//               })}
//             </CardContent>
//           </Card>
//         </div>
//       </div>
//     </AppLayout>
//   );
// };

// // Wrapper to handle globalAi prop
// function CameraAiTileWrapper({ camera, globalAi }: { camera: any; globalAi: boolean }) {
//   return <CameraAiTile camera={camera} />;
// }

// export default AiAnalytics;

import { useState, useRef } from "react";
import {
  Activity,
  Users,
  ShieldAlert,
  Bot,
  Flame,
  Eye,
  BrainCircuit,
  AlertTriangle,
  Zap,
  Camera,
  TrendingUp,
  Clock,
  CheckCircle2,
  XCircle,
  RefreshCw,
  Upload,
  Radio,
} from "lucide-react";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { Progress } from "@/components/ui/progress";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs";
import { AppLayout } from "@/components/layout/AppLayout";
import { useQuery } from "@tanstack/react-query";
import {
  apiGet,
  apiPostForm,
  detectObjects,
  type DetectResult,
} from "@/lib/api";
import { useToast } from "@/hooks/use-toast";
import { useEvents } from "@/hooks/use-events";
import { useCameras } from "@/hooks/use-cameras";
import { formatDistanceToNow } from "date-fns";

const RAILWAY_EVENTS = [
  {
    key: "CROWD_DENSITY",
    icon: Users,
    label: "Crowd / Congestion",
    color: "text-orange-600 dark:text-orange-400",
    bg: "bg-orange-500/10",
    sev: "WARNING",
  },
  {
    key: "INTRUSION",
    icon: ShieldAlert,
    label: "Intrusion / Trespass",
    color: "text-red-600 dark:text-red-400",
    bg: "bg-red-500/10",
    sev: "CRITICAL",
  },
  {
    key: "SMOKE",
    icon: Flame,
    label: "Smoke / Fire / Fumes",
    color: "text-red-700 dark:text-red-500",
    bg: "bg-red-500/10",
    sev: "CRITICAL",
  },
  {
    key: "PERSON_FALLEN",
    icon: AlertTriangle,
    label: "Person Fallen / Leaning",
    color: "text-red-600 dark:text-red-400",
    bg: "bg-red-500/10",
    sev: "CRITICAL",
  },
  {
    key: "PHONE_USE",
    icon: Radio,
    label: "Mobile Phone Use",
    color: "text-yellow-600 dark:text-yellow-400",
    bg: "bg-yellow-500/10",
    sev: "WARNING",
  },
  {
    key: "STONE_PELTING",
    icon: Zap,
    label: "Stone Pelting Alert",
    color: "text-orange-700 dark:text-orange-500",
    bg: "bg-orange-500/10",
    sev: "CRITICAL",
  },
  {
    key: "OBSTACLE",
    icon: Eye,
    label: "Track Obstacle",
    color: "text-purple-600 dark:text-purple-400",
    bg: "bg-purple-500/10",
    sev: "CRITICAL",
  },
  {
    key: "OHE_DEFECT",
    icon: Zap,
    label: "OHE / Pantograph",
    color: "text-yellow-700 dark:text-yellow-500",
    bg: "bg-yellow-500/10",
    sev: "ERROR",
  },
  {
    key: "CREW_ABSENT",
    icon: Users,
    label: "Crew Absent from Seat",
    color: "text-blue-600 dark:text-blue-400",
    bg: "bg-blue-500/10",
    sev: "WARNING",
  },
  {
    key: "EMERGENCY_BRAKE",
    icon: AlertTriangle,
    label: "Emergency Brake Use",
    color: "text-red-700 dark:text-red-500",
    bg: "bg-red-500/10",
    sev: "CRITICAL",
  },
  {
    key: "ANIMAL_DETECTION",
    icon: Eye,
    label: "Animal on Track",
    color: "text-purple-600 dark:text-purple-400",
    bg: "bg-purple-500/10",
    sev: "WARNING",
  },
  {
    key: "MOTION",
    icon: Activity,
    label: "Motion Detected",
    color: "text-blue-500 dark:text-blue-400",
    bg: "bg-blue-500/10",
    sev: "INFO",
  },
];

const AiAnalytics = () => {
  const { toast } = useToast();
  const fileRef = useRef<HTMLInputElement>(null);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const [mode, setMode] = useState<"detect" | "people" | "intrusion">("detect");
  const [result, setResult] = useState<any>(null);
  const [busy, setBusy] = useState(false);
  const [previewUrl, setPreviewUrl] = useState<string | null>(null);

  const { data: eventsResp } = useEvents({ limit: 500 });
  const events = eventsResp?.events ?? [];
  const { data: cameras } = useCameras();
  const { data: analytics, refetch: refetchAnalytics } = useQuery({
    queryKey: ["ai-analytics"],
    queryFn: () => apiGet<any>("/api/ai/analytics"),
    refetchInterval: 60000,
  });
  const { data: aiHealth } = useQuery({
    queryKey: ["ai-health"],
    queryFn: () => apiGet<any>("/api/ai/health"),
    refetchInterval: 15000,
  });

  const endpointMap = {
    detect: "/api/ai/detect",
    people: "/api/ai/people-count",
    intrusion: "/api/ai/intrusion",
  };

  const handleFile = async (e: React.ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0];
    if (!file) return;
    const url = URL.createObjectURL(file);
    setPreviewUrl(url);
    setBusy(true);
    setResult(null);
    try {
      const res = await detectObjects(file, {
        conf: 0.35,
        endpoint: endpointMap[mode],
      });
      setResult(res);
      drawBoxes(url, res.detections);
      toast({
        title: "Analysis complete",
        description: `${res.detections.length} objects · ${res.inference_ms}ms`,
      });
    } catch (err: any) {
      toast({
        title: "AI Error",
        description: err.message,
        variant: "destructive",
      });
    } finally {
      setBusy(false);
    }
  };

  const drawBoxes = (imgUrl: string, dets: any[]) => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const img = new Image();
    img.onload = () => {
      canvas.width = img.width;
      canvas.height = img.height;
      const ctx = canvas.getContext("2d")!;
      ctx.drawImage(img, 0, 0);
      dets.forEach((d) => {
        const [x1, y1, x2, y2] = d.bbox;
        const isPerson = d.label === "person";
        ctx.strokeStyle = isPerson ? "#00ff88" : "#ff6600";
        ctx.lineWidth = 2;
        ctx.strokeRect(x1, y1, x2 - x1, y2 - y1);
        const lbl = `${d.label} ${(d.confidence * 100).toFixed(0)}%`;
        ctx.font = "13px system-ui";
        const tw = ctx.measureText(lbl).width + 8;
        ctx.fillStyle = isPerson ? "#00ff88cc" : "#ff6600cc";
        ctx.fillRect(x1, y1 - 18, tw, 18);
        ctx.fillStyle = "#000";
        ctx.fillText(lbl, x1 + 4, y1 - 4);
      });
    };
    img.src = imgUrl;
  };

  const countByType = (key: string) =>
    events?.filter((e) => e.event_type === key).length ?? 0;
  const activeCams = cameras?.filter((c) => c.status === "ACTIVE").length ?? 0;
  const totalEvents = events?.length ?? 0;
  const criticalCount =
    events?.filter((e) => ["CRITICAL", "EMERGENCY"].includes(e.severity))
      .length ?? 0;
  const crowdEvents = countByType("CROWD_DENSITY");

  return (
    <AppLayout>
      <div className="space-y-5">
        {/* Header */}
        <div className="flex items-start justify-between">
          <div>
            <h1 className="text-2xl font-bold text-foreground flex items-center gap-2">
              <BrainCircuit className="h-6 w-6 text-primary" />
              AI Surveillance Analytics
            </h1>
            <p className="text-sm text-muted-foreground mt-0.5">
              Railway mNVR · EN 50155 · YOLO inference
            </p>
          </div>
          <div className="flex items-center gap-2">
            {aiHealth?.sidecar === "up" ? (
              <Badge className="bg-green-500/10 text-green-700 dark:text-green-400 border-green-500/30 gap-1">
                <CheckCircle2 className="h-3 w-3" />
                AI Online
              </Badge>
            ) : (
              <Badge className="bg-red-500/10 text-red-700 dark:text-red-400 border-red-500/30 gap-1">
                <XCircle className="h-3 w-3" />
                AI Offline
              </Badge>
            )}
            <Button
              size="sm"
              variant="outline"
              onClick={() => refetchAnalytics()}
            >
              <RefreshCw className="h-3 w-3 mr-1" />
              Refresh
            </Button>
          </div>
        </div>

        {/* Sidecar offline warning */}
        {aiHealth?.sidecar !== "up" && (
          <Card className="border-destructive/40 bg-destructive/5">
            <CardContent className="p-3 flex items-center gap-3">
              <XCircle className="h-4 w-4 text-destructive shrink-0" />
              <div>
                <p className="text-sm font-medium text-destructive">
                  YOLO AI sidecar is not running
                </p>
                <p className="text-xs text-muted-foreground mt-0.5">
                  Start with:{" "}
                  <code className="bg-muted px-1 rounded">bash start.sh</code> —
                  AI starts automatically
                </p>
              </div>
            </CardContent>
          </Card>
        )}

        {/* Summary stats */}
        <div className="grid grid-cols-2 lg:grid-cols-4 gap-3">
          {[
            {
              label: "Active Cameras",
              val: activeCams,
              icon: Camera,
              color: "text-blue-600 dark:text-blue-400",
            },
            {
              label: "Events (24h)",
              val: totalEvents,
              icon: Activity,
              color: "text-primary",
            },
            {
              label: "Critical Alerts",
              val: criticalCount,
              icon: AlertTriangle,
              color: "text-red-600 dark:text-destructive",
            },
            {
              label: "Crowd Events",
              val: crowdEvents,
              icon: Users,
              color: "text-orange-600 dark:text-warning",
            },
          ].map(({ label, val, icon: Icon, color }) => (
            <Card key={label} className="bg-card border-border">
              <CardContent className="p-4 flex items-center gap-3">
                <div
                  className={`w-9 h-9 rounded-lg flex items-center justify-center shrink-0 bg-muted`}
                >
                  <Icon className={`h-4 w-4 ${color}`} />
                </div>
                <div>
                  <p className="text-xl font-bold text-foreground">{val}</p>
                  <p className="text-xs text-muted-foreground">{label}</p>
                </div>
              </CardContent>
            </Card>
          ))}
        </div>

        {/* Railway AI event type grid */}
        <div className="grid grid-cols-3 sm:grid-cols-4 lg:grid-cols-6 gap-2">
          {RAILWAY_EVENTS.map((ev) => {
            const count = countByType(ev.key);
            const Icon = ev.icon;
            return (
              <Card
                key={ev.key}
                className={`border-border transition-colors ${count > 0 ? ev.bg : "bg-card"}`}
              >
                <CardContent className="p-2.5 text-center">
                  <Icon
                    className={`h-4 w-4 mx-auto mb-1 ${count > 0 ? ev.color : "text-muted-foreground"}`}
                  />
                  <p
                    className={`text-sm font-bold ${count > 0 ? ev.color : "text-muted-foreground"}`}
                  >
                    {count}
                  </p>
                  <p className="text-[9px] text-muted-foreground leading-tight line-clamp-2 mt-0.5">
                    {ev.label}
                  </p>
                </CardContent>
              </Card>
            );
          })}
        </div>

        <Tabs defaultValue="upload">
          <TabsList>
            <TabsTrigger value="upload">Image Analysis</TabsTrigger>
            <TabsTrigger value="history">Event History</TabsTrigger>
            <TabsTrigger value="stats">Statistics</TabsTrigger>
          </TabsList>

          {/* ── Upload / analyse tab ── */}
          <TabsContent value="upload" className="space-y-4 mt-4">
            {/* Mode selector */}
            <Card className="bg-card border-border">
              <CardContent className="p-4">
                <p className="text-xs font-medium text-muted-foreground mb-2">
                  Select Detection Mode
                </p>
                <div className="flex flex-wrap gap-2 mb-4">
                  {[
                    { k: "detect", l: "Object Detection" },
                    { k: "people", l: "People Count + Density" },
                    { k: "intrusion", l: "Intrusion Detection" },
                  ].map(({ k, l }) => (
                    <Button
                      key={k}
                      size="sm"
                      variant={mode === k ? "default" : "outline"}
                      onClick={() => setMode(k as any)}
                    >
                      {l}
                    </Button>
                  ))}
                </div>
                <div
                  className="border-2 border-dashed border-border rounded-lg p-8 text-center cursor-pointer hover:border-primary/50 transition-colors"
                  onClick={() => fileRef.current?.click()}
                >
                  <Upload className="h-10 w-10 mx-auto mb-2 text-muted-foreground" />
                  <p className="text-sm text-muted-foreground font-medium">
                    {busy
                      ? "Analysing frame…"
                      : "Click to upload image (JPEG / PNG)"}
                  </p>
                  <p className="text-[10px] text-muted-foreground mt-1">
                    Upload any frame from a recording or screenshot
                  </p>
                  <input
                    ref={fileRef}
                    type="file"
                    accept="image/*"
                    className="hidden"
                    onChange={handleFile}
                  />
                </div>
              </CardContent>
            </Card>

            {/* Result */}
            {previewUrl && (
              <Card className="bg-card border-border">
                <CardHeader className="pb-2">
                  <CardTitle className="text-sm flex items-center gap-2 text-foreground">
                    <Bot className="h-4 w-4 text-primary" />
                    Detection Result
                  </CardTitle>
                </CardHeader>
                <CardContent className="space-y-3">
                  <canvas
                    ref={canvasRef}
                    className="w-full rounded border border-border"
                  />
                  {result && (
                    <div className="space-y-2">
                      <div className="flex flex-wrap gap-2">
                        <Badge variant="outline">
                          {result.detections?.length ?? 0} objects
                        </Badge>
                        <Badge variant="outline">{result.inference_ms}ms</Badge>
                        {result.people_count !== undefined && (
                          <Badge className="bg-green-500/20 text-green-700 dark:text-green-400">
                            {result.people_count} people
                          </Badge>
                        )}
                        {result.density_level && (
                          <Badge
                            className={
                              result.density_level === "HIGH"
                                ? "bg-destructive/20 text-destructive"
                                : result.density_level === "MEDIUM"
                                  ? "bg-yellow-500/20 text-yellow-700 dark:text-yellow-400"
                                  : "bg-muted"
                            }
                          >
                            {result.density_level} density{" "}
                            {result.density_percent}%
                          </Badge>
                        )}
                        {result.intrusion_detected !== undefined && (
                          <Badge
                            className={
                              result.intrusion_detected
                                ? "bg-destructive/20 text-destructive"
                                : "bg-green-500/20 text-green-700 dark:text-green-400"
                            }
                          >
                            {result.intrusion_detected
                              ? `INTRUSION: ${result.intruder_count}`
                              : "Clear"}
                          </Badge>
                        )}
                      </div>
                      {result.density_percent !== undefined && (
                        <Progress
                          value={result.density_percent}
                          className="h-2"
                        />
                      )}
                      {result.summary && (
                        <div className="flex flex-wrap gap-1">
                          {Object.entries(result.summary).map(([k, v]) => (
                            <Badge
                              key={k}
                              variant="secondary"
                              className="text-xs"
                            >
                              {k}: {v as number}
                            </Badge>
                          ))}
                        </div>
                      )}
                    </div>
                  )}
                </CardContent>
              </Card>
            )}
          </TabsContent>

          {/* ── History tab ── */}
          <TabsContent value="history" className="mt-4">
            <Card className="bg-card border-border">
              <CardContent className="p-0">
                <div className="divide-y divide-border max-h-96 overflow-y-auto">
                  {events
                    ?.filter((e) =>
                      RAILWAY_EVENTS.some((r) => r.key === e.event_type),
                    )
                    .slice(0, 30)
                    .map((ev) => {
                      const def = RAILWAY_EVENTS.find(
                        (r) => r.key === ev.event_type,
                      );
                      const Icon = def?.icon || Activity;
                      return (
                        <div
                          key={ev.event_id}
                          className="flex items-center gap-2.5 px-4 py-2.5"
                        >
                          <Icon
                            className={`h-4 w-4 shrink-0 ${def?.color || "text-muted-foreground"}`}
                          />
                          <div className="flex-1 min-w-0">
                            <p className="text-xs font-medium text-foreground truncate">
                              {ev.title}
                            </p>
                            <p className="text-[10px] text-muted-foreground">
                              {ev.camera_name} ·{" "}
                              {formatDistanceToNow(new Date(ev.occurred_at), {
                                addSuffix: true,
                              })}
                            </p>
                          </div>
                          <Badge
                            variant="outline"
                            className={`text-[9px] shrink-0 ${
                              ev.severity === "CRITICAL" ||
                              ev.severity === "EMERGENCY"
                                ? "border-red-500/40 text-red-600 dark:text-red-400"
                                : ev.severity === "WARNING"
                                  ? "border-yellow-500/40 text-yellow-600 dark:text-yellow-400"
                                  : ""
                            }`}
                          >
                            {ev.severity}
                          </Badge>
                        </div>
                      );
                    })}
                  {!events?.some((e) =>
                    RAILWAY_EVENTS.some((r) => r.key === e.event_type),
                  ) && (
                    <p className="text-sm text-muted-foreground text-center py-10">
                      No AI events yet — enable AI on cameras or upload an image
                      above
                    </p>
                  )}
                </div>
              </CardContent>
            </Card>
          </TabsContent>

          {/* ── Stats tab ── */}
          <TabsContent value="stats" className="mt-4">
            <div className="grid lg:grid-cols-2 gap-4">
              <Card className="bg-card border-border">
                <CardHeader className="pb-2">
                  <CardTitle className="text-sm text-foreground flex items-center gap-2">
                    <TrendingUp className="h-4 w-4 text-muted-foreground" />
                    Event Type Breakdown (24h)
                  </CardTitle>
                </CardHeader>
                <CardContent className="space-y-2.5">
                  {analytics?.by_type?.length > 0 ? (
                    analytics.by_type.slice(0, 8).map((row: any) => {
                      const def = RAILWAY_EVENTS.find(
                        (r) => r.key === row.event_type,
                      );
                      const Icon = def?.icon || Activity;
                      const max = Math.max(
                        1,
                        ...analytics.by_type.map((r: any) => parseInt(r.count)),
                      );
                      return (
                        <div
                          key={row.event_type}
                          className="flex items-center gap-2"
                        >
                          <Icon
                            className={`h-3.5 w-3.5 shrink-0 ${def?.color || "text-muted-foreground"}`}
                          />
                          <div className="flex-1 min-w-0">
                            <div className="flex justify-between text-[10px] mb-0.5">
                              <span className="text-foreground truncate">
                                {def?.label ||
                                  row.event_type.replace(/_/g, " ")}
                              </span>
                              <span
                                className={
                                  def?.color || "text-muted-foreground"
                                }
                              >
                                {row.count}
                              </span>
                            </div>
                            <div className="h-1 bg-muted rounded-full overflow-hidden">
                              <div
                                className={`h-full rounded-full ${def?.sev === "CRITICAL" ? "bg-red-500" : def?.sev === "WARNING" ? "bg-yellow-500" : "bg-blue-500"}`}
                                style={{
                                  width: `${(parseInt(row.count) / max) * 100}%`,
                                }}
                              />
                            </div>
                          </div>
                        </div>
                      );
                    })
                  ) : (
                    <p className="text-sm text-muted-foreground text-center py-6">
                      No analytics data yet
                    </p>
                  )}
                </CardContent>
              </Card>

              <Card className="bg-card border-border">
                <CardHeader className="pb-2">
                  <CardTitle className="text-sm text-foreground flex items-center gap-2">
                    <Camera className="h-4 w-4 text-muted-foreground" />
                    Events by Camera (24h)
                  </CardTitle>
                </CardHeader>
                <CardContent className="space-y-2">
                  {analytics?.by_camera?.length > 0 ? (
                    analytics.by_camera.slice(0, 8).map((row: any) => (
                      <div
                        key={row.camera_id}
                        className="flex items-center justify-between py-1 border-b border-border/40 last:border-0"
                      >
                        <div className="min-w-0">
                          <p className="text-xs font-medium text-foreground truncate">
                            {row.camera_name || `Camera ${row.camera_id}`}
                          </p>
                        </div>
                        <Badge variant="secondary" className="text-xs shrink-0">
                          {row.count} events
                        </Badge>
                      </div>
                    ))
                  ) : (
                    <p className="text-sm text-muted-foreground text-center py-6">
                      No camera event data yet
                    </p>
                  )}
                </CardContent>
              </Card>
            </div>
          </TabsContent>
        </Tabs>
      </div>
    </AppLayout>
  );
};

export default AiAnalytics;
