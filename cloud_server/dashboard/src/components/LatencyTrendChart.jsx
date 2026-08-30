import { Line } from 'react-chartjs-2'
import {
  Chart as ChartJS, CategoryScale, LinearScale,
  PointElement, LineElement, Tooltip, Filler, Legend,
} from 'chart.js'
import styles from './LatencyTrendChart.module.css'

ChartJS.register(CategoryScale, LinearScale, PointElement, LineElement, Tooltip, Filler, Legend)

const MAX = 30

export default function LatencyTrendChart({ events }) {
  const vis = events.slice(-MAX)
  const labels = vis.map(e => `#${e.session_id}`)

  const e2e  = vis.map(e => (e.kw_to_connect_ms??0) + (e.receive_gap_ms??0) + (e.transcribe_ms??0))
  const txc  = vis.map(e => e.transcribe_ms     ?? 0)

  const data = {
    labels,
    datasets: [
      {
        label: 'Transcribe',
        data: txc,
        borderColor: 'rgba(167,139,250,0.8)',
        backgroundColor: 'rgba(167,139,250,0.05)',
        borderWidth: 1.5,
        tension: 0.4,
        fill: true,
        pointRadius: 0,
        pointHoverRadius: 4,
      },
      {
        label: 'End-to-End',
        data: e2e,
        borderColor: '#00c8ff',
        backgroundColor: 'rgba(0,200,255,0.08)',
        borderWidth: 2,
        tension: 0.4,
        fill: true,
        pointRadius: vis.length <= 15 ? 4 : 0,
        pointHoverRadius: 6,
        pointBackgroundColor: '#00c8ff',
        pointBorderColor: '#0b0f1a',
        pointBorderWidth: 2,
      },
    ],
  }

  const options = {
    responsive: true,
    maintainAspectRatio: false,
    animation: { duration: 600 },
    interaction: { mode: 'index', intersect: false },
    plugins: {
      legend: {
        position: 'top', align: 'end',
        labels: {
          color: '#475569', font: { family: "'JetBrains Mono'", size: 10 },
          boxWidth: 12, padding: 16, usePointStyle: true, pointStyle: 'circle',
        }
      },
      tooltip: {
        backgroundColor: 'rgba(17,24,39,0.96)',
        borderColor: 'rgba(0,200,255,0.25)',
        borderWidth: 1,
        titleColor: '#f1f5f9',
        bodyColor: '#94a3b8',
        titleFont: { family: "'JetBrains Mono'", size: 12, weight: 'bold' },
        bodyFont:  { family: "'JetBrains Mono'", size: 11 },
        padding: 12, cornerRadius: 10,
        callbacks: { label: ctx => `  ${ctx.dataset.label}: ${ctx.parsed.y} ms` }
      }
    },
    scales: {
      x: {
        grid: { color: 'rgba(255,255,255,0.03)', drawBorder: false },
        ticks: { color: '#475569', font: { family: "'JetBrains Mono'", size: 9 }, maxTicksLimit: 8 },
        border: { display: false },
      },
      y: {
        grid: { color: 'rgba(255,255,255,0.04)', drawBorder: false },
        ticks: { color: '#475569', font: { family: "'JetBrains Mono'", size: 9 }, callback: v => `${v}ms` },
        border: { display: false },
        beginAtZero: true,
      },
    },
  }

  return (
    <div className={styles.wrap}>
      <div className={styles.header}>
        <div>
          <h2 className={styles.title}>Latency Trend</h2>
          <p className={styles.sub}>End-to-end response time over the last {Math.min(events.length, MAX)} sessions</p>
        </div>
        <span className="badge badge-accent">Live</span>
      </div>
      <div className={styles.chart}>
        {events.length === 0 ? (
          <div className={styles.empty}>
            <span style={{ fontSize: 28 }}>📈</span>
            <span>Chart populates as detections arrive</span>
          </div>
        ) : <Line data={data} options={options} />}
      </div>
    </div>
  )
}
