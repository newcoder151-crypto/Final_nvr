// // import { useState, useRef, useEffect, useCallback } from "react";
// // import { useSearchParams } from "react-router-dom";
// // import {
// //   Play, Pause, Download, SkipBack, SkipForward,
// //   Volume2, VolumeX, Bot, Camera, Network, Users,
// //   ShieldAlert, Eye, Cpu, Activity, CheckCircle2
// // } from "lucide-react";
// // import { AppLayout } from "@/components/layout/AppLayout";
// // import { Button } from "@/components/ui/button";
// // import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
// // import { Badge } from "@/components/ui/badge";
// // import { Progress } from "@/components/ui/progress";
// // import { Separator } from "@/components/ui/separator";
// // import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui/select";
// // import { useRecordings } from "@/hooks/use-recordings";
// // import { useCameras } from "@/hooks/use-cameras";
// // import { getStreamUrl, getDownloadUrl, getHlsUrl, detectObjects, type Detection, API_BASE, tokenStore } from "@/lib/api";
// // import { useToast } from "@/hooks/use-toast";
// // import { format } from "date-fns";

// // const AI_MODES = [
// //   { key: "detect",    label: "Object Detection",   endpoint: "/api/ai/detect",       color: "#ff8800" },
// //   { key: "people",    label: "People Count",        endpoint: "/api/ai/people-count", color: "#00ff88" },
// //   { key: "intrusion", label: "Intrusion Detection", endpoint: "/api/ai/intrusion",    color: "#ff4444" },
// // ];

// // const BOX_COLORS: Record<string, string> = {
// //   person: "#00ff88", fire: "#ff0000", smoke: "#ff6600",
// //   "cell phone": "#ffff00", car: "#ff8800", truck: "#ff8800",
// //   cow: "#aa44ff", dog: "#aa44ff", cat: "#aa44ff",
// // };
// // const boxColor = (lbl: string) => BOX_COLORS[lbl] ?? "#ff8800";

// // // ── AI results panel ─────────────────────────────────────────────────────────
// // function AiPanel({ detections, inferenceMs, peopleCount, densityPct, densityLevel,
// //                    intrusionDetected, intruderCount, frameCount, aiMode }: {
// //   detections: Detection[]; inferenceMs: number | null; peopleCount: number | null;
// //   densityPct: number | null; densityLevel: string | null;
// //   intrusionDetected: boolean | null; intruderCount: number | null;
// //   frameCount: number; aiMode: string;
// // }) {
// //   const summary: Record<string, number> = {};
// //   detections.forEach(d => { summary[d.label] = (summary[d.label] || 0) + 1; });
// //   const densityColor = densityLevel === "HIGH" ? "text-red-400"
// //     : densityLevel === "MEDIUM" ? "text-yellow-400" : "text-green-400";

// //   return (
// //     <Card className="bg-card border-border">
// //       <CardHeader className="pb-2 border-b border-border/60">
// //         <CardTitle className="text-sm flex items-center gap-2">
// //           <Cpu className="h-4 w-4 text-ai-glow" />
// //           AI Analysis
// //           {inferenceMs != null && <Badge variant="secondary" className="text-[10px] ml-auto">{inferenceMs}ms</Badge>}
// //           {frameCount > 0 && <span className="text-[10px] text-muted-foreground ml-1">#{frameCount}</span>}
// //         </CardTitle>
// //       </CardHeader>
// //       <CardContent className="p-3 space-y-3">
// //         {frameCount === 0 ? (
// //           <div className="text-center py-6 text-muted-foreground">
// //             <Bot className="h-8 w-8 mx-auto mb-2 opacity-30" />
// //             <p className="text-xs">Enable AI then play a video</p>
// //             <p className="text-[10px] mt-1 opacity-70">Overlay + results appear here</p>
// //           </div>
// //         ) : (
// //           <>
// //             {/* People count */}
// //             {aiMode === "people" && (
// //               <div className="p-2.5 rounded-lg bg-green-500/5 border border-green-500/20 space-y-2">
// //                 <div className="flex items-center justify-between">
// //                   <span className="text-xs font-medium flex items-center gap-1.5">
// //                     <Users className="h-3.5 w-3.5 text-green-400" />People
// //                   </span>
// //                   <span className="text-xl font-bold text-green-400">{peopleCount ?? 0}</span>
// //                 </div>
// //                 {densityPct != null && (
// //                   <>
// //                     <div className="flex justify-between text-[10px]">
// //                       <span className="text-muted-foreground">Crowd Density</span>
// //                       <span className={densityColor}>{densityPct}% — {densityLevel}</span>
// //                     </div>
// //                     <Progress value={densityPct} className="h-1.5" />
// //                   </>
// //                 )}
// //               </div>
// //             )}

// //             {/* Intrusion */}
// //             {aiMode === "intrusion" && intrusionDetected != null && (
// //               <div className={`p-2.5 rounded-lg border ${intrusionDetected
// //                 ? "bg-red-500/10 border-red-500/30" : "bg-green-500/5 border-green-500/20"}`}>
// //                 <div className="flex items-center gap-2">
// //                   <ShieldAlert className={`h-4 w-4 ${intrusionDetected ? "text-red-400 animate-pulse" : "text-green-400"}`} />
// //                   <span className={`text-sm font-semibold ${intrusionDetected ? "text-red-400" : "text-green-400"}`}>
// //                     {intrusionDetected ? `INTRUSION — ${intruderCount} object(s)` : "Zone Clear"}
// //                   </span>
// //                 </div>
// //               </div>
// //             )}

// //             {/* Object summary */}
// //             {Object.keys(summary).length > 0 && (
// //               <div className="space-y-1.5">
// //                 <p className="text-[10px] font-medium text-muted-foreground uppercase tracking-wide">Detected</p>
// //                 {Object.entries(summary).sort(([,a],[,b]) => b-a).map(([label, count]) => (
// //                   <div key={label} className="flex items-center gap-2">
// //                     <div className="w-2 h-2 rounded-full shrink-0" style={{ backgroundColor: boxColor(label) }} />
// //                     <span className="text-xs text-foreground capitalize flex-1">{label}</span>
// //                     <Badge variant="outline" className="text-[10px] h-4 px-1">{count}</Badge>
// //                   </div>
// //                 ))}
// //               </div>
// //             )}

// //             {detections.length === 0 && (
// //               <div className="flex items-center gap-2 text-green-400">
// //                 <CheckCircle2 className="h-4 w-4" />
// //                 <span className="text-xs">No objects detected</span>
// //               </div>
// //             )}

// //             {/* All detections scroll */}
// //             {detections.length > 0 && (
// //               <div className="max-h-32 overflow-y-auto space-y-0.5">
// //                 <p className="text-[10px] font-medium text-muted-foreground uppercase tracking-wide">
// //                   All ({detections.length})
// //                 </p>
// //                 {detections.map((d, i) => (
// //                   <div key={i} className="flex items-center gap-1.5 py-0.5">
// //                     <span className="text-[10px] text-muted-foreground w-4">{i+1}</span>
// //                     <span className="text-[10px] text-foreground capitalize flex-1">{d.label}</span>
// //                     <span className="text-[10px] font-mono" style={{ color: boxColor(d.label) }}>
// //                       {(d.confidence*100).toFixed(0)}%
// //                     </span>
// //                   </div>
// //                 ))}
// //               </div>
// //             )}
// //           </>
// //         )}
// //       </CardContent>
// //     </Card>
// //   );
// // }

// // // ── Main ─────────────────────────────────────────────────────────────────────
// // const VideoPlayer = () => {
// //   const [searchParams] = useSearchParams();
// //   const { toast } = useToast();
// //   const videoRef  = useRef<HTMLVideoElement>(null);
// //   const canvasRef = useRef<HTMLCanvasElement>(null);
// //   const hlsRef    = useRef<any>(null);
// //   const aiTimer   = useRef<ReturnType<typeof setInterval> | null>(null);

// //   const initRecId = searchParams.get("recording") ? parseInt(searchParams.get("recording")!) : null;
// //   const initCamId = searchParams.get("camera")    ? parseInt(searchParams.get("camera")!)    : null;
// //   const initLive  = searchParams.get("live") === "true";

// //   const [selectedRecId, setSelectedRecId] = useState<number | null>(initRecId);
// //   const [selectedCamId, setSelectedCamId] = useState<number | null>(initCamId);
// //   const [liveMode, setLiveMode]           = useState(initLive || !!initCamId);
// //   const [playing, setPlaying]             = useState(false);
// //   const [muted, setMuted]                 = useState(true);
// //   const [currentTime, setCurrentTime]     = useState(0);
// //   const [duration, setDuration]           = useState(0);
// //   const [aiEnabled, setAiEnabled]         = useState(false);
// //   const [aiMode, setAiMode]               = useState("detect");
// //   const [detections, setDetections]       = useState<Detection[]>([]);
// //   const [inferenceMs, setInferenceMs]     = useState<number | null>(null);
// //   const [peopleCount, setPeopleCount]     = useState<number | null>(null);
// //   const [densityPct, setDensityPct]       = useState<number | null>(null);
// //   const [densityLevel, setDensityLevel]   = useState<string | null>(null);
// //   const [intrusionDetected, setIntrusion] = useState<boolean | null>(null);
// //   const [intruderCount, setIntruderCount] = useState<number | null>(null);
// //   const [frameCount, setFrameCount]       = useState(0);

// //   const { data: cameras } = useCameras();
// //   const { data: recordings } = useRecordings(
// //     selectedCamId ? { camera_id: selectedCamId, limit: 100 } : { limit: 50 }
// //   );

// //   const currentRecording = recordings?.find(r => r.recording_id === selectedRecId);
// //   const currentCamera    = cameras?.find(c => c.camera_id === selectedCamId);

// //   // ── Load HLS live stream ───────────────────────────────────────────────────
// //   useEffect(() => {
// //     if (!liveMode || !selectedCamId) return;
// //     const video = videoRef.current;
// //     if (!video) return;
// //     hlsRef.current?.destroy();
// //     const token = tokenStore.get() ?? "";
// //     const url = `${API_BASE}/api/streaming/hls/${selectedCamId}/stream.m3u8?token=${encodeURIComponent(token)}`;

// //     if (video.canPlayType("application/vnd.apple.mpegurl")) {
// //       video.src = url; video.load(); video.play().catch(() => {});
// //       return () => { video.src = ""; };
// //     }
// //     let cancelled = false;
// //     import("hls.js").then(({ default: Hls }) => {
// //       if (cancelled || !videoRef.current) return;
// //       if (!Hls.isSupported()) return;
// //       const hls = new Hls({ enableWorker: false, lowLatencyMode: true, backBufferLength: 30 });
// //       hlsRef.current = hls;
// //       hls.loadSource(url);
// //       hls.attachMedia(videoRef.current);
// //       hls.on(Hls.Events.MANIFEST_PARSED, () => videoRef.current?.play().catch(() => {}));
// //       hls.on(Hls.Events.ERROR, (_: any, d: any) => {
// //         if (d.fatal) toast({ title: "Stream not ready yet — recorder starts in up to 60s", variant: "destructive" });
// //       });
// //     });
// //     return () => { cancelled = true; hlsRef.current?.destroy(); };
// //   }, [liveMode, selectedCamId]);

// //   // ── Load MP4 recording ─────────────────────────────────────────────────────
// //   useEffect(() => {
// //     if (liveMode || !selectedRecId) return;
// //     const video = videoRef.current;
// //     if (!video) return;
// //     hlsRef.current?.destroy();
// //     video.src = getStreamUrl(selectedRecId);
// //     video.load();
// //   }, [selectedRecId, liveMode]);

// //   const togglePlay = () => {
// //     const v = videoRef.current;
// //     if (!v) return;
// //     if (v.paused) { v.play(); setPlaying(true); } else { v.pause(); setPlaying(false); }
// //   };
// //   const skip = (s: number) => { if (videoRef.current) videoRef.current.currentTime += s; };

// //   // ── AI inference ────────────────────────────────────────────────────────────
// //   const runAi = useCallback(async () => {
// //     const v = videoRef.current;
// //     const canvas = canvasRef.current;
// //     if (!v || !canvas || v.paused || v.ended || v.videoWidth === 0) return;

// //     const tmp = document.createElement("canvas");
// //     tmp.width = v.videoWidth; tmp.height = v.videoHeight;
// //     tmp.getContext("2d")?.drawImage(v, 0, 0);

