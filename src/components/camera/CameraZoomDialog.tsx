import { useState, useRef } from "react";
import { Camera, Maximize2, Bot, Volume2, VolumeX, ZoomIn, ZoomOut, Move } from "lucide-react";
import { Dialog, DialogContent, DialogHeader, DialogTitle } from "@/components/ui/dialog";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";
import { Label } from "@/components/ui/label";
import { Slider } from "@/components/ui/slider";
import { CameraControlPanel } from "./CameraControlPanel";
import type { CameraData } from "./CameraTile";

interface CameraZoomDialogProps {
  camera: CameraData | null;
  onClose: () => void;
}

const CameraZoomDialog = ({ camera, onClose }: CameraZoomDialogProps) => {
  const [zoom, setZoom] = useState(1);
  const [pan, setPan] = useState({ x: 0, y: 0 });
  const [isDragging, setIsDragging] = useState(false);
  const dragStart = useRef({ x: 0, y: 0 });
  const containerRef = useRef<HTMLDivElement>(null);

  if (!camera) return null;

  const handleWheel = (e: React.WheelEvent) => {
    e.preventDefault();
    const delta = e.deltaY > 0 ? -0.1 : 0.1;
    setZoom((prev) => Math.max(1, Math.min(5, prev + delta)));
  };

  const handleMouseDown = (e: React.MouseEvent) => {
    if (zoom <= 1) return;
    setIsDragging(true);
    dragStart.current = { x: e.clientX - pan.x, y: e.clientY - pan.y };
  };

  const handleMouseMove = (e: React.MouseEvent) => {
    if (!isDragging) return;
    setPan({
      x: e.clientX - dragStart.current.x,
      y: e.clientY - dragStart.current.y,
    });
  };

  const handleMouseUp = () => setIsDragging(false);

  const resetZoom = () => {
    setZoom(1);
    setPan({ x: 0, y: 0 });
  };

  return (
    <Dialog open={!!camera} onOpenChange={() => { resetZoom(); onClose(); }}>
      <DialogContent className="sm:max-w-6xl max-h-[90vh] overflow-y-auto">
        <DialogHeader>
          <DialogTitle className="flex items-center gap-2">
            <Camera className="h-5 w-5" /> {camera.name} — {camera.location}
          </DialogTitle>
        </DialogHeader>
        <div className="grid lg:grid-cols-[1.4fr_1fr] gap-4">
        <div className="space-y-4">
          {/* Zoomed video feed */}
          <div
            ref={containerRef}
            className="relative rounded-lg overflow-hidden bg-muted border border-border cursor-grab active:cursor-grabbing select-none"
            style={{ aspectRatio: "16/9" }}
            onWheel={handleWheel}
            onMouseDown={handleMouseDown}
            onMouseMove={handleMouseMove}
            onMouseUp={handleMouseUp}
            onMouseLeave={handleMouseUp}
          >
            <div
              className="absolute inset-0 flex items-center justify-center transition-transform duration-100"
              style={{
                transform: `scale(${zoom}) translate(${pan.x / zoom}px, ${pan.y / zoom}px)`,
              }}
            >
              <Camera className="h-16 w-16 text-muted-foreground/20" />

              {/* AI overlay in zoomed view */}
              {camera.status === "online" && camera.detections.length > 0 && (
                <div className="absolute inset-0 pointer-events-none">
                  {camera.detections.map((det, di) => (
                    <div
                      key={di}
                      className="absolute border-2 border-ai-glow rounded"
                      style={{
                        left: `${25 + di * 20}%`,
                        top: `${20 + di * 15}%`,
                        width: "20%",
                        height: "40%",
                      }}
                    >
                      <span className="absolute -top-5 left-0 text-[10px] bg-ai-glow/90 text-foreground px-1.5 py-0.5 rounded font-mono">
                        {det}
                      </span>
                    </div>
                  ))}
                </div>
              )}
            </div>

            {/* Zoom indicator */}
            <div className="absolute top-3 left-3 flex gap-2">
              <Badge className="ai-badge text-[10px]">
                <Bot className="h-3 w-3" /> {camera.aiLabel}
              </Badge>
              <Badge variant="outline" className="text-[10px] bg-card/50 backdrop-blur border-border text-foreground">
                {zoom.toFixed(1)}x
              </Badge>
            </div>

            {/* Status overlay */}
            {camera.status !== "online" && (
              <div className="absolute inset-0 bg-background/60 flex items-center justify-center">
                <Badge className={`text-sm ${camera.status === "offline" ? "bg-destructive text-destructive-foreground" : "bg-warning text-warning-foreground"}`}>
                  {camera.status === "offline" ? "FEED OFFLINE" : "WARNING"}
                </Badge>
              </div>
            )}
          </div>

          {/* Zoom controls */}
          <div className="flex items-center gap-3 p-2 rounded-md bg-muted/30 border border-border">
            <ZoomOut className="h-4 w-4 text-muted-foreground" />
            <Slider
              value={[zoom]}
              onValueChange={([v]) => setZoom(v)}
              min={1}
              max={5}
              step={0.1}
              className="flex-1"
            />
            <ZoomIn className="h-4 w-4 text-muted-foreground" />
            <Button variant="ghost" size="sm" className="text-xs gap-1" onClick={resetZoom}>
              <Move className="h-3 w-3" /> Reset
            </Button>
          </div>

          {/* Camera details */}
          <div className="grid grid-cols-2 gap-4">
            <div>
              <Label className="text-xs text-muted-foreground">Status</Label>
              <p className="text-sm font-medium text-foreground flex items-center gap-2">
                <span className={`status-dot status-dot-${camera.aiStatus}`} />
                {camera.status} — {camera.aiLabel}
              </p>
            </div>
            <div>
              <Label className="text-xs text-muted-foreground">Audio</Label>
              <p className="text-sm font-medium text-foreground flex items-center gap-1">
                {camera.hasAudio ? <Volume2 className="h-3 w-3" /> : <VolumeX className="h-3 w-3" />}
                {camera.hasAudio ? "Enabled" : "Disabled"}
              </p>
            </div>
            <div>
              <Label className="text-xs text-muted-foreground">AI Detections</Label>
              <div className="flex gap-1 flex-wrap mt-1">
                {camera.detections.length > 0 ? camera.detections.map((d, i) => (
                  <Badge key={i} variant="outline" className="text-[10px]">{d}</Badge>
                )) : <span className="text-xs text-muted-foreground">None</span>}
              </div>
            </div>
            <div>
              <Label className="text-xs text-muted-foreground">Zoom</Label>
              <p className="text-sm text-muted-foreground">Scroll to zoom, drag to pan</p>
            </div>
          </div>

          <div className="flex gap-2">
            <Button variant="outline" size="sm" className="gap-1"><Maximize2 className="h-3 w-3" /> Full Screen</Button>
          </div>
        </div>

        {/* ONVIF Profile S controls: streams / PTZ / image / audio */}
        <div className="lg:max-h-[70vh] lg:overflow-y-auto">
          <CameraControlPanel
            cameraId={camera.id}
            cameraName={camera.name}
          />
        </div>
        </div>
      </DialogContent>
    </Dialog>
  );
};

export default CameraZoomDialog;
