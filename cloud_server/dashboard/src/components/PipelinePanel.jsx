import { useMemo } from 'react'
import { IconCpu, IconWifi, IconSatellite, IconSearch, IconTrendChart } from './Icons.jsx'

const STEPS = [
  { key: 'mic',       label: 'Mic Buffer', sub: 'I2S',       fixed: 12, icon: IconSearch },
  { key: 'mfcc',      label: 'Feature Ext',sub: 'TFLite',    fixed: 28, icon: IconCpu },
  { key: 'tx',        label: 'Transmit',   sub: 'ESP32 Wi-Fi',prop: 'kw_to_connect_ms', color: 'var(--sky)', icon: IconWifi },
  { key: 'rx',        label: 'Receive',    sub: 'Cloud WS',  prop: 'receive_gap_ms',   color: 'var(--primary)', icon: IconSatellite },
  { key: 'asr',       label: 'Inference',  sub: 'Whisper',   prop: 'transcribe_ms',    color: 'var(--pink)', icon: IconCpu },
]

function LatencyMiniChart({ events }) {
  const data = useMemo(() => {
    return events.slice(-20).map(e =>
      (e.kw_to_connect_ms ?? 0) + (e.receive_gap_ms ?? 0) + (e.transcribe_ms ?? 0)
    )
  }, [events])

  if (data.length < 2) return null

  const max = Math.max(...data, 1)
  const w = 100, h = 36
  const pts = data.map((v, i) => [
    (i / (data.length - 1)) * w,
    h - (v / max) * h * 0.85 - 4,
  ])
  const path = pts.map((p, i) => `${i === 0 ? 'M' : 'L'}${p[0]},${p[1]}`).join(' ')
  const fill = `${path} L${w},${h} L0,${h} Z`

  return (
    <svg viewBox={`0 0 ${w} ${h}`} style={{ width: '100%', height: 36, overflow: 'visible' }}>
      <defs>
        <linearGradient id="chartGrad" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stopColor="var(--primary)" stopOpacity="0.3" />
          <stop offset="100%" stopColor="var(--primary)" stopOpacity="0" />
        </linearGradient>
      </defs>
      <path d={fill} fill="url(#chartGrad)" />
      <path d={path} fill="none" stroke="var(--primary)" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
      <circle cx={pts[pts.length-1][0]} cy={pts[pts.length-1][1]} r="2.5" fill="var(--primary)" />
    </svg>
  )
}

export default function PipelinePanel({ latestEvent, events }) {
  const active = latestEvent && (Date.now() - new Date(latestEvent.timestamp).getTime() < 3500)

  const values = {
    mic:  active ? 12 : 0,
    mfcc: active ? 28 : 0,
    tx:   active ? (latestEvent?.kw_to_connect_ms ?? 0) : 0,
    rx:   active ? (latestEvent?.receive_gap_ms    ?? 0) : 0,
    asr:  active ? (latestEvent?.transcribe_ms     ?? 0) : 0,
  }
  const total = active
    ? values.mic + values.mfcc + values.tx + values.rx + values.asr
    : 0
  const totalColor = total < 400 ? 'var(--green)' : total < 800 ? 'var(--yellow)' : 'var(--red)'

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%', gap: 8 }}>

      {/* E2E Header */}
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'flex-start' }}>
        <div>
          <div style={{ fontSize: 10, color: 'var(--t2)', fontWeight: 600, letterSpacing: '1px', textTransform: 'uppercase', marginBottom: 2 }}>End-to-End Latency</div>
          <div style={{ fontSize: 11, color: 'var(--t2)' }}>Full round trip duration</div>
        </div>
        <div style={{
          fontFamily: 'var(--font-mono)', fontSize: 32, fontWeight: 800,
          color: active ? totalColor : 'var(--t2)', transition: 'color 0.4s', lineHeight: 1,
          textShadow: active ? `0 0 16px ${totalColor}44` : 'none',
        }}>
          {active ? total : '—'}
          <span style={{ fontSize: 13, fontWeight: 500, color: 'var(--t2)', marginLeft: 4 }}>ms</span>
        </div>
      </div>

      {/* Pipeline steps */}
      <div style={{ display: 'flex', flexDirection: 'column', gap: 4, flex: 1, justifyContent: 'center' }}>
        {STEPS.map((s, i) => {
          const val = values[s.key]
          const max = 500
          const pct = Math.min(100, (val / max) * 100)
          const col = s.color || 'var(--sky)'

          return (
            <div key={s.key} style={{ display: 'flex', alignItems: 'center', gap: 10, position: 'relative' }}>
              {/* Connector line (drawn behind dots) */}
              {i < STEPS.length - 1 && (
                <div style={{
                  position: 'absolute', left: 11, top: 14, width: 2, height: 20,
                  background: 'rgba(255,255,255,0.08)', zIndex: 0,
                }} />
              )}
              
              {/* Icon / Dot Container */}
              <div style={{
                width: 24, height: 24, borderRadius: '50%', flexShrink: 0,
                background: active && val > 0 ? `rgba(255,255,255,0.08)` : 'rgba(255,255,255,0.04)',
                border: `1px solid ${active && val > 0 ? 'rgba(255,255,255,0.15)' : 'rgba(255,255,255,0.02)'}`,
                display: 'flex', alignItems: 'center', justifyContent: 'center',
                zIndex: 1,
                boxShadow: active && val > 0 ? `0 0 10px ${col}33` : 'none',
                transition: 'all 0.3s',
              }}>
                <s.icon size={12} color={active && val > 0 ? col : 'var(--t2)'} />
              </div>

              {/* Text */}
              <div style={{ width: 70 }}>
                <div style={{ fontSize: 11, fontWeight: 600, color: active && val > 0 ? 'var(--t1)' : 'var(--t2)' }}>{s.label}</div>
                <div style={{ fontSize: 9, color: active && val > 0 ? 'var(--t2)' : 'var(--t3)' }}>{s.sub}</div>
              </div>

              {/* Progress Bar */}
              <div style={{ flex: 1, height: 5, background: 'rgba(255,255,255,0.08)', borderRadius: 4, overflow: 'hidden' }}>
                <div style={{
                  height: '100%', width: `${active ? pct : 0}%`,
                  background: col, borderRadius: 4, transition: 'width 0.5s ease',
                  boxShadow: active ? `0 0 8px ${col}99` : 'none',
                }} />
              </div>

              {/* Metric Text */}
              <div style={{
                width: 44, textAlign: 'right',
                fontFamily: 'var(--font-mono)', fontSize: 12, fontWeight: 700,
                color: active && val > 0 ? col : 'var(--t2)',
                transition: 'color 0.3s',
              }}>
                {active && val > 0 ? `${val}ms` : '—'}
              </div>
            </div>
          )
        })}
      </div>

      {/* Mini sparkline chart */}
      {events.length > 1 && (
        <div style={{
          marginTop: 'auto',
          background: 'rgba(255,255,255,0.03)',
          border: '1px solid rgba(255,255,255,0.08)',
          borderRadius: 8, padding: '8px 10px',
        }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginBottom: 8 }}>
            <IconTrendChart size={12} color="var(--t2)" />
            <div style={{ fontSize: 9, color: 'var(--t2)', letterSpacing: '1px', textTransform: 'uppercase', fontWeight: 600 }}>
              Trend (Last {Math.min(events.length, 20)})
            </div>
          </div>
          <LatencyMiniChart events={events} />
        </div>
      )}

    </div>
  )
}
