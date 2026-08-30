import { useState, useEffect, useCallback } from 'react'
import HudHeader from './components/HudHeader.jsx'
import HardwareTwin from './components/HardwareTwin.jsx'
import CinematicBlob from './components/CinematicBlob.jsx'
import CyberTerminal from './components/CyberTerminal.jsx'
import LatencyRadar from './components/LatencyRadar.jsx'
import './App.css'

const API = 'http://localhost:8080'

export default function App() {
  const [events, setEvents] = useState([])
  const [health, setHealth] = useState(null)
  const [serverUp, setServerUp] = useState(false)
  const [activeTab, setActiveTab] = useState('live') // 'live', 'config', 'diag'

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
    const h = setInterval(fetchHealth, 3000)
    const e = setInterval(fetchEvents, 800) // Fast poll for the HUD feel
    return () => { clearInterval(h); clearInterval(e) }
  }, [fetchHealth, fetchEvents])

  // Get the most recent event to trigger the Cinematic Blob
  const latestEvent = events.length > 0 ? events[events.length - 1] : null

  return (
    <div className="hud-layout">
      {/* Top Header */}
      <div className="hud-panel header-area">
        <HudHeader 
          health={health} 
          serverUp={serverUp} 
          totalEvents={events.length} 
          activeTab={activeTab} 
          setActiveTab={setActiveTab} 
        />
      </div>

      {activeTab === 'live' && (
        <>
          {/* Left Panel: Digital Twin Hardware representation */}
          <div className="hud-panel hardware-area">
            <HardwareTwin latestEvent={latestEvent} />
          </div>

          {/* Center: The Magic Blob & Transcription Typography */}
          <div className="hud-panel center-area">
            <CinematicBlob latestEvent={latestEvent} />
          </div>

          {/* Right Panel: Hacker-style rolling log feed */}
          <div className="hud-panel terminal-area">
            <CyberTerminal events={events} />
          </div>

          {/* Bottom Panel: Latency Area Chart Radar */}
          <div className="hud-panel radar-area">
            <LatencyRadar events={events} />
          </div>
        </>
      )}

      {activeTab === 'config' && (
        <div className="hud-panel full-area" style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center' }}>
          <h1 className="hero-text glow-purple" style={{ fontSize: 60, marginBottom: 40 }}>NODE CONFIG</h1>
          <div style={{ width: 600, height: 600 }}>
             <HardwareTwin latestEvent={latestEvent} />
          </div>
        </div>
      )}

      {activeTab === 'diag' && (
        <div className="hud-panel full-area" style={{ padding: 40, display: 'flex', flexDirection: 'column' }}>
          <h1 className="hero-text glow-cyan" style={{ fontSize: 40, marginBottom: 20 }}>SYS.DIAGNOSTICS</h1>
          <div style={{ flex: 1 }}>
            <LatencyRadar events={events} />
          </div>
        </div>
      )}

    </div>
  )
}
