import styles from './HudHeader.module.css'

export default function HudHeader({ health, serverUp, totalEvents, activeTab, setActiveTab }) {
  const uptime = health?.uptime_seconds
  const fmt = s => {
    if (s == null) return '00:00:00'
    const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60
    return `${h.toString().padStart(2,'0')}:${m.toString().padStart(2,'0')}:${sec.toString().padStart(2,'0')}`
  }

  return (
    <div className={styles.header}>
      <div className={styles.left}>
        <div className={styles.logoGroup}>
          <div className={styles.logoMark}>HV</div>
          <div className={styles.titleBox}>
            <h1 className="hero-text">HEY VAANI</h1>
            <span className={styles.version}>SYS.VER 2.0.26</span>
          </div>
        </div>
      </div>

      <div className={styles.center}>
        <div className={styles.navLinks}>
          <span 
            className={activeTab === 'live' ? styles.navItemActive : styles.navItem}
            onClick={() => setActiveTab('live')}
          >
            LIVE TRACKING
          </span>
          <span 
            className={activeTab === 'config' ? styles.navItemActive : styles.navItem}
            onClick={() => setActiveTab('config')}
          >
            NODE CONFIG
          </span>
          <span 
            className={activeTab === 'diag' ? styles.navItemActive : styles.navItem}
            onClick={() => setActiveTab('diag')}
          >
            DIAGNOSTICS
          </span>
        </div>
      </div>

      <div className={styles.right}>
        <div className={styles.statBox}>
          <span className={styles.statLabel}>DETECTS</span>
          <span className="mono glow-cyan">{totalEvents.toString().padStart(4, '0')}</span>
        </div>
        <div className={styles.statBox}>
          <span className={styles.statLabel}>UPTIME</span>
          <span className="mono glow-purple">{fmt(uptime)}</span>
        </div>
        <div className={styles.statusBox}>
          {serverUp ? (
            <span className={styles.online}><span className={styles.pulseGreen}></span> UPLINK STABLE</span>
          ) : (
            <span className={styles.offline}><span className={styles.pulseRed}></span> NO SIGNAL</span>
          )}
        </div>
      </div>
    </div>
  )
}