// //     const mode = AI_MODES.find(m => m.key === aiMode)!;
// //     tmp.toBlob(async (blob) => {
// //       if (!blob) return;
// //       try {
// //         const res = await detectObjects(blob, { conf: 0.35, endpoint: mode.endpoint });
// //         setDetections(res.detections);
// //         setInferenceMs(res.inference_ms);
// //         setFrameCount(c => c + 1);
// //         if (res.people_count !== undefined)      setPeopleCount(res.people_count);
// //         if (res.density_percent !== undefined)   setDensityPct(res.density_percent);
// //         if (res.density_level !== undefined)     setDensityLevel(res.density_level);
// //         if (res.intrusion_detected !== undefined){ setIntrusion(res.intrusion_detected); setIntruderCount(res.intruder_count ?? 0); }

// //         // Draw overlay
// //         canvas.width = tmp.width; canvas.height = tmp.height;
// //         const ctx = canvas.getContext("2d")!;
// //         ctx.clearRect(0, 0, canvas.width, canvas.height);
// //         res.detections.forEach((d: Detection) => {
// //           const [x1,y1,x2,y2] = d.bbox;
// //           const col = boxColor(d.label);
// //           ctx.strokeStyle = col; ctx.lineWidth = 2;
// //           ctx.strokeRect(x1,y1,x2-x1,y2-y1);
// //           const lbl = `${d.label} ${(d.confidence*100).toFixed(0)}%`;
// //           ctx.font = "bold 12px system-ui";
// //           const tw = ctx.measureText(lbl).width + 6;
// //           ctx.fillStyle = col + "cc"; ctx.fillRect(x1,y1-18,tw,18);
// //           ctx.fillStyle = "#000"; ctx.fillText(lbl,x1+3,y1-4);
// //         });
// //         // Density bar
// //         if (mode.key === "people" && res.people_count) {
// //           const dp = res.density_percent ?? 0;
// //           const col = res.density_level==="HIGH"?"#ef4444":res.density_level==="MEDIUM"?"#f59e0b":"#22c55e";
// //           ctx.fillStyle = col+"99"; ctx.fillRect(0,canvas.height-8,(dp/100)*canvas.width,8);
// //           ctx.fillStyle="#ffffffdd"; ctx.font="bold 13px system-ui";
// //           ctx.fillText(`${res.people_count} people · ${res.density_level} ${dp}%`,8,canvas.height-12);
// //         }
// //       } catch {}
// //     }, "image/jpeg", 0.75);
// //   }, [aiMode]);

// //   useEffect(() => {
// //     if (aiEnabled) { aiTimer.current = setInterval(runAi, 2000); }
// //     else {
// //       if (aiTimer.current) clearInterval(aiTimer.current);
// //       setDetections([]); setInferenceMs(null); setPeopleCount(null);
// //       setDensityPct(null); setDensityLevel(null); setIntrusion(null); setIntruderCount(null);
// //       setFrameCount(0);
// //       const ctx = canvasRef.current?.getContext("2d");
// //       if (ctx && canvasRef.current) ctx.clearRect(0,0,canvasRef.current.width,canvasRef.current.height);
// //     }
// //     return () => { if (aiTimer.current) clearInterval(aiTimer.current); };
// //   }, [aiEnabled, runAi]);

// //   return (
// //     <AppLayout>
// //       <div className="space-y-4">
// //         <div className="flex items-center justify-between">
// //           <div>
// //             <h1 className="text-2xl font-bold text-foreground">Video Player</h1>
// //             <p className="text-sm text-muted-foreground">
// //               {liveMode ? "Live stream (HLS from recorder)" : "Recording playback"} · Real-time AI overlay
// //             </p>
// //           </div>
// //           <div className="flex gap-2">
// //             <Button variant={liveMode ? "default" : "outline"} size="sm" onClick={() => setLiveMode(true)}>Live</Button>
// //             <Button variant={!liveMode ? "default" : "outline"} size="sm" onClick={() => setLiveMode(false)}>Recordings</Button>
// //           </div>
// //         </div>

// //         <div className="grid xl:grid-cols-4 gap-4">
// //           {/* ── Video area ─────────────────────────────────────────────────── */}
// //           <div className="xl:col-span-3 space-y-3">
// //             <div className="relative bg-black rounded-xl overflow-hidden aspect-video">
// //               <video ref={videoRef} className="w-full h-full object-contain"
// //                 onPlay={() => setPlaying(true)} onPause={() => setPlaying(false)}
// //                 onTimeUpdate={e => setCurrentTime((e.target as HTMLVideoElement).currentTime)}
// //                 onLoadedMetadata={e => setDuration((e.target as HTMLVideoElement).duration)}
// //                 muted={muted} playsInline />
// //               {aiEnabled && (
// //                 <canvas ref={canvasRef} className="absolute inset-0 w-full h-full pointer-events-none"
// //                   style={{ objectFit: "contain" }} />
// //               )}
// //               {/* AI badge overlay */}
// //               {aiEnabled && (
// //                 <div className="absolute top-2 right-2 flex flex-col items-end gap-1">
// //                   <Badge className="bg-black/70 text-ai-glow border-ai-glow/30 gap-1 text-[10px]">
// //                     <Cpu className="h-2.5 w-2.5 animate-pulse" />
// //                     {AI_MODES.find(m => m.key === aiMode)?.label}
// //                     {inferenceMs ? ` · ${inferenceMs}ms` : ""}
// //                   </Badge>
// //                   {aiMode === "people" && peopleCount !== null && (
// //                     <Badge className="bg-black/70 text-green-400 border-green-500/30 text-[10px]">
// //                       <Users className="h-2.5 w-2.5 mr-1" />{peopleCount} people
// //                     </Badge>
// //                   )}
// //                   {intrusionDetected && (
// //                     <Badge className="bg-red-500/80 text-white text-[10px] animate-pulse">
// //                       <ShieldAlert className="h-2.5 w-2.5 mr-1" />INTRUSION
// //                     </Badge>
// //                   )}
// //                 </div>
// //               )}
// //               {/* No source */}
// //               {!selectedRecId && !selectedCamId && (
// //                 <div className="absolute inset-0 flex flex-col items-center justify-center text-muted-foreground">
// //                   <Camera className="h-14 w-14 mb-3 opacity-20" />
// //                   <p className="text-sm">Select a camera or recording →</p>
// //                 </div>
// //               )}
// //             </div>

// //             {/* Progress bar */}
// //             {!liveMode && duration > 0 && (
// //               <div className="flex items-center gap-2">
// //                 <span className="text-[10px] font-mono text-muted-foreground w-10">
// //                   {Math.floor(currentTime/60)}:{String(Math.floor(currentTime%60)).padStart(2,"0")}
// //                 </span>
// //                 <input type="range" min={0} max={duration} value={currentTime} step={0.5}
// //                   className="flex-1 accent-primary h-1.5"
// //                   onChange={e => { if (videoRef.current) videoRef.current.currentTime = parseFloat(e.target.value); }} />
// //                 <span className="text-[10px] font-mono text-muted-foreground w-10 text-right">
// //                   {Math.floor(duration/60)}:{String(Math.floor(duration%60)).padStart(2,"0")}
// //                 </span>
// //               </div>
// //             )}

// //             {/* Controls */}
// //             <div className="flex items-center gap-2 flex-wrap">
// //               {!liveMode && <Button size="icon" variant="ghost" className="h-8 w-8" onClick={() => skip(-10)}><SkipBack className="h-4 w-4" /></Button>}
// //               <Button size="icon" className="h-8 w-8" onClick={togglePlay}>
// //                 {playing ? <Pause className="h-4 w-4" /> : <Play className="h-4 w-4" />}
// //               </Button>
// //               {!liveMode && <Button size="icon" variant="ghost" className="h-8 w-8" onClick={() => skip(10)}><SkipForward className="h-4 w-4" /></Button>}
// //               <Button size="icon" variant="ghost" className="h-8 w-8" onClick={() => setMuted(!muted)}>
// //                 {muted ? <VolumeX className="h-4 w-4" /> : <Volume2 className="h-4 w-4" />}
// //               </Button>
// //               <Separator orientation="vertical" className="h-6 mx-1" />
// //               <Select value={aiMode} onValueChange={v => { setAiMode(v); setFrameCount(0); }}>
// //                 <SelectTrigger className="h-8 w-44 text-xs">
// //                   <Cpu className="h-3 w-3 mr-1.5 text-ai-glow shrink-0" /><SelectValue />
// //                 </SelectTrigger>
// //                 <SelectContent>
// //                   {AI_MODES.map(m => <SelectItem key={m.key} value={m.key}>{m.label}</SelectItem>)}
// //                 </SelectContent>
// //               </Select>
// //               <Button variant={aiEnabled ? "default" : "outline"} size="sm" className="h-8 gap-1.5"
// //                 onClick={() => setAiEnabled(!aiEnabled)}>
// //                 <Bot className="h-3.5 w-3.5" />{aiEnabled ? "AI ON" : "Enable AI"}
// //               </Button>
// //               {selectedRecId && (
// //                 <Button size="sm" variant="ghost" className="h-8 gap-1 ml-auto" asChild>
// //                   <a href={getDownloadUrl(selectedRecId)} download><Download className="h-3.5 w-3.5" />Download</a>
// //                 </Button>
// //               )}
// //             </div>

// //             {/* Info card */}
// //             {(currentRecording || (currentCamera && liveMode)) && (
// //               <Card className="bg-card border-border">
// //                 <CardContent className="p-3 grid grid-cols-2 sm:grid-cols-4 gap-2 text-xs">
// //                   {currentRecording && !liveMode && <>
// //                     <div><p className="text-muted-foreground">Camera</p><p className="font-medium">{currentRecording.camera_name}</p></div>
// //                     <div><p className="text-muted-foreground">IP Address</p><p className="font-mono">{currentRecording.ip_address || "—"}</p></div>
// //                     <div><p className="text-muted-foreground">Started</p><p className="font-mono">{format(new Date(currentRecording.start_timestamp),"dd MMM HH:mm:ss")}</p></div>
// //                     <div><p className="text-muted-foreground">Duration</p><p>{currentRecording.duration_seconds ? `${Math.floor(currentRecording.duration_seconds/60)}m ${currentRecording.duration_seconds%60}s` : "—"}</p></div>
// //                     <div><p className="text-muted-foreground">Codec</p><p>{currentRecording.video_codec} · {currentRecording.resolution_width}×{currentRecording.resolution_height}</p></div>
// //                     {currentRecording.gps_speed_kmh && <div><p className="text-muted-foreground">Speed</p><p>🚂 {currentRecording.gps_speed_kmh} km/h</p></div>}
// //                   </>}
// //                   {currentCamera && liveMode && <>
// //                     <div><p className="text-muted-foreground">Camera</p><p className="font-medium">{currentCamera.camera_name}</p></div>
// //                     <div><p className="text-muted-foreground">IP Address</p><p className="font-mono">{currentCamera.ip_address || "—"}</p></div>
// //                     <div><p className="text-muted-foreground">Location</p><p>{currentCamera.location_description || currentCamera.camera_type}</p></div>
// //                     <div><p className="text-muted-foreground">UDP Port</p><p className="font-mono">{5000 + currentCamera.camera_id * 2} (mNVR core)</p></div>
// //                   </>}
// //                 </CardContent>
// //               </Card>
// //             )}
// //           </div>

// //           {/* ── Sidebar ────────────────────────────────────────────────────── */}
// //           <div className="space-y-3">
// //             <AiPanel detections={detections} inferenceMs={inferenceMs}
// //               peopleCount={peopleCount} densityPct={densityPct} densityLevel={densityLevel}
// //               intrusionDetected={intrusionDetected} intruderCount={intruderCount}
// //               frameCount={frameCount} aiMode={aiMode} />

// //             {/* Cameras */}
// //             <Card className="bg-card border-border">
// //               <CardHeader className="pb-2"><CardTitle className="text-xs text-muted-foreground uppercase tracking-wide">Cameras</CardTitle></CardHeader>
// //               <CardContent className="p-2 pt-0 max-h-36 overflow-y-auto space-y-0.5">
// //                 {cameras?.map(cam => (
// //                   <button key={cam.camera_id}
// //                     onClick={() => { setSelectedCamId(cam.camera_id); setLiveMode(true); setSelectedRecId(null); }}
// //                     className={`w-full text-left px-2 py-1.5 rounded text-xs flex items-start gap-2 transition-colors hover:bg-muted/50 ${selectedCamId===cam.camera_id?"bg-primary/10 border border-primary/30":""}`}>
// //                     <div className={`w-1.5 h-1.5 rounded-full mt-1 shrink-0 ${cam.status==="ACTIVE"?"bg-green-400":"bg-gray-400"}`} />
// //                     <div className="min-w-0">
// //                       <p className="truncate font-medium">{cam.camera_name}</p>
// //                       <p className="font-mono text-[10px] text-muted-foreground">{cam.ip_address || "—"}</p>
// //                     </div>
// //                   </button>
// //                 ))}
// //               </CardContent>
// //             </Card>

