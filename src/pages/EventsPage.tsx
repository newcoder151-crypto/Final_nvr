import { useState } from "react";
import {
  AlertTriangle,
  CheckCircle2,
  RefreshCw,
  Filter,
  Camera,
} from "lucide-react";
import { AppLayout } from "@/components/layout/AppLayout";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import { Input } from "@/components/ui/input";
import { useEvents, useAcknowledgeEvent } from "@/hooks/use-events";
import { useCameras } from "@/hooks/use-cameras";
import { formatDistanceToNow, format } from "date-fns";
import { useToast } from "@/hooks/use-toast";
import { useNavigate } from "react-router-dom";

const severityColor: Record<string, string> = {
  CRITICAL: "bg-destructive/10 text-destructive border-destructive/20",
  EMERGENCY: "bg-destructive/10 text-destructive border-destructive/20",
  ERROR: "bg-destructive/10 text-destructive border-destructive/20",
  WARNING: "bg-warning/10 text-warning border-warning/20",
  INFO: "bg-blue-500/10 text-blue-400 border-blue-500/20",
};

const EventsPage = () => {
  const { toast } = useToast();
  const navigate = useNavigate();
  const [severityFilter, setSeverityFilter] = useState("");
  const [typeFilter, setTypeFilter] = useState("");
  const [cameraFilter, setCameraFilter] = useState("");
  const [search, setSearch] = useState("");
  const [page, setPage] = useState(0);
  const limit = 50;

  const {
    data: resp,
    isLoading,
    refetch,
  } = useEvents({
    severity: severityFilter || undefined,
    event_type: typeFilter || undefined,
    camera_id: cameraFilter ? parseInt(cameraFilter) : undefined,
    limit,
    offset: page * limit,
  });
  const events = resp?.events ?? [];
  const { data: cameras } = useCameras();
  const acknowledge = useAcknowledgeEvent();

  const filtered = events.filter(
    (e) =>
      !search ||
      e.title?.toLowerCase().includes(search.toLowerCase()) ||
      e.description?.toLowerCase().includes(search.toLowerCase()),
  );

  const handleAck = async (id: number) => {
    try {
      await acknowledge.mutateAsync(id);
      toast({ title: "Event acknowledged" });
      refetch();
    } catch (err: any) {
      toast({
        title: "Error",
        description: err.message,
        variant: "destructive",
      });
    }
  };

  return (
    <AppLayout>
      <div className="space-y-4">
        <div className="flex items-center justify-between flex-wrap gap-2">
          <div>
            <h1 className="text-2xl font-bold text-foreground">
              Events & Alarms
            </h1>
            <p className="text-sm text-muted-foreground">
              {filtered.length} events
            </p>
          </div>
          <Button size="sm" variant="outline" onClick={() => refetch()}>
            <RefreshCw className="h-3.5 w-3.5 mr-1" />
            Refresh
          </Button>
        </div>

        {/* Filters */}
        <div className="flex flex-wrap gap-2 items-center">
          <Filter className="h-4 w-4 text-muted-foreground" />
          <Input
            placeholder="Search…"
            value={search}
            onChange={(e) => setSearch(e.target.value)}
            className="w-44 h-8 text-xs"
          />
          <Select
            value={severityFilter}
            onValueChange={(v) => {
              setSeverityFilter(v === "all" ? "" : v);
              setPage(0);
            }}
          >
            <SelectTrigger className="w-32 h-8 text-xs">
              <SelectValue placeholder="Severity" />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value="all">All Severities</SelectItem>
              {["CRITICAL", "EMERGENCY", "ERROR", "WARNING", "INFO"].map(
                (s) => (
                  <SelectItem key={s} value={s}>
                    {s}
                  </SelectItem>
                ),
              )}
            </SelectContent>
          </Select>
          <Select
            value={typeFilter}
            onValueChange={(v) => {
              setTypeFilter(v === "all" ? "" : v);
              setPage(0);
            }}
          >
            <SelectTrigger className="w-44 h-8 text-xs">
              <SelectValue placeholder="Event Type" />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value="all">All Types</SelectItem>
              {[
                "CROWD_DENSITY",
                "INTRUSION",
                "MOTION",
                "ALARM",
                "TAMPERING",
                "SYSTEM",
                "CAMERA_OFFLINE",
              ].map((t) => (
                <SelectItem key={t} value={t}>
                  {t}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>
          <Select
            value={cameraFilter}
            onValueChange={(v) => {
              setCameraFilter(v === "all" ? "" : v);
              setPage(0);
            }}
          >
            <SelectTrigger className="w-44 h-8 text-xs">
              <SelectValue placeholder="Camera" />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value="all">All Cameras</SelectItem>
              {cameras?.map((c) => (
                <SelectItem key={c.camera_id} value={c.camera_id.toString()}>
                  {c.camera_name}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>
          {(severityFilter || typeFilter || cameraFilter || search) && (
            <Button
              size="sm"
              variant="ghost"
              className="text-xs"
              onClick={() => {
                setSeverityFilter("");
                setTypeFilter("");
                setCameraFilter("");
                setSearch("");
              }}
            >
              Clear
            </Button>
          )}
        </div>

        {/* Events list */}
        <div className="space-y-2">
          {isLoading ? (
            Array.from({ length: 8 }).map((_, i) => (
              <div
                key={i}
                className="h-16 bg-muted/30 rounded-lg animate-pulse"
              />
            ))
          ) : filtered.length === 0 ? (
            <div className="text-center py-16 text-muted-foreground">
              <AlertTriangle className="h-12 w-12 mx-auto mb-2 opacity-20" />
              <p className="text-sm">No events found</p>
            </div>
          ) : (
            filtered.map((event) => (
              <Card
                key={event.event_id}
                className={`bg-card border-border hover:border-primary/30 transition-colors ${event.is_acknowledged === 0 ? "border-l-2 border-l-warning" : ""}`}
              >
                <CardContent className="p-3">
                  <div className="flex items-start justify-between gap-3">
                    <div className="flex items-start gap-3 min-w-0 flex-1">
                      <div
                        className={`w-2 h-2 rounded-full mt-1.5 flex-shrink-0 ${event.severity === "CRITICAL" || event.severity === "EMERGENCY" ? "bg-destructive animate-pulse" : event.severity === "WARNING" ? "bg-warning" : "bg-blue-400"}`}
                      />
                      <div className="min-w-0">
                        <div className="flex items-center gap-2 flex-wrap">
                          <p className="text-sm font-medium text-foreground">
                            {event.title}
                          </p>
                          <Badge
                            variant="outline"
                            className={`text-[10px] py-0 ${severityColor[event.severity] || ""}`}
                          >
                            {event.severity}
                          </Badge>
                          <Badge
                            variant="secondary"
                            className="text-[10px] py-0"
                          >
                            {event.event_type}
                          </Badge>
                          {event.is_acknowledged === 1 && (
                            <Badge className="text-[10px] py-0 bg-muted text-muted-foreground">
                              ACK
                            </Badge>
                          )}
                        </div>
                        {event.description && (
                          <p className="text-xs text-muted-foreground mt-0.5 truncate">
                            {event.description}
                          </p>
                        )}
                        <div className="flex items-center gap-3 mt-1 text-[10px] text-muted-foreground flex-wrap">
                          {event.camera_name && (
                            <span
                              className="flex items-center gap-1 cursor-pointer hover:text-foreground"
                              onClick={() => navigate(`/cameras`)}
                            >
                              <Camera className="h-2.5 w-2.5" />
                              {event.camera_name}
                            </span>
                          )}
                          <span>
                            {formatDistanceToNow(new Date(event.occurred_at), {
                              addSuffix: true,
                            })}
                          </span>
                          <span className="hidden sm:block">
                            {format(
                              new Date(event.occurred_at),
                              "yyyy-MM-dd HH:mm:ss",
                            )}
                          </span>
                        </div>
                      </div>
                    </div>
                    <div className="flex items-center gap-1 flex-shrink-0">
                      {event.camera_id && (
                        <Button
                          size="sm"
                          variant="ghost"
                          className="h-7 text-xs"
                          title="Jump to this moment in recordings"
                          onClick={() =>
                            navigate(
                              `/player?camera=${event.camera_id}&at=${encodeURIComponent(event.occurred_at)}`,
                            )
                          }
                        >
                          Watch
                        </Button>
                      )}
                      {event.is_acknowledged === 0 && (
                        <Button
                          size="sm"
                          variant="outline"
                          className="h-7 text-xs gap-1"
                          onClick={() => handleAck(event.event_id)}
                        >
                          <CheckCircle2 className="h-3 w-3" />
                          Ack
                        </Button>
                      )}
                    </div>
                  </div>
                </CardContent>
              </Card>
            ))
          )}
        </div>

        {/* Pagination */}
        <div className="flex items-center justify-center gap-2">
          <Button
            size="sm"
            variant="outline"
            disabled={page === 0}
            onClick={() => setPage((p) => p - 1)}
          >
            ← Prev
          </Button>
          <span className="text-xs text-muted-foreground">Page {page + 1}</span>
          <Button
            size="sm"
            variant="outline"
            disabled={filtered.length < limit}
            onClick={() => setPage((p) => p + 1)}
          >
            Next →
          </Button>
        </div>
      </div>
    </AppLayout>
  );
};
export default EventsPage;
