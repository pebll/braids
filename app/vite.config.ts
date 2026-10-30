// vite.config.ts
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

import { defineConfig } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import { braidsBridge } from './dev/bridge.ts';

// The Tauri dev server runs on a fixed port (see src-tauri/tauri.conf.json).
// In a plain browser (`npm run dev`), the bridge plugin connects the UI to a
// running braidsd instead of the Tauri shell.
export default defineConfig({
  plugins: [svelte(), braidsBridge()],
  clearScreen: false,
  server: {
    port: 1420,
    strictPort: true,
    watch: { ignored: ['**/src-tauri/**'] },
  },
  envPrefix: ['VITE_', 'TAURI_ENV_'],
  build: {
    target: 'es2022',
    sourcemap: false,
  },
});