// //             {/* Recordings */}
// //             {!liveMode && (
// //               <Card className="bg-card border-border">
// //                 <CardHeader className="pb-2"><CardTitle className="text-xs text-muted-foreground uppercase tracking-wide">Recordings {recordings?.length ? `(${recordings.length})` : ""}</CardTitle></CardHeader>
// //                 <CardContent className="p-2 pt-0 max-h-64 overflow-y-auto space-y-0.5">
// //                   {!recordings?.length && <p className="text-[11px] text-muted-foreground text-center py-4">Recordings appear after first 60s segment</p>}
// //                   {recordings?.map(rec => (
// //                     <button key={rec.recording_id}
// //                       onClick={() => { setSelectedRecId(rec.recording_id); setLiveMode(false); }}
// //                       className={`w-full text-left px-2 py-1.5 rounded text-xs hover:bg-muted/50 transition-colors border ${selectedRecId===rec.recording_id?"bg-primary/10 border-primary/30":"border-transparent"}`}>
// //                       <p className="font-medium truncate">{rec.camera_name || `Camera ${rec.camera_id}`}</p>
// //                       <p className="text-[10px] text-muted-foreground">
// //                         {format(new Date(rec.start_timestamp),"dd MMM HH:mm")}
// //                         {rec.duration_seconds ? ` · ${Math.floor(rec.duration_seconds/60)}m` : ""}
// //                       </p>
// //                       {rec.ip_address && <p className="text-[10px] font-mono text-muted-foreground">{rec.ip_address}</p>}
// //                     </button>
// //                   ))}
// //                 </CardContent>
// //               </Card>
// //             )}
// //           </div>
// //         </div>
// //       </div>
// //     </AppLayout>
// //   );
// // };
// // export default VideoPlayer;

// import { useState, useRef, useEffect, useCallback } from "react";
// import { useSearchParams } from "react-router-dom";
// import {
//   Play,
//   Pause,
//   Download,
//   SkipBack,
//   SkipForward,
//   Volume2,
//   VolumeX,
//   Bot,
//   Camera,
//   Network,
//   Users,
//   ShieldAlert,
//   Eye,
//   Cpu,
//   CheckCircle2,
// } from "lucide-react";
// import { AppLayout } from "@/components/layout/AppLayout";
// import { Button } from "@/components/ui/button";
// import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
// import { Badge } from "@/components/ui/badge";
// import { Progress } from "@/components/ui/progress";
// import { Separator } from "@/components/ui/separator";
// import {
//   Select,
//   SelectContent,
//   SelectItem,
//   SelectTrigger,
//   SelectValue,
// } from "@/components/ui/select";
// import { useRecordings } from "@/hooks/use-recordings";
// import { useCameras } from "@/hooks/use-cameras";
// import {
//   getStreamUrl,
//   getDownloadUrl,
//   detectObjects,
//   type Detection,
// } from "@/lib/api";
// import { useToast } from "@/hooks/use-toast";
// import { format } from "date-fns";

// const AI_MODES = [
//   {
//     key: "detect",
//     label: "Object Detection",
//     endpoint: "/api/ai/detect",
//     color: "#ff8800",
//   },
//   {
//     key: "people",
//     label: "People Count",
//     endpoint: "/api/ai/people-count",
//     color: "#00ff88",
//   },
//   {
//     key: "intrusion",
//     label: "Intrusion Detection",
//     endpoint: "/api/ai/intrusion",
//     color: "#ff4444",
//   },
// ];

// const BOX_COLORS: Record<string, string> = {
//   person: "#00ff88",
//   fire: "#ff0000",
//   smoke: "#ff6600",
//   "cell phone": "#ffff00",
//   car: "#ff8800",
//   truck: "#ff8800",
//   cow: "#aa44ff",
//   dog: "#aa44ff",
//   cat: "#aa44ff",
// };
// const boxColor = (lbl: string) => BOX_COLORS[lbl] ?? "#ff8800";

// // ── AI panel ──────────────────────────────────────────────────────────────────
// function AiPanel({
//   detections,
//   inferenceMs,
//   peopleCount,
//   densityPct,
//   densityLevel,
//   intrusionDetected,
//   intruderCount,
//   frameCount,
//   aiMode,
// }: {
//   detections: Detection[];
//   inferenceMs: number | null;
//   peopleCount: number | null;
//   densityPct: number | null;
//   densityLevel: string | null;
//   intrusionDetected: boolean | null;
//   intruderCount: number | null;
//   frameCount: number;
//   aiMode: string;
// }) {
//   const summary: Record<string, number> = {};
//   detections.forEach((d) => {
//     summary[d.label] = (summary[d.label] || 0) + 1;
//   });
//   const densityColor =
//     densityLevel === "HIGH"
//       ? "text-red-500 dark:text-red-400"
//       : densityLevel === "MEDIUM"
//         ? "text-yellow-600 dark:text-yellow-400"
//         : "text-green-600 dark:text-green-400";

//   return (
//     <Card className="bg-card border-border h-full">
//       <CardHeader className="pb-2 border-b border-border/60">
//         <CardTitle className="text-sm flex items-center gap-2 text-foreground">
//           <Cpu className="h-4 w-4 text-primary" />
//           AI Analysis
//           {inferenceMs != null && (
//             <Badge variant="secondary" className="text-[10px] ml-auto">
//               {inferenceMs}ms
//             </Badge>
//           )}
//           {frameCount > 0 && (
//             <span className="text-[10px] text-muted-foreground">
//               #{frameCount}
//             </span>
//           )}
//         </CardTitle>
//       </CardHeader>
//       <CardContent className="p-3 space-y-3">
//         {frameCount === 0 ? (
//           <div className="text-center py-6 text-muted-foreground">
//             <Bot className="h-8 w-8 mx-auto mb-2 opacity-30" />
//             <p className="text-xs text-foreground">
//               Enable AI then play a recording
//             </p>
//             <p className="text-[10px] mt-1 text-muted-foreground">
//               Overlay + results appear here
//             </p>
//           </div>
//         ) : (
//           <>
//             {/* People */}
//             {aiMode === "people" && (
//               <div className="p-2.5 rounded-lg bg-green-500/10 border border-green-500/20 space-y-2">
//                 <div className="flex items-center justify-between">
//                   <span className="text-xs font-medium text-foreground flex items-center gap-1.5">
//                     <Users className="h-3.5 w-3.5 text-green-600 dark:text-green-400" />
//                     People
//                   </span>
//                   <span className="text-xl font-bold text-green-600 dark:text-green-400">
//                     {peopleCount ?? 0}
//                   </span>
//                 </div>
//                 {densityPct != null && (
//                   <>
//                     <div className="flex justify-between text-[10px]">
//                       <span className="text-muted-foreground">Density</span>
//                       <span className={densityColor}>
//                         {densityPct}% — {densityLevel}
//                       </span>
//                     </div>
//                     <Progress value={densityPct} className="h-1.5" />
//                   </>
//                 )}
//               </div>
//             )}
//             {/* Intrusion */}
//             {aiMode === "intrusion" && intrusionDetected != null && (
//               <div
//                 className={`p-2.5 rounded-lg border ${intrusionDetected ? "bg-red-500/10 border-red-500/30" : "bg-green-500/5 border-green-500/20"}`}
//               >
//                 <div className="flex items-center gap-2">
//                   <ShieldAlert
//                     className={`h-4 w-4 ${intrusionDetected ? "text-red-500 dark:text-red-400 animate-pulse" : "text-green-600 dark:text-green-400"}`}
//                   />
//                   <span
//                     className={`text-sm font-semibold ${intrusionDetected ? "text-red-600 dark:text-red-400" : "text-green-700 dark:text-green-400"}`}
//                   >
//                     {intrusionDetected
//                       ? `INTRUSION — ${intruderCount} object(s)`
//                       : "Zone Clear"}
//                   </span>
//                 </div>
//               </div>
//             )}
//             {/* Summary */}
//             {Object.keys(summary).length > 0 && (
//               <div className="space-y-1.5">
//                 <p className="text-[10px] font-medium text-muted-foreground uppercase tracking-wide">
//                   Detected
//                 </p>
//                 {Object.entries(summary)
//                   .sort(([, a], [, b]) => b - a)
//                   .map(([label, count]) => (
//                     <div key={label} className="flex items-center gap-2">
//                       <div
//                         className="w-2 h-2 rounded-full shrink-0"
//                         style={{ backgroundColor: boxColor(label) }}
//                       />
//                       <span className="text-xs text-foreground capitalize flex-1">
//                         {label}
//                       </span>
//                       <Badge variant="outline" className="text-[10px] h-4 px-1">
//                         {count}
//                       </Badge>
//                     </div>
//                   ))}
//               </div>
//             )}
//             {detections.length === 0 && (
//               <div className="flex items-center gap-2 text-green-600 dark:text-green-400">
//                 <CheckCircle2 className="h-4 w-4" />
//                 <span className="text-xs">No objects detected</span>
//               </div>
//             )}
//             {/* All detections */}
//             {detections.length > 0 && (
//               <div className="max-h-32 overflow-y-auto space-y-0.5">
//                 <p className="text-[10px] font-medium text-muted-foreground uppercase tracking-wide">
//                   All ({detections.length})
//                 </p>
//                 {detections.map((d, i) => (
//                   <div key={i} className="flex items-center gap-1.5 py-0.5">
//                     <span className="text-[10px] text-muted-foreground w-4">
//                       {i + 1}
//                     </span>
//                     <span className="text-[10px] text-foreground capitalize flex-1">
//                       {d.label}
//                     </span>
//                     <span
//                       className="text-[10px] font-mono"
//                       style={{ color: boxColor(d.label) }}
//                     >
//                       {(d.confidence * 100).toFixed(0)}%
//                     </span>
//                   </div>
//                 ))}
//               </div>
//             )}
//           </>
//         )}
//       </CardContent>
//     </Card>
//   );
// }

// // ── Main ──────────────────────────────────────────────────────────────────────
// const VideoPlayer = () => {
//   const [searchParams] = useSearchParams();
//   const { toast } = useToast();
//   const videoRef = useRef<HTMLVideoElement>(null);
//   const canvasRef = useRef<HTMLCanvasElement>(null);
//   const hlsRef = useRef<any>(null);
//   const aiTimer = useRef<ReturnType<typeof setInterval> | null>(null);

//   const initRecId = searchParams.get("recording")
//     ? parseInt(searchParams.get("recording")!)
//     : null;
//   const initCamId = searchParams.get("camera")
//     ? parseInt(searchParams.get("camera")!)
//     : null;

//   const [selectedRecId, setSelectedRecId] = useState<number | null>(initRecId);
//   const [filterCamId, setFilterCamId] = useState<number | null>(initCamId);
//   const [playing, setPlaying] = useState(false);
//   const [muted, setMuted] = useState(true);
//   const [curTime, setCurTime] = useState(0);
//   const [duration, setDuration] = useState(0);

//   // AI
//   const [aiEnabled, setAiEnabled] = useState(false);
//   const [aiMode, setAiMode] = useState("detect");
//   const [detections, setDetections] = useState<Detection[]>([]);
//   const [inferenceMs, setInferenceMs] = useState<number | null>(null);
//   const [peopleCount, setPeopleCount] = useState<number | null>(null);
//   const [densityPct, setDensityPct] = useState<number | null>(null);
//   const [densityLevel, setDensityLevel] = useState<string | null>(null);
//   const [intrusionDetected, setIntrusion] = useState<boolean | null>(null);
//   const [intruderCount, setIntruderCount] = useState<number | null>(null);
//   const [frameCount, setFrameCount] = useState(0);

//   const { data: cameras } = useCameras();
//   const { data: recordings } = useRecordings(
//     filterCamId ? { camera_id: filterCamId, limit: 200 } : { limit: 100 },
//   );

//   const currentRec = recordings?.find((r) => r.recording_id === selectedRecId);

//   // ── Load MP4 recording ─────────────────────────────────────────────────────
//   useEffect(() => {
//     if (!selectedRecId) return;
//     const video = videoRef.current;
//     if (!video) return;
//     hlsRef.current?.destroy();
//     hlsRef.current = null;
//     video.src = getStreamUrl(selectedRecId);
//     video.load();
//     setPlaying(false);
//     setCurTime(0);
//     setDuration(0);
//   }, [selectedRecId]);

