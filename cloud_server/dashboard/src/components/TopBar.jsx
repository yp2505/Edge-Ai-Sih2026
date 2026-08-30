import styles from './TopBar.module.css'

export default function TopBar({ search, onSearch, timeIdx, onTime, timeFilters, health, serverUp }) {
  const uptime = health?.uptime_seconds
  const fmt = s => s == null ? '—' : s < 60 ? `${s}s` : s < 3600 ? `${Math.floor(s/60)}m ${s%60}s` : `${Math.floor(s/3600)}h ${Math.floor((s%3600)/60)}m`

  return (
    <div className={styles.topbar}>
      {/* Left — page title */}
      <div className={styles.titleBlock}>
        <h1 className={styles.title}>Detection Overview</h1>
        <p className={styles.subtitle}>
          Monitor wake-word activations, ASR transcripts and latency in real time
        </p>
      </div>

      {/* Right cluster */}
      <div className={styles.right}>
        {/* Search */}
        <div className={styles.searchWrap}>
          <span className={styles.searchIcon}>🔍</span>
          <input
            id="search-transcripts"
            className={styles.search}
            placeholder="Search transcripts or session ID…"
            value={search}
            onChange={e => onSearch(e.target.value)}
          />
          {search && (
            <button className={styles.clearBtn} onClick={() => onSearch('')} title="Clear">✕</button>
          )}
        </div>

        {/* Time filter pills */}
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

        {/* Uptime chip */}
        {health && (
          <div className={styles.uptimeChip}>
            <span className={styles.uptimeDot} />
            <span className={`${styles.uptimeVal} mono`}>{fmt(uptime)}</span>
          </div>
        )}
      </div>
    </div>
  )
}
