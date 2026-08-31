import { Line } from 'react-chartjs-2'
import {
  Chart as ChartJS, CategoryScale, LinearScale,
  PointElement, LineElement, Tooltip, Filler,
} from 'chart.js'
import styles from './LatencyRadar.module.css'

ChartJS.register(CategoryScale, LinearScale, PointElement, LineElement, Tooltip, Filler)

export default function LatencyRadar({ events, isMinimal }) {
  const vis = events.slice(-40)
  
  const labels = vis.map(e => `#${e.session_id}`)
  const e2e = vis.map(e => (e.kw_to_connect_ms||0) + (e.receive_gap_ms||0) + (e.transcribe_ms||0))

  const data = {
    labels,
    datasets: [{
      data: e2e,
      borderColor: '#ff3b00',
      backgroundColor: (context) => {
        const ctx = context.chart.ctx
        const gradient = ctx.createLinearGradient(0, 0, 0, 200)
        gradient.addColorStop(0, 'rgba(255, 59, 0, 0.4)')
        gradient.addColorStop(1, 'rgba(255, 59, 0, 0.0)')
        return gradient
      },
      borderWidth: 2,
      tension: 0.4,
      fill: true,
      pointRadius: 0,
      pointHoverRadius: 4,
      pointBackgroundColor: '#fff',
    }]
  }

  const options = {
    responsive: true,
    maintainAspectRatio: false,
    animation: { duration: 0 },
    interaction: { mode: 'index', intersect: false },
    plugins: {
      legend: { display: false },
      tooltip: {
        backgroundColor: 'rgba(255,255,255,0.9)',
        borderColor: 'rgba(0,0,0,0.1)', borderWidth: 1,
        titleFont: { family: "'JetBrains Mono'", size: 10 },
        titleColor: '#888',
        bodyFont:  { family: "'JetBrains Mono'", size: 14, weight: 'bold' },
        bodyColor: '#ff3b00',
        displayColors: false,
        callbacks: { label: ctx => `${ctx.parsed.y} MS` }
      }
    },
    scales: {
      x: { display: false },
      y: {
        display: !isMinimal,
        grid: { color: 'rgba(255,255,255,0.05)', drawBorder: false },
        ticks: { color: '#888', font: { family: "'JetBrains Mono'", size: 9 }, maxTicksLimit: 5 },
        beginAtZero: true,
        position: 'right'
      }
    },
  }

  return (
    <div className={styles.radar} style={isMinimal ? { padding: 0, background: 'transparent', border: 'none' } : {}}>
      {!isMinimal && (
        <div className={styles.header}>
          <span className={styles.title}>RADAR // E2E LATENCY</span>
        </div>
      )}
      <div className={styles.chartWrap} style={isMinimal ? { padding: 0 } : {}}>
        {events.length === 0 ? (
          <div className={styles.empty}>NO DATA</div>
        ) : (
          <Line data={data} options={options} />
        )}
      </div>
    </div>
  )
}
