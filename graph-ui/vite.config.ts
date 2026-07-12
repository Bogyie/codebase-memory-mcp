/// <reference types="vitest" />
import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import tailwindcss from "@tailwindcss/vite";
import path from "path";

export default defineConfig({
  plugins: [react(), tailwindcss()],
  resolve: {
    alias: {
      "@": path.resolve(__dirname, "./src"),
    },
    dedupe: ["react", "react-dom", "three"],
  },
  test: {
    environment: "jsdom",
    globals: true,
  },
  build: {
    outDir: "dist",
    assetsDir: "assets",
    sourcemap: false,
    // Three.js is a single, tree-shaken engine module. It is loaded only when
    // the graph tab opens and compresses to well below this threshold.
    chunkSizeWarningLimit: 750,
    rollupOptions: {
      output: {
        manualChunks(id) {
          if (!id.includes("node_modules")) return undefined;
          if (id.includes("/postprocessing/") || id.includes("/@react-three/postprocessing/")) {
            return "graph-effects";
          }
          if (id.includes("/three-stdlib/") || id.includes("/@react-three/drei/")) {
            return "graph-controls";
          }
          if (id.includes("/@react-three/fiber/")) return "graph-react";
          if (id.includes("/three/")) return "graph-three";
          return undefined;
        },
      },
    },
  },
  server: {
    port: 5173,
    proxy: {
      "/rpc": "http://127.0.0.1:9749",
      "/api": "http://127.0.0.1:9749",
    },
  },
});
