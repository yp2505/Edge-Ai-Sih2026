import { useState, useEffect, useCallback } from 'react'
import Header from './components/Header.jsx'
import DetectionFeed from './components/DetectionFeed.jsx'
import LatencyChart from './components/LatencyChart.jsx'
import StatsPanel from './components/StatsPanel.jsx'
import './App.css'

const API_BASE = 'http://localhost:8080'
const POLL_MS  = 1500   // poll every 1.5 seconds

export default function App() {
  const [events,    setEvents]    = useState([])      // server_log.json entries
  const [health,    setHealth]    = useState(null)    // {status, uptime_seconds, session_count}
  const [serverUp,  setServerUp]  = useState(false)
  const [lastFetch, setLastFetch] = useState(null)

  // ── Fetch /api/health ──────────────────────────────────────────────────
  const fetchHealth = useCallback(async () => {
    try {
      const r = await fetch(`${API_BASE}/api/health`, { signal: AbortSignal.timeout(2000) })
      if (r.ok) {
        const data = await r.json()
        setHealth(data)
        setServerUp(true)
      } else {
        setServerUp(false)
      }
    } catch {
      setServerUp(false)
    }
  }, [])

  // ── Fetch /api/events ──────────────────────────────────────────────────
  const fetchEvents = useCallback(async () => {
    try {
      const r = await fetch(`${API_BASE}/api/events`, { signal: AbortSignal.timeout(3000) })
      if (r.ok) {
        const data = await r.json()
        setEvents(data)
        setLastFetch(new Date())
      }
    } catch {
      // server may be down — health check will handle status indicator
    }
  }, [])

  // ── Poll on mount ──────────────────────────────────────────────────────
  useEffect(() => {
    fetchHealth()
    fetchEvents()
    const healthTimer  = setInterval(fetchHealth,  5000)
    const eventsTimer  = setInterval(fetchEvents,  POLL_MS)
    return () => {
      clearInterval(healthTimer)
      clearInterval(eventsTimer)
    }
  }, [fetchHealth, fetchEvents])

  // ── Derived stats ──────────────────────────────────────────────────────
  const totals = events.map(e =>
    (e.kw_to_connect_ms || 0) + (e.receive_gap_ms || 0) + (e.transcribe_ms || 0)
  )
  const stats = {
    count:   events.length,
    avg:     totals.length ? Math.round(totals.reduce((a,b) => a+b, 0) / totals.length) : 0,
    min:     totals.length ? Math.min(...totals) : 0,
    max:     totals.length ? Math.max(...totals) : 0,
  }

  return (
    <div className="app-root">
      <Header serverUp={serverUp} health={health} lastFetch={lastFetch} />

      {/* Main 3-column grid */}
      <div className="dashboard-layout">

        {/* LEFT — Stats + Model Specs */}
        <aside className="sidebar">
          <StatsPanel stats={stats} />
        </aside>

        {/* CENTER — Live Detection Feed */}
        <main className="feed-col">
          <DetectionFeed events={[...events].reverse()} />
        </main>

        {/* RIGHT — Latency Chart */}
        <section className="chart-col">
          <LatencyChart events={events} />
        </section>

      </div>
    </div>
  )
}
