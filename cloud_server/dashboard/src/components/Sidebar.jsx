import { VaaniLogo, IconGrid, IconRadio, IconClock, IconCpu, IconReport, IconSettings, IconWifi } from './Icons.jsx'
import styles from './Sidebar.module.css'

const NAV_ITEMS = [
  { id: 'overview',  Icon: IconGrid,    label: 'Overview' },
  { id: 'livefeed',  Icon: IconRadio,   label: 'Live Feed' },
  { id: 'latency',   Icon: IconClock,   label: 'Latency' },
  { id: 'device',    Icon: IconCpu,     label: 'Device Specs' },
  { id: 'reports',   Icon: IconReport,  label: 'Reports' },
]
const SYS_ITEMS = [
  { id: 'settings',  Icon: IconSettings, label: 'Settings' },
]

export default function Sidebar({ activeNav, onNav, serverUp }) {
  return (
    <aside className={styles.sidebar}>

      {/* ── Logo ── */}
      <div className={styles.logoRow}>
        <VaaniLogo size={38} />
        <div>
          <div className={styles.logoName}>Hey Vaani</div>
          <div className={styles.logoSub}>Edge AI Platform</div>
        </div>
      </div>

      {/* ── Connection status ── */}
      <div className={styles.statusWrap}>
        <div className={`${styles.statusPill} ${serverUp ? styles.up : styles.down}`}>
          <span className={`${styles.dot} ${serverUp ? styles.dotLive : ''}`} />
          <span>{serverUp ? 'Server Online' : 'Server Offline'}</span>
          <IconWifi size={13} color={serverUp ? 'var(--green)' : 'var(--red)'} />
        </div>
      </div>

      <hr className={styles.divider} />

      {/* ── Main nav ── */}
      <nav className={styles.nav}>
        {NAV_ITEMS.map(({ id, Icon, label }) => (
          <button
            key={id}
            id={`nav-${id}`}
            onClick={() => onNav(id)}
            className={`${styles.navItem} ${activeNav === id ? styles.active : ''}`}
          >
            <span className={styles.navIcon}>
              <Icon size={15} color={activeNav === id ? 'var(--accent)' : 'var(--t3)'} />
            </span>
            <span className={styles.navLabel}>{label}</span>
            {id === 'livefeed' && serverUp && (
              <span className={styles.liveBadge}>LIVE</span>
            )}
          </button>
        ))}
      </nav>

      <div className={styles.sectionLabel}>SYSTEM</div>

      <nav className={styles.nav}>
        {SYS_ITEMS.map(({ id, Icon, label }) => (
          <button
            key={id}
            id={`nav-${id}`}
            onClick={() => onNav(id)}
            className={`${styles.navItem} ${activeNav === id ? styles.active : ''}`}
          >
            <span className={styles.navIcon}>
              <Icon size={15} color={activeNav === id ? 'var(--accent)' : 'var(--t3)'} />
            </span>
            <span className={styles.navLabel}>{label}</span>
          </button>
        ))}
      </nav>

      {/* ── ESP32 device card ── */}
      <div className={styles.deviceCard}>
        <div className={styles.deviceAvatar}>
          <IconCpu size={14} color="var(--accent)" />
        </div>
        <div className={styles.deviceInfo}>
          <div className={styles.deviceName}>ESP32-S3</div>
          <div className={styles.deviceSub}>DS-CNN INT8 · HVP1 v1</div>
        </div>
        <div className={`${styles.deviceDot} ${serverUp ? styles.deviceOnline : ''}`} />
      </div>

    </aside>
  )
}
