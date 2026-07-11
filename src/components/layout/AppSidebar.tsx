import {
  LayoutDashboard,
  Camera,
  Play,
  Search,
  AlertTriangle,
  Users,
  Settings,
  Shield,
  Activity,
  Cpu,
} from "lucide-react";
import { NavLink } from "@/components/NavLink";
import { useLocation } from "react-router-dom";
import {
  Sidebar,
  SidebarContent,
  SidebarGroup,
  SidebarGroupContent,
  SidebarGroupLabel,
  SidebarMenu,
  SidebarMenuButton,
  SidebarMenuItem,
  useSidebar,
} from "@/components/ui/sidebar";

const mainNav = [
  { title: "Dashboard", url: "/", icon: LayoutDashboard },
  { title: "Camera Grid", url: "/cameras", icon: Camera },
  { title: "Video Player", url: "/player", icon: Play },
  { title: "Search", url: "/search", icon: Search },
  { title: "Events & Alarms", url: "/events", icon: AlertTriangle },
];

const adminNav = [
  { title: "User Admin", url: "/admin/users", icon: Users },
  { title: "Settings", url: "/settings", icon: Settings },
];

const aiNav = [
  { title: "AI Analytics", url: "/ai/analytics", icon: Cpu },
  { title: "AI Health", url: "/ai/health", icon: Activity },
];

export function AppSidebar() {
  const { state } = useSidebar();
  const collapsed = state === "collapsed";
  const location = useLocation();

  const renderGroup = (label: string, items: typeof mainNav) => (
    <SidebarGroup>
      <SidebarGroupLabel className="text-sidebar-foreground/50 text-[10px] uppercase tracking-wider">
        {label}
      </SidebarGroupLabel>
      <SidebarGroupContent>
        <SidebarMenu>
          {items.map((item) => (
            <SidebarMenuItem key={item.title}>
              <SidebarMenuButton asChild>
                <NavLink
                  to={item.url}
                  end={item.url === "/"}
                  className="text-sidebar-foreground/70 hover:text-sidebar-foreground hover:bg-sidebar-accent/50 transition-colors"
                  activeClassName="bg-sidebar-accent text-sidebar-primary font-medium"
                >
                  <item.icon className="mr-2 h-4 w-4 shrink-0" />
                  {!collapsed && <span className="text-sm">{item.title}</span>}
                </NavLink>
              </SidebarMenuButton>
            </SidebarMenuItem>
          ))}
        </SidebarMenu>
      </SidebarGroupContent>
    </SidebarGroup>
  );

  return (
    <Sidebar collapsible="icon" className="border-r border-sidebar-border">
      <SidebarContent className="py-2">
        {renderGroup("Monitoring", mainNav)}
        {renderGroup("AI Modules", aiNav)}
        {renderGroup("Administration", adminNav)}
      </SidebarContent>
    </Sidebar>
  );
}
