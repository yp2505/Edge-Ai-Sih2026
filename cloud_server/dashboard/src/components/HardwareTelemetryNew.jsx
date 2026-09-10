import { IconCpu, IconFlash, IconWifi, IconWarning } from './Icons.jsx'

function RadialGauge({ label, value, color, icon }) {
  const known = Number.isFinite(value)
  const pct = known ? Math.max(0, Math.min(100, value)) : 0
  const r = 28
  const circ = 2 * Math.PI * r
  const dash = circ * (pct / 100)

  return (
    <div className="hw-gauge">
      <div style={{ position: 'relative', width: 68, height: 68 }}>
        <svg width="68" height="68" viewBox="0 0 68 68" style={{ transform: 'rotate(-90deg)' }}>
          <circle cx="34" cy="34" r={r} fill="none" stroke="rgba(255,255,255,0.04)" strokeWidth="6" />
          <circle
            cx="34" cy="34" r={r}
            fill="none" stroke={color} strokeWidth="6" strokeLinecap="round"
            strokeDasharray={`${dash} ${circ - dash}`}
            style={{ transition: 'stroke-dasharray 0.6s ease', filter: known ? `drop-shadow(0 0 4px ${color})` : 'none' }}
          />
        </svg>
        <div style={{ position: 'absolute', inset: 0, display: 'flex', alignItems: 'center', justifyContent: 'center', flexDirection: 'column' }}>
          <span style={{ fontSize: known ? 13 : 10, fontWeight: 800, fontFamily: 'var(--font-mono)', color: known ? color : 'var(--t4)' }}>
            {known ? `${value.toFixed(0)}%` : '—'}
          </span>
        </div>
      </div>
      <div className="hw-gauge-label">
        {icon}<span>{label}</span>
      </div>
    </div>
  )
}

export default function HardwareTelemetryNew({ telemetry }) {
  const connected = telemetry && telemetry.received_at && Date.now() - new Date(telemetry.received_at).getTime() < 3500
  const heapUsed = connected ? telemetry.heap_total_bytes - telemetry.free_heap_bytes : NaN
  const heapPct = connected && telemetry.heap_total_bytes ? heapUsed * 100 / telemetry.heap_total_bytes : NaN
  const duty = connected ? telemetry.inference_duty_pct : NaN
  const confidence = connected ? telemetry.keyword_confidence * 100 : NaN

  const micRms = connected && telemetry.mic_rms != null ? telemetry.mic_rms : null
  const rmsColor = micRms == null ? 'var(--t4)'
    : micRms >= 0.05 ? 'var(--green)'
    : micRms >= 0.01 ? 'var(--amber)'
    : 'var(--red)'
  const rmsLabel = micRms == null ? '—'
    : micRms >= 0.05 ? `${micRms.toFixed(4)} 🎙`
    : micRms >= 0.01 ? `${micRms.toFixed(4)} ~`
    : `${micRms.toFixed(4)} 🔇`
  const rmsBarPct = micRms != null ? Math.min(100, (micRms / 0.15) * 100) : 0

  const heapText = connected ? `${(heapUsed / 1024).toFixed(1)} KB` : 'Waiting...'

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 10, height: '100%' }}>
      {/* Gauges */}
      <div className="hw-gauges">
        <RadialGauge label="DUTY" value={duty} color="var(--cyan)" icon={<IconCpu size={9} color="var(--t3)" />} />
        <RadialGauge label="HEAP" value={heapPct} color="var(--ice)" icon={<IconCpu size={9} color="var(--t3)" />} />
        <RadialGauge label="CONF." value={confidence} color="var(--teal)" icon={<IconFlash size={9} color="var(--t3)" />} />
      </div>

      {/* MIC RMS */}
      <div className="hw-rms">
        <div className="hw-row">
          <span className="hw-row-label">🎤 Mic RMS</span>
          <span className="hw-row-val" style={{ color: rmsColor }}>{rmsLabel}</span>
        </div>
        <div className="hw-rms-bar">
          <div className="hw-rms-fill" style={{
            width: `${rmsBarPct}%`,
            background: rmsColor,
            boxShadow: micRms >= 0.05 ? `0 0 8px ${rmsColor}` : 'none',
          }} />
        </div>
        <div className="hw-rms-scale">
          <span>0.00</span>
          <span style={{ color: micRms >= 0.05 ? 'var(--green)' : 'var(--t4)' }}>speech ≥0.05</span>
          <span>0.15</span>
        </div>
      </div>

      {/* Row data */}
      <div className="hw-rows">
        {[
          { label: 'RAM', value: heapText, color: connected ? 'var(--ice)' : 'var(--t4)', icon: <IconCpu size={9} color="var(--t3)" /> },
          { label: 'TFLITE', value: connected ? `${(telemetry.tflite_arena_bytes / 1024).toFixed(1)} KB` : '—', color: 'var(--t2)', icon: <IconCpu size={9} color="var(--t3)" /> },
          { label: 'BUFFERS', value: connected ? `${(telemetry.audio_buffer_bytes / 1024).toFixed(1)} KB` : '—', color: 'var(--t2)', icon: <IconCpu size={9} color="var(--t3)" /> },
          { label: 'Wi-Fi RSSI', value: connected ? `${telemetry.wifi_rssi_dbm} dBm` : '—', color: 'var(--green)', icon: <IconWifi size={9} color="var(--t3)" /> },
        ].map(row => (
          <div key={row.label} className="hw-row">
            <span className="hw-row-label">{row.icon}{row.label}</span>
            <span className="hw-row-val" style={{ color: row.color }}>{row.value}</span>
          </div>
        ))}
      </div>
    </div>
  )
}