//   const togglePlay = () => {
//     const v = videoRef.current;
//     if (!v) return;
//     if (v.paused) {
//       v.play();
//       setPlaying(true);
//     } else {
//       v.pause();
//       setPlaying(false);
//     }
//   };
//   const skip = (s: number) => {
//     if (videoRef.current) videoRef.current.currentTime += s;
//   };

//   // ── AI inference ───────────────────────────────────────────────────────────
//   const runAi = useCallback(async () => {
//     const v = videoRef.current;
//     const canvas = canvasRef.current;
//     if (!v || !canvas || v.paused || v.ended || v.videoWidth === 0) return;
//     const tmp = document.createElement("canvas");
//     tmp.width = v.videoWidth;
//     tmp.height = v.videoHeight;
//     tmp.getContext("2d")?.drawImage(v, 0, 0);
//     const mode = AI_MODES.find((m) => m.key === aiMode)!;
//     tmp.toBlob(
//       async (blob) => {
//         if (!blob) return;
//         try {
//           const res = await detectObjects(blob, {
//             conf: 0.35,
//             endpoint: mode.endpoint,
//           });
//           setDetections(res.detections);
//           setInferenceMs(res.inference_ms);
//           setFrameCount((c) => c + 1);
//           if (res.people_count !== undefined) setPeopleCount(res.people_count);
//           if (res.density_percent !== undefined)
//             setDensityPct(res.density_percent);
//           if (res.density_level !== undefined)
//             setDensityLevel(res.density_level);
//           if (res.intrusion_detected !== undefined) {
//             setIntrusion(res.intrusion_detected);
//             setIntruderCount(res.intruder_count ?? 0);
//           }
//           canvas.width = tmp.width;
//           canvas.height = tmp.height;
//           const ctx = canvas.getContext("2d")!;
//           ctx.clearRect(0, 0, canvas.width, canvas.height);
//           res.detections.forEach((d: Detection) => {
//             const [x1, y1, x2, y2] = d.bbox;
//             const col = boxColor(d.label);
//             ctx.strokeStyle = col;
//             ctx.lineWidth = 2;
//             ctx.strokeRect(x1, y1, x2 - x1, y2 - y1);
//             const lbl = `${d.label} ${(d.confidence * 100).toFixed(0)}%`;
//             ctx.font = "bold 12px system-ui";
//             const tw = ctx.measureText(lbl).width + 6;
//             ctx.fillStyle = col + "cc";
//             ctx.fillRect(x1, y1 - 18, tw, 18);
//             ctx.fillStyle = "#000";
//             ctx.fillText(lbl, x1 + 3, y1 - 4);
//           });
//           if (mode.key === "people" && res.people_count) {
//             const dp = res.density_percent ?? 0;
//             const col =
//               res.density_level === "HIGH"
//                 ? "#ef4444"
//                 : res.density_level === "MEDIUM"
//                   ? "#f59e0b"
//                   : "#22c55e";
//             ctx.fillStyle = col + "99";
//             ctx.fillRect(0, canvas.height - 8, (dp / 100) * canvas.width, 8);
//             ctx.fillStyle = "#ffffffee";
//             ctx.font = "bold 13px system-ui";
//             ctx.fillText(
//               `${res.people_count} people · ${res.density_level} ${dp}%`,
//               8,
//               canvas.height - 12,
//             );
//           }
//         } catch {}
//       },
//       "image/jpeg",
//       0.75,
//     );
//   }, [aiMode]);

//   useEffect(() => {
//     if (aiEnabled) {
//       aiTimer.current = setInterval(runAi, 2000);
//     } else {
//       if (aiTimer.current) clearInterval(aiTimer.current);
//       setDetections([]);
//       setInferenceMs(null);
//       setPeopleCount(null);
//       setDensityPct(null);
//       setDensityLevel(null);
//       setIntrusion(null);
//       setIntruderCount(null);
//       setFrameCount(0);
//       const ctx = canvasRef.current?.getContext("2d");
//       if (ctx && canvasRef.current)
//         ctx.clearRect(0, 0, canvasRef.current.width, canvasRef.current.height);
//     }
//     return () => {
//       if (aiTimer.current) clearInterval(aiTimer.current);
//     };
//   }, [aiEnabled, runAi]);

//   return (
//     <AppLayout>
//       <div className="space-y-4">
//         <div>
//           <h1 className="text-2xl font-bold text-foreground">Video Player</h1>
//           <p className="text-sm text-muted-foreground">
//             Recording playback with real-time AI analysis
//           </p>
//         </div>

//         <div className="grid xl:grid-cols-4 gap-4">
//           {/* ── Video ──────────────────────────────────────────────────────── */}
//           <div className="xl:col-span-3 space-y-3">
//             <div className="relative bg-black rounded-xl overflow-hidden aspect-video">
//               <video
//                 ref={videoRef}
//                 className="w-full h-full object-contain"
//                 onPlay={() => setPlaying(true)}
//                 onPause={() => setPlaying(false)}
//                 onTimeUpdate={(e) =>
//                   setCurTime((e.target as HTMLVideoElement).currentTime)
//                 }
//                 onLoadedMetadata={(e) =>
//                   setDuration((e.target as HTMLVideoElement).duration)
//                 }
//                 muted={muted}
//                 playsInline
//               />
//               {aiEnabled && (
//                 <canvas
//                   ref={canvasRef}
//                   className="absolute inset-0 w-full h-full pointer-events-none"
//                   style={{ objectFit: "contain" }}
//                 />
//               )}
//               {aiEnabled && (
//                 <div className="absolute top-2 right-2 flex flex-col items-end gap-1">
//                   <Badge className="bg-black/70 text-white border-0 gap-1 text-[10px]">
//                     <Cpu className="h-2.5 w-2.5 animate-pulse" />
//                     {AI_MODES.find((m) => m.key === aiMode)?.label}
//                     {inferenceMs ? ` · ${inferenceMs}ms` : ""}
//                   </Badge>
//                   {aiMode === "people" && peopleCount !== null && (
//                     <Badge className="bg-black/70 text-green-400 border-0 text-[10px]">
//                       <Users className="h-2.5 w-2.5 mr-1" />
//                       {peopleCount} people
//                     </Badge>
//                   )}
//                   {intrusionDetected && (
//                     <Badge className="bg-red-500/80 text-white border-0 text-[10px] animate-pulse">
//                       <ShieldAlert className="h-2.5 w-2.5 mr-1" />
//                       INTRUSION
//                     </Badge>
//                   )}
//                 </div>
//               )}
//               {!selectedRecId && (
//                 <div className="absolute inset-0 flex flex-col items-center justify-center text-muted-foreground">
//                   <Camera className="h-14 w-14 mb-3 opacity-20" />
//                   <p className="text-sm text-foreground">
//                     Select a recording →
//                   </p>
//                 </div>
//               )}
//             </div>

//             {/* Seek bar */}
//             {duration > 0 && (
//               <div className="flex items-center gap-2">
//                 <span className="text-[10px] font-mono text-muted-foreground w-10">
//                   {Math.floor(curTime / 60)}:
//                   {String(Math.floor(curTime % 60)).padStart(2, "0")}
//                 </span>
//                 <input
//                   type="range"
//                   min={0}
//                   max={duration}
//                   value={curTime}
//                   step={0.5}
//                   className="flex-1 accent-primary h-1.5"
//                   onChange={(e) => {
//                     if (videoRef.current)
//                       videoRef.current.currentTime = parseFloat(e.target.value);
//                   }}
//                 />
//                 <span className="text-[10px] font-mono text-muted-foreground w-10 text-right">
//                   {Math.floor(duration / 60)}:
//                   {String(Math.floor(duration % 60)).padStart(2, "0")}
//                 </span>
//               </div>
//             )}

//             {/* Controls */}
//             <div className="flex items-center gap-2 flex-wrap">
//               <Button
//                 size="icon"
//                 variant="ghost"
//                 className="h-8 w-8"
//                 onClick={() => skip(-10)}
//               >
//                 <SkipBack className="h-4 w-4" />
//               </Button>
//               <Button size="icon" className="h-8 w-8" onClick={togglePlay}>
//                 {playing ? (
//                   <Pause className="h-4 w-4" />
//                 ) : (
//                   <Play className="h-4 w-4" />
//                 )}
//               </Button>
//               <Button
//                 size="icon"
//                 variant="ghost"
//                 className="h-8 w-8"
//                 onClick={() => skip(10)}
//               >
//                 <SkipForward className="h-4 w-4" />
//               </Button>
//               <Button
//                 size="icon"
//                 variant="ghost"
//                 className="h-8 w-8"
//                 onClick={() => setMuted(!muted)}
//               >
//                 {muted ? (
//                   <VolumeX className="h-4 w-4" />
//                 ) : (
//                   <Volume2 className="h-4 w-4" />
//                 )}
//               </Button>
//               <Separator orientation="vertical" className="h-6 mx-1" />
//               <Select
//                 value={aiMode}
//                 onValueChange={(v) => {
//                   setAiMode(v);
//                   setFrameCount(0);
//                 }}
//               >
//                 <SelectTrigger className="h-8 w-44 text-xs">
//                   <Cpu className="h-3 w-3 mr-1.5 text-primary shrink-0" />
//                   <SelectValue />
//                 </SelectTrigger>
//                 <SelectContent>
//                   {AI_MODES.map((m) => (
//                     <SelectItem key={m.key} value={m.key}>
//                       {m.label}
//                     </SelectItem>
//                   ))}
//                 </SelectContent>
//               </Select>
//               <Button
//                 variant={aiEnabled ? "default" : "outline"}
//                 size="sm"
//                 className="h-8 gap-1.5"
//                 onClick={() => setAiEnabled(!aiEnabled)}
//               >
//                 <Bot className="h-3.5 w-3.5" />
//                 {aiEnabled ? "AI ON" : "Enable AI"}
//               </Button>
//               {selectedRecId && (
//                 <Button
//                   size="sm"
//                   variant="ghost"
//                   className="h-8 gap-1 ml-auto"
//                   asChild
//                 >
//                   <a href={getDownloadUrl(selectedRecId)} download>
//                     <Download className="h-3.5 w-3.5" />
//                     Download
//                   </a>
//                 </Button>
//               )}
//             </div>

//             {/* Recording info */}
//             {currentRec && (
//               <Card className="bg-card border-border">
//                 <CardContent className="p-3 grid grid-cols-2 sm:grid-cols-4 gap-2 text-xs">
//                   <div>
//                     <p className="text-muted-foreground">Camera</p>
//                     <p className="font-medium text-foreground">
//                       {currentRec.camera_name}
//                     </p>
//                   </div>
//                   <div>
//                     <p className="text-muted-foreground">IP Address</p>
//                     <p className="font-mono text-foreground">
//                       {currentRec.ip_address || "—"}
//                     </p>
//                   </div>
//                   <div>
//                     <p className="text-muted-foreground">Started</p>
//                     <p className="font-mono text-foreground">
//                       {format(
//                         new Date(currentRec.start_timestamp),
//                         "dd MMM HH:mm:ss",
//                       )}
//                     </p>
//                   </div>
//                   <div>
//                     <p className="text-muted-foreground">Duration</p>
//                     <p className="text-foreground">
//                       {currentRec.duration_seconds
//                         ? `${Math.floor(currentRec.duration_seconds / 60)}m ${currentRec.duration_seconds % 60}s`
//                         : "—"}
//                     </p>
//                   </div>
//                   <div>
//                     <p className="text-muted-foreground">Codec</p>
//                     <p className="text-foreground">
//                       {currentRec.video_codec} · {currentRec.resolution_width}×
//                       {currentRec.resolution_height}
//                     </p>
//                   </div>
//                   <div>
//                     <p className="text-muted-foreground">Location</p>
//                     <p className="text-foreground truncate">
//                       {currentRec.location_description || "—"}
//                     </p>
//                   </div>
//                   {currentRec.gps_speed_kmh && (
//                     <div>
//                       <p className="text-muted-foreground">Speed</p>
//                       <p className="text-foreground">
//                         🚂 {currentRec.gps_speed_kmh} km/h
//                       </p>
//                     </div>
//                   )}
//                 </CardContent>
//               </Card>
//             )}
//           </div>

//           {/* ── Sidebar ────────────────────────────────────────────────────── */}
//           <div className="space-y-3">
//             <AiPanel
//               detections={detections}
//               inferenceMs={inferenceMs}
//               peopleCount={peopleCount}
//               densityPct={densityPct}
//               densityLevel={densityLevel}
//               intrusionDetected={intrusionDetected}
//               intruderCount={intruderCount}
//               frameCount={frameCount}
//               aiMode={aiMode}
//             />

