import styles from './Header.module.css'

function formatUptime(seconds) {
  if (!seconds && seconds !== 0) return '—'
  const h = Math.floor(seconds / 3600)
  const m = Math.floor((seconds % 3600) / 60)
  const s = seconds % 60
  if (h > 0) return `${h}h ${m}m ${s}s`
  if (m > 0) return `${m}m ${s}s`
  return `${s}s`
}

export default function Header({ serverUp, health, lastFetch }) {
  return (
    <header className={`glass ${styles.header}`}>
      {/* Logo */}
      <div className={styles.logo}>
        <div className={styles.logoIcon}>🎙️</div>
        <div className={styles.logoText}>
          <h1 className={`gradient-text ${styles.title}`}>Hey Vaani</h1>
          <p className={styles.subtitle}>Edge AI · ESP32 · DS-CNN INT8 · SIH 2026</p>
        </div>
      </div>

      {/* Centre — wave visualiser (decorative) */}
      <div className={styles.wave} aria-hidden="true">
        {[0.1,0.3,0.5,0.2,0.4,0.6,0.15,0.35,0.55].map((delay, i) => (
          <div key={i} className={styles.waveBar} style={{ animationDelay: `${delay}s` }} />
        ))}
      </div>

      {/* Right — status cluster */}
      <div className={styles.statusCluster}>
        {health && (
          <>
            <div className={styles.metaChip}>
              <span className="text-muted">Uptime</span>
              <span className="mono text-cyan">{formatUptime(health.uptime_seconds)}</span>
            </div>
            <div className={styles.metaChip}>
              <span className="text-muted">Sessions</span>
              <span className="mono text-bright">{health.session_count}</span>
            </div>
          </>
        )}
        {lastFetch && (
          <div className={styles.metaChip}>
            <span className="text-muted">Updated</span>
            <span className="mono text-muted">{lastFetch.toLocaleTimeString()}</span>
          </div>
        )}

        {/* Status badge */}
        <div className={`${styles.statusBadge} ${serverUp ? styles.badgeUp : styles.badgeDown}`}>
          <span className={`${styles.statusDot} ${serverUp ? styles.dotUp : styles.dotDown}`} />
          {serverUp ? '🟢 Server Running' : '🔴 Server Stopped'}
        </div>
      </div>
    </header>
  )
}
