import {
  Chart as ChartJS, CategoryScale, LinearScale,
  BarElement, Tooltip, Legend,
} from 'chart.js'
import { Bar } from 'react-chartjs-2'
import styles from './LatencyChart.module.css'

ChartJS.register(CategoryScale, LinearScale, BarElement, Tooltip, Legend)

const MAX = 15

export default function LatencyChart({ events }) {
  const vis    = events.slice(-MAX)
  const labels = vis.map(e => `#${e.session_id}`)
  const kw     = vis.map(e => e.kw_to_connect_ms  ?? 0)
  const rcv    = vis.map(e => e.receive_gap_ms     ?? 0)
  const txc    = vis.map(e => e.transcribe_ms      ?? 0)
  const totals = vis.map((_, i) => kw[i] + rcv[i] + txc[i])

  const data = {
    labels,
    datasets: [
      { label: 'kw→connect', data: kw,  backgroundColor: 'rgba(191,90,242,0.75)', stack: 'l', borderRadius: { topLeft:0,topRight:0,bottomLeft:4,bottomRight:4 } },
      { label: 'rcv gap',    data: rcv, backgroundColor: 'rgba(50,173,230,0.8)',  stack: 'l' },
      { label: 'transcribe', data: txc, backgroundColor: 'rgba(10,132,255,0.85)', stack: 'l', borderRadius: { topLeft:5,topRight:5,bottomLeft:0,bottomRight:0 } },
    ],
  }

  const options = {
    responsive: true,
    maintainAspectRatio: false,
    animation: { duration: 500, easing: 'easeOutQuart' },
    plugins: {
      legend: {
        position: 'top',
        labels: {
          color: '#6b7280',
          font: { family: "'JetBrains Mono', monospace", size: 10 },
          boxWidth: 10, padding: 12, usePointStyle: true, pointStyle: 'circle',
        },
      },
      tooltip: {
        backgroundColor: 'rgba(8,12,22,0.95)',
        borderColor: 'rgba(50,173,230,0.3)',
        borderWidth: 1,
        titleColor: '#f5f5f7',
        bodyColor: '#98989f',
        titleFont: { family: "'JetBrains Mono'", size: 12, weight: 'bold' },
        bodyFont:  { family: "'JetBrains Mono'", size: 11 },
        padding: 12, cornerRadius: 12,
        callbacks: {
          afterBody: (items) => {
            const i = items[0].dataIndex
            return [`──────────────`, `Total: ${totals[i]} ms`]
          },
          label: ctx => `  ${ctx.dataset.label}: ${ctx.parsed.y} ms`,
        }
      }
    },
    scales: {
      x: {
        stacked: true,
        grid: { color: 'rgba(255,255,255,0.03)' },
        ticks: { color: '#48484a', font: { family: "'JetBrains Mono'", size: 9 } },
        border: { color: 'rgba(255,255,255,0.04)' },
      },
      y: {
        stacked: true,
        grid: { color: 'rgba(255,255,255,0.04)' },
        ticks: { color: '#48484a', font: { family: "'JetBrains Mono'", size: 9 }, callback: v => `${v}ms` },
        border: { color: 'rgba(255,255,255,0.04)' },
      },
    },
  }

  const avg = totals.length ? Math.round(totals.reduce((a,b) => a+b,0) / totals.length) : 0
  const mn  = totals.length ? Math.min(...totals) : 0
  const mx  = totals.length ? Math.max(...totals) : 0

  return (
    <div className={styles.panel}>
      <div className={styles.header}>
        <h2 className={styles.title}>📊 Latency Chart</h2>
        <span className="pill-sm pill-ghost">last {Math.min(events.length, MAX)}</span>
      </div>

      <div className={styles.chartBox}>
        {events.length === 0 ? (
          <div className={styles.empty}>
            <span style={{ fontSize: 32 }}>📈</span>
            <span>Chart appears as sessions come in</span>
          </div>
        ) : <Bar data={data} options={options} />}
      </div>

      {/* Bottom pill strip */}
      {events.length > 0 && (
        <div className={styles.strip}>
          {[
            { label: 'avg', val: `${avg} ms`, color: 'var(--cyan)' },
            { label: 'min', val: `${mn} ms`,  color: 'var(--green)' },
            { label: 'max', val: `${mx} ms`,  color: mx > 800 ? 'var(--red)' : 'var(--yellow)' },
          ].map(({ label, val, color }) => (
            <div key={label} className={`${styles.stripPill} glass pill`}>
              <span className={styles.stripLabel}>{label}</span>
              <span className={`${styles.stripVal} mono`} style={{ color }}>{val}</span>
            </div>
          ))}
        </div>
      )}
    </div>
  )
}
