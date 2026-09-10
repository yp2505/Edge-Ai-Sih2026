import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// https://vitejs.dev/config/
export default defineConfig({
  plugins: [
    react(),
    {
      name: 'realtime-sync',
      configureServer(server) {
        server.middlewares.use('/api/realtime', async (req, res) => {
          res.setHeader('Content-Type', 'application/json')
          res.setHeader('Access-Control-Allow-Origin', '*')
          try {
            const controller = new AbortController()
            const timeout = setTimeout(() => controller.abort(), 3000)
            const r = await fetch('https://timeapi.io/api/time/current/zone?timeZone=Asia/Kolkata', {
              signal: controller.signal
            })
            clearTimeout(timeout)
            if (r.ok) {
              const data = await r.json()
              const realEpoch = new Date(data.dateTime).getTime()
              return res.end(JSON.stringify({
                ok: true,
                epochMs: realEpoch,
                dateTime: data.dateTime,
                timeZone: data.timeZone,
                offsetMs: realEpoch - Date.now()
              }))
            }
          } catch (e) {}

          // Fallback: If network offline, calculate standard IST offset (+5.5 hours from CMOS UTC)
          // Since local CMOS is set to UTC, IST is +5.5 hours
          const fallbackEpoch = Date.now() + (5.5 * 60 * 60 * 1000)
          res.end(JSON.stringify({
            ok: true,
            epochMs: fallbackEpoch,
            fallback: true,
            offsetMs: 5.5 * 60 * 60 * 1000
          }))
        })
      }
    }
  ],
  server: {
    port: 5173,
    proxy: {
      // Proxy API calls to the Python server during dev
      '/api': {
        target: 'http://localhost:8080',
        changeOrigin: true,
      }
    }
  }
})
