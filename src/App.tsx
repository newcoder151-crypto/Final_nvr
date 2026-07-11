import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { BrowserRouter, Route, Routes } from "react-router-dom";
import { Toaster as Sonner } from "@/components/ui/sonner";
import { Toaster } from "@/components/ui/toaster";
import { TooltipProvider } from "@/components/ui/tooltip";
import { ThemeProvider } from "@/contexts/ThemeContext";
import { AuthProvider } from "@/contexts/AuthContext";
import Dashboard from "./pages/Dashboard";
import CameraGrid from "./pages/CameraGrid";
import VideoPlayer from "./pages/VideoPlayer";
import SearchPage from "./pages/SearchPage";
import EventsPage from "./pages/EventsPage";
import UserAdmin from "./pages/UserAdmin";
import SettingsPage from "./pages/SettingsPage";
import AiAnalytics from "./pages/AiAnalytics";
import AiHealth from "./pages/AiHealth";
import Login from "./pages/Login";
import Register from "./pages/Register";
import NotFound from "./pages/NotFound";

const queryClient = new QueryClient();

const App = () => (
  <QueryClientProvider client={queryClient}>
    <ThemeProvider>
      <AuthProvider>
        <TooltipProvider>
          <Toaster />
          <Sonner />
          <BrowserRouter>
            <Routes>
              <Route path="/" element={<Dashboard />} />
              <Route path="/cameras" element={<CameraGrid />} />
              <Route path="/player" element={<VideoPlayer />} />
              <Route path="/search" element={<SearchPage />} />
              <Route path="/events" element={<EventsPage />} />
              <Route path="/admin/users" element={<UserAdmin />} />
              <Route path="/settings" element={<SettingsPage />} />
              <Route path="/ai/analytics" element={<AiAnalytics />} />
              <Route path="/ai/health" element={<AiHealth />} />
              <Route path="/login" element={<Login />} />
              <Route path="/register" element={<Register />} />
              <Route path="*" element={<NotFound />} />
            </Routes>
          </BrowserRouter>
        </TooltipProvider>
      </AuthProvider>
    </ThemeProvider>
  </QueryClientProvider>
);

export default App;
