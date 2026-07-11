import { useState, useEffect, useRef, useCallback } from "react";
import {
  Search, Download, Play, Mic, X, Clock, Camera,
  Users, ShieldAlert, Flame, AlertTriangle, Zap, Eye,
  Activity, Loader2, ChevronRight
} from "lucide-react";
import { AppLayout } from "@/components/layout/AppLayout";
import { Input } from "@/components/ui/input";
import { Button } from "@/components/ui/button";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import { Badge } from "@/components/ui/badge";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui/select";
import { Label } from "@/components/ui/label";
import { useRecordings } from "@/hooks/use-recordings";
import { useCameras } from "@/hooks/use-cameras";
import { getDownloadUrl, apiPost } from "@/lib/api";
import { format } from "date-fns";
import { useNavigate } from "react-router-dom";
import { useToast } from "@/hooks/use-toast";

// Smart search suggestion presets
const SMART_PRESETS = [
  { icon: Users,        label: "Man in red shirt",         query: "person red shirt passenger area",   color: "text-orange-400" },
  { icon: Users,        label: "Crowd near door",          query: "crowd congestion near door",         color: "text-orange-400" },
  { icon: Flame,        label: "Smoke / Cigarette",        query: "smoke fumes cigarette passenger",    color: "text-red-400"    },
  { icon: ShieldAlert,  label: "Intrusion alert",          query: "intrusion trespass",                 color: "text-red-400"    },
  { icon: AlertTriangle,label: "Person fallen / leaning",  query: "person fallen leaning outside",      color: "text-red-400"    },
  { icon: Zap,          label: "Stone pelting",            query: "stone pelting exterior side camera", color: "text-orange-500" },
  { icon: Eye,          label: "Animal on track",          query: "animal obstacle track front camera", color: "text-purple-400" },
  { icon: Activity,     label: "Emergency brake",          query: "emergency brake driver cab",          color: "text-red-500"   },
  { icon: Camera,       label: "Driver cab — yesterday",   query: "driver cab yesterday",               color: "text-blue-400"   },
  { icon: Users,        label: "Crew absent from seat",    query: "crew absent seat",                   color: "text-yellow-400" },
  { icon: Zap,          label: "OHE / Pantograph defect",  query: "ohe pantograph arcing defect",       color: "text-yellow-500" },
  { icon: Activity,     label: "Mobile phone use",         query: "mobile phone crew",                  color: "text-yellow-400" },
];

const SEVERITY_COLORS: Record<string, string> = {
  CRITICAL: "bg-red-500/10 text-red-400 border-red-500/20",
  EMERGENCY:"bg-red-500/10 text-red-400 border-red-500/20",
  ERROR:    "bg-red-500/10 text-red-400 border-red-500/20",
  WARNING:  "bg-yellow-500/10 text-yellow-400 border-yellow-500/20",
  INFO:     "bg-blue-500/10 text-blue-400 border-blue-500/20",
};

