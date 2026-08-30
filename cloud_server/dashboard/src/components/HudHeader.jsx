import styles from './HudHeader.module.css'

export default function HudHeader({ health, serverUp, totalEvents }) {
  const uptime = health?.uptime_seconds
  const fmt = s => {
    if (s == null) return '00:00:00'
    const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60
    return `${h.toString().padStart(2,'0')}:${m.toString().padStart(2,'0')}:${sec.toString().padStart(2,'0')}`
  }

  return (
    <div className={styles.header}>
      <div className={styles.left}>
        <div className={styles.logoMark}>HV</div>
        <h1 className={styles.brandTitle}>HEY VAANI <span className={styles.version}>SPATIAL 3.0</span></h1>
      </div>

      <div className={styles.right}>
        <div className={styles.statGroup}>
          <div className={styles.stat}>
            <span className={styles.sLabel}>EVENTS</span>
            <span className="mono">{totalEvents}</span>
          </div>
          <div className={styles.stat}>
            <span className={styles.sLabel}>UPTIME</span>
            <span className="mono">{fmt(uptime)}</span>
          </div>
        </div>
        <div className={styles.connection}>
          {serverUp ? (
            <span className={styles.online}><div className={styles.dotGreen}></div> SYSTEM ONLINE</span>
          ) : (
            <span className={styles.offline}><div className={styles.dotRed}></div> OFFLINE</span>
          )}
        </div>
      </div>
    </div>
  )
}
