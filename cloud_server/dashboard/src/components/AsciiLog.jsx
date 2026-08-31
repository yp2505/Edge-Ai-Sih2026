import { useRef, useEffect } from 'react'

export default function AsciiLog({ events }) {
  const scrollRef = useRef(null)
  
  useEffect(() => {
    if (scrollRef.current) {
      scrollRef.current.scrollTop = scrollRef.current.scrollHeight
    }
  }, [events.length])

  // Simple pure-text graph for latency
  const makeBar = (val) => {
    const bars = Math.min(20, Math.floor(val / 50))
    return '[' + '#'.repeat(bars) + '-'.repeat(20 - bars) + ']'
  }

  return (
    <pre style={{ height: '100%', display: 'flex', flexDirection: 'column' }}>
{`
--- EVENT_LOGS ---
`}
      <div ref={scrollRef} style={{ flex: 1, overflowY: 'auto', display: 'flex', flexDirection: 'column', gap: '10px' }}>
        {events.length === 0 ? (
          <span className="muted">NO DATA IN BUFFER...</span>
        ) : (
          events.map((e, i) => {
            const total = (e.kw_to_connect_ms||0)+(e.receive_gap_ms||0)+(e.transcribe_ms||0)
            const slow = total > 500
            const clr = slow ? 'alert' : ''
            return (
              <div key={e.session_id}>
                <span style={{ color: 'var(--text-muted)' }}>[{e.session_id.toString().padStart(4,'0')}]</span> <span className="mono" style={{ fontSize: 12 }}>{e.transcript}</span>
                <br/>
                <span className="mono" style={{ fontSize: 10, color: clr }}>LATENCY: {total.toString().padStart(4,'0')}ms {makeBar(total)}</span>
              </div>
            )
          })
        )}
      </div>
    </pre>
  )
}
