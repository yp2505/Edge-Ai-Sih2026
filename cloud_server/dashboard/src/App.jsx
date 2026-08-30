import { useState, useEffect, useCallback } from 'react'
import Header from './components/Header.jsx'
import HeroOrb from './components/HeroOrb.jsx'
import DetectionFeed from './components/DetectionFeed.jsx'
import LatencyChart from './components/LatencyChart.jsx'
import StatsPanel from './components/StatsPanel.jsx'
import './App.css'

const API_BASE    = 'http://localhost:8080'
const POLL_EVENTS = 1500
const POLL_HEALTH = 5000

export default function App() {
  const [events,   setEvents]   = useState([])
  const [health,   setHealth]   = useState(null)
  const [serverUp, setServerUp] = useState(false)
  const [newPing,  setNewPing]  = useState(0)   // increments to trigger orb pulse

  const fetchHealth = useCallback(async () => {
    try {
      const r = await fetch(`${API_BASE}/api/health`, { signal: AbortSignal.timeout(2500) })
      if (r.ok) { setHealth(await r.json()); setServerUp(true) }
      else setServerUp(false)
    } catch { setServerUp(false) }
  }, [])

  const fetchEvents = useCallback(async () => {
    try {
      const r = await fetch(`${API_BASE}/api/events`, { signal: AbortSignal.timeout(3000) })
      if (r.ok) {
        const data = await r.json()
        setEvents(prev => {
          if (data.length > prev.length) setNewPing(n => n + 1)
          return data
        })
      }
    } catch { /* server down — health check handles indicator */ }
  }, [])

  useEffect(() => {
    fetchHealth(); fetchEvents()
    const hTimer = setInterval(fetchHealth, POLL_HEALTH)
    const eTimer = setInterval(fetchEvents, POLL_EVENTS)
    return () => { clearInterval(hTimer); clearInterval(eTimer) }
  }, [fetchHealth, fetchEvents])

  // Derived stats
  const totals = events.map(e =>
    (e.kw_to_connect_ms ?? 0) + (e.receive_gap_ms ?? 0) + (e.transcribe_ms ?? 0)
  )
  const stats = {
    count: events.length,
    avg:   totals.length ? Math.round(totals.reduce((a,b) => a+b, 0) / totals.length) : 0,
    min:   totals.length ? Math.min(...totals) : 0,
    max:   totals.length ? Math.max(...totals) : 0,
  }

  const latest = events.length ? events[events.length - 1] : null

  return (
    <div className="app-shell">
      {/* Ambient background orbs */}
      <div className="ambient" aria-hidden="true">
        <div className="ambient-orb orb-1" />
        <div className="ambient-orb orb-2" />
        <div className="ambient-orb orb-3" />
      </div>

      <Header serverUp={serverUp} health={health} />

      {/* Hero orb + last transcript */}
      <HeroOrb latest={latest} serverUp={serverUp} pingCount={newPing} />

      {/* Three-column main content */}
      <div className="main-grid">
        <aside className="col-left">
          <StatsPanel stats={stats} serverUp={serverUp} />
        </aside>

        <section className="col-center">
          <DetectionFeed events={[...events].reverse()} />
        </section>

        <section className="col-right">
          <LatencyChart events={events} />
        </section>
      </div>
    </div>
  )
}
