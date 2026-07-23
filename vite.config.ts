import { defineConfig } from "vite";
import react from "@vitejs/plugin-react-swc";
import path from "path";

export default defineConfig(({ mode }) => ({
  server: {
    host: "::",
    port: 8080,
    strictPort: true, // fail immediately if 8080 is taken — never silently bump to 8081
    hmr: { overlay: false },
    proxy: {
      "/api": { target: "http://localhost:3001", changeOrigin: true },
      "/ws": { target: "ws://localhost:3001", ws: true },
    },
  },

  // Pre-bundle every heavy dep so Vite doesn't lazy-compile on first browser request.
  // This cuts "blank screen for 10s" cold-start down to ~1-2s after the first run.
  optimizeDeps: {
    include: [
      "react",
      "react-dom",
      "react/jsx-runtime",
      "react-router-dom",
      "@tanstack/react-query",
      "lucide-react",
      "clsx",
      "tailwind-merge",
      "class-variance-authority",
      "date-fns",
      "recharts",
      "framer-motion",
      "hls.js",
      "zod",
      "react-hook-form",
      "@hookform/resolvers/zod",
      "sonner",
      "next-themes",
      "cmdk",
      "vaul",
      "embla-carousel-react",
      "react-day-picker",
      "react-resizable-panels",
      // radix primitives (most expensive to on-demand compile)
      "@radix-ui/react-dialog",
      "@radix-ui/react-dropdown-menu",
      "@radix-ui/react-select",
      "@radix-ui/react-tabs",
      "@radix-ui/react-toast",
      "@radix-ui/react-tooltip",
      "@radix-ui/react-switch",
      "@radix-ui/react-label",
      "@radix-ui/react-separator",
      "@radix-ui/react-slot",
      "@radix-ui/react-avatar",
      "@radix-ui/react-checkbox",
      "@radix-ui/react-popover",
      "@radix-ui/react-scroll-area",
      "@radix-ui/react-alert-dialog",
      "@radix-ui/react-accordion",
      "@radix-ui/react-collapsible",
      "@radix-ui/react-progress",
      "@radix-ui/react-slider",
      "@radix-ui/react-radio-group",
    ],
    // Don't force re-bundle on every start — use the cache
    force: false,
  },

  plugins: [react()],

  resolve: {
    alias: { "@": path.resolve(__dirname, "./src") },
    dedupe: [
      "react",
      "react-dom",
      "react/jsx-runtime",
      "@tanstack/react-query",
    ],
  },

  // Suppress noisy TS/circular warnings that slow down the console
  build: {
    sourcemap: mode === "development",
    rollupOptions: {
      onwarn(warning, warn) {
        if (warning.code === "CIRCULAR_DEPENDENCY") return;
        warn(warning);
      },
    },
  },
}));
