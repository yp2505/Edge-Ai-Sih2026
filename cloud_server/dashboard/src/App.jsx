import { useState, useEffect, useCallback } from 'react'
import TopBar from './components/TopBar2.jsx'
import OrbPanel from './components/OrbPanel.jsx'
import WavePanel from './components/WavePanel.jsx'
import HwPanel from './components/HwPanel.jsx'
import FeedPanel from './components/FeedPanel.jsx'
import PipePanel from './components/PipePanel.jsx'
import SettingsModal from './components/SettingsModal.jsx'
import LiveBackground from './components/LiveBackground.jsx'
import { IconWaveform, IconReport, IconCpu, IconTrendChart } from './components/Icons.jsx'
import './App.css'

const API = 'http://localhost:8080'

export default function App() {
  const [events, setEvents] = useState([])
  const [health, setHealth] = useState(null)
  const [telemetry, setTelemetry] = useState(null)
  const [serverUp, setServerUp] = useState(false)
  const [settings, setSettings] = useState(false)

  const get = useCallback(async (path, setter, transform) => {
    try {
      const r = await fetch(`${API}${path}`, { signal: AbortSignal.timeout(2000) })
      if (r.ok) { const d = await r.json(); setter(transform ? transform(d) : d) }
    } catch {}
  }, [])

  const fetchHealth = useCallback(async () => {
    try {
      const r = await fetch(`${API}/api/health`, { signal: AbortSignal.timeout(2000) })
      if (r.ok) { setHealth(await r.json()); setServerUp(true) }
      else setServerUp(false)
    } catch { setServerUp(false) }
  }, [])

  useEffect(() => {
    fetchHealth()
    get('/api/events', setEvents)
    get('/api/telemetry', setTelemetry, d => Object.keys(d).length ? d : null)
    const h = setInterval(fetchHealth, 2000)
    const e = setInterval(() => get('/api/events', setEvents), 800)
    const t = setInterval(() => get('/api/telemetry', setTelemetry, d => Object.keys(d).length ? d : null), 1000)
    return () => { clearInterval(h); clearInterval(e); clearInterval(t) }
  }, [fetchHealth, get])

  const latest = events.length ? events[events.length - 1] : null
  const telemetryStale = !serverUp || !telemetry || !telemetry.received_at || (Date.now() - new Date(telemetry.received_at).getTime() > 5000)

  return (
    <>
      <LiveBackground />

      {settings && <SettingsModal telemetry={telemetry} serverUp={serverUp} health={health} onClose={() => setSettings(false)} />}

      <div className="dash">

        {/* ── Top Bar ── */}
        <div className="a-bar glass"><TopBar health={health} up={serverUp} total={events.length} telemetryStale={telemetryStale} onSettings={() => setSettings(true)} /></div>

        {/* ── Left Column ── */}
        <div className="a-left">
          {/* AI Companion */}
          <div className="glass" style={{ flex: 1, minHeight: 0 }}>
            <div className="ph">
              <div className="ph-l">
                <div className="ph-dot" style={{ background: serverUp ? 'var(--green)' : 'var(--t4)', boxShadow: serverUp ? '0 0 10px var(--green)' : 'none' }} />
                <span className="ph-tag">AI Companion</span>
              </div>
              {serverUp && <div className="live-badge"><div className="live-badge-dot" /><span>LIVE</span></div>}
            </div>
            <OrbPanel latest={latest} up={serverUp} pings={events.length} />
          </div>

          {/* Hardware */}
          <div className="glass" style={{ flexShrink: 0 }}>
            <div className="ph">
              <div className="ph-l">
                <IconCpu size={11} color="var(--t3)" />
                <span className="ph-tag">Hardware Telemetry</span>
              </div>
              <div className="live-badge" style={{ background: 'transparent', border: 'none', padding: 0 }}>
                <span style={{ fontSize: 8, color: 'var(--t4)', fontFamily: 'var(--mono)', letterSpacing: 1.5 }}>LIVE</span>
              </div>
            </div>
            <div className="hw-body"><HwPanel telemetry={telemetry} stale={telemetryStale} serverUp={serverUp} /></div>
          </div>
        </div>

        {/* ── Center: Voice ── */}
        <div className="a-mid glass">
          <div className="ph">
            <div className="ph-l">
              <IconWaveform size={11} color="var(--t3)" />
              <span className="ph-tag">Voice Activity</span>
            </div>
            <div className="live-badge"><div className="live-badge-dot" /><span>LIVE</span></div>
          </div>
          <WavePanel latestEvent={latest} serverUp={serverUp} telemetry={telemetry} stale={telemetryStale} />
        </div>

        {/* ── Right: Feed ── */}
        <div className="a-right glass">
          <div className="ph">
            <div className="ph-l">
              <IconReport size={11} color="var(--t3)" />
              <span className="ph-tag">Detection Feed</span>
            </div>
            <span className="count-badge">{events.length}</span>
          </div>
          <div className="feed-body" style={{ overflowY: 'auto' }}>
            <FeedPanel events={events} />
          </div>
        </div>

        {/* ── Bottom: Pipeline ── */}
        <div className="a-bot glass">
          <div className="ph">
            <div className="ph-l">
              <IconTrendChart size={11} color="var(--t3)" />
              <span className="ph-tag">Inference Pipeline</span>
            </div>
            <span style={{ fontSize: 7, fontWeight: 700, color: 'var(--t4)', fontFamily: 'var(--mono)', letterSpacing: 1.5 }}>E2E TIMING</span>
          </div>
          <div className="pl-body"><PipePanel latest={latest} events={events} /></div>
        </div>

      </div>
    </>
  )
}
