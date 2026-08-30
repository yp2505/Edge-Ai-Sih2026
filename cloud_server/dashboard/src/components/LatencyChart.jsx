import {
  Chart as ChartJS,
  CategoryScale,
  LinearScale,
  BarElement,
  PointElement,
  LineElement,
  Tooltip,
  Legend,
  Filler,
} from 'chart.js'
import { Bar } from 'react-chartjs-2'
import styles from './LatencyChart.module.css'

ChartJS.register(
  CategoryScale, LinearScale, BarElement,
  PointElement, LineElement, Tooltip, Legend, Filler
)

const MAX_VISIBLE = 20   // show last N sessions on chart

export default function LatencyChart({ events }) {
  // Use last MAX_VISIBLE events
  const visible = events.slice(-MAX_VISIBLE)

  const labels = visible.map(e => `#${e.session_id}`)
  const kwData  = visible.map(e => e.kw_to_connect_ms  ?? 0)
  const rcvData = visible.map(e => e.receive_gap_ms     ?? 0)
  const txcData = visible.map(e => e.transcribe_ms      ?? 0)

  const chartData = {
    labels,
    datasets: [
      {
        label: 'kw→connect (ESP32)',
        data: kwData,
        backgroundColor: 'rgba(127, 0, 255, 0.75)',
        borderRadius: { topLeft: 0, topRight: 0 },
        stack: 'latency',
      },
      {
        label: 'rcv gap (server)',
        data: rcvData,
        backgroundColor: 'rgba(79, 172, 254, 0.80)',
        stack: 'latency',
      },
      {
        label: 'transcribe (Whisper)',
        data: txcData,
        backgroundColor: 'rgba(0, 242, 254, 0.80)',
        borderRadius: { topLeft: 6, topRight: 6, bottomLeft: 0, bottomRight: 0 },
        stack: 'latency',
      },
    ],
  }

  const options = {
    responsive: true,
    maintainAspectRatio: false,
    animation: { duration: 400, easing: 'easeOutQuart' },
    plugins: {
      legend: {
        position: 'top',
        labels: {
          color: '#9ca3af',
          font: { family: "'JetBrains Mono', monospace", size: 11 },
          boxWidth: 12,
          padding: 14,
        },
      },
      tooltip: {
        backgroundColor: 'rgba(14,20,36,0.95)',
        borderColor: 'rgba(0,242,254,0.3)',
        borderWidth: 1,
        titleColor: '#f9fafb',
        bodyColor: '#9ca3af',
        titleFont: { family: "'JetBrains Mono', monospace", size: 13, weight: 'bold' },
        bodyFont:  { family: "'JetBrains Mono', monospace", size: 12 },
        padding: 12,
        callbacks: {
          afterBody: (items) => {
            const idx   = items[0].dataIndex
            const total = (kwData[idx] ?? 0) + (rcvData[idx] ?? 0) + (txcData[idx] ?? 0)
            return [`─────────────`, `Total E2E: ${total} ms`]
          },
          label: (ctx) => `  ${ctx.dataset.label}: ${ctx.parsed.y} ms`,
        }
      }
    },
    scales: {
      x: {
        stacked: true,
        grid: { color: 'rgba(255,255,255,0.04)' },
        ticks: { color: '#6b7280', font: { family: "'JetBrains Mono', monospace", size: 10 } },
      },
      y: {
        stacked: true,
        grid: { color: 'rgba(255,255,255,0.04)' },
        ticks: {
          color: '#6b7280',
          font: { family: "'JetBrains Mono', monospace", size: 10 },
          callback: v => `${v} ms`,
        },
        title: {
          display: true,
          text: 'Latency (ms)',
          color: '#6b7280',
          font: { family: "'Outfit', sans-serif", size: 12 },
        }
      },
    },
  }

  // Totals for the sparkline summary below chart
  const totals  = visible.map((e, i) => (kwData[i] + rcvData[i] + txcData[i]))
  const avgTotal = totals.length ? Math.round(totals.reduce((a,b)=>a+b,0)/totals.length) : 0
  const maxTotal = totals.length ? Math.max(...totals) : 0
  const minTotal = totals.length ? Math.min(...totals) : 0

  return (
    <div className={`glass ${styles.panel}`}>
      <div className={styles.panelHeader}>
        <h2 className={styles.panelTitle}>📊 Latency Chart</h2>
        <span className="text-muted" style={{ fontSize: 12 }}>
          Last {Math.min(events.length, MAX_VISIBLE)} sessions · stacked breakdown
        </span>
      </div>

      <div className={styles.chartWrap}>
        {events.length === 0 ? (
          <div className={styles.noData}>
            <span style={{ fontSize: 36 }}>📈</span>
            <p>Chart will populate as detections come in</p>
          </div>
        ) : (
          <Bar data={chartData} options={options} />
        )}
      </div>

      {/* Mini summary strip below chart */}
      {events.length > 0 && (
        <div className={styles.summaryStrip}>
          {[
            { label: 'Avg E2E', value: `${avgTotal} ms`, color: 'var(--cyan)' },
            { label: 'Min E2E', value: `${minTotal} ms`, color: 'var(--green-soft)' },
            { label: 'Max E2E', value: `${maxTotal} ms`, color: maxTotal > 800 ? '#f87171' : 'var(--yellow)' },
          ].map(({ label, value, color }) => (
            <div key={label} className={styles.stripItem}>
              <span className={styles.stripLabel}>{label}</span>
              <span className={`mono ${styles.stripValue}`} style={{ color }}>{value}</span>
            </div>
          ))}
        </div>
      )}
    </div>
  )
}
