export default function InferencePipeline({ latestEvent }) {
  const active = latestEvent && (Date.now() - new Date(latestEvent.timestamp).getTime() < 3000)
  
  const tx = latestEvent?.kw_to_connect_ms || 0
  const rcv = latestEvent?.receive_gap_ms || 0
  const inf = latestEvent?.transcribe_ms || 0
  const total = tx + rcv + inf

  const steps = [
    { id: 1, name: 'MIC BUFFER', time: active ? 12 : 0, color: 'var(--accent-cyan)' },
    { id: 2, name: 'MFCC & TFLITE', time: active ? 32 : 0, color: 'var(--accent-cyan)' },
    { id: 3, name: 'WIFI TX', time: active ? tx : 0, color: 'var(--accent-magenta)' },
    { id: 4, name: 'SERVER RX', time: active ? rcv : 0, color: 'var(--accent-magenta)' },
    { id: 5, name: 'WHISPER', time: active ? inf : 0, color: 'var(--accent-purple)' }
  ]

  return (
    <div style={{ height: '100%', display: 'flex', flexDirection: 'column', gap: 12 }}>
      
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline' }}>
        <span style={{ fontSize: 10, color: 'var(--text-muted)' }}>TOTAL E2E LATENCY</span>
        <span className="mono" style={{ fontSize: 24, fontWeight: 700, color: total > 500 ? 'var(--accent-magenta)' : 'var(--accent-cyan)', textShadow: `0 0 10px ${total > 500 ? 'var(--accent-magenta)' : 'var(--accent-cyan)'}` }}>
          {active ? (total + 44) : 0}<span style={{ fontSize: 12, color: 'var(--text-muted)' }}>ms</span>
        </span>
      </div>

      {/* Flow State Diagram */}
      <div style={{ flex: 1, position: 'relative', display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '0 20px' }}>
        
        {/* Connecting Line */}
        <div style={{ position: 'absolute', top: '50%', left: 40, right: 40, height: 2, background: 'rgba(255,255,255,0.1)', zIndex: 0 }}>
          {active && (
            <div style={{ 
              position: 'absolute', top: -3, left: 0, width: 20, height: 8, 
              background: 'var(--accent-cyan)', borderRadius: 4, boxShadow: '0 0 10px var(--accent-cyan)',
              animation: 'slide-flow 1.5s ease-in-out infinite' 
            }}></div>
          )}
        </div>

        {/* Nodes */}
        {steps.map((s, i) => (
          <div key={i} style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 10, zIndex: 1 }}>
            <div style={{ 
              width: 16, height: 16, borderRadius: '50%', background: '#000', 
              border: `2px solid ${active ? s.color : 'rgba(255,255,255,0.2)'}`,
              boxShadow: active ? `0 0 15px ${s.color}` : 'none',
              transition: 'all 0.3s'
            }}></div>
            <div style={{ textAlign: 'center' }}>
              <div style={{ fontSize: 9, color: 'var(--text-muted)', width: 60, whiteSpace: 'nowrap' }}>{s.name}</div>
              <div className="mono" style={{ fontSize: 10, fontWeight: 700, color: active ? 'var(--text-main)' : 'var(--text-muted)' }}>{s.time}ms</div>
            </div>
          </div>
        ))}
      </div>

      <style>{`
        @keyframes slide-flow {
          0% { left: 0; opacity: 1; background: var(--accent-cyan); box-shadow: 0 0 10px var(--accent-cyan); }
          50% { background: var(--accent-magenta); box-shadow: 0 0 10px var(--accent-magenta); }
          100% { left: 95%; opacity: 0; background: var(--accent-purple); box-shadow: 0 0 10px var(--accent-purple); }
        }
      `}</style>
      
    </div>
  )
}
