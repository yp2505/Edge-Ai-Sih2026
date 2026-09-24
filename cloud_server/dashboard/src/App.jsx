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
  const [deviceInfo, setDeviceInfo] = useState({ connected: false, port: null, device: null })
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
      const r = await fetch(`${API}/api/health`, { signal: AbortSignal.timeout(1500) })
      if (r.ok) {
        const d = await r.json()
        setHealth(d)
        const isOnline = d.server_online !== undefined ? Boolean(d.server_online) : true
        setServerUp(isOnline)
        if (d.device) setDeviceInfo(d.device)
      } else {
        setServerUp(false)
        setHealth(null)
      }
    } catch {
      setServerUp(false)
      setHealth(null)
    }
  }, [])

  const fetchDevice = useCallback(async () => {
    try {
      const r = await fetch(`${API}/api/device`, { signal: AbortSignal.timeout(1500) })
      if (r.ok) {
        const d = await r.json()
        setDeviceInfo(d)
      }
    } catch {}
  }, [])

  useEffect(() => {
    fetchHealth()
    fetchDevice()
    get('/api/events', setEvents)
    get('/api/telemetry', setTelemetry, d => Object.keys(d).length ? d : null)
    const h = setInterval(fetchHealth, 1200)
    const dev = setInterval(fetchDevice, 1000)
    const e = setInterval(() => get('/api/events', setEvents), 800)
    // SSE for real-time telemetry instead of polling
    let eventSource = null
    try {
      eventSource = new EventSource(`${API}/api/telemetry-stream`)
      eventSource.onmessage = (ev) => {
        try {
          const d = JSON.parse(ev.data)
          if (d) {
            if (d.type === 'device') {
              setDeviceInfo(d)
            } else if (Object.keys(d).length) {
              setTelemetry(d)
              if (d.device_connected !== undefined) {
                setDeviceInfo(prev => ({
                  ...prev,
                  connected: d.device_connected,
                  port: d.device_port || prev.port,
                  device: d.device_connected ? (prev.device || 'ESP32 DevKit') : null
                }))
              }
            }
          }
        } catch {}
      }
      eventSource.onerror = () => {
        eventSource.close()
      }
    } catch {}
    const tFallback = eventSource ? null : setInterval(() => get('/api/telemetry', setTelemetry, d => Object.keys(d).length ? d : null), 1000)
    return () => { clearInterval(h); clearInterval(dev); clearInterval(e); if (tFallback) clearInterval(tFallback); if (eventSource) eventSource.close() }
  }, [fetchHealth, fetchDevice, get])

  const latest = events.length ? events[events.length - 1] : null
  const isDevicePlugged = Boolean(deviceInfo && deviceInfo.connected)
  const telemetryStale = !serverUp || (!isDevicePlugged && (!telemetry || !telemetry.received_at || (Date.now() - new Date(telemetry.received_at).getTime() > 5000)))
  const isDeviceConnected = Boolean(isDevicePlugged || (!telemetryStale && Boolean(telemetry?.received_at)))

  const handleClearFeed = async () => {
    setEvents([])
    try {
      await fetch(`${API}/api/events/clear`, { method: 'POST' })
    } catch {}
  }

  return (
    <>
      <LiveBackground />

      {settings && <SettingsModal telemetry={telemetry} serverUp={serverUp} health={health} onClose={() => setSettings(false)} />}

      <div className="dash">

        {/* ── Top Bar ── */}
        <div className="a-bar glass"><TopBar health={health} up={serverUp} total={events.length} telemetryStale={telemetryStale} onSettings={() => setSettings(true)} deviceInfo={deviceInfo} /></div>

        {/* ── AI Companion (Top Left) ── */}
        <div className="a-ai glass ai-panel">
          <div className="ph">
            <div className="ph-l">
              <div className="ph-dot" style={{
                background: isDeviceConnected ? 'var(--green)' : 'rgba(255,255,255,0.3)',
                boxShadow: isDeviceConnected ? '0 0 10px var(--green)' : 'none'
              }} />
              <span className="ph-tag">AI Companion</span>
            </div>
            <div className="live-badge" style={{
              background: isDeviceConnected ? undefined : 'rgba(255,255,255,0.03)',
              borderColor: isDeviceConnected ? undefined : 'rgba(255,255,255,0.1)',
              color: isDeviceConnected ? undefined : 'rgba(255,255,255,0.5)'
            }}>
              <div className="live-badge-dot" style={{
                background: isDeviceConnected ? 'var(--green)' : 'rgba(255,255,255,0.3)',
                boxShadow: isDeviceConnected ? '0 0 6px var(--green)' : 'none'
              }} />
              <span>{isDeviceConnected ? 'LIVE' : 'STANDBY'}</span>
            </div>
          </div>
          <OrbPanel latest={latest} up={serverUp} pings={events.length} deviceConnected={isDeviceConnected} />
        </div>

        {/* ── Center: Voice (Top Center) ── */}
        <div className="a-mid glass">
          <div className="ph">
            <div className="ph-l">
              <IconWaveform size={11} color="var(--t3)" />
              <span className="ph-tag">Voice Activity</span>
            </div>
            <div className="live-badge" style={{
              background: isDeviceConnected ? undefined : 'rgba(255,255,255,0.03)',
              borderColor: isDeviceConnected ? undefined : 'rgba(255,255,255,0.1)',
              color: isDeviceConnected ? undefined : 'rgba(255,255,255,0.5)'
            }}>
              <div className="live-badge-dot" style={{
                background: isDeviceConnected ? 'var(--green)' : 'rgba(255,255,255,0.3)',
                boxShadow: isDeviceConnected ? '0 0 6px var(--green)' : 'none'
              }} />
              <span>{isDeviceConnected ? 'LIVE' : 'STANDBY'}</span>
            </div>
          </div>
          <WavePanel latestEvent={latest} serverUp={serverUp} telemetry={telemetry} stale={telemetryStale} deviceConnected={isDeviceConnected} />
        </div>

        {/* ── Hardware Telemetry (Bottom Left) ── */}
        <div className="a-hw glass hardware-panel">
          <div className="ph">
            <div className="ph-l">
              <IconCpu size={11} color="var(--t3)" />
              <span className="ph-tag">Hardware Telemetry</span>
            </div>
            <div className="live-badge" style={{ background: 'transparent', border: 'none', padding: 0 }}>
              <span style={{ fontSize: 8, color: isDeviceConnected ? 'var(--green)' : 'rgba(255,255,255,0.45)', fontFamily: 'var(--mono)', letterSpacing: 1.5 }}>
                {isDevicePlugged ? 'CABLE CONNECTED' : isDeviceConnected ? 'WIFI CONNECTED' : 'DISCONNECTED'}
              </span>
            </div>
          </div>
          <div className="hw-body"><HwPanel telemetry={telemetry} stale={telemetryStale} serverUp={serverUp} health={health} deviceInfo={deviceInfo} /></div>
        </div>

        {/* ── Right: Feed (Spans Rows 2 & 3) ── */}
        <div className="a-right glass">
          <div className="ph">
            <div className="ph-l">
              <IconReport size={11} color="var(--t3)" />
              <span className="ph-tag">Detection Feed</span>
            </div>
            <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
              <span className="count-badge">{events.length}</span>
              {events.length > 0 && (
                <button
                  className="feed-clear-btn"
                  onClick={handleClearFeed}
                  title="Clear detection feed"
                >
                  CLEAR
                </button>
              )}
            </div>
          </div>
          <div className="feed-body" style={{ overflowY: 'auto' }}>
            <FeedPanel events={events} />
          </div>
        </div>

        {/* ── Bottom: Pipeline / Terminal (Bottom Center) ── */}
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
            <div className="pl-body"><PipePanel latest={latest} events={events} telemetry={telemetry} deviceConnected={isDeviceConnected} /></div>
          ) : (
            <LogTerminal />
          )}
        </div>

      </div>
    </>
  )
}
