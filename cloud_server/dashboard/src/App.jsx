import { useState, useEffect, useCallback } from 'react'
import MissionHeader from './components/MissionHeader.jsx'
import NodeTopology from './components/NodeTopology.jsx'
import HardwareTelemetry from './components/HardwareTelemetry.jsx'
import Spectrogram from './components/Spectrogram.jsx'
import InferencePipeline from './components/InferencePipeline.jsx'
import RightPanel from './components/RightPanel.jsx'
import SettingsModal from './components/SettingsModal.jsx'
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
    const h = setInterval(fetchHealth, 2000) // Faster polling for density
    const e = setInterval(fetchEvents, 800)
    return () => { clearInterval(h); clearInterval(e) }
  }, [fetchHealth, fetchEvents])

  const latestEvent = events.length > 0 ? events[events.length - 1] : null

  return (
    <>
      {/* Animated 3D Background */}
      <div className="bg-grid"><div className="bg-grid-inner"></div></div>
      <div className="bg-orb orb-1"></div>
      <div className="bg-orb orb-2"></div>

      {/* Settings Modal Overlay */}
      {isSettingsOpen && <SettingsModal onClose={() => setIsSettingsOpen(false)} />}

      <div className="mission-layout">
      
      <div className="mc-panel header-area" style={{ borderRadius: 0, border: 'none', borderBottom: '1px solid var(--panel-border)' }}>
        <MissionHeader health={health} serverUp={serverUp} totalEvents={events.length} onSettingsClick={() => setIsSettingsOpen(true)} />
      </div>

      <div className="mc-panel node-area">
        <div className="mc-panel-header">NODE TOPOLOGY</div>
        <div className="mc-panel-content">
          <NodeTopology serverUp={serverUp} latestEvent={latestEvent} />
        </div>
      </div>

      <div className="mc-panel telemetry-area">
        <div className="mc-panel-header">HARDWARE TELEMETRY</div>
        <div className="mc-panel-content">
          <HardwareTelemetry latestEvent={latestEvent} />
        </div>
      </div>

      <div className="mc-panel spectrogram-area">
        <div className="mc-panel-header">
          <span>AI ACOUSTIC MODEL (SPECTROGRAM)</span>
          <span style={{ color: 'var(--accent-cyan)' }}>LIVE</span>
        </div>
        <div className="mc-panel-content" style={{ padding: 0 }}>
          <Spectrogram latestEvent={latestEvent} />
        </div>
      </div>

      <div className="mc-panel pipeline-area">
        <div className="mc-panel-header">INFERENCE PIPELINE & TIMING</div>
        <div className="mc-panel-content">
          <InferencePipeline latestEvent={latestEvent} />
        </div>
      </div>

      <div className="mc-panel datagrid-area" style={{ padding: 0 }}>
        <RightPanel events={events} />
      </div>

    </div>
    </>
  )
}
