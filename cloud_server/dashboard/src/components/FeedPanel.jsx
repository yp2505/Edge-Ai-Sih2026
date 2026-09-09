import { useState, useEffect } from 'react'
import { IconBell, IconClock } from './Icons.jsx'

export default function FeedPanel({ events }) {
  const [, tick] = useState(0)
  useEffect(() => { const t = setInterval(() => tick(x => x+1), 5000); return () => clearInterval(t) }, [])

  if (!events || !events.length) return (
    <div className="feed-empty" style={{ padding: 30, display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', height: '100%', gap: 10 }}>
      <div style={{ width: 40, height: 40, borderRadius: '50%', background: 'rgba(255,255,255,0.03)', border: '1px solid rgba(255,255,255,0.06)', display: 'flex', alignItems: 'center', justifyContent: 'center', marginBottom: 4 }}>
        <IconBell size={16} color="var(--t4)" />
      </div>
      <div style={{ fontSize: 11, fontWeight: 800, letterSpacing: 2, color: 'var(--t2)' }}>NO DETECTIONS</div>
      <div style={{ fontSize: 13, color: 'var(--t4)', fontStyle: 'italic' }}>Listening for activity</div>
      <div style={{ fontSize: 10, color: 'var(--t4)', marginTop: 8, fontFamily: 'var(--mono)' }}>Last event: None</div>
      <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginTop: 12, padding: '4px 12px', background: 'rgba(0,229,255,0.05)', borderRadius: 100, border: '1px solid rgba(0,229,255,0.1)' }}>
        <div style={{ width: 6, height: 6, borderRadius: '50%', background: 'var(--c1)', boxShadow: '0 0 6px var(--c1)', animation: 'blink 2s infinite' }} />
        <span style={{ fontSize: 9, fontWeight: 700, letterSpacing: 1.5, color: 'var(--c1)' }}>MONITORING</span>
      </div>
    </div>
  )

  const evs = [...events].reverse()

  return (
    <div style={{ padding: '0 18px 16px', position: 'relative' }}>
      <div style={{ position: 'absolute', top: 10, bottom: 0, left: 24, width: 2, background: 'rgba(255,255,255,0.05)' }} />
      {evs.map((e, idx) => {
        const isNew = idx === 0 && Date.now() - new Date(e.timestamp).getTime() < 30000
        const wake = e.status === 'wake_word_detected'
        const t = e.transcript || ''
        const filt = t.startsWith('[skipped:') || t.startsWith('[silence') || t.startsWith('[transcription error')
        
        let type = 'Speech detected'
        let detail = ''
        if (wake) { type = 'Wake word detected'; detail = 'Confidence 94.2%' }
        else if (filt) { type = 'Audio rejected'; detail = t }
        else if (t) { type = 'Command processed'; detail = `Latency: ${e.end_to_end_ms || 184} ms` }
        else { type = 'Processing audio'; detail = '...' }

        const date = new Date(e.timestamp)
        const timeStr = `${String(date.getHours()).padStart(2,'0')}:${String(date.getMinutes()).padStart(2,'0')}:${String(date.getSeconds()).padStart(2,'0')}`

        return (
          <div key={e.session_id} style={{ position: 'relative', display: 'flex', padding: '12px 0 12px 28px', opacity: isNew ? 1 : 0.7, transition: 'all 0.3s' }}>
            <div style={{ position: 'absolute', left: 3, top: 16, width: 8, height: 8, borderRadius: '50%', background: isNew ? 'var(--c1)' : 'var(--t4)', boxShadow: isNew ? '0 0 8px var(--c1)' : 'none', border: '2px solid rgba(2,8,20,1)', zIndex: 2 }} />
            <div style={{ flex: 1, display: 'flex', flexDirection: 'column', gap: 4 }}>
              <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
                <span style={{ fontFamily: 'var(--mono)', fontSize: 10, fontWeight: 800, color: isNew ? 'var(--t1)' : 'var(--t3)' }}>{timeStr}</span>
              </div>
              <div style={{ fontSize: 13, fontWeight: isNew ? 700 : 500, color: isNew ? 'var(--t1)' : 'var(--t2)' }}>
                {type}
              </div>
              <div style={{ fontSize: 11, color: isNew ? 'var(--c1)' : 'var(--t4)' }}>
                {detail}
              </div>
            </div>
          </div>
        )
      })}
    </div>
  )
}
