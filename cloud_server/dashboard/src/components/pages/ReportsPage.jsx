import { IconDownload } from '../Icons.jsx'
import styles from './Pages.module.css'

export default function ReportsPage({ events }) {
  const totals = events.map(e => (e.kw_to_connect_ms??0)+(e.receive_gap_ms??0)+(e.transcribe_ms??0))
  const avg = totals.length ? Math.round(totals.reduce((a,b)=>a+b,0)/totals.length) : 0
  const min = totals.length ? Math.min(...totals) : 0
  const max = totals.length ? Math.max(...totals) : 0
  const fast = totals.filter(t => t < 400).length
  const slow = totals.filter(t => t >= 800).length

  const downloadCSV = () => {
    const header = 'session_id,timestamp,transcript,kw_to_connect_ms,receive_gap_ms,transcribe_ms,end_to_end_ms\n'
    const rows = events.map(e => {
      const t = (e.kw_to_connect_ms??0)+(e.receive_gap_ms??0)+(e.transcribe_ms??0)
      return `${e.session_id},${e.timestamp},"${(e.transcript||'').replace(/"/g,'""')}",${e.kw_to_connect_ms??0},${e.receive_gap_ms??0},${e.transcribe_ms??0},${t}`
    })
    const blob = new Blob([header + rows.join('\n')], { type: 'text/csv' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url; a.download = `vaani_session_${new Date().toISOString().slice(0,10)}.csv`
    a.click(); URL.revokeObjectURL(url)
  }

  return (
    <div className={styles.page}>
      <div className={styles.reportRow}>
        {[
          { label: 'Total Sessions',  val: events.length, color: 'var(--accent)' },
          { label: 'Avg E2E Latency', val: avg ? `${avg} ms` : '—', color: 'var(--t1)' },
          { label: 'Min Latency',     val: min ? `${min} ms` : '—', color: 'var(--green)' },
          { label: 'Max Latency',     val: max ? `${max} ms` : '—', color: max>800?'var(--red)':'var(--yellow)' },
          { label: 'Fast (<400ms)',   val: fast, color: 'var(--green)' },
          { label: 'Slow (≥800ms)',   val: slow, color: slow>0?'var(--red)':'var(--t3)' },
        ].map(({ label, val, color }) => (
          <div key={label} className={`card ${styles.reportStat}`}>
            <div className={styles.reportLabel}>{label}</div>
            <div className={`mono ${styles.reportVal}`} style={{ color }}>{val}</div>
          </div>
        ))}
      </div>

      <div className={`card ${styles.downloadCard}`}>
        <div>
          <div className={styles.dlTitle}>Export Session Log</div>
          <div className={styles.dlSub}>Download all {events.length} detections as a CSV file for offline analysis</div>
        </div>
        <button
          id="btn-download-csv"
          className={styles.dlBtn}
          onClick={downloadCSV}
          disabled={events.length === 0}
        >
          <IconDownload size={15} color="#000" />
          Export CSV
        </button>
      </div>

      <div className={`card ${styles.apiCard}`}>
        <div className={styles.dlTitle}>API Endpoints (for external tools)</div>
        {[
          { method: 'GET', path: 'http://localhost:8080/api/events', desc: 'All session log entries as JSON array' },
          { method: 'GET', path: 'http://localhost:8080/api/health', desc: 'Server status, uptime, session count' },
        ].map(({ method, path, desc }) => (
          <div key={path} className={styles.apiRow}>
            <span className="badge badge-accent mono" style={{fontSize:10}}>{method}</span>
            <span className={`mono ${styles.apiPath}`}>{path}</span>
            <span className={styles.apiDesc}>{desc}</span>
          </div>
        ))}
      </div>
    </div>
  )
}
