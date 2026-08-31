import { useState, useEffect, useCallback } from 'react'
import HeroOrb from './components/HeroOrb.jsx'
import WavePanel from './components/WavePanel.jsx'
import DetectionFeedNew from './components/DetectionFeedNew.jsx'
import HardwareTelemetryNew from './components/HardwareTelemetryNew.jsx'
import PipelinePanel from './components/PipelinePanel.jsx'
import TopBar from './components/TopBarNew.jsx'
import SettingsModal from './components/SettingsModal.jsx'
import { IconSearch, IconWaveform, IconReport, IconCpu, IconTrendChart } from './components/Icons.jsx'
import './App.css'

const API = 'http://localhost:8080'

export default function App() {
  const [events, setEvents] = useState([])
  const [health, setHealth] = useState(null)
  const [serverUp, setServerUp] = useState(false)
  const [isSettingsOpen, setIsSettingsOpen] = useState(false)

  const fetchHealth = useCallback(async () => {
    try {
      const r = await fetch(`${API}/api/health`, { signal: AbortSignal.timeout(2000) })
      if (r.ok) { setHealth(await r.json()); setServerUp(true) }
      else setServerUp(false)
    } catch { setServerUp(false) }
  }, [])

  const fetchEvents = useCallback(async () => {
    try {
      const r = await fetch(`${API}/api/events`, { signal: AbortSignal.timeout(2000) })
      if (r.ok) setEvents(await r.json())
    } catch {}
  }, [])

  useEffect(() => {
    fetchHealth(); fetchEvents()
    const h = setInterval(fetchHealth, 2000)
    const e = setInterval(fetchEvents, 800)
    return () => { clearInterval(h); clearInterval(e) }
  }, [fetchHealth, fetchEvents])

  const latestEvent = events.length > 0 ? events[events.length - 1] : null

  return (
    <>
      {/* Soft ambient background */}
      <div className="bg-blobs" aria-hidden="true">
        <div className="blob blob-1" />
        <div className="blob blob-2" />
        <div className="blob blob-3" />
      </div>

      {isSettingsOpen && <SettingsModal onClose={() => setIsSettingsOpen(false)} />}

      <div className="app-layout">
        {/* Top Bar */}
        <div className="area-topbar panel" style={{ borderRadius: 16 }}>
          <TopBar health={health} serverUp={serverUp} totalEvents={events.length} onSettingsClick={() => setIsSettingsOpen(true)} />
        </div>

        {/* Hero — Slime Character */}
        <div className="area-hero panel">
          <div className="panel-header">
            <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
              <IconSearch size={12} color="var(--t3)" />
              <span>AI Companion</span>
            </div>
            <div className="panel-header-dot" style={{ background: serverUp ? 'var(--green)' : 'var(--t3)', boxShadow: serverUp ? '0 0 8px var(--green)' : 'none' }} />
          </div>
          <div className="panel-content" style={{ display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
            <HeroOrb latest={latestEvent} serverUp={serverUp} pingCount={events.length} />
          </div>
        </div>

        {/* Voice Waveform + Transcript */}
        <div className="area-wave panel">
          <div className="panel-header">
            <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
              <IconWaveform size={12} color="var(--t3)" />
              <span>Voice Activity</span>
            </div>
            <div style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
              <div style={{ width: 6, height: 6, borderRadius: '50%', background: 'var(--sky)', animation: 'blink-dot 2s infinite' }} />
              <span style={{ fontSize: 9, color: 'var(--sky)' }}>LIVE</span>
            </div>
          </div>
          <div className="panel-content no-pad" style={{ display: 'flex', flexDirection: 'column' }}>
            <WavePanel latestEvent={latestEvent} serverUp={serverUp} />
          </div>
        </div>

        {/* Detection Feed */}
        <div className="area-feed panel">
          <div className="panel-header">
            <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
              <IconReport size={12} color="var(--t3)" />
              <span>Detection Feed</span>
            </div>
            <span className="badge badge-primary">{events.length}</span>
          </div>
          <div className="panel-content no-pad scroll">
            <DetectionFeedNew events={events} />
          </div>
        </div>

        {/* Hardware Telemetry */}
        <div className="area-hw panel">
          <div className="panel-header">
            <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
              <IconCpu size={12} color="var(--t3)" />
              <span>Hardware</span>
            </div>
            <span style={{ fontSize: 9, color: 'var(--t3)' }}>LIVE</span>
          </div>
          <div className="panel-content" style={{ padding: '12px 16px' }}>
            <HardwareTelemetryNew latestEvent={latestEvent} />
          </div>
        </div>

        {/* Pipeline + Latency */}
        <div className="area-pipeline panel">
          <div className="panel-header">
            <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
              <IconTrendChart size={12} color="var(--t3)" />
              <span>Inference Pipeline</span>
            </div>
            <span className="mono" style={{ fontSize: 9, color: 'var(--t3)' }}>E2E TIMING</span>
          </div>
          <div className="panel-content" style={{ padding: '12px 16px' }}>
            <PipelinePanel latestEvent={latestEvent} events={events} />
          </div>
        </div>
      </div>
    </>
  )
}
