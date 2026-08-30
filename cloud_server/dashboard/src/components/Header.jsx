import styles from './Header.module.css'

function formatUptime(s) {
  if (!s && s !== 0) return '—'
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60
  return h > 0 ? `${h}h ${m}m` : m > 0 ? `${m}m ${sec}s` : `${sec}s`
}

export default function Header({ serverUp, health }) {
  return (
    <header className={styles.header}>
      <div className={styles.inner}>

        {/* Left — branding */}
        <div className={styles.brand}>
          <div className={styles.logoRing}>
            <div className={styles.logoCore}>
              <span className={styles.logoEmoji}>🎙</span>
            </div>
          </div>
          <div className={styles.brandText}>
            <span className={styles.brandName}>Hey Vaani</span>
            <span className={styles.brandSub}>Edge AI  ·  SIH 2026</span>
          </div>
        </div>

        {/* Centre — nav pills */}
        <nav className={styles.nav}>
          {['Live Feed', 'Latency', 'Specs'].map(label => (
            <span key={label} className={styles.navItem}>{label}</span>
          ))}
        </nav>

        {/* Right — status cluster */}
        <div className={styles.statusRow}>
          {health && (
            <>
              <div className={`${styles.chip} glass pill`}>
                <span className={styles.chipLabel}>Uptime</span>
                <span className={`${styles.chipVal} mono text-cyan`}>{formatUptime(health.uptime_seconds)}</span>
              </div>
              <div className={`${styles.chip} glass pill`}>
                <span className={styles.chipLabel}>Sessions</span>
                <span className={`${styles.chipVal} mono`}>{health.session_count}</span>
              </div>
            </>
          )}

          <div className={`${styles.statusPill} pill ${serverUp ? styles.up : styles.down}`}>
            <span className={`${styles.dot} ${serverUp ? styles.dotUp : styles.dotDown}`} />
            <span>{serverUp ? 'Server Running' : 'Server Offline'}</span>
          </div>
        </div>

      </div>
    </header>
  )
}
