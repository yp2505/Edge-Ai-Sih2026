import { IconSatellite, IconClock, IconFlash, IconBarChart } from './Icons.jsx'
import styles from './KpiCards.module.css'

function KpiCard({ id, icon, label, value, sub, subColor, trend, trendDir }) {
  return (
    <div className={`${styles.card} card`} id={id}>
      <div className={styles.top}>
        <div className={styles.label}>{label}</div>
        <span className={styles.iconWrap}>{icon}</span>
      </div>
      <div className={styles.value}>{value}</div>
      {(trend != null || sub) && (
        <div className={styles.bottom}>
          {trend != null && (
            <span className={trendDir === 'up' ? 'trend-up' : 'trend-down'}>
              {trendDir === 'up' ? '▲' : '▼'} {trend}
            </span>
          )}
          {sub && <span className={styles.sub} style={{ color: subColor }}>{sub}</span>}
        </div>
      )}
    </div>
  )
}

export default function KpiCards({ stats, serverUp }) {
  const { count, avg, min, max } = stats

  const latClass = avg === 0 ? '' : avg < 400 ? styles.good : avg < 800 ? styles.warn : styles.bad
  const latLabel = avg === 0 ? '—' : avg < 400 ? 'On target' : avg < 800 ? 'Moderate' : 'High'
  const latColor = avg === 0 ? 'var(--t3)' : avg < 400 ? 'var(--green)' : avg < 800 ? 'var(--yellow)' : 'var(--red)'

  return (
    <div className={styles.grid}>
      <KpiCard
        id="kpi-total-detections"
        icon={<IconSatellite size={16} />}
        label="Total Detections"
        value={count}
        sub={serverUp ? 'Live session' : 'Server offline'}
        subColor={serverUp ? 'var(--green)' : 'var(--red)'}
      />
      <KpiCard
        id="kpi-avg-latency"
        icon={<IconClock size={16} />}
        label="Avg End-to-End"
        value={avg ? `${avg} ms` : '—'}
        sub={latLabel}
        subColor={latColor}
      />
      <KpiCard
        id="kpi-best-latency"
        icon={<IconFlash size={16} />}
        label="Best Latency"
        value={min ? `${min} ms` : '—'}
        sub="Fastest session"
        subColor="var(--green)"
      />
      <KpiCard
        id="kpi-worst-latency"
        icon={<IconBarChart size={16} />}
        label="Worst Latency"
        value={max ? `${max} ms` : '—'}
        sub={max > 800 ? 'Spike detected' : max > 0 ? 'Acceptable' : '—'}
        subColor={max > 800 ? 'var(--red)' : 'var(--t3)'}
      />
    </div>
  )
}