const SearchPage = () => {
  const navigate = useNavigate();
  const { toast } = useToast();
  const inputRef = useRef<HTMLInputElement>(null);

  // Smart search state
  const [smartQuery, setSmartQuery] = useState("");
  const [smartResults, setSmartResults] = useState<any>(null);
  const [smartLoading, setSmartLoading] = useState(false);
  const [suggestions, setSuggestions] = useState<any[]>([]);
  const [showSuggestions, setShowSuggestions] = useState(false);

  // Traditional filter state
  const [cameraId, setCameraId] = useState<string>("all");
  const [startDate, setStartDate] = useState("");
  const [endDate, setEndDate] = useState("");
  const [status, setStatus] = useState<string>("all");
  const [activeTab, setActiveTab] = useState<"smart"|"filter">("smart");

  const { data: cameras } = useCameras();
  const { data: recordings, isLoading: recLoading } = useRecordings({
    camera_id: cameraId !== "all" ? parseInt(cameraId) : undefined,
    start_date: startDate || undefined,
    end_date: endDate || undefined,
    status: status !== "all" ? status : undefined,
    limit: 100,
  });

  // Fetch autocomplete suggestions
  const fetchSuggestions = useCallback(async (q: string) => {
    if (q.length < 2) { setSuggestions([]); return; }
    try {
      const data = await fetch(`/api/search/suggestions?q=${encodeURIComponent(q)}`, {
        headers: { Authorization: `Bearer ${localStorage.getItem("nvr_token") ?? ""}` },
      }).then(r => r.json());
      setSuggestions(data.suggestions || []);
    } catch {}
  }, []);

  useEffect(() => {
    const t = setTimeout(() => { if (smartQuery) fetchSuggestions(smartQuery); }, 250);
    return () => clearTimeout(t);
  }, [smartQuery, fetchSuggestions]);

  const runSmartSearch = async (q: string) => {
    if (!q.trim()) return;
    setSmartLoading(true);
    setShowSuggestions(false);
    setSmartResults(null);
    try {
      const data = await apiPost<any>("/api/search", { q, limit: 50 });
      setSmartResults(data);
    } catch (err: any) {
      toast({ title: "Search failed", description: err.message, variant: "destructive" });
    } finally {
      setSmartLoading(false);
    }
  };

  const handleKeyDown = (e: React.KeyboardEvent) => {
    if (e.key === "Enter") { runSmartSearch(smartQuery); }
    if (e.key === "Escape") { setShowSuggestions(false); }
  };

  const formatSize = (b: number | null) => {
    if (!b) return "—";
    if (b > 1e9) return `${(b / 1e9).toFixed(1)} GB`;
    return `${(b / 1e6).toFixed(0)} MB`;
  };

  return (
    <AppLayout>
      <div className="space-y-5">
        {/* Header */}
        <div>
          <h1 className="text-2xl font-bold text-foreground">Smart Search</h1>
          <p className="text-sm text-muted-foreground">
            Natural language search — try "man in red shirt", "smoke near door", "intrusion yesterday"
          </p>
        </div>

        {/* Tab bar */}
        <div className="flex gap-1 border-b border-border">
          {(["smart", "filter"] as const).map(tab => (
            <button
              key={tab}
              onClick={() => setActiveTab(tab)}
              className={`px-4 py-2 text-sm font-medium transition-colors border-b-2 -mb-px ${
                activeTab === tab
                  ? "border-primary text-primary"
                  : "border-transparent text-muted-foreground hover:text-foreground"
              }`}
            >
              {tab === "smart" ? "🔍 AI Smart Search" : "⚙️ Filter Search"}
            </button>
          ))}
        </div>

        {/* ── Smart search tab ── */}
        {activeTab === "smart" && (
          <div className="space-y-4">
            {/* Search bar */}
            <div className="relative">
              <div className="flex gap-2">
                <div className="relative flex-1">
                  <Search className="absolute left-3 top-1/2 -translate-y-1/2 h-4 w-4 text-muted-foreground" />
                  <Input
                    ref={inputRef}
                    placeholder='e.g. "man in red shirt near door" or "crowd passenger area morning"'
                    className="pl-9 pr-9 h-11 text-sm"
                    value={smartQuery}
                    onChange={e => { setSmartQuery(e.target.value); setShowSuggestions(true); }}
                    onKeyDown={handleKeyDown}
                    onFocus={() => setShowSuggestions(true)}
                  />
                  {smartQuery && (
                    <button className="absolute right-3 top-1/2 -translate-y-1/2 text-muted-foreground hover:text-foreground"
                      onClick={() => { setSmartQuery(""); setSmartResults(null); setSuggestions([]); }}>
                      <X className="h-4 w-4" />
                    </button>
                  )}
                  {/* Suggestions dropdown */}
                  {showSuggestions && suggestions.length > 0 && (
                    <div className="absolute top-full left-0 right-0 z-50 mt-1 bg-card border border-border rounded-lg shadow-lg overflow-hidden">
                      {suggestions.map((s, i) => (
                        <button key={i} className="w-full text-left px-3 py-2 text-sm hover:bg-muted/50 flex items-center gap-2 transition-colors"
                          onClick={() => { setSmartQuery(s.query); setShowSuggestions(false); runSmartSearch(s.query); }}>
                          <ChevronRight className="h-3 w-3 text-muted-foreground shrink-0" />
                          <span className="font-medium text-foreground">{s.label}</span>
                          {s.description && <span className="text-xs text-muted-foreground truncate">{s.description}</span>}
                        </button>
                      ))}
                    </div>
                  )}
                </div>
                <Button className="h-11 px-6" onClick={() => runSmartSearch(smartQuery)} disabled={smartLoading || !smartQuery}>
                  {smartLoading ? <Loader2 className="h-4 w-4 animate-spin" /> : "Search"}
                </Button>
              </div>
            </div>

            {/* Preset chips */}
            {!smartResults && !smartLoading && (
              <>
                <div>
                  <p className="text-xs text-muted-foreground mb-2 font-medium uppercase tracking-wide">Quick searches</p>
                  <div className="flex flex-wrap gap-2">
                    {SMART_PRESETS.map(p => {
                      const Icon = p.icon;
                      return (
                        <button
                          key={p.query}
                          onClick={() => { setSmartQuery(p.query); runSmartSearch(p.query); }}
                          className="flex items-center gap-1.5 px-3 py-1.5 rounded-full border border-border bg-card hover:border-primary/40 hover:bg-muted/50 transition-colors text-sm"
                        >
                          <Icon className={`h-3.5 w-3.5 ${p.color}`} />
                          <span className="text-foreground">{p.label}</span>
                        </button>
                      );
                    })}
                  </div>
                </div>
              </>
            )}

            {/* Search results */}
            {smartLoading && (
              <div className="text-center py-12">
                <Loader2 className="h-8 w-8 mx-auto animate-spin text-primary" />
                <p className="text-sm text-muted-foreground mt-2">Searching across events and recordings…</p>
              </div>
            )}

            {smartResults && !smartLoading && (
              <div className="space-y-4">
                {/* Parsed query info */}
                {smartResults.parsed && (
                  <div className="flex flex-wrap gap-1.5 items-center">
                    <span className="text-xs text-muted-foreground">Detected:</span>
                    {smartResults.parsed.eventTypes?.map((t: string) => (
                      <Badge key={t} variant="secondary" className="text-[10px]">📌 {t}</Badge>
                    ))}
                    {smartResults.parsed.cameraTypes?.map((t: string) => (
                      <Badge key={t} variant="secondary" className="text-[10px]">📷 {t}</Badge>
                    ))}
                    {smartResults.parsed.colors?.map((c: string) => (
                      <Badge key={c} variant="secondary" className="text-[10px]">🎨 {c}</Badge>
                    ))}
                    {smartResults.parsed.clothing?.map((c: string) => (
                      <Badge key={c} variant="secondary" className="text-[10px]">👕 {c}</Badge>
                    ))}
                    {smartResults.parsed.timeFilter && (
                      <Badge variant="secondary" className="text-[10px]">🕐 time filtered</Badge>
                    )}
                    <span className="text-xs text-muted-foreground ml-1">→ {smartResults.total} result(s)</span>
                  </div>
                )}

                {/* Events results */}
                {smartResults.events?.length > 0 && (
                  <div>
                    <p className="text-xs font-semibold text-muted-foreground uppercase tracking-wide mb-2">
                      Events ({smartResults.events.length})
                    </p>
                    <div className="space-y-2">
                      {smartResults.events.map((ev: any) => (
                        <Card key={ev.event_id} className="bg-card border-border hover:border-primary/30 transition-colors">
                          <CardContent className="p-3">
                            <div className="flex items-start gap-3">
                              <div className={`mt-0.5 w-2 h-2 rounded-full shrink-0 ${
                                ev.severity === "CRITICAL" || ev.severity === "EMERGENCY" ? "bg-red-500" :
                                ev.severity === "WARNING" ? "bg-yellow-400" : "bg-blue-400"
                              }`} />
                              <div className="flex-1 min-w-0">
                                <div className="flex flex-wrap gap-1.5 items-center mb-0.5">
                                  <span className="text-sm font-medium text-foreground">{ev.title}</span>
                                  <Badge variant="outline" className={`text-[9px] ${SEVERITY_COLORS[ev.severity] || ""}`}>{ev.severity}</Badge>
                                  <Badge variant="secondary" className="text-[9px]">{ev.event_type}</Badge>
                                </div>
                                {ev.description && <p className="text-xs text-muted-foreground truncate">{ev.description}</p>}
                                <div className="flex gap-3 mt-1 text-[10px] text-muted-foreground">
                                  {ev.camera_name && <span>📷 {ev.camera_name}</span>}
                                  {ev.camera_type && <span>{ev.camera_type}</span>}
                                  <span>{format(new Date(ev.occurred_at), "dd MMM yyyy HH:mm")}</span>
                                </div>
                              </div>
                              {ev.video_clip_path && (
                                <Button size="sm" variant="ghost" className="h-7 w-7 p-0 shrink-0">
                                  <Play className="h-3 w-3" />
                                </Button>
                              )}
                            </div>
                          </CardContent>
                        </Card>
                      ))}
                    </div>
                  </div>
                )}

                {/* Recording results */}
                {smartResults.recordings?.length > 0 && (
                  <div>
                    <p className="text-xs font-semibold text-muted-foreground uppercase tracking-wide mb-2">
                      Recordings ({smartResults.recordings.length})
                    </p>
                    <div className="space-y-2">
                      {smartResults.recordings.map((rec: any) => (
                        <Card key={rec.recording_id} className="bg-card border-border hover:border-primary/30">
                          <CardContent className="p-3 flex items-center justify-between">
                            <div className="min-w-0 flex-1">
                              <p className="text-sm font-medium text-foreground truncate">{rec.camera_name || `Camera ${rec.camera_id}`}</p>
                              <div className="flex gap-2 text-xs text-muted-foreground mt-0.5">
                                <span>{format(new Date(rec.start_timestamp), "dd MMM yyyy HH:mm")}</span>
                                {rec.duration_seconds && <span>{Math.floor(rec.duration_seconds / 60)}m</span>}
                                <span>{formatSize(rec.file_size_bytes)}</span>
                                {rec.gps_speed_kmh && <span>🚂 {rec.gps_speed_kmh}km/h</span>}
                              </div>
                            </div>
                            <div className="flex gap-1 shrink-0 ml-2">
                              <Button size="sm" variant="ghost" className="h-7 w-7 p-0"
                                onClick={() => navigate(`/player?recording=${rec.recording_id}`)}>
                                <Play className="h-3 w-3" />
                              </Button>
                              <Button size="sm" variant="ghost" className="h-7 w-7 p-0" asChild>
                                <a href={getDownloadUrl(rec.recording_id)} download><Download className="h-3 w-3" /></a>
                              </Button>
                            </div>
                          </CardContent>
                        </Card>
                      ))}
                    </div>
                  </div>
                )}

                {/* No results */}
                {smartResults.events?.length === 0 && smartResults.recordings?.length === 0 && (
                  <div className="text-center py-10">
                    <Search className="h-10 w-10 mx-auto mb-2 text-muted-foreground/30" />
                    <p className="text-sm font-medium text-foreground">No results found</p>
                    <p className="text-xs text-muted-foreground mt-1">Try one of the suggested queries below</p>
                    {smartResults.suggestions && (
                      <div className="flex flex-wrap gap-2 justify-center mt-3">
                        {smartResults.suggestions.map((s: string) => (
                          <button key={s} onClick={() => { setSmartQuery(s.replace("Try: ", "").replace(/"/g, "")); runSmartSearch(s.replace("Try: ", "").replace(/"/g, "")); }}
                            className="text-xs px-2 py-1 rounded bg-muted text-foreground hover:bg-muted/80">
                            {s}
                          </button>
                        ))}
                      </div>
                    )}
                  </div>
                )}
              </div>
            )}
          </div>
        )}

        {/* ── Filter search tab ── */}
        {activeTab === "filter" && (
          <div className="space-y-4">
            <Card className="bg-card border-border">
              <CardContent className="p-4">
                <div className="grid grid-cols-2 md:grid-cols-4 gap-3">
                  <div>
                    <Label className="text-xs">Camera</Label>
                    <Select value={cameraId} onValueChange={setCameraId}>
                      <SelectTrigger className="h-8 mt-1"><SelectValue placeholder="All cameras" /></SelectTrigger>
                      <SelectContent>
                        <SelectItem value="all">All cameras</SelectItem>
                        {cameras?.map(c => <SelectItem key={c.camera_id} value={String(c.camera_id)}>{c.camera_name}</SelectItem>)}
                      </SelectContent>
                    </Select>
                  </div>
                  <div>
                    <Label className="text-xs">From</Label>
                    <Input type="datetime-local" className="h-8 mt-1 text-xs" value={startDate} onChange={e => setStartDate(e.target.value)} />
                  </div>
                  <div>
                    <Label className="text-xs">To</Label>
                    <Input type="datetime-local" className="h-8 mt-1 text-xs" value={endDate} onChange={e => setEndDate(e.target.value)} />
                  </div>
                  <div>
                    <Label className="text-xs">Status</Label>
                    <Select value={status} onValueChange={setStatus}>
                      <SelectTrigger className="h-8 mt-1"><SelectValue /></SelectTrigger>
                      <SelectContent>
                        <SelectItem value="all">All</SelectItem>
                        <SelectItem value="COMPLETED">Completed</SelectItem>
                        <SelectItem value="RECORDING">Recording</SelectItem>
                      </SelectContent>
                    </Select>
                  </div>
                </div>
              </CardContent>
            </Card>

            <p className="text-sm text-muted-foreground">{recLoading ? "Searching..." : `${recordings?.length || 0} recordings found`}</p>

            <div className="space-y-2">
              {recordings?.map(rec => (
                <Card key={rec.recording_id} className="bg-card border-border hover:border-primary/30">
                  <CardContent className="p-3 flex items-center justify-between">
                    <div className="flex-1 min-w-0">
                      <div className="flex items-center gap-2 mb-1">
                        <span className="font-medium text-sm text-foreground truncate">{rec.camera_name || `Camera ${rec.camera_id}`}</span>
                        <Badge variant="outline" className="text-[10px] shrink-0">{rec.status}</Badge>
                      </div>
                      <div className="flex gap-3 text-xs text-muted-foreground flex-wrap">
                        <span>{format(new Date(rec.start_timestamp), "dd MMM yyyy HH:mm:ss")}</span>
                        {rec.duration_seconds && <span>{Math.floor(rec.duration_seconds / 60)}m {rec.duration_seconds % 60}s</span>}
                        <span>{formatSize(rec.file_size_bytes)}</span>
                        <span>{rec.video_codec}</span>
                        {rec.gps_speed_kmh && <span>🚂 {rec.gps_speed_kmh}km/h</span>}
                      </div>
                      {rec.location_description && <p className="text-[10px] text-muted-foreground mt-0.5 truncate">{rec.location_description}</p>}
                    </div>
                    <div className="flex gap-1 shrink-0 ml-2">
                      <Button size="sm" variant="ghost" className="h-7 w-7 p-0" onClick={() => navigate(`/player?recording=${rec.recording_id}`)}>
                        <Play className="h-3 w-3" />
                      </Button>
                      <Button size="sm" variant="ghost" className="h-7 w-7 p-0" asChild>
                        <a href={getDownloadUrl(rec.recording_id)} download><Download className="h-3 w-3" /></a>
                      </Button>
                    </div>
                  </CardContent>
                </Card>
              ))}
              {!recLoading && !recordings?.length && (
                <div className="text-center py-12 text-muted-foreground">
                  <Search className="h-10 w-10 mx-auto mb-2 opacity-30" />
                  <p className="text-sm">No recordings match your filters</p>
                </div>
              )}
            </div>
          </div>
        )}
      </div>
    </AppLayout>
  );
};

export default SearchPage;
