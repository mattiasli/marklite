// @ts-check
import { readFileSync } from 'node:fs';
import { defineConfig } from 'astro/config';

// The app version lives in VERSION at the repository root, one level above this folder.
function appVersion() {
  try {
    const v = readFileSync(new URL('../VERSION', import.meta.url), 'utf8').trim();
    if (v) return v;
  } catch {
    // fall through
  }
  console.warn('[marklite] ../VERSION not found, using the fallback version');
  return '1.0.0';
}

export default defineConfig({
  site: 'https://marklite-app.vercel.app',
  trailingSlash: 'ignore',
  build: {
    format: 'file',
  },
  devToolbar: {
    enabled: false,
  },
  vite: {
    define: {
      __APP_VERSION__: JSON.stringify(appVersion()),
    },
  },
});
