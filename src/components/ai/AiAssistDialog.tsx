import { Dialog, DialogContent, DialogHeader, DialogTitle } from "@/components/ui/dialog";
import { Bot, Camera, AlertTriangle, TrendingUp, Shield } from "lucide-react";

interface AiAssistDialogProps {
  open: boolean;
  onOpenChange: (open: boolean) => void;
}

const mockSummary = {
  normalCameras: 13,
  warningCameras: 2,
  alertCameras: 1,
  topAlert: "Smoke detected in Coach S3 - Camera 12",
  crowdLevel: "Moderate",
  recommendation: "Camera 7 feed quality degraded — consider cleaning lens.",
};

export const AiAssistDialog = ({ open, onOpenChange }: AiAssistDialogProps) => {
  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className="sm:max-w-md">
        <DialogHeader>
          <DialogTitle className="flex items-center gap-2">
            <Bot className="h-5 w-5 text-ai-glow" />
            AI Assist — Grid Summary
          </DialogTitle>
        </DialogHeader>

        <div className="space-y-4">
          <div className="grid grid-cols-3 gap-3">
            <SummaryCard icon={<Camera className="h-4 w-4" />} label="Normal" value={mockSummary.normalCameras} color="text-success" />
            <SummaryCard icon={<AlertTriangle className="h-4 w-4" />} label="Warning" value={mockSummary.warningCameras} color="text-warning" />
            <SummaryCard icon={<Shield className="h-4 w-4" />} label="Alert" value={mockSummary.alertCameras} color="text-destructive" />
          </div>

          <div className="rounded-lg border border-border bg-muted/50 p-3 space-y-2">
            <p className="text-xs font-medium text-foreground flex items-center gap-1">
              <TrendingUp className="h-3 w-3 text-ai-glow" /> Top Alert
            </p>
            <p className="text-sm text-muted-foreground">{mockSummary.topAlert}</p>
          </div>

          <div className="rounded-lg border border-border bg-muted/50 p-3 space-y-2">
            <p className="text-xs font-medium text-foreground">Crowd Level: {mockSummary.crowdLevel}</p>
            <p className="text-xs text-muted-foreground">{mockSummary.recommendation}</p>
          </div>
        </div>
      </DialogContent>
    </Dialog>
  );
};

const SummaryCard = ({ icon, label, value, color }: { icon: React.ReactNode; label: string; value: number; color: string }) => (
  <div className="rounded-lg border border-border bg-card p-3 text-center">
    <div className={`flex justify-center mb-1 ${color}`}>{icon}</div>
    <p className="text-lg font-bold text-foreground">{value}</p>
    <p className="text-[10px] text-muted-foreground">{label}</p>
  </div>
);
