import { useState, useEffect, useCallback } from 'react'
import NeuralBackground from './components/NeuralBackground.jsx'
import HudHeader from './components/HudHeader.jsx'
import CinematicBlob from './components/CinematicBlob.jsx'
import CyberTerminal from './components/CyberTerminal.jsx'
import LatencyRadar from './components/LatencyRadar.jsx'
import './App.css'

const API = 'http://localhost:8080'

export default function App() {
  const [events, setEvents] = useState([])
  const [health, setHealth] = useState(null)
  const [serverUp, setServerUp] = useState(false)

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
    const e = setInterval(fetchEvents, 800)
    return () => { clearInterval(h); clearInterval(e) }
  }, [fetchHealth, fetchEvents])

  const latestEvent = events.length > 0 ? events[events.length - 1] : null
  const isProcessing = latestEvent && (Date.now() - new Date(latestEvent.timestamp).getTime() < 3000)

  return (
    <div className="spatial-layout">
      {/* 3D Neural Background */}
      <div className="neural-canvas-container">
        <NeuralBackground isActive={isProcessing} />
      </div>

      {/* Glass Header */}
      <div className="spatial-header">
        <HudHeader health={health} serverUp={serverUp} totalEvents={events.length} />
      </div>

      {/* Main Glass Grid */}
      <div className="spatial-content">
        
        {/* Left: Giant Brutalist Latency */}
        <div className="glass-panel" style={{ padding: 40, justifyContent: 'center' }}>
          <h2 className="brutal-title" style={{ fontSize: 16, color: 'var(--text-muted)', marginBottom: 20 }}>END-TO-END LATENCY</h2>
          {latestEvent ? (
            <div>
              <div className="mono" style={{ fontSize: 80, fontWeight: 900, color: 'var(--accent-orange)', lineHeight: 0.9, letterSpacing: -4 }}>
                {(latestEvent.kw_to_connect_ms||0)+(latestEvent.receive_gap_ms||0)+(latestEvent.transcribe_ms||0)}
              </div>
              <div className="mono" style={{ fontSize: 20, color: 'var(--text-main)', marginTop: 10 }}>MILLISECONDS</div>
            </div>
          ) : (
            <div className="mono" style={{ fontSize: 60, color: 'var(--text-muted)' }}>--</div>
          )}
          
          <div style={{ marginTop: 60 }}>
            <h2 className="brutal-title" style={{ fontSize: 14, color: 'var(--text-muted)', marginBottom: 10 }}>NETWORK RADAR</h2>
            <div style={{ height: 180, margin: '0 -20px' }}>
              <LatencyRadar events={events} isMinimal={true} />
            </div>
          </div>
        </div>

        {/* Center: Apple Vision Pro style Orb */}
        <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center' }}>
          <CinematicBlob latestEvent={latestEvent} />
        </div>

        {/* Right: Glass Terminal */}
        <div className="glass-panel">
          <CyberTerminal events={events} isGlass={true} />
        </div>

      </div>
    </div>
  )
}
