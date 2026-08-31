import { useState, useEffect } from 'react'
import { IconBell, IconClock } from './Icons.jsx'

function timeAgo(iso) {
  if (!iso) return '—'
  const diff = (Date.now() - new Date(iso).getTime()) / 1000
  if (diff < 5)     return 'just now'
  if (diff < 60)    return `${Math.floor(diff)}s ago`
  if (diff < 3600)  return `${Math.floor(diff/60)}m ago`
  if (diff < 86400) return `${Math.floor(diff/3600)}h ago`
  return new Date(iso).toLocaleDateString([], { month: 'short', day: 'numeric' })
}

function latencyColor(total) {
  if (!total) return 'var(--t3)'
  if (total < 400) return 'var(--green)'
  if (total < 800) return 'var(--yellow)'
  return 'var(--red)'
}
function latencyClass(total) {
  if (!total) return 'badge-muted'
  if (total < 400) return 'badge-green'
  if (total < 800) return 'badge-yellow'
  return 'badge-red'
}

function groupEvents(events) {
  const now = Date.now()
  return {
    'Just Now':  events.filter(e => (now - new Date(e.timestamp).getTime()) < 30000),
    'Last Hour': events.filter(e => { const d = (now - new Date(e.timestamp).getTime()); return d >= 30000 && d < 3600000 }),
    'Today':     events.filter(e => { const d = (now - new Date(e.timestamp).getTime()); return d >= 3600000 && d < 86400000 }),
    'Earlier':   events.filter(e => (now - new Date(e.timestamp).getTime()) >= 86400000),
  }
}

function LatencyMini({ kw, rcv, asr }) {
  const total = (kw || 0) + (rcv || 0) + (asr || 0)
  if (!total) return null
  const segs = [
    { val: kw || 0, color: 'var(--primary)' },
    { val: rcv || 0, color: 'var(--sky)' },
    { val: asr || 0, color: 'var(--pink)' },
  ]
  return (
    <div style={{ display: 'flex', height: 4, borderRadius: 4, overflow: 'hidden', gap: 1, marginTop: 6 }}>
      {segs.map((s, i) => (
        <div key={i} style={{
          flex: s.val,
          background: s.color,
          opacity: 0.7,
          minWidth: 2,
        }} />
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
      <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', height: '100%', gap: 14, padding: 24 }}>
        <div style={{
          width: 56, height: 56, borderRadius: 16,
          background: 'rgba(124,106,247,0.08)',
          border: '1px solid rgba(124,106,247,0.15)',
          display: 'flex', alignItems: 'center', justifyContent: 'center',
        }}>
          <IconBell size={24} color="var(--t3)" />
        </div>
        <div style={{ textAlign: 'center' }}>
          <div style={{ color: 'var(--t2)', fontSize: 13, fontWeight: 500, marginBottom: 4 }}>No detections yet</div>
          <div style={{ color: 'var(--t3)', fontSize: 11 }}>Say "Hey Vaani" to begin</div>
        </div>
      </div>
    )
  }

  const groups = groupEvents([...events].reverse())

  return (
    <div style={{ padding: '4px 0 8px' }}>
      {Object.entries(groups).map(([name, evts]) => {
        if (!evts.length) return null
        return (
          <div key={name}>
            {/* Group label */}
            <div style={{
              padding: '10px 14px 4px',
              display: 'flex', alignItems: 'center', gap: 8,
            }}>
              <IconClock size={10} color="var(--t3)" />
              <span style={{ fontSize: 9, fontWeight: 700, color: 'var(--t3)', letterSpacing: '1.5px', textTransform: 'uppercase' }}>{name}</span>
              <span style={{
                fontSize: 9, fontWeight: 600, color: 'var(--t3)',
                background: 'rgba(255,255,255,0.05)',
                padding: '1px 7px', borderRadius: 100,
              }}>{evts.length}</span>
            </div>

            {evts.map((e, idx) => {
              const total = (e.kw_to_connect_ms ?? 0) + (e.receive_gap_ms ?? 0) + (e.transcribe_ms ?? 0)
              const isNewest = name === 'Just Now' && idx === 0

              return (
                <div
                  key={e.session_id}
                  style={{
                    margin: '3px 10px',
                    padding: '11px 13px',
                    borderRadius: 10,
                    background: isNewest
                      ? 'linear-gradient(135deg, rgba(124,106,247,0.1), rgba(88,192,224,0.05))'
                      : 'rgba(255,255,255,0.025)',
                    border: `1px solid ${isNewest ? 'rgba(124,106,247,0.18)' : 'rgba(255,255,255,0.04)'}`,
                    animation: isNewest ? 'slide-up 0.35s ease' : 'none',
                    transition: 'background 0.3s',
                  }}
                >
                  {/* Top row */}
                  <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 7 }}>
                    <div style={{ display: 'flex', alignItems: 'center', gap: 7 }}>
                      {/* Session ID chip */}
                      <div style={{
                        padding: '2px 8px',
                        background: isNewest ? 'rgba(124,106,247,0.15)' : 'rgba(255,255,255,0.05)',
                        border: `1px solid ${isNewest ? 'rgba(124,106,247,0.2)' : 'rgba(255,255,255,0.06)'}`,
                        borderRadius: 6,
                        fontSize: 9, fontFamily: 'var(--font-mono)', fontWeight: 700,
                        color: isNewest ? 'var(--primary)' : 'var(--t3)',
                      }}>#{e.session_id}</div>
                      <span style={{ fontSize: 10, color: 'var(--t3)', fontFamily: 'var(--font-mono)' }}>
                        {e.client_ip ?? 'esp32'}
                      </span>
                    </div>
                    <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                      {total > 0 && (
                        <span style={{
                          fontSize: 10, fontFamily: 'var(--font-mono)', fontWeight: 700,
                          color: latencyColor(total),
                        }}>{total}ms</span>
                      )}
                      <span style={{ fontSize: 10, color: 'var(--t3)' }}>{timeAgo(e.timestamp)}</span>
                    </div>
                  </div>

                  {/* Transcript */}
                  <div style={{
                    fontSize: 12, fontWeight: isNewest ? 500 : 400,
                    color: isNewest ? 'var(--t1)' : 'var(--t2)',
                    overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
                    marginBottom: total > 0 ? 0 : 0,
                  }}>
                    {e.transcript
                      ? `"${e.transcript}"`
                      : <em style={{ color: 'var(--t3)', fontSize: 11 }}>Processing…</em>
                    }
                  </div>

                  {/* Latency mini bar */}
                  <LatencyMini kw={e.kw_to_connect_ms} rcv={e.receive_gap_ms} asr={e.transcribe_ms} />
                </div>
              )
            })}
          </div>
        )
      })}
    </div>
  )
}
