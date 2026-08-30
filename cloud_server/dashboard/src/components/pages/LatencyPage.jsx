import LatencyTrendChart from '../LatencyTrendChart.jsx'
import BreakdownDonut from '../BreakdownDonut.jsx'
import styles from './Pages.module.css'

export default function LatencyPage({ events }) {
  const totals = events.map(e => (e.kw_to_connect_ms??0)+(e.receive_gap_ms??0)+(e.transcribe_ms??0))
  const avg = totals.length ? Math.round(totals.reduce((a,b)=>a+b,0)/totals.length) : 0
  const min = totals.length ? Math.min(...totals) : 0
  const max = totals.length ? Math.max(...totals) : 0

  return (
    <div className={styles.page}>
      {/* Summary row */}
      <div className={styles.latencyStats}>
        {[
          { label: 'Avg E2E', val: avg ? `${avg} ms` : '—', color: 'var(--accent)' },
          { label: 'Best',    val: min ? `${min} ms` : '—', color: 'var(--green)' },
          { label: 'Worst',   val: max ? `${max} ms` : '—', color: max > 800 ? 'var(--red)' : 'var(--yellow)' },
          { label: 'Sessions',val: events.length, color: 'var(--t1)' },
        ].map(({ label, val, color }) => (
          <div key={label} className={`card ${styles.latStat}`}>
            <div className={styles.latStatLabel}>{label}</div>
            <div className={`mono ${styles.latStatVal}`} style={{ color }}>{val}</div>
          </div>
        ))}
      </div>
      {/* Charts */}
      <div className={styles.chartsRow}>
        <div className={`card ${styles.chartMain}`}>
          <LatencyTrendChart events={events} />
        </div>
        <div className={`card ${styles.chartSide}`}>
          <BreakdownDonut events={events} />
        </div>
      </div>
    </div>
  )
}
