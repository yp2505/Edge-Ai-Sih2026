import { IconSearch, IconX, IconBell, IconWifi } from './Icons.jsx'
import styles from './TopBar.module.css'

const PAGE_TITLES = {
  overview: { title: 'Detection Overview', sub: 'Monitor wake-word activations, latency and ASR transcripts in real time' },
  livefeed: { title: 'Live Detection Feed', sub: 'Streaming wake-word events from ESP32 as they happen' },
  latency:  { title: 'Latency Analytics',   sub: 'End-to-end breakdown: kw→connect, receive gap, Whisper transcription' },
  device:   { title: 'Device Specs',         sub: 'ESP32-S3 hardware configuration and model efficiency metrics' },
  reports:  { title: 'Reports',              sub: 'Session summaries and downloadable logs' },
  settings: { title: 'Settings',             sub: 'Configure API endpoint, model specs and display preferences' },
}

export default function TopBar({ activeNav, search, onSearch, timeIdx, onTime, timeFilters, health, serverUp }) {
  const { title, sub } = PAGE_TITLES[activeNav] ?? PAGE_TITLES.overview

  const uptime = health?.uptime_seconds
  const fmt = s => {
    if (s == null) return '—'
    const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60
    return h > 0 ? `${h}h ${m}m` : m > 0 ? `${m}m ${sec}s` : `${sec}s`
  }

  return (
    <div className={styles.topbar}>
      {/* Left — page title (changes with nav) */}
      <div className={styles.titleBlock}>
        <h1 className={styles.title}>{title}</h1>
        <p className={styles.subtitle}>{sub}</p>
      </div>

      {/* Right controls */}
      <div className={styles.right}>
        {/* Search (only on overview / livefeed) */}
        {(activeNav === 'overview' || activeNav === 'livefeed') && (
          <div className={styles.searchWrap}>
            <span className={styles.searchIcon}><IconSearch size={13} color="var(--t3)" /></span>
            <input
              id="search-transcripts"
              className={styles.search}
              placeholder="Search transcripts…"
              value={search}
              onChange={e => onSearch(e.target.value)}
            />
            {search && (
              <button className={styles.clearBtn} onClick={() => onSearch('')} title="Clear">
                <IconX size={12} color="var(--t3)" />
              </button>
            )}
          </div>
        )}

        {/* Time filter pills (only on latency + overview) */}
        {(activeNav === 'overview' || activeNav === 'latency') && (
          <div className={styles.timeFilters}>
            {timeFilters.map((f, i) => (
              <button
                key={f.label}
                id={`time-filter-${i}`}
                className={`${styles.timeBtn} ${timeIdx === i ? styles.timeBtnActive : ''}`}
                onClick={() => onTime(i)}
              >
                {f.label}
              </button>
            ))}
          </div>
        )}

        {/* Wireless status chip */}
        <div className={`${styles.chip} ${serverUp ? styles.chipOnline : styles.chipOffline}`}>
          <IconWifi size={13} color={serverUp ? 'var(--green)' : 'var(--t3)'} />
          <span className={`${styles.chipLabel} mono`}>{serverUp ? 'Wireless OK' : 'No Signal'}</span>
        </div>

        {/* Uptime */}
        {health && (
          <div className={styles.uptimeChip}>
            <span className={styles.uptimeDot} />
            <span className={`mono ${styles.uptimeVal}`}>{fmt(uptime)}</span>
          </div>
        )}

        {/* Bell */}
        <button className={styles.iconBtn} id="btn-notifications" title="Notifications">
          <IconBell size={16} color="var(--t2)" />
        </button>
      </div>
    </div>
  )
}
