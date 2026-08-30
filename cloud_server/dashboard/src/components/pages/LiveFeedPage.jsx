import { useRef, useEffect } from 'react'
import styles from './Pages.module.css'
import feedStyles from '../RecentDetections.module.css'

function timeAgo(iso) {
  if (!iso) return '—'
  const d = (Date.now() - new Date(iso).getTime()) / 1000
  if (d < 5) return 'just now'
  if (d < 60) return `${Math.floor(d)}s ago`
  if (d < 3600) return `${Math.floor(d/60)}m ago`
  return new Date(iso).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
}

function latBadge(t) {
  if (!t) return { cls: 'badge-muted', label: '—' }
  if (t < 400) return { cls: 'badge-green',  label: `${t} ms ⚡` }
  if (t < 800) return { cls: 'badge-yellow', label: `${t} ms` }
  return { cls: 'badge-red', label: `${t} ms ⚠` }
}

export default function LiveFeedPage({ events }) {
  const topRef = useRef(null)
  useEffect(() => { topRef.current?.scrollIntoView({ behavior: 'smooth', block: 'nearest' }) }, [events.length])

  return (
    <div className={styles.page}>
      <div className={`card ${styles.liveFeedPanel}`}>
        <div className={styles.feedHeader}>
          <div className={styles.feedHeaderLeft}>
            <span className={styles.liveDot} />
            <span className={styles.feedTitle}>Live Detection Feed</span>
          </div>
          <span className="badge badge-muted">{events.length} events</span>
        </div>

        <div className={styles.feedScroll} id="live-feed-scroll">
          <div ref={topRef} />
          {events.length === 0 ? (
            <div className={styles.emptyFeed}>
              <div className={styles.emptyIcon}>
                <svg width="40" height="40" viewBox="0 0 24 24" fill="none" stroke="var(--t3)" strokeWidth="1.5" strokeLinecap="round">
                  <path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M1.42 9a16 16 0 0 1 21.16 0"/>
                  <path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><circle cx="12" cy="20" r="1" fill="var(--t3)" stroke="none"/>
                </svg>
              </div>
              <p className={styles.emptyTitle}>Waiting for wake-word…</p>
              <p className={styles.emptySub}>Say "Hey Vaani" near your ESP32 to trigger a detection</p>
            </div>
          ) : (
            events.map((e, i) => {
              const total = (e.kw_to_connect_ms??0)+(e.receive_gap_ms??0)+(e.transcribe_ms??0)
              const { cls, label } = latBadge(total)
              return (
                <div key={e.session_id} className={`${styles.feedCard} ${i===0 ? styles.feedCardNew : ''}`}>
                  <div className={styles.cardLeft}>
                    <span className={`${styles.sessionId} mono`}>#{e.session_id}</span>
                    <span className={styles.cardTime}>{timeAgo(e.timestamp)}</span>
                  </div>
                  <div className={styles.cardCenter}>
                    <div className={styles.cardTranscript}>&ldquo;{e.transcript || 'Processing…'}&rdquo;</div>
                    <div className={styles.cardMetrics}>
                      <span className="mono" style={{fontSize:11, color:'var(--t3)'}}>kw→connect: <b style={{color:'var(--purple)'}}>{e.kw_to_connect_ms??0}ms</b></span>
                      <span className="mono" style={{fontSize:11, color:'var(--t3)'}}>rcv gap: <b style={{color:'var(--accent)'}}>{e.receive_gap_ms??0}ms</b></span>
                      <span className="mono" style={{fontSize:11, color:'var(--t3)'}}>transcribe: <b style={{color:'#60a5fa'}}>{e.transcribe_ms??0}ms</b></span>
                      {e.client_ip && <span style={{fontSize:10, color:'var(--t3)'}}>from {e.client_ip}</span>}
                    </div>
                  </div>
                  <div className={styles.cardRight}>
                    <span className={`badge ${cls}`}>{label}</span>
                  </div>
                </div>
              )
            })
          )}
        </div>
      </div>
    </div>
  )
}
