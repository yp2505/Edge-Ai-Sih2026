import styles from './HardwareTwin.module.css'
import { IconCpu } from './Icons.jsx'

export default function HardwareTwin({ latestEvent }) {
  const isProcessing = latestEvent && (Date.now() - new Date(latestEvent.timestamp).getTime() < 3000)

  return (
    <div className={styles.twin}>
      <h2 className="hero-text">NODE ALPHA</h2>
      <div className={styles.subHeader}>ESP32-S3 INT8 ACCELERATOR</div>

      {/* The Digital Twin Graphic */}
      <div className={styles.boardGraphic}>
        {/* Fake isometric board */}
        <div className={styles.boardBase}>
          <div className={`${styles.chip} ${styles.cpu} ${isProcessing ? styles.chipActive : ''}`}>
            <span>MCU</span>
          </div>
          <div className={`${styles.chip} ${styles.wifi} ${isProcessing ? styles.chipActive : ''}`}>
            <span>TX</span>
          </div>
          <div className={`${styles.chip} ${styles.mic} ${isProcessing ? styles.chipActive : ''}`}>
            <span>MIC</span>
          </div>
          
          {/* Traces */}
          <div className={`${styles.trace} ${styles.trace1} ${isProcessing ? styles.traceActive : ''}`}></div>
          <div className={`${styles.trace} ${styles.trace2} ${isProcessing ? styles.traceActive : ''}`}></div>
        </div>
      </div>

      <div className={styles.metricsGrid}>
        <div className={styles.metric}>
          <span className={styles.mLabel}>CORE TEMP</span>
          <span className="mono glow-cyan">42°C</span>
        </div>
        <div className={styles.metric}>
          <span className={styles.mLabel}>VRAM LOAD</span>
          <span className="mono glow-purple">54KB</span>
        </div>
        <div className={styles.metric}>
          <span className={styles.mLabel}>T-ARENA</span>
          <span className="mono glow-cyan">92%</span>
        </div>
        <div className={styles.metric}>
          <span className={styles.mLabel}>UPLINK</span>
          <span className="mono glow-green">TCP</span>
        </div>
      </div>
      
      {/* Live latency from latest event */}
      <div className={styles.liveAction}>
        <div className={styles.mLabel}>LAST EVENT LATENCY</div>
        <div className={styles.latencyBars}>
          <div className={styles.barWrap}>
            <span style={{fontSize:10}}>TX</span>
            <div className={styles.barBg}><div className={styles.barFill} style={{width: `${Math.min((latestEvent?.kw_to_connect_ms||0)/5, 100)}%`, background: 'var(--neon-purple)'}}></div></div>
            <span className="mono">{latestEvent?.kw_to_connect_ms||0}ms</span>
          </div>
          <div className={styles.barWrap}>
            <span style={{fontSize:10}}>INF</span>
            <div className={styles.barBg}><div className={styles.barFill} style={{width: `${Math.min((latestEvent?.transcribe_ms||0)/10, 100)}%`, background: 'var(--neon-cyan)'}}></div></div>
            <span className="mono">{latestEvent?.transcribe_ms||0}ms</span>
          </div>
        </div>
      </div>

    </div>
  )
}
