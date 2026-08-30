import { useState, useEffect, useCallback, useMemo } from 'react'
import Sidebar from './components/Sidebar.jsx'
import TopBar from './components/TopBar.jsx'
import KpiCards from './components/KpiCards.jsx'
import LatencyTrendChart from './components/LatencyTrendChart.jsx'
import BreakdownDonut from './components/BreakdownDonut.jsx'
import RecentDetections from './components/RecentDetections.jsx'
import './App.css'

const API = 'http://localhost:8080'

const TIME_FILTERS = [
  { label: 'Last 10 min', ms: 10 * 60 * 1000 },
  { label: 'Last 1h',     ms: 60 * 60 * 1000 },
  { label: 'Today',       ms: 24 * 60 * 60 * 1000 },
  { label: 'All Time',    ms: Infinity },
]

export default function App() {
  const [events,    setEvents]    = useState([])
  const [health,    setHealth]    = useState(null)
  const [serverUp,  setServerUp]  = useState(false)
  const [search,    setSearch]    = useState('')
  const [timeIdx,   setTimeIdx]   = useState(3)   // default: All Time
  const [activeNav, setActiveNav] = useState('overview')

  const fetchHealth = useCallback(async () => {
    try {
      const r = await fetch(`${API}/api/health`, { signal: AbortSignal.timeout(2500) })
      if (r.ok) { setHealth(await r.json()); setServerUp(true) }
      else        setServerUp(false)
    } catch { setServerUp(false) }
  }, [])

  const fetchEvents = useCallback(async () => {
    try {
      const r = await fetch(`${API}/api/events`, { signal: AbortSignal.timeout(3000) })
      if (r.ok) setEvents(await r.json())
    } catch {}
  }, [])

  useEffect(() => {
    fetchHealth(); fetchEvents()
    const h = setInterval(fetchHealth, 5000)
    const e = setInterval(fetchEvents, 1500)
    return () => { clearInterval(h); clearInterval(e) }
  }, [fetchHealth, fetchEvents])

  // Apply time filter + search
  const filtered = useMemo(() => {
    const cutoff = Date.now() - TIME_FILTERS[timeIdx].ms
    return events.filter(e => {
      const ts = e.timestamp ? new Date(e.timestamp).getTime() : 0
      const inTime = TIME_FILTERS[timeIdx].ms === Infinity || ts >= cutoff
      const q = search.toLowerCase()
      const matchSearch = !q || (e.transcript ?? '').toLowerCase().includes(q) || String(e.session_id).includes(q)
      return inTime && matchSearch
    })
  }, [events, timeIdx, search])

  // KPI stats from filtered set
  const totals = filtered.map(e =>
    (e.kw_to_connect_ms ?? 0) + (e.receive_gap_ms ?? 0) + (e.transcribe_ms ?? 0)
  )
  const stats = {
    count:  filtered.length,
    avg:    totals.length ? Math.round(totals.reduce((a,b)=>a+b,0)/totals.length) : 0,
    min:    totals.length ? Math.min(...totals) : 0,
    max:    totals.length ? Math.max(...totals) : 0,
    prevCount: Math.max(0, events.length - filtered.length),  // rough comparison
  }

  return (
    <div className="shell">
      <Sidebar activeNav={activeNav} onNav={setActiveNav} serverUp={serverUp} />

      <div className="main">
        <TopBar
          search={search} onSearch={setSearch}
          timeIdx={timeIdx} onTime={setTimeIdx}
          timeFilters={TIME_FILTERS}
          health={health} serverUp={serverUp}
        />

        <div className="content">
          {/* Row 1 — KPI cards */}
          <KpiCards stats={stats} serverUp={serverUp} />

          {/* Row 2 — charts */}
          <div className="charts-row">
            <div className="chart-main card">
              <LatencyTrendChart events={filtered} />
            </div>
            <div className="chart-side card">
              <BreakdownDonut events={filtered} />
            </div>
          </div>

          {/* Row 3 — recent detections table */}
          <div className="card">
            <RecentDetections events={[...filtered].reverse()} />
          </div>
        </div>
      </div>
    </div>
  )
}
