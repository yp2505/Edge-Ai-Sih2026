import { useState, useEffect, useCallback } from 'react'
import TopBar from './components/TopBar2.jsx'
import OrbPanel from './components/OrbPanel.jsx'
import WavePanel from './components/WavePanel.jsx'
import HwPanel from './components/HwPanel.jsx'
import FeedPanel from './components/FeedPanel.jsx'
import PipePanel from './components/PipePanel.jsx'
import SettingsModal from './components/SettingsModal.jsx'
import LiveBackground from './components/LiveBackground.jsx'
import LogTerminal from './components/LogTerminal.jsx'
import { IconWaveform, IconReport, IconCpu, IconTrendChart } from './components/Icons.jsx'
import './App.css'

const API = import.meta.env.VITE_API_URL || ''

export default function App() {
  const [events, setEvents] = useState([])
  const [health, setHealth] = useState(null)
  const [telemetry, setTelemetry] = useState(null)
  const [serverUp, setServerUp] = useState(false)
  const [settings, setSettings] = useState(false)
  const [bottomView, setBottomView] = useState('pipeline')

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
    // SSE for real-time telemetry instead of polling
    let eventSource = null
    try {
      eventSource = new EventSource(`${API}/api/telemetry-stream`)
      eventSource.onmessage = (ev) => {
        try {
          const d = JSON.parse(ev.data)
          if (d && Object.keys(d).length) setTelemetry(d)
        } catch {}
      }
      eventSource.onerror = () => {
        // Fallback to polling if SSE fails
        eventSource.close()
      }
    } catch {
      // Fallback: poll telemetry every 1s if EventSource not available
    }
    const tFallback = eventSource ? null : setInterval(() => get('/api/telemetry', setTelemetry, d => Object.keys(d).length ? d : null), 1000)
    return () => { clearInterval(h); clearInterval(e); if (tFallback) clearInterval(tFallback); if (eventSource) eventSource.close() }
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
          <div className="glass ai-panel" style={{ flex: 1, minHeight: 0 }}>
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
          <div className="glass hardware-panel" style={{ flexShrink: 0 }}>
            <div className="ph">
              <div className="ph-l">
                <IconCpu size={11} color="var(--t3)" />
                <span className="ph-tag">Hardware Telemetry</span>
              </div>
              <div className="live-badge" style={{ background: 'transparent', border: 'none', padding: 0 }}>
                <span style={{ fontSize: 8, color: 'var(--t4)', fontFamily: 'var(--mono)', letterSpacing: 1.5 }}>LIVE</span>
              </div>
            </div>
            <div className="hw-body"><HwPanel telemetry={telemetry} stale={telemetryStale} serverUp={serverUp} health={health} /></div>
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
              <span className="ph-tag">{bottomView === 'pipeline' ? 'Inference Pipeline' : 'Wireless Terminal'}</span>
            </div>
            <div className="bottom-tabs" role="tablist" aria-label="Bottom dashboard view">
              <button className={bottomView === 'pipeline' ? 'active' : ''} onClick={() => setBottomView('pipeline')} role="tab" aria-selected={bottomView === 'pipeline'}>PIPELINE</button>
              <button className={bottomView === 'terminal' ? 'active' : ''} onClick={() => setBottomView('terminal')} role="tab" aria-selected={bottomView === 'terminal'}>TERMINAL</button>
            </div>
          </div>
          {bottomView === 'pipeline' ? (
            <div className="pl-body"><PipePanel latest={latest} events={events} /></div>
          ) : (
            <LogTerminal />
          )}
        </div>

      </div>
    </>
  )
}
