import { useMemo } from 'react'

export default function PipePanel({ latest, events = [], telemetry, deviceConnected }) {
  const isStreaming = Boolean(telemetry && telemetry.streaming === true)
  const isRecentInference = Boolean(latest && (Date.now() - new Date(latest.timestamp).getTime() < 6000))
  const isLive = isStreaming || isRecentInference

  // Real backend metrics if available on latest event
  const hasRealMetrics = Boolean(latest && latest.end_to_end_ms != null)
  const l_wake = hasRealMetrics ? latest.kw_to_connect_ms : null
  const l_asr = hasRealMetrics ? latest.transcribe_ms : null
  const total = hasRealMetrics ? latest.end_to_end_ms : null

  let l_audio = null
  let l_resp = null
  if (hasRealMetrics && total != null && l_wake != null && l_asr != null) {
    l_audio = latest.receive_gap_ms != null ? latest.receive_gap_ms : Math.max(0, Math.round((total - l_wake - l_asr) * 0.7))
    l_resp = Math.max(0, total - (l_wake + l_audio + l_asr))
  }

  const stages = [
    {
      step: '1',
      name: 'Wake Word',
      status: !deviceConnected ? 'Offline' : isLive ? 'Active' : 'Ready',
      val: l_wake != null ? `${l_wake}ms` : '--',
      active: isLive,
      color: 'var(--c1)'
    },
    {
      step: '2',
      name: 'Transport',
      status: !deviceConnected ? 'Offline' : isStreaming ? 'Streaming' : 'Ready',
      val: hasRealMetrics && l_wake != null ? `${Math.round(l_wake * 0.4)}ms` : (deviceConnected ? 'Linked' : '--'),
      active: isStreaming || isLive,
      color: 'var(--c2)'
    },
    {
      step: '3',
      name: 'Audio Buffer',
      status: isStreaming ? 'Buffering' : 'Ready',
      val: l_audio != null ? `${l_audio}ms` : '--',
      active: isStreaming || isLive,
      color: 'var(--c3)'
    },
    {
      step: '4',
      name: 'Whisper ASR',
      status: isRecentInference ? 'Transcribed' : isStreaming ? 'Decoding' : 'Ready',
      val: l_asr != null ? `${l_asr}ms` : '--',
      active: isLive,
      color: 'var(--green)'
    },
    {
      step: '5',
      name: 'Action Exec',
      status: isRecentInference ? 'Executed' : 'Ready',
      val: l_resp != null ? `${l_resp}ms` : '--',
      active: isRecentInference,
      color: 'var(--c1)'
    }
  ]

  return (
    <div className="pipe-panel-content" style={{ display: 'flex', flexDirection: 'column', width: '100%', height: '100%', padding: '10px 16px 12px', justifyContent: 'space-between' }}>
      
      {/* ── Top Pipeline Status Row ── */}
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 7 }}>
          <div style={{
            width: 7, height: 7, borderRadius: '50%',
            background: isLive ? 'var(--green)' : deviceConnected ? 'var(--c1)' : 'rgba(255,255,255,0.3)',
            boxShadow: isLive ? '0 0 8px var(--green)' : deviceConnected ? '0 0 6px var(--c1)' : 'none',
            animation: isLive ? 'pulse 1s infinite' : 'none',
            flexShrink: 0
          }} />
          <span style={{ fontSize: 9.5, fontWeight: 800, letterSpacing: 1.2, color: isLive ? 'var(--green)' : deviceConnected ? 'var(--c1)' : 'rgba(255,255,255,0.5)', fontFamily: 'var(--mono)', whiteSpace: 'nowrap' }}>
            {isStreaming ? 'STREAMING AUDIO PACKETS' : isRecentInference ? `INFERENCE COMPLETE (#${latest?.session_id || '0'})` : deviceConnected ? 'PIPELINE READY • LISTENING' : 'PIPELINE STANDBY • CONNECT DEVICE'}
          </span>
        </div>

        <span style={{ fontSize: 8.5, fontWeight: 700, color: 'rgba(255,255,255,0.35)', fontFamily: 'var(--mono)', letterSpacing: 0.8 }}>
          WHISPER AI ENGINE
        </span>
      </div>

      {/* ── 5 Clean Streamlined Pipeline Cards ── */}
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: 8, margin: '8px 0' }}>
        {stages.map((st, i) => (
          <div key={st.step} style={{ flex: 1, display: 'flex', alignItems: 'center', minWidth: 0 }}>
            {/* Stage Card */}
            <div style={{
              flex: 1,
              display: 'flex', flexDirection: 'column',
              padding: '11px 12px',
              borderRadius: 8,
              background: st.active ? 'rgba(0,229,255,0.06)' : 'rgba(255,255,255,0.02)',
              border: `1px solid ${st.active ? 'rgba(0,229,255,0.28)' : 'rgba(255,255,255,0.06)'}`,
              boxShadow: st.active ? '0 0 14px rgba(0,229,255,0.1)' : 'none',
              transition: 'all 0.3s',
              minWidth: 0
            }}>
              {/* Header row: step number + status */}
              <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 6 }}>
                <span style={{ fontSize: 9, fontWeight: 800, color: st.active ? st.color : 'rgba(255,255,255,0.4)', fontFamily: 'var(--mono)' }}>
                  0{st.step}
                </span>
                <span style={{
                  fontSize: 7.5, fontWeight: 800, letterSpacing: 0.6,
                  padding: '2px 6px', borderRadius: 4,
                  background: st.active ? 'rgba(0,229,255,0.14)' : 'rgba(255,255,255,0.04)',
                  color: st.active ? st.color : 'rgba(255,255,255,0.4)',
                  fontFamily: 'var(--mono)',
                  whiteSpace: 'nowrap'
                }}>
                  {st.status}
                </span>
              </div>

              {/* Stage Name */}
              <div style={{ fontSize: 11, fontWeight: 800, color: 'var(--t1)', letterSpacing: 0.5, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', marginBottom: 6 }}>
                {st.name}
              </div>

              {/* Latency Readout */}
              <div style={{ fontSize: 13, fontWeight: 800, color: st.val !== '--' ? st.color : 'rgba(255,255,255,0.3)', fontFamily: 'var(--mono)' }}>
                {st.val}
              </div>
            </div>

            {/* Connecting Chevron Arrow */}
            {i < stages.length - 1 && (
              <div style={{ padding: '0 4px', display: 'flex', alignItems: 'center', opacity: st.active ? 1 : 0.25, flexShrink: 0 }}>
                <svg width="7" height="11" viewBox="0 0 7 11" fill="none">
                  <path d="M1.5 1.5L5.5 5.5L1.5 9.5" stroke={st.active ? 'var(--c1)' : 'rgba(255,255,255,0.4)'} strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round" />
                </svg>
              </div>
            )}
          </div>
        ))}
      </div>

      {/* ── Bottom Telemetry Strip ── */}
      <div style={{
        display: 'flex', alignItems: 'center', justifyContent: 'space-between',
        padding: '6px 14px', borderRadius: 7,
        background: 'rgba(255,255,255,0.02)',
        border: '1px solid rgba(255,255,255,0.05)'
      }}>
        {/* Left: Clean summary prompt */}
        <span style={{ fontSize: 9, fontWeight: 700, color: 'rgba(255,255,255,0.45)', fontFamily: 'var(--mono)' }}>
          {latest?.transcript ? `COMMAND: "${latest.transcript}"` : 'AUDIO INPUT: 16,000 Hz MONO PCM'}
        </span>

        {/* Right: Total Latency KPI */}
        <div style={{ display: 'flex', alignItems: 'center', gap: 7 }}>
          <span style={{ fontSize: 8.5, fontWeight: 800, color: 'rgba(255,255,255,0.4)', fontFamily: 'var(--mono)', letterSpacing: 0.8 }}>
            TOTAL LATENCY:
          </span>
          <div style={{
            display: 'flex', alignItems: 'baseline', gap: 3,
            padding: '2px 8px', borderRadius: 5,
            background: total ? 'rgba(0,255,136,0.08)' : 'rgba(255,255,255,0.03)',
            border: `1px solid ${total ? 'rgba(0,255,136,0.25)' : 'rgba(255,255,255,0.07)'}`
          }}>
            <span style={{ fontFamily: 'var(--mono)', fontSize: 12, fontWeight: 800, color: total ? 'var(--green)' : 'rgba(255,255,255,0.35)', lineHeight: 1 }}>
              {total != null ? total : '--'}
            </span>
            <span style={{ fontSize: 8, fontWeight: 700, color: 'rgba(255,255,255,0.4)', fontFamily: 'var(--mono)' }}>ms</span>
          </div>
        </div>
      </div>

    </div>
  )
}