//             {/* Camera filter */}
//             <Card className="bg-card border-border">
//               <CardHeader className="pb-2">
//                 <CardTitle className="text-xs text-muted-foreground uppercase tracking-wide">
//                   Filter by Camera
//                 </CardTitle>
//               </CardHeader>
//               <CardContent className="p-2 pt-0 max-h-36 overflow-y-auto space-y-0.5">
//                 <button
//                   onClick={() => {
//                     setFilterCamId(null);
//                     setSelectedRecId(null);
//                   }}
//                   className={`w-full text-left px-2 py-1.5 rounded text-xs transition-colors hover:bg-muted/50 ${!filterCamId ? "bg-primary/10 border border-primary/30" : "border border-transparent"}`}
//                 >
//                   <p className="font-medium text-foreground">All cameras</p>
//                 </button>
//                 {cameras?.map((cam) => (
//                   <button
//                     key={cam.camera_id}
//                     onClick={() => {
//                       setFilterCamId(cam.camera_id);
//                       setSelectedRecId(null);
//                     }}
//                     className={`w-full text-left px-2 py-1.5 rounded text-xs transition-colors hover:bg-muted/50 ${filterCamId === cam.camera_id ? "bg-primary/10 border border-primary/30" : "border border-transparent"}`}
//                   >
//                     <p className="font-medium truncate text-foreground">
//                       {cam.camera_name}
//                     </p>
//                     <p className="font-mono text-[10px] text-muted-foreground">
//                       {cam.ip_address || "—"}
//                     </p>
//                   </button>
//                 ))}
//               </CardContent>
//             </Card>

//             {/* Recordings list */}
//             <Card className="bg-card border-border">
//               <CardHeader className="pb-2">
//                 <CardTitle className="text-xs text-muted-foreground uppercase tracking-wide">
//                   Recordings{" "}
//                   {recordings?.length ? `(${recordings.length})` : ""}
//                 </CardTitle>
//               </CardHeader>
//               <CardContent className="p-2 pt-0 max-h-96 overflow-y-auto space-y-0.5">
//                 {!recordings?.length && (
//                   <p className="text-[11px] text-muted-foreground text-center py-4">
//                     No recordings yet — recorder generates files every 60s
//                   </p>
//                 )}
//                 {recordings?.map((rec) => (
//                   <button
//                     key={rec.recording_id}
//                     onClick={() => setSelectedRecId(rec.recording_id)}
//                     className={`w-full text-left px-2 py-1.5 rounded text-xs hover:bg-muted/50 transition-colors border ${selectedRecId === rec.recording_id ? "bg-primary/10 border-primary/30" : "border-transparent"}`}
//                   >
//                     <p className="font-medium truncate text-foreground">
//                       {rec.camera_name || `Camera ${rec.camera_id}`}
//                     </p>
//                     <p className="text-[10px] text-muted-foreground font-mono">
//                       {format(new Date(rec.start_timestamp), "dd MMM HH:mm")}
//                       {rec.duration_seconds
//                         ? ` · ${Math.floor(rec.duration_seconds / 60)}m`
//                         : ""}
//                     </p>
//                     {rec.ip_address && (
//                       <p className="text-[10px] font-mono text-muted-foreground">
//                         {rec.ip_address}
//                       </p>
//                     )}
//                   </button>
//                 ))}
//               </CardContent>
//             </Card>
//           </div>
//         </div>
//       </div>
//     </AppLayout>
//   );
// };

// export default VideoPlayer;

import { useState, useRef, useEffect, useCallback, useMemo } from "react";
import { useSearchParams } from "react-router-dom";
import {
  Play,
  Pause,
  Download,
  SkipBack,
  SkipForward,
  Volume2,
  VolumeX,
  Bot,
  Camera,
  Network,
  Users,
  ShieldAlert,
  Cpu,
  CheckCircle2,
  ChevronLeft,
  ChevronRight,
  Calendar,
  Clock,
  ZoomIn,
  ZoomOut,
  AlertCircle,
} from "lucide-react";
import { AppLayout } from "@/components/layout/AppLayout";
import { Button } from "@/components/ui/button";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import { Badge } from "@/components/ui/badge";
import { Progress } from "@/components/ui/progress";
import { Separator } from "@/components/ui/separator";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import { useRecordings, type Recording } from "@/hooks/use-recordings";
import { useCameras } from "@/hooks/use-cameras";
import { useEvents, type EventRow } from "@/hooks/use-events";
import {
  getStreamUrl,
  getDownloadUrl,
  detectObjects,
  type Detection,
} from "@/lib/api";
import { useToast } from "@/hooks/use-toast";
import { Popover, PopoverContent, PopoverTrigger } from "@/components/ui/popover";
import { Calendar as DatePickerCalendar } from "@/components/ui/calendar";
import {
  format,
  startOfDay,
  addDays,
  subDays,
  differenceInSeconds,
  parseISO,
} from "date-fns";

const AI_MODES = [
  {
    key: "detect",
    label: "Object Detection",
    endpoint: "/api/ai/detect",
    color: "#ff8800",
  },
  {
    key: "people",
    label: "People Count",
    endpoint: "/api/ai/people-count",
    color: "#00ff88",
  },
  {
    key: "intrusion",
    label: "Intrusion Detection",
    endpoint: "/api/ai/intrusion",
    color: "#ff4444",
  },
];

const BOX_COLORS: Record<string, string> = {
  person: "#00ff88",
  fire: "#ff0000",
  smoke: "#ff6600",
  "cell phone": "#ffff00",
  car: "#ff8800",
  truck: "#ff8800",
  cow: "#aa44ff",
  dog: "#aa44ff",
  cat: "#aa44ff",
};
const boxColor = (lbl: string) => BOX_COLORS[lbl] ?? "#ff8800";

// ── Helpers ────────────────────────────────────────────────────────────────────
const SECS_IN_DAY = 86400;

function secsToHMS(s: number) {
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = Math.floor(s % 60);
  return `${String(h).padStart(2, "0")}:${String(m).padStart(2, "0")}:${String(sec).padStart(2, "0")}`;
}

function clamp(v: number, min: number, max: number) {
  return Math.max(min, Math.min(max, v));
}

// ── Timeline bar ──────────────────────────────────────────────────────────────
interface TimelineProps {
  recordings: Recording[];
  events: EventRow[];
  selectedId: number | null;
  currentSec: number; // seconds from midnight of viewDate
  viewDate: Date;
  zoomHours: number; // visible window in hours (1–24)
  offsetSec: number; // seconds from midnight at left edge of view
  onSeek: (secFromMidnight: number) => void;
  onSelectRecording: (rec: Recording) => void;
  onSelectEvent?: (event: EventRow) => void;
}

function Timeline({
  recordings,
  events,
  selectedId,
  currentSec,
  viewDate,
  zoomHours,
  offsetSec,
  onSeek,
  onSelectRecording,
  onSelectEvent,
}: TimelineProps) {
  const barRef = useRef<HTMLDivElement>(null);
  const windowSecs = zoomHours * 3600;
  const dayStart = startOfDay(viewDate).getTime();

  // Filter recordings belonging to viewDate
  const dayRecs = useMemo(
    () =>
      recordings.filter((r) => {
        const s = new Date(r.start_timestamp).getTime();
        return s >= dayStart && s < dayStart + SECS_IN_DAY * 1000;
      }),
    [recordings, dayStart],
  );

  // Filter events belonging to viewDate — these render as red marks on the bar
  const dayEvents = useMemo(
    () =>
      events.filter((e) => {
        const t = new Date(e.occurred_at).getTime();
        return t >= dayStart && t < dayStart + SECS_IN_DAY * 1000;
      }),
    [events, dayStart],
  );

  const secToPercent = (sec: number) =>
    clamp(((sec - offsetSec) / windowSecs) * 100, 0, 100);

  const handleClick = (e: React.MouseEvent<HTMLDivElement>) => {
    if (!barRef.current) return;
    const rect = barRef.current.getBoundingClientRect();
    const pct = (e.clientX - rect.left) / rect.width;
    const sec = offsetSec + pct * windowSecs;
    onSeek(clamp(sec, 0, SECS_IN_DAY));
  };

  // Hour tick marks
  const ticks = useMemo(() => {
    const interval =
      zoomHours <= 2 ? 0.25 : zoomHours <= 6 ? 0.5 : zoomHours <= 12 ? 1 : 2;
    const ticks: number[] = [];
    let t = Math.ceil(offsetSec / (interval * 3600)) * interval * 3600;
    while (t <= offsetSec + windowSecs) {
      ticks.push(t);
      t += interval * 3600;
    }
    return ticks;
  }, [offsetSec, windowSecs, zoomHours]);

  const nowPct = secToPercent(currentSec);

  return (
    <div className="select-none">
      {/* Timeline bar */}
      <div
        ref={barRef}
        className="relative h-16 bg-muted/30 border border-border rounded-lg overflow-hidden cursor-crosshair"
        onClick={handleClick}
      >
        {/* Hour ticks */}
        {ticks.map((sec) => {
          const pct = secToPercent(sec);
          if (pct < 0 || pct > 100) return null;
          const label = `${String(Math.floor(sec / 3600)).padStart(2, "0")}:${String(Math.floor((sec % 3600) / 60)).padStart(2, "0")}`;
          return (
            <div
              key={sec}
              className="absolute top-0 bottom-0 flex flex-col justify-end"
              style={{ left: `${pct}%` }}
            >
              <div className="w-px h-2/3 bg-border/60" />
              <span className="text-[9px] text-muted-foreground ml-0.5 mb-1 whitespace-nowrap">
                {label}
              </span>
            </div>
          );
        })}

        {/* Recording segments */}
        {dayRecs.map((rec) => {
          const recStartSec = differenceInSeconds(
            new Date(rec.start_timestamp),
            startOfDay(viewDate),
          );
          const recEndSec = rec.end_timestamp
            ? differenceInSeconds(
                new Date(rec.end_timestamp),
                startOfDay(viewDate),
              )
            : recStartSec + (rec.duration_seconds ?? 60);
          const left = secToPercent(recStartSec);
          const right = secToPercent(recEndSec);
          const width = clamp(right - left, 0.3, 100);
          if (right < 0 || left > 100) return null;
          const isSelected = rec.recording_id === selectedId;
          return (
            <div
              key={rec.recording_id}
              className={`absolute top-2 h-8 rounded-sm cursor-pointer transition-opacity hover:opacity-100 ${
                isSelected ? "opacity-100 border border-primary" : "opacity-70"
              }`}
              style={{
                left: `${clamp(left, 0, 99)}%`,
                width: `${width}%`,
                background: isSelected
                  ? "rgba(99,102,241,0.7)"
                  : "rgba(34,197,94,0.5)",
              }}
              onClick={(e) => {
                e.stopPropagation();
                onSelectRecording(rec);
              }}
              title={`${rec.camera_name} — ${format(new Date(rec.start_timestamp), "HH:mm:ss")}`}
            />
          );
        })}

        {/* Event markers — red ticks at the exact moment each event occurred */}
        {dayEvents.map((ev) => {
          const evSec = differenceInSeconds(
            new Date(ev.occurred_at),
            startOfDay(viewDate),
          );
          const pct = secToPercent(evSec);
          if (pct < 0 || pct > 100) return null;
          return (
            <div
              key={`evt-${ev.event_id}`}
              className="absolute top-0 bottom-0 z-20 cursor-pointer group"
              style={{ left: `${pct}%`, transform: "translateX(-50%)" }}
              onClick={(e) => {
                e.stopPropagation();
                onSelectEvent?.(ev);
              }}
              title={`${ev.title || ev.event_type} — ${format(new Date(ev.occurred_at), "HH:mm:ss")}`}
            >
              {/* Vertical red line spanning the bar */}
              <div className="w-0.5 h-full bg-red-500 opacity-80 group-hover:opacity-100 group-hover:w-1 transition-all" />
              {/* Top marker dot */}
              <div className="absolute -top-1 left-1/2 -translate-x-1/2 w-2 h-2 rounded-full bg-red-500 ring-1 ring-background group-hover:scale-125 transition-transform" />
            </div>
          );
        })}

        {/* Playhead */}
        {nowPct >= 0 && nowPct <= 100 && (
          <div
            className="absolute top-0 bottom-0 flex flex-col items-center pointer-events-none z-10"
            style={{ left: `${nowPct}%`, transform: "translateX(-50%)" }}
          >
            <div className="w-0 h-0 border-l-4 border-r-4 border-t-4 border-l-transparent border-r-transparent border-t-primary" />
            <div className="w-0.5 flex-1 bg-primary" />
            <div className="text-[9px] text-primary font-bold whitespace-nowrap mb-0.5">
              {secsToHMS(currentSec)}
            </div>
          </div>
        )}

        {/* No recordings message */}
        {dayRecs.length === 0 && (
          <div className="absolute inset-0 flex items-center justify-center">
            <span className="text-[10px] text-muted-foreground">
              No recordings for {format(viewDate, "dd MMM yyyy")}
            </span>
          </div>
        )}
      </div>

      {/* Legend */}
      <div className="flex gap-4 mt-1.5 px-1">
        <div className="flex items-center gap-1">
          <div className="w-3 h-2 rounded-sm bg-green-500/60" />
          <span className="text-[10px] text-muted-foreground">
            Recording ({dayRecs.length})
          </span>
        </div>
        <div className="flex items-center gap-1">
          <div className="w-3 h-2 rounded-sm bg-primary/70" />
          <span className="text-[10px] text-muted-foreground">Selected</span>
        </div>
        <div className="flex items-center gap-1">
          <div className="w-0.5 h-3 bg-primary" />
          <span className="text-[10px] text-muted-foreground">Playhead</span>
        </div>
        <div className="flex items-center gap-1">
          <div className="w-2 h-2 rounded-full bg-red-500" />
          <span className="text-[10px] text-muted-foreground">
            Event ({dayEvents.length})
          </span>
        </div>
      </div>
    </div>
  );
}

