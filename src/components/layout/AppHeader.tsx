import { Moon, Sun, Bot, LogOut, User } from "lucide-react";
import { useTheme } from "@/contexts/ThemeContext";
import { useAuth } from "@/contexts/AuthContext";
import { Button } from "@/components/ui/button";
import { SidebarTrigger } from "@/components/ui/sidebar";
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuTrigger,
} from "@/components/ui/dropdown-menu";
import { useState } from "react";
import { AiAssistDialog } from "@/components/ai/AiAssistDialog";
import NotificationsDropdown from "@/components/notifications/NotificationsDropdown";

export const AppHeader = () => {
  const { theme, toggleTheme } = useTheme();
  const { user, signOut } = useAuth();
  const [aiOpen, setAiOpen] = useState(false);

  return (
    <>
      <header className="h-14 border-b border-border bg-card/80 backdrop-blur-md flex items-center justify-between px-4 sticky top-0 z-30">
        <div className="flex items-center gap-3">
          <SidebarTrigger className="text-muted-foreground hover:text-foreground" />
          <div className="flex items-center gap-2">
            <div className="w-8 h-8 rounded-md railway-gradient flex items-center justify-center">
              <span className="text-primary-foreground font-bold text-sm">
                NVR
              </span>
            </div>
            <div className="hidden sm:block">
              <h1 className="text-sm font-semibold text-foreground leading-tight">
                Railway NVR
              </h1>
              <p className="text-[10px] text-muted-foreground leading-tight">
                AI-Powered Surveillance
              </p>
            </div>
          </div>
        </div>

        <div className="flex items-center gap-2">
          <Button
            variant="ghost"
            size="icon"
            className="relative text-muted-foreground hover:text-foreground"
            onClick={() => setAiOpen(true)}
          >
            <Bot className="h-4 w-4" />
            <span className="absolute -top-0.5 -right-0.5 w-2 h-2 rounded-full bg-ai-glow animate-pulse-glow" />
          </Button>

          <NotificationsDropdown />

          <Button
            variant="ghost"
            size="icon"
            className="text-muted-foreground hover:text-foreground"
            onClick={toggleTheme}
          >
            {theme === "dark" ? (
              <Sun className="h-4 w-4" />
            ) : (
              <Moon className="h-4 w-4" />
            )}
          </Button>

          <DropdownMenu>
            <DropdownMenuTrigger asChild>
              <Button
                variant="ghost"
                size="icon"
                className="text-muted-foreground hover:text-foreground"
              >
                <User className="h-4 w-4" />
              </Button>
            </DropdownMenuTrigger>
            <DropdownMenuContent align="end">
              <DropdownMenuItem className="text-xs text-muted-foreground">
                {user?.email || "operator@railway.gov.in"}
              </DropdownMenuItem>
              <DropdownMenuItem onClick={signOut}>
                <LogOut className="mr-2 h-4 w-4" /> Sign Out
              </DropdownMenuItem>
            </DropdownMenuContent>
          </DropdownMenu>
        </div>
      </header>

      <AiAssistDialog open={aiOpen} onOpenChange={setAiOpen} />
    </>
  );
};
