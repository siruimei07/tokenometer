import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

const tauriDevHost = process.env.TAURI_DEV_HOST;

export default defineConfig({
  plugins: [react()],
  clearScreen: false,
  build: {
    target: "chrome105",
  },
  server: {
    port: 1420,
    strictPort: true,
    host: tauriDevHost || false,
    ...(tauriDevHost
      ? {
          hmr: {
            protocol: "ws",
            host: tauriDevHost,
            port: 1421,
          },
        }
      : {}),
    watch: {
      ignored: ["**/src-tauri/**"],
    },
  },
});
