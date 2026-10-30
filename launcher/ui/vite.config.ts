import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { viteSingleFile } from 'vite-plugin-singlefile';

// The launcher embeds the UI as one self-contained HTML file (scripts, styles and fonts inlined), so it loads
// without a web server. (The friends list talks to public Nostr relays over WebSockets: docs/14 section 4.3.)
export default defineConfig({
  plugins: [react(), viteSingleFile()],
  build: {
    target: 'es2020',
    assetsInlineLimit: 100_000_000,
    cssCodeSplit: false,
    outDir: 'dist',
  },
});
