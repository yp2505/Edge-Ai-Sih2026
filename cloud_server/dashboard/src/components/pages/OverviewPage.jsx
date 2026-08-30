import KpiCards from '../KpiCards.jsx'
import LatencyTrendChart from '../LatencyTrendChart.jsx'
import BreakdownDonut from '../BreakdownDonut.jsx'
import RecentDetections from '../RecentDetections.jsx'
import styles from './Pages.module.css'

export default function OverviewPage({ stats, events, serverUp }) {
  return (
    <div className={styles.page}>
      <KpiCards stats={stats} serverUp={serverUp} />
      <div className={styles.chartsRow}>
        <div className={`${styles.chartMain} card`}>
          <LatencyTrendChart events={events} />
        </div>
        <div className={`${styles.chartSide} card`}>
          <BreakdownDonut events={events} />
        </div>
      </div>
      <div className="card" style={{ flex: 1, display: 'flex', flexDirection: 'column', minHeight: 0 }}>
        <RecentDetections events={[...events].reverse()} />
      </div>
    </div>
  )
}
