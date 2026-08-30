import styles from './Sidebar.module.css'

const NAV = [
  { id: 'overview',  icon: '⊞', label: 'Overview' },
  { id: 'feed',      icon: '📡', label: 'Live Feed' },
  { id: 'latency',   icon: '⏱',  label: 'Latency' },
  { id: 'device',    icon: '🔬', label: 'Device Specs' },
  { id: 'reports',   icon: '📋', label: 'Reports' },
]
const SYSTEM = [
  { id: 'settings',  icon: '⚙', label: 'Settings' },
]

export default function Sidebar({ activeNav, onNav, serverUp }) {
  return (
    <aside className={styles.sidebar}>
      {/* Logo */}
      <div className={styles.logo}>
        <div className={styles.logoIcon}>
          <div className={styles.siriRing} />
          <span className={styles.logoEmoji}>🎙</span>
        </div>
        <div>
          <div className={styles.logoName}>Hey Vaani</div>
          <div className={styles.logoSub}>Edge AI Dashboard</div>
        </div>
      </div>

      {/* Status pill */}
      <div className={`${styles.statusWrap}`}>
        <div className={`${styles.statusPill} ${serverUp ? styles.up : styles.down}`}>
          <span className={`${styles.dot} ${serverUp ? styles.dotUp : ''}`} />
          {serverUp ? 'Server Online' : 'Server Offline'}
        </div>
      </div>

      <hr className={`${styles.divider} divider`} />

      {/* Nav */}
      <nav className={styles.nav}>
        {NAV.map(item => (
          <button
            key={item.id}
            className={`${styles.navItem} ${activeNav === item.id ? styles.navActive : ''}`}
            onClick={() => onNav(item.id)}
            id={`nav-${item.id}`}
          >
            <span className={styles.navIcon}>{item.icon}</span>
            <span className={styles.navLabel}>{item.label}</span>
          </button>
        ))}
      </nav>

      <div className={styles.section}>SYSTEM</div>
      <nav className={styles.nav}>
        {SYSTEM.map(item => (
          <button
            key={item.id}
            className={`${styles.navItem} ${activeNav === item.id ? styles.navActive : ''}`}
            onClick={() => onNav(item.id)}
            id={`nav-${item.id}`}
          >
            <span className={styles.navIcon}>{item.icon}</span>
            <span className={styles.navLabel}>{item.label}</span>
          </button>
        ))}
      </nav>

      {/* Bottom — ESP32 device info */}
      <div className={styles.bottom}>
        <hr className={`${styles.divider} divider`} style={{ marginBottom: 14 }} />
        <div className={styles.deviceCard}>
          <div className={styles.deviceAvatar}>ESP</div>
          <div className={styles.deviceInfo}>
            <div className={styles.deviceName}>ESP32-S3</div>
            <div className={styles.deviceSub}>DS-CNN INT8 · HVP1 v1</div>
          </div>
        </div>
      </div>
    </aside>
  )
}
