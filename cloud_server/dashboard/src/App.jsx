import { useState, useEffect, useCallback, useMemo } from 'react'
import Sidebar from './components/Sidebar.jsx'
import TopBar from './components/TopBar.jsx'
import OverviewPage     from './components/pages/OverviewPage.jsx'
import LiveFeedPage     from './components/pages/LiveFeedPage.jsx'
import LatencyPage      from './components/pages/LatencyPage.jsx'
import DeviceSpecsPage  from './components/pages/DeviceSpecsPage.jsx'
import ReportsPage      from './components/pages/ReportsPage.jsx'
import SettingsPage     from './components/pages/SettingsPage.jsx'
import './App.css'

const API = 'http://localhost:8080'

// Removed: Last 10 min. Added: Live Feed (last 60 s)
const TIME_FILTERS = [
  { label: 'Live Feed', ms: 60 * 1000 },
  { label: 'Last 1h',   ms: 60 * 60 * 1000 },
  { label: 'Today',     ms: 24 * 60 * 60 * 1000 },
  { label: 'All Time',  ms: Infinity },
]

export default function App() {
  const [events,   setEvents]   = useState([])
  const [health,   setHealth]   = useState(null)
  const [serverUp, setServerUp] = useState(false)
  const [search,   setSearch]   = useState('')
  const [timeIdx,  setTimeIdx]  = useState(3)          // default: All Time
  const [nav,      setNav]      = useState('overview')  // active nav page

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
    // Live feed polls faster when on livefeed page
    const interval = nav === 'livefeed' ? 800 : 1500
    const e = setInterval(fetchEvents, interval)
    return () => { clearInterval(h); clearInterval(e) }
  }, [fetchHealth, fetchEvents, nav])

  // Apply time filter + search filter
  const filteredEvents = useMemo(() => {
    const cutoff = Date.now() - TIME_FILTERS[timeIdx].ms
    return events.filter(e => {
      const ts = e.timestamp ? new Date(e.timestamp).getTime() : 0
      const inTime = TIME_FILTERS[timeIdx].ms === Infinity || ts >= cutoff
      const q = search.toLowerCase()
      const hit = !q
        || (e.transcript ?? '').toLowerCase().includes(q)
        || String(e.session_id).includes(q)
      return inTime && hit
    })
  }, [events, timeIdx, search])

  // KPI stats
  const totals = filteredEvents.map(e =>
    (e.kw_to_connect_ms ?? 0) + (e.receive_gap_ms ?? 0) + (e.transcribe_ms ?? 0)
  )
  const stats = {
    count: filteredEvents.length,
    avg:   totals.length ? Math.round(totals.reduce((a,b)=>a+b,0)/totals.length) : 0,
    min:   totals.length ? Math.min(...totals) : 0,
    max:   totals.length ? Math.max(...totals) : 0,
  }

  // Render the active page
  const renderPage = () => {
    switch (nav) {
      case 'overview':  return <OverviewPage stats={stats} events={filteredEvents} serverUp={serverUp} />
      case 'livefeed':  return <LiveFeedPage events={[...filteredEvents].reverse()} />
      case 'latency':   return <LatencyPage  events={filteredEvents} />
      case 'device':    return <DeviceSpecsPage />
      case 'reports':   return <ReportsPage events={events} />
      case 'settings':  return <SettingsPage />
      default:          return <OverviewPage stats={stats} events={filteredEvents} serverUp={serverUp} />
    }
  }

  return (
    <div className="shell">
      <Sidebar activeNav={nav} onNav={setNav} serverUp={serverUp} />

      <div className="main">
        <TopBar
          activeNav={nav}
          search={search}       onSearch={setSearch}
          timeIdx={timeIdx}     onTime={setTimeIdx}
          timeFilters={TIME_FILTERS}
          health={health}       serverUp={serverUp}
        />
        <div className="content">
          {renderPage()}
        </div>
      </div>
    </div>
  )
}
