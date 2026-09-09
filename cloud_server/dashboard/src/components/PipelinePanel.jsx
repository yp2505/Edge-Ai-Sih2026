import { useMemo } from 'react'
import { IconCpu, IconWifi, IconSatellite, IconTrendChart } from './Icons.jsx'

const STEPS = [
  { key: 'tx',  label: 'Wake to Server', sub: 'ESP32 → Wi-Fi', color: 'var(--cyan)', icon: IconWifi },
  { key: 'rx',  label: 'First Audio Byte', sub: 'Server receive',  color: 'var(--ice)',  icon: IconSatellite },
  { key: 'asr', label: 'Whisper ASR',     sub: 'Server CPU',       color: 'var(--teal)', icon: IconCpu },
]

function Sparkline({ events }) {
  const data = useMemo(() =>
    events.slice(-24).map(e =>
      (e.kw_to_connect_ms ?? 0) + (e.receive_gap_ms ?? 0) + (e.transcribe_ms ?? 0)
    )
  , [events])

  if (data.length < 2) return null

  const max = Math.max(...data, 1)
  const W = 100, H = 34
  const pts = data.map((v, i) => [
    (i / (data.length - 1)) * W,
    H - (v / max) * H * 0.85 - 3,
  ])
  const path = pts.map((p, i) => `${i === 0 ? 'M' : 'L'}${p[0].toFixed(1)},${p[1].toFixed(1)}`).join(' ')
  const area = `${path} L${W},${H} L0,${H} Z`

  return (
    <svg viewBox={`0 0 ${W} ${H}`} style={{ width: '100%', height: 34, overflow: 'visible' }}>
      <defs>
        <linearGradient id="sparkGrad" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stopColor="var(--cyan)" stopOpacity="0.25" />
          <stop offset="100%" stopColor="var(--cyan)" stopOpacity="0" />
        </linearGradient>
      </defs>
      <path d={area} fill="url(#sparkGrad)" />
      <path d={path} fill="none" stroke="var(--cyan)" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
      <circle cx={pts[pts.length - 1][0]} cy={pts[pts.length - 1][1]} r="2.5" fill="var(--cyan)" />
    </svg>
  )
}

export default function PipelinePanel({ latestEvent, events }) {
  const active = latestEvent && (Date.now() - new Date(latestEvent.timestamp).getTime() < 4000)

  const vals = {
    tx:  active ? (latestEvent?.kw_to_connect_ms ?? 0) : 0,
    rx:  active ? (latestEvent?.receive_gap_ms    ?? 0) : 0,
    asr: active ? (latestEvent?.transcribe_ms     ?? 0) : 0,
  }
  const total = active ? (latestEvent?.end_to_end_ms ?? vals.tx + vals.rx + vals.asr) : 0
  const totalColor = total > 0 ? (total < 400 ? 'var(--green)' : total < 800 ? 'var(--amber)' : 'var(--red)') : 'var(--t4)'

  return (
    <>
      {/* E2E Header */}
      <div className="pipeline-e2e">
        <div>
          <div className="pipeline-e2e-label">End-to-End Latency</div>
          <div className="pipeline-e2e-sub">Wake → Wi-Fi → Audio → Whisper</div>
        </div>
        <div className="pipeline-e2e-value" style={{
          color: active ? totalColor : 'var(--t4)',
          textShadow: active ? `0 0 20px ${totalColor}55` : 'none',
        }}>
          {active ? total : '—'}
          <span className="pipeline-e2e-unit">ms</span>
        </div>
      </div>

      {/* Steps */}
      <div className="pipeline-steps">
        {STEPS.map((s) => {
          const val = vals[s.key]
          const pct = Math.min(100, (val / 500) * 100)
          const isActive = active && val > 0

          return (
            <div key={s.key} className="pipeline-step">
              <div className={`pipeline-step-icon ${isActive ? 'active' : ''}`}
                style={{ borderColor: isActive ? `${s.color}40` : undefined,
                         boxShadow: isActive ? `0 0 10px ${s.color}30` : undefined }}>
                <s.icon size={12} color={isActive ? s.color : 'var(--t3)'} />
              </div>
              <div className="pipeline-step-text">
                <div className={`pipeline-step-name ${isActive ? 'active' : ''}`}>{s.label}</div>
                <div className={`pipeline-step-sub ${isActive ? 'active' : ''}`}>{s.sub}</div>
              </div>
              <div className="pipeline-step-bar">
                <div className="pipeline-step-fill" style={{
                  width: `${active ? pct : 0}%`,
                  background: s.color,
                  boxShadow: isActive ? `0 0 8px ${s.color}80` : 'none',
                }} />
              </div>
              <div className={`pipeline-step-val ${isActive ? 'active' : ''}`}
                style={{ color: isActive ? s.color : undefined }}>
                {isActive ? `${val}ms` : '—'}
              </div>
            </div>
          )
        })}
      </div>

      {/* Sparkline */}
      {events.length > 1 && (
        <div className="pipeline-chart">
          <div className="pipeline-chart-label">
            <IconTrendChart size={10} color="var(--t3)" />
            Trend · Last {Math.min(events.length, 24)} detections
          </div>
          <Sparkline events={events} />
        </div>
      )}
    </>
  )
}
