import { useState, useEffect } from 'react'
import { IconBell, IconClock } from './Icons.jsx'

function timeAgo(iso) {
  if (!iso) return '—'
  const diff = (Date.now() - new Date(iso).getTime()) / 1000
  if (diff < 5)     return 'just now'
  if (diff < 60)    return `${Math.floor(diff)}s ago`
  if (diff < 3600)  return `${Math.floor(diff / 60)}m ago`
  if (diff < 86400) return `${Math.floor(diff / 3600)}h ago`
  return new Date(iso).toLocaleDateString([], { month: 'short', day: 'numeric' })
}

function latencyColor(ms) {
  if (!ms) return 'var(--t4)'
  if (ms < 400) return 'var(--green)'
  if (ms < 800) return 'var(--amber)'
  return 'var(--red)'
}

function groupEvents(events) {
  const now = Date.now()
  return {
    'Just Now':  events.filter(e => (now - new Date(e.timestamp).getTime()) < 30000),
    'Last Hour': events.filter(e => { const d = now - new Date(e.timestamp).getTime(); return d >= 30000 && d < 3600000 }),
    'Today':     events.filter(e => { const d = now - new Date(e.timestamp).getTime(); return d >= 3600000 && d < 86400000 }),
    'Earlier':   events.filter(e => (now - new Date(e.timestamp).getTime()) >= 86400000),
  }
}

function LatencyBar({ kw, rcv, asr }) {
  const total = (kw || 0) + (rcv || 0) + (asr || 0)
  if (!total) return null
  return (
    <div className="feed-card-bar">
      {[
        { val: kw || 0, color: 'var(--cyan)' },
        { val: rcv || 0, color: 'var(--ice)' },
        { val: asr || 0, color: 'var(--teal)' },
      ].map((s, i) => (
        <div key={i} style={{ flex: s.val, background: s.color, opacity: 0.7, minWidth: 2 }} />
      ))}
    </div>
  )
}

export default function DetectionFeedNew({ events }) {
  const [, setTick] = useState(0)
  useEffect(() => {
    const t = setInterval(() => setTick(x => x + 1), 5000)
    return () => clearInterval(t)
  }, [])

  if (events.length === 0) {
    return (
      <div className="feed-empty">
        <div className="feed-empty-icon">
          <IconBell size={22} color="var(--t3)" />
        </div>
        <div style={{ textAlign: 'center' }}>
          <div style={{ color: 'var(--t2)', fontSize: 12, fontWeight: 500, marginBottom: 4 }}>No detections yet</div>
          <div style={{ color: 'var(--t4)', fontSize: 10 }}>Say "Hey Vaani" to begin</div>
        </div>
      </div>
    )
  }

  const groups = groupEvents([...events].reverse())

  return (
    <div style={{ paddingBottom: 8 }}>
      {Object.entries(groups).map(([name, evts]) => {
        if (!evts.length) return null
        return (
          <div key={name}>
            <div className="feed-group-label">
              <IconClock size={9} color="var(--t4)" />
              <span className="feed-group-text">{name}</span>
              <span className="feed-group-count">{evts.length}</span>
            </div>

            {evts.map((e, idx) => {
              const total = e.end_to_end_ms ?? ((e.kw_to_connect_ms ?? 0) + (e.receive_gap_ms ?? 0) + (e.transcribe_ms ?? 0))
              const isNewest = name === 'Just Now' && idx === 0
              const tx = e.transcript || ''
              const isFiltered = tx.startsWith('[skipped:') || tx.startsWith('[silence') || tx.startsWith('[transcription error')
              const isWake = e.status === 'wake_word_detected'

              return (
                <div
                  key={e.session_id}
                  className={`feed-card ${isNewest ? 'newest' : ''}`}
                >
                  <div className="feed-card-top">
                    <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                      <span className={`feed-card-id ${isNewest ? 'newest' : ''}`}>#{e.session_id}</span>
                      <span style={{ fontSize: 9, color: 'var(--t4)', fontFamily: 'var(--font-mono)' }}>{e.client_ip ?? 'esp32'}</span>
                    </div>
                    <div className="feed-card-meta">
                      {total > 0 && (
                        <span className="feed-card-latency" style={{ color: latencyColor(total) }}>{total}ms</span>
                      )}
                      <span className="feed-card-time">{timeAgo(e.timestamp)}</span>
                    </div>
                  </div>

                  <div className={`feed-card-transcript ${isNewest ? 'newest' : ''} ${isFiltered ? 'muted' : ''}`}>
                    {isWake
                      ? <span style={{ color: 'var(--green)', fontWeight: 700 }}>⚡ HEY VAANI DETECTED</span>
                      : isFiltered
                      ? tx
                      : tx
                      ? `"${tx}"`
                      : <em style={{ color: 'var(--t4)', fontSize: 10 }}>Processing…</em>
                    }
                  </div>

                  <LatencyBar kw={e.kw_to_connect_ms} rcv={e.receive_gap_ms} asr={e.transcribe_ms} />
                </div>
              )
            })}
          </div>
        )
      })}
    </div>
  )
}
