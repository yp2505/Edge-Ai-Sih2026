import { useRef, useEffect } from 'react'
import styles from './DetectionFeed.module.css'

function timeAgo(iso) {
  if (!iso) return '—'
  const diff = (Date.now() - new Date(iso).getTime()) / 1000
  if (diff < 5)   return 'just now'
  if (diff < 60)  return `${Math.floor(diff)}s ago`
  if (diff < 3600) return `${Math.floor(diff/60)}m ago`
  return new Date(iso).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
}

function totalLatency(e) {
  return (e.kw_to_connect_ms ?? 0) + (e.receive_gap_ms ?? 0) + (e.transcribe_ms ?? 0)
}

function latencyPill(total) {
  if (total < 400)  return { cls: 'pill-green',  label: 'Fast' }
  if (total < 800)  return { cls: 'pill-yellow', label: 'Moderate' }
  return                   { cls: 'pill-red',    label: 'Slow' }
}

function DetectionCard({ entry, isNewest }) {
  const total = totalLatency(entry)
  const { cls, label } = latencyPill(total)

  return (
    <article className={`${styles.card} glass ${isNewest ? styles.cardNew : ''}`}>

      {/* Top row */}
      <div className={styles.topRow}>
        <div className={styles.sessionTag}>
          <div className={styles.sessionDot} />
          <span className="mono" style={{ fontSize: 12 }}>Session #{entry.session_id}</span>
        </div>
        <div className={styles.topRight}>
          <span className={`pill-sm ${cls}`}>{label} · {total} ms</span>
          <span className={styles.timeStamp}>{timeAgo(entry.timestamp)}</span>
        </div>
      </div>

      {/* Transcript */}
      <p className={styles.transcript}>&ldquo;{entry.transcript || 'Processing…'}&rdquo;</p>

      {/* Latency breakdown — pill row */}
      <div className={styles.pillRow}>
        <span className={`${styles.metricPill} pill`}>
          <span className={styles.mDot} style={{ background: 'var(--purple)' }} />
          <span className={styles.mLabel}>kw→connect</span>
          <span className={`${styles.mVal} mono`}>{entry.kw_to_connect_ms ?? 0} ms</span>
        </span>
        <span className={`${styles.metricPill} pill`}>
          <span className={styles.mDot} style={{ background: 'var(--cyan)' }} />
          <span className={styles.mLabel}>rcv gap</span>
          <span className={`${styles.mVal} mono`}>{entry.receive_gap_ms ?? 0} ms</span>
        </span>
        <span className={`${styles.metricPill} pill`}>
          <span className={styles.mDot} style={{ background: 'var(--blue)' }} />
          <span className={styles.mLabel}>transcribe</span>
          <span className={`${styles.mVal} mono`}>{entry.transcribe_ms ?? 0} ms</span>
        </span>
        {entry.client_ip && (
          <span className={`${styles.metricPill} ${styles.ipPill} pill`}>
            <span className={styles.mLabel}>from</span>
            <span className={`${styles.mVal} mono`}>{entry.client_ip}</span>
          </span>
        )}
      </div>

    </article>
  )
}

export default function DetectionFeed({ events }) {
  const topRef = useRef(null)

  useEffect(() => {
    topRef.current?.scrollIntoView({ behavior: 'smooth', block: 'nearest' })
  }, [events.length])

  return (
    <div className={styles.panel}>

      {/* Header */}
      <div className={styles.panelHeader}>
        <div className={styles.titleRow}>
          <div className={styles.titleDot} />
          <h2 className={styles.title}>Live Detection Feed</h2>
          <span className={`pill-sm pill-ghost`}>{events.length} events</span>
        </div>
        <p className={styles.subtitle}>Auto-refreshing · newest first · polling every 1.5s</p>
      </div>

      {/* Feed */}
      <div className={styles.feed} id="detection-feed">
        <div ref={topRef} />

        {events.length === 0 ? (
          <div className={styles.empty}>
            <div className={styles.emptyOrb}>📡</div>
            <p className={styles.emptyTitle}>No detections yet</p>
            <p className={styles.emptySub}>Say &ldquo;Hey Vaani&rdquo; near your ESP32 device to trigger a session</p>
          </div>
        ) : (
          events.map((e, i) => (
            <DetectionCard key={e.session_id} entry={e} isNewest={i === 0} />
          ))
        )}
      </div>

    </div>
  )
}
