import { useQuery } from "@tanstack/react-query";
import {
  RefreshCw,
  Wifi,
  WifiOff,
  Video,
  Network,
  Activity,
  Camera,
} from "lucide-react";
import { AppLayout } from "@/components/layout/AppLayout";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { apiGet } from "@/lib/api";
import { useCameras } from "@/hooks/use-cameras";

const STATUS_COLOR: Record<string, string> = {
  ACTIVE:      "text-green-700 dark:text-green-400  bg-green-500/10  border-green-500/20",
  INACTIVE:    "text-gray-600  dark:text-gray-400   bg-gray-500/10   border-gray-500/20",
  FAULTY:      "text-red-700   dark:text-red-400    bg-red-500/10    border-red-500/20",
  MAINTENANCE: "text-yellow-700 dark:text-yellow-400 bg-yellow-500/10 border-yellow-500/20",
};

const AiHealth = () => {
  const { data: cameras, isLoading, refetch } = useCameras();

  // Per-camera health history (latest record per camera)
  const { data: allHealthMap, refetch: refetchHealth } = useQuery({
    queryKey: ["all-camera-health"],
    queryFn: async () => {
      if (!cameras?.length) return {};
      const results: Record<number, any[]> = {};
      await Promise.all(
        cameras.map(async (cam) => {
          try {
            const d = await apiGet<{ health: any[] }>(`/api/cameras/${cam.camera_id}/health`);
            results[cam.camera_id] = d.health ?? [];
          } catch {
            results[cam.camera_id] = [];
          }
        }),
      );
      return results;
    },
    enabled: !!cameras?.length,
    refetchInterval: 15000,
  });

  const latestHealth = (camId: number) => (allHealthMap?.[camId] ?? [])[0] ?? null;

  const onlineCams  = cameras?.filter((c) => c.is_online === 1).length ?? 0;
  const activeCams  = cameras?.filter((c) => c.status === "ACTIVE").length ?? 0;
  const recordingCams = cameras?.filter((c) => c.is_recording === 1).length ?? 0;
  const faultyCams  = cameras?.filter((c) => c.status === "FAULTY").length ?? 0;
  const totalCams   = cameras?.length ?? 0;

  return (
    <AppLayout>
      <div className="space-y-5">
        {/* Header */}
        <div className="flex items-start justify-between">
          <div>
            <h1 className="text-2xl font-bold text-foreground flex items-center gap-2">
              <Camera className="h-6 w-6 text-primary" />
              Camera Health
            </h1>
            <p className="text-sm text-muted-foreground mt-0.5">
              Live connectivity status for all cameras
            </p>
          </div>
          <Button
            size="sm"
            variant="outline"
            onClick={() => { refetch(); refetchHealth(); }}
          >
            <RefreshCw className="h-3.5 w-3.5 mr-1" />
            Refresh
          </Button>
        </div>

        {/* Summary cards */}
        <div className="grid grid-cols-2 md:grid-cols-4 gap-3">
          {[
            { label: "Total Cameras",       val: totalCams,     icon: Video,     color: "text-foreground" },
            { label: "Online / Connected",  val: onlineCams,    icon: Wifi,      color: "text-green-600 dark:text-green-400" },
            { label: "Currently Recording", val: recordingCams, icon: Activity,  color: "text-red-600 dark:text-red-400" },
            { label: "Faulty",              val: faultyCams,    icon: Network,   color: faultyCams > 0 ? "text-red-600 dark:text-red-400" : "text-muted-foreground" },
          ].map(({ label, val, icon: Icon, color }) => (
            <Card key={label} className="bg-card border-border">
              <CardContent className="p-4 flex items-center gap-3">
                <div className="w-9 h-9 rounded-lg bg-muted flex items-center justify-center shrink-0">
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

        {/* Camera list */}
        <Card className="bg-card border-border">
          <CardHeader className="pb-3 border-b border-border/60">
            <CardTitle className="text-sm flex items-center gap-2 text-foreground">
              <Video className="h-4 w-4 text-muted-foreground" />
              Camera Status
              <span className="ml-auto text-xs font-normal text-muted-foreground">
                {activeCams} active · {totalCams} total
              </span>
            </CardTitle>
          </CardHeader>
          <CardContent className="p-0">
            {isLoading && (
              <div className="p-4 space-y-3">
                {Array.from({ length: 4 }).map((_, i) => (
                  <div key={i} className="h-14 bg-muted/30 rounded-lg animate-pulse" />
                ))}
              </div>
            )}

            {!isLoading && !cameras?.length && (
              <div className="text-center py-12 text-muted-foreground">
                <Video className="h-8 w-8 mx-auto mb-2 opacity-30" />
                <p className="text-sm text-foreground">No cameras configured</p>
                <p className="text-xs mt-1">Add cameras via the Camera Grid page</p>
              </div>
            )}

            <div className="divide-y divide-border">
              {cameras?.map((cam) => {
                const health   = latestHealth(cam.camera_id);
                const isOnline = (health?.is_online ?? cam.is_online) === 1;
                const isRec    = (health?.is_recording ?? cam.is_recording) === 1;

                return (
                  <div key={cam.camera_id} className="flex items-center gap-3 px-4 py-3">
                    {/* Connection indicator dot */}
                    <div
                      className={`w-2.5 h-2.5 rounded-full shrink-0 ${
                        cam.status === "FAULTY"
                          ? "bg-red-500"
                          : isOnline
                          ? "bg-green-500"
                          : cam.status === "ACTIVE"
                          ? "bg-yellow-500 animate-pulse"
                          : "bg-gray-400"
                      }`}
                    />

                    {/* Name + location */}
                    <div className="flex-1 min-w-0">
                      <div className="flex items-center gap-2 flex-wrap">
                        <p className="text-sm font-semibold text-foreground">
                          {cam.camera_name}
                        </p>
                        <Badge
                          variant="outline"
                          className={`text-[10px] ${STATUS_COLOR[cam.status] ?? ""}`}
                        >
                          {cam.status}
                        </Badge>
                        {isRec && (
                          <Badge className="text-[10px] bg-red-500/10 text-red-600 dark:text-red-400 border-red-500/20 animate-pulse">
                            ● REC
                          </Badge>
                        )}
                      </div>
                      <div className="flex items-center gap-3 mt-0.5 flex-wrap">
                        <span className="flex items-center gap-1 text-[10px] text-muted-foreground font-mono">
                          <Network className="h-2.5 w-2.5 shrink-0" />
                          {cam.ip_address || "—"}
                        </span>
                        {cam.location_description && (
                          <span className="text-[10px] text-muted-foreground">
                            {cam.location_description}
                          </span>
                        )}
                      </div>
                    </div>

                    {/* Connection status badge */}
                    <div className="shrink-0">
                      {isOnline ? (
                        <span className="flex items-center gap-1 text-xs font-medium text-green-700 dark:text-green-400">
                          <Wifi className="h-3.5 w-3.5" />
                          Connected
                        </span>
                      ) : (
                        <span className="flex items-center gap-1 text-xs font-medium text-muted-foreground">
                          <WifiOff className="h-3.5 w-3.5" />
                          Offline
                        </span>
                      )}
                    </div>
                  </div>
                );
              })}
            </div>
          </CardContent>
        </Card>
      </div>
    </AppLayout>
  );
};

export default AiHealth;
