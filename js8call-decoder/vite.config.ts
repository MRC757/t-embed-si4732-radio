import { defineConfig } from 'vite';

export default defineConfig({
  server: {
    headers: {
      // SharedArrayBuffer is NOT used anymore, but these headers don't hurt
      // and some SDR WebSocket bridges set expectations around CORS.
      'Cross-Origin-Opener-Policy':   'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
    },
  },
  preview: {
    headers: {
      'Cross-Origin-Opener-Policy':   'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
    },
  },
  // Tell Vite to treat .wasm files as static assets.
  assetsInclude: ['**/*.wasm'],
  optimizeDeps: {
    exclude: ['js8call-wasm'],
  },
});