// ── AI panel ──────────────────────────────────────────────────────────────────
function AiPanel({
  detections,
  inferenceMs,
  peopleCount,
  densityPct,
  densityLevel,
  intrusionDetected,
  intruderCount,
  frameCount,
  aiMode,
}: {
  detections: Detection[];
  inferenceMs: number | null;
  peopleCount: number | null;
  densityPct: number | null;
  densityLevel: string | null;
  intrusionDetected: boolean | null;
  intruderCount: number | null;
  frameCount: number;
  aiMode: string;
}) {
  const summary: Record<string, number> = {};
  detections.forEach((d) => {
    summary[d.label] = (summary[d.label] || 0) + 1;
  });
  const densityColor =
    densityLevel === "HIGH"
      ? "text-red-500 dark:text-red-400"
      : densityLevel === "MEDIUM"
        ? "text-yellow-600 dark:text-yellow-400"
        : "text-green-600 dark:text-green-400";

  return (
    <Card className="bg-card border-border">
      <CardHeader className="pb-2 border-b border-border/60">
        <CardTitle className="text-sm flex items-center gap-2 text-foreground">
          <Cpu className="h-4 w-4 text-primary" />
          AI Analysis
          {inferenceMs != null && (
            <Badge variant="secondary" className="text-[10px] ml-auto">
              {inferenceMs}ms
            </Badge>
          )}
        </CardTitle>
      </CardHeader>
      <CardContent className="p-3 space-y-3">
        {frameCount === 0 ? (
          <div className="text-center py-5 text-muted-foreground">
            <Bot className="h-7 w-7 mx-auto mb-2 opacity-30" />
            <p className="text-xs text-foreground">
              Enable AI + play a recording
            </p>
          </div>
        ) : (
          <>
            {aiMode === "people" && (
              <div className="p-2.5 rounded-lg bg-green-500/10 border border-green-500/20 space-y-2">
                <div className="flex items-center justify-between">
                  <span className="text-xs font-medium text-foreground flex items-center gap-1.5">
                    <Users className="h-3.5 w-3.5 text-green-600 dark:text-green-400" />
                    People
                  </span>
                  <span className="text-xl font-bold text-green-600 dark:text-green-400">
                    {peopleCount ?? 0}
                  </span>
                </div>
                {densityPct != null && (
                  <>
                    <div className="flex justify-between text-[10px]">
                      <span className="text-muted-foreground">Density</span>
                      <span className={densityColor}>
                        {densityPct}% — {densityLevel}
                      </span>
                    </div>
                    <Progress value={densityPct} className="h-1.5" />
                  </>
                )}
              </div>
            )}
            {aiMode === "intrusion" && intrusionDetected != null && (
              <div
                className={`p-2.5 rounded-lg border ${intrusionDetected ? "bg-red-500/10 border-red-500/30" : "bg-green-500/5 border-green-500/20"}`}
              >
                <div className="flex items-center gap-2">
                  <ShieldAlert
                    className={`h-4 w-4 ${intrusionDetected ? "text-red-500 animate-pulse" : "text-green-600 dark:text-green-400"}`}
                  />
                  <span
                    className={`text-sm font-semibold ${intrusionDetected ? "text-red-600 dark:text-red-400" : "text-green-700 dark:text-green-400"}`}
                  >
                    {intrusionDetected
                      ? `INTRUSION — ${intruderCount}`
                      : "Zone Clear"}
                  </span>
                </div>
              </div>
            )}
            {Object.keys(summary).length > 0 && (
              <div className="space-y-1.5">
                <p className="text-[10px] font-medium text-muted-foreground uppercase tracking-wide">
                  Detected
                </p>
                {Object.entries(summary)
                  .sort(([, a], [, b]) => b - a)
                  .map(([label, count]) => (
                    <div key={label} className="flex items-center gap-2">
                      <div
                        className="w-2 h-2 rounded-full shrink-0"
                        style={{ backgroundColor: boxColor(label) }}
                      />
                      <span className="text-xs text-foreground capitalize flex-1">
                        {label}
                      </span>
                      <Badge variant="outline" className="text-[10px] h-4 px-1">
                        {count}
                      </Badge>
                    </div>
                  ))}
              </div>
            )}
            {detections.length === 0 && (
              <div className="flex items-center gap-2 text-green-600 dark:text-green-400">
                <CheckCircle2 className="h-4 w-4" />
                <span className="text-xs">No objects detected</span>
              </div>
            )}
          </>
        )}
      </CardContent>
    </Card>
  );
}

