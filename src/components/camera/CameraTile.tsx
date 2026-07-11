import { motion } from "framer-motion";
import { Camera, Volume2, VolumeX, Maximize2, Bot, WifiOff } from "lucide-react";
import { Badge } from "@/components/ui/badge";

export interface CameraData {
  id: number;
  name: string;
  location: string;
  status: "online" | "offline" | "warning";
  aiStatus: "green" | "yellow" | "red";
  aiLabel: string;
  hasAudio: boolean;
  detections: string[];
}

interface CameraTileProps {
  cam: CameraData;
  index: number;
  showOverlay: boolean;
  onClick: () => void;
}

const CameraTile = ({ cam, index, showOverlay, onClick }: CameraTileProps) => {
  return (
    <motion.div
      initial={{ opacity: 0, scale: 0.95 }}
      animate={{ opacity: 1, scale: 1 }}
      transition={{ delay: index * 0.03 }}
      className="camera-tile group cursor-pointer"
      onClick={onClick}
    >
      <div className={`absolute inset-0 ${cam.status === "offline" ? "bg-muted" : "bg-gradient-to-br from-muted/80 to-muted"}`}>
        {cam.status === "offline" ? (
          <div className="flex items-center justify-center h-full">
            <WifiOff className="h-8 w-8 text-muted-foreground/30" />
          </div>
        ) : (
          <div className="flex items-center justify-center h-full">
            <Camera className="h-8 w-8 text-muted-foreground/20" />
          </div>
        )}
      </div>

      {showOverlay && cam.status === "online" && cam.detections.length > 0 && (
        <div className="absolute inset-0 pointer-events-none">
          {cam.detections.map((det, di) => (
            <div
              key={di}
              className="absolute border-2 border-ai-glow rounded"
              style={{
                left: `${20 + di * 25}%`,
                top: `${15 + di * 20}%`,
                width: "30%",
                height: "50%",
              }}
            >
              <span className="absolute -top-4 left-0 text-[9px] bg-ai-glow/90 text-foreground px-1 rounded">
                {det}
              </span>
            </div>
          ))}
        </div>
      )}

      <div className="camera-tile-overlay" />

      <div className="absolute top-0 left-0 right-0 p-1.5 flex items-center justify-between">
        <div className="flex items-center gap-1">
          <div className={`status-dot status-dot-${cam.aiStatus}`} />
          <span className="text-[10px] font-medium text-primary-foreground drop-shadow">{cam.name}</span>
        </div>
        <div className="ai-badge text-[9px] py-0">
          <Bot className="h-2.5 w-2.5" />
          {cam.aiLabel}
        </div>
      </div>

      <div className="absolute bottom-0 left-0 right-0 p-1.5 flex items-center justify-between">
        <span className="text-[10px] text-primary-foreground/80 drop-shadow">{cam.location}</span>
        <div className="flex items-center gap-1 opacity-0 group-hover:opacity-100 transition-opacity">
          {cam.hasAudio ? (
            <Volume2 className="h-3 w-3 text-primary-foreground/80" />
          ) : (
            <VolumeX className="h-3 w-3 text-primary-foreground/40" />
          )}
          <Maximize2 className="h-3 w-3 text-primary-foreground/80" />
        </div>
      </div>

      {cam.status !== "online" && (
        <Badge
          className={`absolute top-1/2 left-1/2 -translate-x-1/2 -translate-y-1/2 text-[10px] ${
            cam.status === "offline" ? "bg-destructive text-destructive-foreground" : "bg-warning text-warning-foreground"
          }`}
        >
          {cam.status === "offline" ? "OFFLINE" : "WARNING"}
        </Badge>
      )}
    </motion.div>
  );
};

export default CameraTile;
