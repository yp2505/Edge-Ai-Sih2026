import { useRef, useEffect } from 'react'
import styles from './DetectionFeed.module.css'

function formatTime(isoStr) {
  if (!isoStr) return '—'
  try {
    return new Date(isoStr).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })
  } catch {
    return isoStr
  }
}

function latencyColor(total) {
  if (total < 400)  return '#34d399'  // green  — fast
  if (total < 800)  return '#f59e0b'  // yellow — moderate
  return '#f87171'                    // red    — slow
}

function DetectionCard({ entry, isNewest }) {
  const kw   = entry.kw_to_connect_ms  ?? 0
  const rcv  = entry.receive_gap_ms    ?? 0
  const txc  = entry.transcribe_ms     ?? 0
  const total = kw + rcv + txc

  return (
    <div className={`${styles.card} ${isNewest ? styles.cardNew : ''}`}>
      {/* Card header row */}
      <div className={styles.cardHeader}>
        <span className="pill pill-cyan mono">#{entry.session_id}</span>
        <span className={styles.timestamp}>{formatTime(entry.timestamp)}</span>
        {entry.client_ip && (
          <span className={`pill pill-blue mono ${styles.ipBadge}`}>{entry.client_ip}</span>
        )}
      </div>

      {/* Transcript — large, readable at 2 metres */}
      <div className={styles.transcript}>
        &ldquo;{entry.transcript || 'Processing…'}&rdquo;
      </div>

      {/* Latency breakdown row */}
      <div className={styles.metricsRow}>
        <div className={styles.metricBox}>
          <span className={styles.metricLabel}>kw→connect</span>
          <span className={`${styles.metricValue} mono`}>{kw} ms</span>
        </div>
        <div className={styles.metricDivider} />
        <div className={styles.metricBox}>
          <span className={styles.metricLabel}>rcv gap</span>
          <span className={`${styles.metricValue} mono`}>{rcv} ms</span>
        </div>
        <div className={styles.metricDivider} />
        <div className={styles.metricBox}>
          <span className={styles.metricLabel}>transcribe</span>
          <span className={`${styles.metricValue} mono`}>{txc} ms</span>
        </div>
        <div className={styles.metricDivider} />
        {/* Total — accented */}
        <div className={`${styles.metricBox} ${styles.metricTotal}`}>
          <span className={styles.metricLabel}>total E2E</span>
          <span
            className={`${styles.metricValueLg} mono`}
            style={{ color: latencyColor(total) }}
          >
            {total} ms
          </span>
        </div>
      </div>

      {/* Audio duration if available */}
      {entry.audio_duration_s != null && (
        <div className={styles.cardFooter}>
          <span className="text-muted" style={{ fontSize: 11 }}>
            Audio: {entry.audio_duration_s.toFixed(2)}s &nbsp;·&nbsp;
            {(entry.audio_bytes / 1024).toFixed(1)} KB &nbsp;·&nbsp;
            avg_logprob: {entry.avg_log_prob ?? '—'}
          </span>
        </div>
      )}
    </div>
  )
}

export default function DetectionFeed({ events }) {
  const topRef = useRef(null)

  // Scroll to top when new entry arrives
  useEffect(() => {
    topRef.current?.scrollIntoView({ behavior: 'smooth', block: 'nearest' })
  }, [events.length])

  return (
    <div className={`glass ${styles.panel}`}>
      {/* Panel header */}
      <div className={styles.panelHeader}>
        <h2 className={styles.panelTitle}>
          <span className={styles.dot} />
          Live Detection Feed
        </h2>
        <span className="text-muted" style={{ fontSize: 12 }}>
          {events.length} event{events.length !== 1 ? 's' : ''} · newest first
        </span>
      </div>

      {/* Scrollable feed */}
      <div className={styles.feedScroll}>
        <div ref={topRef} />
        {events.length === 0 ? (
          <div className={styles.emptyState}>
            <div className={styles.emptyIcon}>📡</div>
            <p className={styles.emptyTitle}>Waiting for detections…</p>
            <p className="text-muted" style={{ fontSize: 13, marginTop: 6 }}>
              Say &ldquo;Hey Vaani&rdquo; near the ESP32 to trigger a wake-word event
            </p>
          </div>
        ) : (
          events.map((entry, i) => (
            <DetectionCard key={entry.session_id} entry={entry} isNewest={i === 0} />
          ))
        )}
      </div>
    </div>
  )
}
