import { Doughnut } from 'react-chartjs-2'
import { Chart as ChartJS, ArcElement, Tooltip, Legend } from 'chart.js'
import styles from './BreakdownDonut.module.css'

ChartJS.register(ArcElement, Tooltip, Legend)

export default function BreakdownDonut({ events }) {
  const kw  = events.reduce((s,e) => s + (e.kw_to_connect_ms  ?? 0), 0)
  const rcv = events.reduce((s,e) => s + (e.receive_gap_ms     ?? 0), 0)
  const txc = events.reduce((s,e) => s + (e.transcribe_ms      ?? 0), 0)
  const total = kw + rcv + txc

  const pct = v => total > 0 ? Math.round((v / total) * 100) : 0
  const kwP  = pct(kw), rcvP = pct(rcv), txcP = pct(txc)
  // Dominant component label in centre
  const dominant = kw >= rcv && kw >= txc ? { label: 'kw→connect', pct: kwP }
                 : rcv >= txc              ? { label: 'rcv gap',    pct: rcvP }
                 :                           { label: 'transcribe', pct: txcP }

  const segments = [
    { label: 'kw→connect (ESP32)', value: kw,  pct: kwP,  color: '#a78bfa' },
    { label: 'rcv gap (server)',    value: rcv, pct: rcvP, color: '#00c8ff' },
    { label: 'transcribe (Whisper)',value: txc, pct: txcP, color: '#0a84ff' },
  ]

  const data = {
    labels: segments.map(s => s.label),
    datasets: [{
      data:            segments.map(s => s.value || 1),
      backgroundColor: segments.map(s => s.color),
      borderColor:     'var(--surface)',
      borderWidth: 3,
      hoverOffset: 6,
    }]
  }

  const options = {
    responsive: true,
    maintainAspectRatio: false,
    cutout: '72%',
    plugins: {
      legend: { display: false },
      tooltip: {
        backgroundColor: 'rgba(17,24,39,0.96)',
        borderColor: 'rgba(0,200,255,0.25)',
        borderWidth: 1,
        titleColor: '#f1f5f9', bodyColor: '#94a3b8',
        titleFont: { family: "'JetBrains Mono'", size: 11, weight: 'bold' },
        bodyFont:  { family: "'JetBrains Mono'", size: 10 },
        padding: 10, cornerRadius: 8,
        callbacks: {
          label: ctx => `  ${ctx.parsed.toLocaleString()} ms  (${segments[ctx.dataIndex].pct}%)`
        }
      }
    }
  }

  return (
    <div className={styles.wrap}>
      <div className={styles.header}>
        <div>
          <h2 className={styles.title}>Latency Breakdown</h2>
          <p className={styles.sub}>Cumulative share per component</p>
        </div>
      </div>

      {/* Donut + centre label */}
      <div className={styles.donutWrap}>
        <div className={styles.donutChart}>
          {total === 0 ? (
            <div className={styles.empty}>
              <span style={{ fontSize: 22 }}>🍩</span>
            </div>
          ) : <Doughnut data={data} options={options} />}
        </div>
        {total > 0 && (
          <div className={styles.centre}>
            <div className={styles.centreVal}>{dominant.pct}%</div>
            <div className={styles.centreLabel}>{dominant.label}</div>
          </div>
        )}
      </div>

      {/* Legend rows — like the reference's bullet list */}
      <div className={styles.legend}>
        {segments.map(s => (
          <div key={s.label} className={styles.legendRow}>
            <span className={styles.legendDot} style={{ background: s.color }} />
            <span className={styles.legendLabel}>{s.label}</span>
            <span className={`mono ${styles.legendPct}`}>{s.pct}%</span>
          </div>
        ))}
      </div>

      {/* Total */}
      {total > 0 && (
        <div className={styles.totalRow}>
          <span className={styles.totalLabel}>Avg total</span>
          <span className={`mono ${styles.totalVal}`}>
            {events.length ? Math.round(total / events.length) : 0} ms
          </span>
        </div>
      )}
    </div>
  )
}
