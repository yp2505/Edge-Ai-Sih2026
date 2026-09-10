import { useMemo } from 'react'

export default function PipePanel({ latest, events }) {
  const active = latest && (Date.now() - new Date(latest.timestamp).getTime() < 4000)
  
  // Real latencies from backend
  const l_wake = active && latest?.kw_to_connect_ms != null ? latest.kw_to_connect_ms : null
  const l_audio = active && latest?.receive_gap_ms != null ? latest.receive_gap_ms : null
  const l_asr = active && latest?.transcribe_ms != null ? latest.transcribe_ms : null
  
  let l_resp = null
  if (active && latest?.end_to_end_ms != null && l_wake != null && l_audio != null && l_asr != null) {
    l_resp = Math.max(0, latest.end_to_end_ms - (l_wake + l_audio + l_asr))
  }
  
  const total = active ? latest?.end_to_end_ms : null
  const tc = active && total > 0 ? (total < 400 ? 'var(--green)' : total < 800 ? 'var(--amber)' : 'var(--red)') : 'var(--t4)'

  const nodes = ['ESP32', 'Wi-Fi', 'Audio Upload', 'Whisper ASR', 'Response']

  return (
    <div className="pipe-panel-content" style={{ display: 'flex', flexDirection: 'column', width: '100%', height: '100%', padding: '4px 20px 16px' }}>
      
      {/* Pipeline Visual */}
      <div className="pipe-graph">
        {nodes.map((node, i) => (
          <div key={node} className="pipe-node-wrap" style={{ flex: i < nodes.length - 1 ? 1 : 'none' }}>
            <div style={{ 
              padding: '6px 10px', borderRadius: 6, fontSize: 9, fontWeight: 800, letterSpacing: 1,
              background: active ? 'rgba(0,229,255,0.08)' : 'rgba(255,255,255,0.02)',
              border: `1px solid ${active ? 'rgba(0,229,255,0.3)' : 'rgba(255,255,255,0.08)'}`,
              color: active ? 'var(--t1)' : 'var(--t4)',
              boxShadow: active ? '0 0 12px rgba(0,229,255,0.2)' : 'none',
              animation: active ? 'pulse-glow 2s infinite alternate' : 'none',
              whiteSpace: 'nowrap'
            }}>
              {node}
            </div>
            {i < nodes.length - 1 && (
              <div className="pipe-node-line" style={{ background: active ? 'var(--c1)' : 'rgba(255,255,255,0.1)' }}>
                <div className="pipe-node-arr" style={{ borderLeftColor: active ? 'var(--c1)' : 'rgba(255,255,255,0.2)', borderTopColor: active ? 'var(--c1)' : 'rgba(255,255,255,0.2)' }} />
              </div>
            )}
          </div>
        ))}
      </div>

      <div className="pipe-details">
        {/* Latency Breakdown */}
        <div style={{ flex: 1, display: 'flex', flexDirection: 'column', gap: 4, justifyContent: 'center' }}>
          {[
            { l: 'ESP32 / Wake → Wi-Fi', v: l_wake },
            { l: 'Audio Upload', v: l_audio },
            { l: 'Whisper ASR',  v: l_asr },
            { l: 'Response',     v: l_resp }
          ].map(row => (
            <div key={row.l} style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', paddingBottom: 2, borderBottom: '1px solid rgba(255,255,255,0.03)' }}>
              <span style={{ fontSize: 10, color: 'var(--t3)', fontWeight: 700, letterSpacing: 0.5 }}>{row.l}</span>
              <span style={{ fontFamily: 'var(--mono)', fontSize: 12, fontWeight: 800, color: active && row.v ? 'var(--t1)' : 'var(--t4)' }}>
                {active && row.v ? `${row.v} ms` : '--'}
              </span>
            </div>
          ))}
        </div>

        <div className="pipe-div" />

        {/* Total Latency */}
        <div className="pipe-total">
          <div style={{ fontSize: 9, fontWeight: 800, letterSpacing: 1.5, color: 'var(--t3)', marginBottom: 8, textAlign: 'right' }}>TOTAL END-TO-END<br/>LATENCY</div>
          <div style={{ display: 'flex', alignItems: 'baseline', gap: 6 }}>
            <div style={{ fontFamily: 'var(--title)', fontSize: 32, fontWeight: 900, color: active ? tc : 'var(--t4)', textShadow: active ? `0 0 15px ${tc}60` : 'none', lineHeight: 1 }}>
              {active && total ? total : '--'}
            </div>
            <div style={{ fontSize: 12, fontWeight: 700, color: 'var(--t4)' }}>ms</div>
          </div>
        </div>
      </div>
    </div>
  )
}