// ── Main VideoPlayer ──────────────────────────────────────────────────────────
const VideoPlayer = () => {
  const [searchParams] = useSearchParams();
  const { toast } = useToast();

  const videoRef = useRef<HTMLVideoElement>(null);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const aiTimer = useRef<ReturnType<typeof setInterval> | null>(null);

  const initRecId = searchParams.get("recording")
    ? parseInt(searchParams.get("recording")!)
    : null;
  const initCamId = searchParams.get("camera")
    ? parseInt(searchParams.get("camera")!)
    : null;
  // ISO timestamp to seek to when opening from a notification/event link
  const initAt = searchParams.get("at") || null;

  // View state
  const [filterCamId, setFilterCamId] = useState<number | null>(initCamId);
  const [selectedRecId, setSelectedRecId] = useState<number | null>(initRecId);
  const [viewDate, setViewDate] = useState<Date>(startOfDay(new Date()));
  const [zoomHours, setZoomHours] = useState(8); // visible hours in timeline
  const [offsetSec, setOffsetSec] = useState(0); // left edge of timeline view (sec from midnight)

  // Playback state
  const [playing, setPlaying] = useState(false);
  const [muted, setMuted] = useState(true);
  const [curTime, setCurTime] = useState(0);
  const [duration, setDuration] = useState(0);

  // Playhead as seconds-from-midnight for the timeline
  const [playheadSec, setPlayheadSec] = useState<number>(0);

  // AI state
  const [aiEnabled, setAiEnabled] = useState(false);
  const [aiMode, setAiMode] = useState("detect");
  const [detections, setDetections] = useState<Detection[]>([]);
  const [inferenceMs, setInferenceMs] = useState<number | null>(null);
  const [peopleCount, setPeopleCount] = useState<number | null>(null);
  const [densityPct, setDensityPct] = useState<number | null>(null);
  const [densityLevel, setDensityLevel] = useState<string | null>(null);
  const [intrusionDetected, setIntrusion] = useState<boolean | null>(null);
  const [intruderCount, setIntruderCount] = useState<number | null>(null);
  const [frameCount, setFrameCount] = useState(0);

  const { data: cameras } = useCameras();
  const { data: recordings } = useRecordings(
    filterCamId ? { camera_id: filterCamId, limit: 500 } : { limit: 500 },
  );
  const { data: eventsResp } = useEvents(
    filterCamId ? { camera_id: filterCamId, limit: 500 } : { limit: 500 },
  );
  const allEvents = eventsResp?.events ?? [];

  const currentRec = recordings?.find((r) => r.recording_id === selectedRecId);

  // ── initAt: auto-select the recording containing the event timestamp ───────
  // Fires once when recordings first arrive and ?at= is in the URL.
  const initAtApplied = useRef(false);
  useEffect(() => {
    if (!initAt || !recordings?.length || initAtApplied.current) return;
    const targetMs = new Date(initAt).getTime();
    if (isNaN(targetMs)) return;

    // Find recording whose window contains the target timestamp
    const camRecs = recordings.filter(
      (r) => !initCamId || r.camera_id === initCamId,
    );
    let hit = camRecs.find((r) => {
      const s = new Date(r.start_timestamp).getTime();
      const e = r.end_timestamp
        ? new Date(r.end_timestamp).getTime()
        : s + (r.duration_seconds ?? 120) * 1000;
      return targetMs >= s && targetMs <= e;
    });
    // Fallback: nearest recording before the timestamp
    if (!hit) {
      hit = [...camRecs]
        .filter((r) => new Date(r.start_timestamp).getTime() <= targetMs)
        .sort(
          (a, b) =>
            new Date(b.start_timestamp).getTime() -
            new Date(a.start_timestamp).getTime(),
        )[0];
    }
    if (!hit) return;

    initAtApplied.current = true;
    setFilterCamId(hit.camera_id);
    setSelectedRecId(hit.recording_id);

    // Seek to the offset within the recording once video loads
    const seekSec = Math.max(
      0,
      (targetMs - new Date(hit.start_timestamp).getTime()) / 1000,
    );
    setTimeout(() => {
      const video = videoRef.current;
      if (!video) return;
      const doSeek = () => {
        video.currentTime = seekSec;
        video.play().catch(() => {});
      };
      if (video.readyState >= 1) doSeek();
      else video.addEventListener("loadedmetadata", doSeek, { once: true });
    }, 250);
  }, [recordings, initAt, initCamId]);

  // ── Load recording ─────────────────────────────────────────────────────────
  useEffect(() => {
    if (!selectedRecId) return;
    const video = videoRef.current;
    if (!video) return;
    video.src = getStreamUrl(selectedRecId);
    video.load();
    // setPlaying is driven purely by onPlay/onPause events on the video element.
    // Reset time UI but don't touch playing state here.
    setCurTime(0);
    setDuration(0);
    setFrameCount(0);

    // Set view date to recording date
    const rec = recordings?.find((r) => r.recording_id === selectedRecId);
    if (rec) {
      const recDate = startOfDay(new Date(rec.start_timestamp));
      setViewDate(recDate);
      const secFromMidnight = differenceInSeconds(
        new Date(rec.start_timestamp),
        recDate,
      );
      setPlayheadSec(secFromMidnight);
      const windowSecs = zoomHours * 3600;
      setOffsetSec(
        clamp(secFromMidnight - windowSecs / 2, 0, SECS_IN_DAY - windowSecs),
      );
    }
  }, [selectedRecId]);

  // ── Update playhead as video plays ────────────────────────────────────────
  useEffect(() => {
    if (!currentRec) return;
    const recStartSec = differenceInSeconds(
      new Date(currentRec.start_timestamp),
      startOfDay(viewDate),
    );
    setPlayheadSec(recStartSec + curTime);
  }, [curTime, currentRec, viewDate]);

  // ── Timeline seek → find recording at that second ─────────────────────────
  const handleTimelineSeek = (secFromMidnight: number) => {
    if (!recordings) return;
    const viewDayStart = startOfDay(viewDate).getTime();
    const clickTs = viewDayStart + secFromMidnight * 1000;

    // Find recording that contains this timestamp
    const hit = recordings.find((r) => {
      const s = new Date(r.start_timestamp).getTime();
      const dur = r.duration_seconds ?? 60;
      const e = r.end_timestamp
        ? new Date(r.end_timestamp).getTime()
        : s + dur * 1000;
      return clickTs >= s && clickTs <= e;
    });

    if (hit) {
      setSelectedRecId(hit.recording_id);
      // Seek to offset within that recording
      const recStart = new Date(hit.start_timestamp).getTime();
      const offsetSecs = (clickTs - recStart) / 1000;
      setTimeout(() => {
        if (videoRef.current) {
          videoRef.current.currentTime = offsetSecs;
          // play() → onPlay event → setPlaying(true)
          videoRef.current.play().catch(() => {});
        }
      }, 400); // small delay for video to load
    } else {
      setPlayheadSec(secFromMidnight);
      toast({
        title: "No recording at this time",
        description: "Click on a green segment to play a recording",
        variant: "destructive",
      });
    }
  };

  // ── Jump to the recording covering a specific event's timestamp ───────────
  const handleSelectEvent = (ev: EventRow) => {
    if (!recordings) return;
    const evTs = new Date(ev.occurred_at).getTime();

    const hit = recordings.find((r) => {
      const s = new Date(r.start_timestamp).getTime();
      const dur = r.duration_seconds ?? 60;
      const e = r.end_timestamp
        ? new Date(r.end_timestamp).getTime()
        : s + dur * 1000;
      return evTs >= s && evTs <= e;
    });

    if (hit) {
      setSelectedRecId(hit.recording_id);
      const recStart = new Date(hit.start_timestamp).getTime();
      const offsetSecs = (evTs - recStart) / 1000;
      setTimeout(() => {
        if (videoRef.current) {
          videoRef.current.currentTime = offsetSecs;
          videoRef.current.play().catch(() => {});
        }
      }, 400);
      toast({ title: ev.title || ev.event_type, description: "Jumped to event in recording" });
    } else {
      const evDate = startOfDay(new Date(ev.occurred_at));
      setViewDate(evDate);
      const secFromMidnight = differenceInSeconds(new Date(ev.occurred_at), evDate);
      setPlayheadSec(secFromMidnight);
      toast({
        title: "No recording covers this event",
        description: ev.title || ev.event_type,
        variant: "destructive",
      });
    }
  };

  // ── Jump ±N seconds ────────────────────────────────────────────────────────
  const skip = (s: number) => {
    if (!videoRef.current) return;
    const newTime = videoRef.current.currentTime + s;
    if (newTime < 0 && currentRec) {
      // Jump to previous recording
      jumpRelativeRecording(-1);
    } else if (newTime > duration && currentRec) {
      jumpRelativeRecording(1);
    } else {
      videoRef.current.currentTime = clamp(newTime, 0, duration);
    }
  };

  // ── Jump to adjacent recording ─────────────────────────────────────────────
  const jumpRelativeRecording = (dir: -1 | 1) => {
    if (!recordings || !selectedRecId) return;
    const camRecs = recordings
      .filter((r) => !filterCamId || r.camera_id === filterCamId)
      .sort(
        (a, b) =>
          new Date(a.start_timestamp).getTime() -
          new Date(b.start_timestamp).getTime(),
      );
    const idx = camRecs.findIndex((r) => r.recording_id === selectedRecId);
    const next = camRecs[idx + dir];
    if (next) {
      setSelectedRecId(next.recording_id);
      const v = videoRef.current;
      if (v) {
        // Don't guess with a fixed delay — wait for the browser's own
        // "I actually have a frame ready" signal before calling play().
        // Calling play() too early leaves the element in a state where
        // .paused is false (so onPlay fires and the button correctly
        // shows "playing") but playback is stalled waiting for data
        // (a `waiting` event, not `pause`) — visually frozen despite the
        // button saying otherwise, and needing an extra pause/play cycle
        // to recover once enough data had arrived in the background.
        const onReady = () => {
          v.removeEventListener("loadeddata", onReady);
          if (dir === -1) v.currentTime = next.duration_seconds ?? 0;
          v.play().catch(() => {
            // Autoplay blocked or src not ready — stay paused, onPause/lack
            // of onPlay already reflects that correctly.
          });
        };
        v.addEventListener("loadeddata", onReady);
      }
    }
  };

  // Play/pause: only update state via native video events (onPlay/onPause,
  // plus onWaiting/onPlaying to catch stalls — a stalled-but-technically-
  // playing video fires `waiting`, not `pause`, and would otherwise leave
  // the button showing "playing" while the picture is frozen).
  // Do NOT call setPlaying here — the event handlers on the <video> tag are the single
  // source of truth, preventing race conditions with async play().
  const togglePlay = () => {
    const v = videoRef.current;
    if (!v) return;
    if (v.paused || v.ended) {
      v.play().catch(() => {
        // If autoplay is blocked or src isn't ready, stay paused — onPause handles state
      });
    } else {
      v.pause();
    }
  };

  // ── AI inference ────────────────────────────────────────────────────────────
  const runAi = useCallback(async () => {
    const v = videoRef.current;
    const canvas = canvasRef.current;
    if (!v || !canvas || v.paused || v.ended || v.videoWidth === 0) return;
    const tmp = document.createElement("canvas");
    tmp.width = v.videoWidth;
    tmp.height = v.videoHeight;
    tmp.getContext("2d")?.drawImage(v, 0, 0);
    const mode = AI_MODES.find((m) => m.key === aiMode)!;
    tmp.toBlob(
      async (blob) => {
        if (!blob) return;
        try {
          const res = await detectObjects(blob, {
            conf: 0.35,
            endpoint: mode.endpoint,
          });
          setDetections(res.detections);
          setInferenceMs(res.inference_ms);
          setFrameCount((c) => c + 1);
          if (res.people_count !== undefined) setPeopleCount(res.people_count);
          if (res.density_percent !== undefined)
            setDensityPct(res.density_percent);
          if (res.density_level !== undefined)
            setDensityLevel(res.density_level);
          if (res.intrusion_detected !== undefined) {
            setIntrusion(res.intrusion_detected);
            setIntruderCount(res.intruder_count ?? 0);
          }
          canvas.width = tmp.width;
          canvas.height = tmp.height;
          const ctx = canvas.getContext("2d")!;
          ctx.clearRect(0, 0, canvas.width, canvas.height);
          res.detections.forEach((d: Detection) => {
            const [x1, y1, x2, y2] = d.bbox;
            const col = boxColor(d.label);
            ctx.strokeStyle = col;
            ctx.lineWidth = 2;
            ctx.strokeRect(x1, y1, x2 - x1, y2 - y1);
            const lbl = `${d.label} ${(d.confidence * 100).toFixed(0)}%`;
            ctx.font = "bold 12px system-ui";
            const tw = ctx.measureText(lbl).width + 6;
            ctx.fillStyle = col + "cc";
            ctx.fillRect(x1, y1 - 18, tw, 18);
            ctx.fillStyle = "#000";
            ctx.fillText(lbl, x1 + 3, y1 - 4);
          });
          if (mode.key === "people" && res.people_count) {
            const dp = res.density_percent ?? 0;
            const col =
              res.density_level === "HIGH"
                ? "#ef4444"
                : res.density_level === "MEDIUM"
                  ? "#f59e0b"
                  : "#22c55e";
            ctx.fillStyle = col + "99";
            ctx.fillRect(0, canvas.height - 8, (dp / 100) * canvas.width, 8);
            ctx.fillStyle = "#ffffffee";
            ctx.font = "bold 13px system-ui";
            ctx.fillText(
              `${res.people_count} people · ${res.density_level} ${dp}%`,
              8,
              canvas.height - 12,
            );
          }
        } catch {}
      },
      "image/jpeg",
      0.75,
    );
  }, [aiMode]);

  useEffect(() => {
    if (aiEnabled) {
      aiTimer.current = setInterval(runAi, 2000);
    } else {
      if (aiTimer.current) clearInterval(aiTimer.current);
      setDetections([]);
      setInferenceMs(null);
      setPeopleCount(null);
      setDensityPct(null);
      setDensityLevel(null);
      setIntrusion(null);
      setIntruderCount(null);
      setFrameCount(0);
      const ctx = canvasRef.current?.getContext("2d");
      if (ctx && canvasRef.current)
        ctx.clearRect(0, 0, canvasRef.current.width, canvasRef.current.height);
    }
    return () => {
      if (aiTimer.current) clearInterval(aiTimer.current);
    };
  }, [aiEnabled, runAi]);

  // ── Timeline pan helpers ───────────────────────────────────────────────────
  const windowSecs = zoomHours * 3600;
  const panTimeline = (dir: -1 | 1) => {
    setOffsetSec((prev) =>
      clamp(prev + dir * windowSecs * 0.4, 0, SECS_IN_DAY - windowSecs),
    );
  };
  const zoom = (dir: -1 | 1) => {
    const steps = [1, 2, 4, 6, 8, 12, 24];
    const idx = steps.indexOf(zoomHours);
    const next = steps[clamp(idx - dir, 0, steps.length - 1)];
    setZoomHours(next);
    setOffsetSec((prev) => clamp(prev, 0, SECS_IN_DAY - next * 3600));
  };

  const camSortedRecs = useMemo(() => {
    if (!recordings) return [];
    return [...recordings]
      .filter((r) => !filterCamId || r.camera_id === filterCamId)
      .sort(
        (a, b) =>
          new Date(b.start_timestamp).getTime() -
          new Date(a.start_timestamp).getTime(), // latest first
      );
  }, [recordings, filterCamId]);

  const selectedIdx = camSortedRecs.findIndex(
    (r) => r.recording_id === selectedRecId,
  );
  const hasPrev = selectedIdx > 0;
  const hasNext = selectedIdx < camSortedRecs.length - 1;

  return (
    <AppLayout>
      <div className="space-y-4">
        {/* Header */}
        <div>
          <h1 className="text-2xl font-bold text-foreground">Video Player</h1>
          <p className="text-sm text-muted-foreground">
            Timeline scrubber · Recording playback · Real-time AI
          </p>
        </div>

        <div className="grid xl:grid-cols-4 gap-4">
          {/* ── Left: video + timeline ───────────────────────────────────── */}
          <div className="xl:col-span-3 space-y-3">
            {/* Video */}
            <div className="relative bg-black rounded-xl overflow-hidden aspect-video">
              <video
                ref={videoRef}
                className="w-full h-full object-contain"
                onPlay={() => setPlaying(true)}
                onPause={() => setPlaying(false)}
                onPlaying={() => setPlaying(true)}
                onWaiting={() => setPlaying(false)}
                onEnded={() => {
                  setPlaying(false);
                  if (hasNext) jumpRelativeRecording(1);
                }}
                onTimeUpdate={(e) =>
                  setCurTime((e.target as HTMLVideoElement).currentTime)
                }
                onLoadedMetadata={(e) =>
                  setDuration((e.target as HTMLVideoElement).duration)
                }
                muted={muted}
                playsInline
              />
              {aiEnabled && (
                <canvas
                  ref={canvasRef}
                  className="absolute inset-0 w-full h-full pointer-events-none"
                  style={{ objectFit: "contain" }}
                />
              )}
              {aiEnabled && (
                <div className="absolute top-2 right-2 flex flex-col items-end gap-1">
                  <Badge className="bg-black/70 text-white border-0 gap-1 text-[10px]">
                    <Cpu className="h-2.5 w-2.5 animate-pulse" />
                    {AI_MODES.find((m) => m.key === aiMode)?.label}
                    {inferenceMs ? ` · ${inferenceMs}ms` : ""}
                  </Badge>
                </div>
              )}
              {!selectedRecId && (
                <div className="absolute inset-0 flex flex-col items-center justify-center text-muted-foreground">
                  <Camera className="h-14 w-14 mb-3 opacity-20" />
                  <p className="text-sm text-foreground">
                    Click a segment on the timeline below
                  </p>
                  <p className="text-xs text-muted-foreground mt-1">
                    or select a recording from the list →
                  </p>
                </div>
              )}
            </div>

            {/* Seek bar */}
            {duration > 0 && (
              <div className="flex items-center gap-2">
                <span className="text-[10px] font-mono text-muted-foreground w-14">
                  {secsToHMS(curTime)}
                </span>
                <input
                  type="range"
                  min={0}
                  max={duration}
                  value={curTime}
                  step={0.25}
                  className="flex-1 accent-primary h-1.5"
                  onChange={(e) => {
                    const v = parseFloat(e.target.value);
                    if (videoRef.current) videoRef.current.currentTime = v;
                    setCurTime(v);
                  }}
                />
                <span className="text-[10px] font-mono text-muted-foreground w-14 text-right">
                  -{secsToHMS(Math.max(0, duration - curTime))}
                </span>
              </div>
            )}

            {/* Playback controls */}
            <div className="flex items-center gap-2 flex-wrap">
              <Button
                size="icon"
                variant="outline"
                className="h-8 w-8"
                disabled={!hasPrev}
                onClick={() => jumpRelativeRecording(-1)}
                title="Previous recording"
              >
                <ChevronLeft className="h-4 w-4" />
              </Button>
              <Button
                size="icon"
                variant="ghost"
                className="h-8 w-8"
                onClick={() => skip(-30)}
                title="Back 30s"
              >
                <SkipBack className="h-4 w-4" />
              </Button>
              <Button
                size="icon"
                variant="ghost"
                className="h-8 w-8"
                onClick={() => skip(-10)}
                title="Back 10s"
              >
                <span className="text-[10px] font-bold">-10</span>
              </Button>
              <Button size="icon" className="h-9 w-9" onClick={togglePlay}>
                {playing ? (
                  <Pause className="h-5 w-5" />
                ) : (
                  <Play className="h-5 w-5" />
                )}
              </Button>
              <Button
                size="icon"
                variant="ghost"
                className="h-8 w-8"
                onClick={() => skip(10)}
                title="Forward 10s"
              >
                <span className="text-[10px] font-bold">+10</span>
              </Button>
              <Button
                size="icon"
                variant="ghost"
                className="h-8 w-8"
                onClick={() => skip(30)}
                title="Forward 30s"
              >
                <SkipForward className="h-4 w-4" />
              </Button>
              <Button
                size="icon"
                variant="outline"
                className="h-8 w-8"
                disabled={!hasNext}
                onClick={() => jumpRelativeRecording(1)}
                title="Next recording"
              >
                <ChevronRight className="h-4 w-4" />
              </Button>

              <Button
                size="icon"
                variant="ghost"
                className="h-8 w-8 ml-1"
                onClick={() => setMuted(!muted)}
              >
                {muted ? (
                  <VolumeX className="h-4 w-4" />
                ) : (
                  <Volume2 className="h-4 w-4" />
                )}
              </Button>

              <Separator orientation="vertical" className="h-6 mx-1" />

              <Select
                value={aiMode}
                onValueChange={(v) => {
                  setAiMode(v);
                  setFrameCount(0);
                }}
              >
                <SelectTrigger className="h-8 w-44 text-xs">
                  <Cpu className="h-3 w-3 mr-1.5 text-primary shrink-0" />
                  <SelectValue />
                </SelectTrigger>
                <SelectContent>
                  {AI_MODES.map((m) => (
                    <SelectItem key={m.key} value={m.key}>
                      {m.label}
                    </SelectItem>
                  ))}
                </SelectContent>
              </Select>
              <Button
                variant={aiEnabled ? "default" : "outline"}
                size="sm"
                className="h-8 gap-1.5"
                onClick={() => setAiEnabled(!aiEnabled)}
              >
                <Bot className="h-3.5 w-3.5" />
                {aiEnabled ? "AI ON" : "AI"}
              </Button>

              {selectedRecId && (
                <Button
                  size="sm"
                  variant="ghost"
                  className="h-8 gap-1 ml-auto"
                  asChild
                >
                  <a href={getDownloadUrl(selectedRecId)} download>
                    <Download className="h-3.5 w-3.5" />
                    Download
                  </a>
                </Button>
              )}
            </div>

            {/* ── Timeline ───────────────────────────────────────────────── */}
            <Card className="bg-card border-border">
              <CardHeader className="pb-2 border-b border-border/60">
                <div className="flex items-center justify-between gap-2 flex-wrap">
                  {/* Date navigation */}
                  <div className="flex items-center gap-1.5">
                    <Button
                      size="icon"
                      variant="ghost"
                      className="h-7 w-7"
                      onClick={() => setViewDate((d) => subDays(d, 1))}
                    >
                      <ChevronLeft className="h-3.5 w-3.5" />
                    </Button>
                    <Popover>
                      <PopoverTrigger asChild>
                        <button className="flex items-center gap-1.5 text-sm font-medium text-foreground hover:text-primary transition-colors px-1.5 py-0.5 rounded-md hover:bg-muted/50">
                          <Calendar className="h-3.5 w-3.5 text-muted-foreground" />
                          {format(viewDate, "dd MMM yyyy")}
                        </button>
                      </PopoverTrigger>
                      <PopoverContent className="w-auto p-0" align="start">
                        <DatePickerCalendar
                          mode="single"
                          selected={viewDate}
                          onSelect={(d) => d && setViewDate(startOfDay(d))}
                          disabled={(d) => startOfDay(d) > startOfDay(new Date())}
                          initialFocus
                        />
                      </PopoverContent>
                    </Popover>
                    <Button
                      size="icon"
                      variant="ghost"
                      className="h-7 w-7"
                      onClick={() => setViewDate((d) => addDays(d, 1))}
                      disabled={startOfDay(viewDate) >= startOfDay(new Date())}
                    >
                      <ChevronRight className="h-3.5 w-3.5" />
                    </Button>
                    <Button
                      size="sm"
                      variant="ghost"
                      className="h-7 text-xs"
                      onClick={() => setViewDate(startOfDay(new Date()))}
                    >
                      Today
                    </Button>
                  </div>

                  {/* Zoom + pan controls */}
                  <div className="flex items-center gap-1">
                    <Button
                      size="icon"
                      variant="ghost"
                      className="h-7 w-7"
                      onClick={() => panTimeline(-1)}
                      title="Pan left"
                    >
                      <ChevronLeft className="h-3.5 w-3.5" />
                    </Button>
                    <Button
                      size="icon"
                      variant="ghost"
                      className="h-7 w-7"
                      onClick={() => zoom(1)}
                      title="Zoom in"
                      disabled={zoomHours === 1}
                    >
                      <ZoomIn className="h-3.5 w-3.5" />
                    </Button>
                    <span className="text-[10px] text-muted-foreground w-8 text-center">
                      {zoomHours}h
                    </span>
                    <Button
                      size="icon"
                      variant="ghost"
                      className="h-7 w-7"
                      onClick={() => zoom(-1)}
                      title="Zoom out"
                      disabled={zoomHours === 24}
                    >
                      <ZoomOut className="h-3.5 w-3.5" />
                    </Button>
                    <Button
                      size="icon"
                      variant="ghost"
                      className="h-7 w-7"
                      onClick={() => panTimeline(1)}
                      title="Pan right"
                    >
                      <ChevronRight className="h-3.5 w-3.5" />
                    </Button>
                  </div>
                </div>
              </CardHeader>
              <CardContent className="p-3">
                <Timeline
                  recordings={recordings ?? []}
                  events={allEvents}
                  selectedId={selectedRecId}
                  currentSec={playheadSec}
                  viewDate={viewDate}
                  zoomHours={zoomHours}
                  offsetSec={offsetSec}
                  onSeek={handleTimelineSeek}
                  onSelectRecording={(rec) =>
                    setSelectedRecId(rec.recording_id)
                  }
                  onSelectEvent={handleSelectEvent}
                />
              </CardContent>
            </Card>

            {/* Current recording info */}
            {currentRec && (
              <Card className="bg-card border-border">
                <CardContent className="p-3 grid grid-cols-2 sm:grid-cols-4 gap-2 text-xs">
                  <div>
                    <p className="text-muted-foreground">Camera</p>
                    <p className="font-medium text-foreground">
                      {currentRec.camera_name}
                    </p>
                  </div>
                  <div>
                    <p className="text-muted-foreground">IP</p>
                    <p className="font-mono text-foreground">
                      {currentRec.ip_address || "—"}
                    </p>
                  </div>
                  <div>
                    <p className="text-muted-foreground">Started</p>
                    <p className="font-mono text-foreground">
                      {format(
                        new Date(currentRec.start_timestamp),
                        "dd MMM HH:mm:ss",
                      )}
                    </p>
                  </div>
                  <div>
                    <p className="text-muted-foreground">Duration</p>
                    <p className="text-foreground">
                      {currentRec.duration_seconds
                        ? `${Math.floor(currentRec.duration_seconds / 60)}m ${currentRec.duration_seconds % 60}s`
                        : "—"}
                    </p>
                  </div>
                  <div>
                    <p className="text-muted-foreground">Codec</p>
                    <p className="text-foreground">
                      {currentRec.video_codec} · {currentRec.resolution_width}×
                      {currentRec.resolution_height}
                    </p>
                  </div>
                  <div>
                    <p className="text-muted-foreground">Location</p>
                    <p className="text-foreground truncate">
                      {currentRec.location_description || "—"}
                    </p>
                  </div>
                  {currentRec.gps_speed_kmh && (
                    <div>
                      <p className="text-muted-foreground">Speed</p>
                      <p className="text-foreground">
                        🚂 {currentRec.gps_speed_kmh} km/h
                      </p>
                    </div>
                  )}
                  <div>
                    <p className="text-muted-foreground">Recording</p>
                    <p className="font-mono text-foreground text-[10px]">
                      {selectedIdx + 1} of {camSortedRecs.length}
                    </p>
                  </div>
                </CardContent>
              </Card>
            )}
          </div>

          {/* ── Right sidebar ────────────────────────────────────────────── */}
          <div className="space-y-3">
            <AiPanel
              detections={detections}
              inferenceMs={inferenceMs}
              peopleCount={peopleCount}
              densityPct={densityPct}
              densityLevel={densityLevel}
              intrusionDetected={intrusionDetected}
              intruderCount={intruderCount}
              frameCount={frameCount}
              aiMode={aiMode}
            />

            {/* Camera filter */}
            <Card className="bg-card border-border">
              <CardHeader className="pb-2">
                <CardTitle className="text-xs text-muted-foreground uppercase tracking-wide">
                  Camera
                </CardTitle>
              </CardHeader>
              <CardContent className="p-2 pt-0 max-h-40 overflow-y-auto space-y-0.5">
                <button
                  onClick={() => {
                    setFilterCamId(null);
                    setSelectedRecId(null);
                  }}
                  className={`w-full text-left px-2 py-1.5 rounded text-xs hover:bg-muted/50 transition-colors border ${!filterCamId ? "bg-primary/10 border-primary/30" : "border-transparent"}`}
                >
                  <p className="font-medium text-foreground">All cameras</p>
                </button>
                {cameras?.map((cam) => (
                  <button
                    key={cam.camera_id}
                    onClick={() => {
                      setFilterCamId(cam.camera_id);
                      setSelectedRecId(null);
                    }}
                    className={`w-full text-left px-2 py-1.5 rounded text-xs hover:bg-muted/50 transition-colors border ${filterCamId === cam.camera_id ? "bg-primary/10 border-primary/30" : "border-transparent"}`}
                  >
                    <p className="font-medium truncate text-foreground">
                      {cam.camera_name}
                    </p>
                    <p className="font-mono text-[10px] text-muted-foreground">
                      {cam.ip_address || "—"}
                    </p>
                  </button>
                ))}
              </CardContent>
            </Card>

            {/* Recordings list */}
            <Card className="bg-card border-border">
              <CardHeader className="pb-2">
                <CardTitle className="text-xs text-muted-foreground uppercase tracking-wide flex items-center gap-1">
                  <Clock className="h-3 w-3" />
                  Recordings ({camSortedRecs.length})
                </CardTitle>
              </CardHeader>
              <CardContent className="p-2 pt-0 max-h-64 overflow-y-auto space-y-0.5">
                {camSortedRecs.length === 0 && (
                  <p className="text-[11px] text-muted-foreground text-center py-4">
                    No recordings yet
                  </p>
                )}
                {camSortedRecs.map((rec, i) => (
                  <button
                    key={rec.recording_id}
                    onClick={() => setSelectedRecId(rec.recording_id)}
                    className={`w-full text-left px-2 py-1.5 rounded text-xs hover:bg-muted/50 transition-colors border ${selectedRecId === rec.recording_id ? "bg-primary/10 border-primary/30" : "border-transparent"}`}
                  >
                    <div className="flex items-center gap-1.5">
                      <span className="text-[9px] text-muted-foreground w-5 shrink-0">
                        {i + 1}
                      </span>
                      <div className="min-w-0">
                        <p className="font-medium truncate text-foreground text-[11px]">
                          {rec.camera_name || `Camera ${rec.camera_id}`}
                        </p>
                        <p className="text-[10px] text-muted-foreground font-mono">
                          {format(
                            new Date(rec.start_timestamp),
                            "dd MMM HH:mm:ss",
                          )}
                          {rec.duration_seconds
                            ? ` · ${Math.floor(rec.duration_seconds / 60)}m${rec.duration_seconds % 60}s`
                            : ""}
                        </p>
                      </div>
                    </div>
                  </button>
                ))}
              </CardContent>
            </Card>
          </div>
        </div>
      </div>
    </AppLayout>
  );
};

export default VideoPlayer;
