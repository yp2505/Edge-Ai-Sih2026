import { IconCpu, IconFlash, IconWifi, IconWarning } from './Icons.jsx'

function Gauge({ label, value, color, icon }) {
  const known = Number.isFinite(value)
  const pct = known ? Math.max(0, Math.min(100, value)) : 0
  return (
    <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 6 }}>
      <div style={{ position: 'relative', width: 74, height: 74 }}>
        <svg width="74" height="74" viewBox="0 0 74 74" style={{ transform: 'rotate(-90deg)' }}>
          <circle cx="37" cy="37" r="30" fill="none" stroke="rgba(255,255,255,0.05)" strokeWidth="6.5" />
          <circle cx="37" cy="37" r="30" fill="none" stroke={color} strokeWidth="6.5" strokeLinecap="round"
            strokeDasharray="188.5" strokeDashoffset={188.5 * (1 - pct / 100)} />
        </svg>
        <div style={{ position: 'absolute', inset: 0, display: 'grid', placeItems: 'center' }}>
          <span style={{ fontSize: known ? 16 : 12, fontWeight: 700, fontFamily: 'var(--font-mono)' }}>{known ? `${value.toFixed(1)}%` : '—'}</span>
        </div>
      </div>
      <div style={{ display: 'flex', alignItems: 'center', gap: 4 }}>{icon}<span style={{ fontSize: 9, color: 'var(--t3)', fontWeight: 700 }}>{label}</span></div>
    </div>
  )
}

function Row({ label, value, note, color = 'var(--t2)', icon }) {
  return <div style={{ display: 'flex', justifyContent: 'space-between', gap: 8, alignItems: 'center' }}>
    <span style={{ display: 'flex', gap: 5, alignItems: 'center', fontSize: 10, color: 'var(--t3)', fontWeight: 700 }}>{icon}{label}</span>
    <span title={note} style={{ fontFamily: 'var(--font-mono)', fontSize: 11, fontWeight: 700, color }}>{value}</span>
  </div>
}

export default function HardwareTelemetryNew({ telemetry }) {
  const connected = telemetry && telemetry.received_at && Date.now() - new Date(telemetry.received_at).getTime() < 3500
  const heapUsed = connected ? telemetry.heap_total_bytes - telemetry.free_heap_bytes : NaN
  const heapPct = connected && telemetry.heap_total_bytes ? heapUsed * 100 / telemetry.heap_total_bytes : NaN
  const duty = connected ? telemetry.inference_duty_pct : NaN
  const confidence = connected ? telemetry.keyword_confidence * 100 : NaN
  const heapText = connected ? `${(heapUsed / 1024).toFixed(1)} KB heap used` : 'Waiting for ESP32 telemetry'

  // Live mic RMS from ESP32 telemetry (0.0 – 1.0 normalised)
  const micRms = connected && telemetry.mic_rms != null ? telemetry.mic_rms : null
  const rmsColor = micRms == null
    ? 'var(--t3)'
    : micRms >= 0.05 ? 'var(--green)'   // speech-level signal
    : micRms >= 0.01 ? 'var(--amber)'   // weak / background noise
    : 'var(--red)'                       // near-silent / likely not picking up
  const rmsLabel = micRms == null
    ? '—'
    : `${micRms.toFixed(5)}${micRms >= 0.05 ? ' 🎙' : micRms >= 0.01 ? ' 〰' : ' 🔇'}`

  // Visual bar: scale 0..0.15 → 0..100% (typical speech is 0.05-0.12)
  const rmsBarPct = micRms != null ? Math.min(100, (micRms / 0.15) * 100) : 0

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 11, height: '100%', justifyContent: 'center' }}>
      <div style={{ display: 'flex', justifyContent: 'space-around' }}>
        <Gauge label="INFERENCE DUTY" value={duty} color="var(--primary)" icon={<IconCpu size={10} color="var(--t3)" />} />
        <Gauge label="HEAP USED" value={heapPct} color="var(--sky)" icon={<IconCpu size={10} color="var(--t3)" />} />
        <Gauge label="WAKE CONF." value={confidence} color="var(--pink)" icon={<IconFlash size={10} color="var(--t3)" />} />
      </div>
      <div style={{ height: 1, background: 'var(--border)' }} />

      {/* ── Live Mic RMS ─────────────────────────────────────────────────── */}
      <div style={{ display: 'flex', flexDirection: 'column', gap: 4 }}>
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
          <span style={{ fontSize: 10, color: 'var(--t3)', fontWeight: 700 }}>🎤 MIC RMS</span>
          <span style={{ fontFamily: 'var(--font-mono)', fontSize: 11, fontWeight: 700, color: rmsColor }}>
            {rmsLabel}
          </span>
        </div>
        {/* Progress bar */}
        <div style={{ height: 5, borderRadius: 4, background: 'rgba(255,255,255,0.06)', overflow: 'hidden' }}>
          <div style={{
            height: '100%',
            width: `${rmsBarPct}%`,
            background: rmsColor,
            borderRadius: 4,
            transition: 'width 0.25s ease, background 0.3s',
            boxShadow: micRms >= 0.05 ? `0 0 8px ${rmsColor}` : 'none',
          }} />
        </div>
        <div style={{ fontSize: 9, color: 'var(--t3)', display: 'flex', justifyContent: 'space-between' }}>
          <span>0.00</span>
          <span style={{ color: micRms >= 0.05 ? 'var(--green)' : 'var(--t3)' }}>speech ≥ 0.05</span>
          <span>0.15</span>
        </div>
      </div>

      <div style={{ height: 1, background: 'var(--border)' }} />
      <Row label="RAM" value={heapText} color={connected ? 'var(--sky)' : 'var(--t3)'} icon={<IconCpu size={10} color="var(--t3)" />} />
      <Row label="TFLITE ARENA" value={connected ? `${(telemetry.tflite_arena_bytes / 1024).toFixed(1)} KB` : '—'} icon={<IconCpu size={10} color="var(--t3)" />} />
      <Row label="AUDIO BUFFERS" value={connected ? `${(telemetry.audio_buffer_bytes / 1024).toFixed(1)} KB` : '—'} icon={<IconCpu size={10} color="var(--t3)" />} />
      <Row label="WI-FI RSSI" value={connected ? `${telemetry.wifi_rssi_dbm} dBm` : '—'} color="var(--green)" icon={<IconWifi size={10} color="var(--t3)" />} />
      <Row label="POWER / TEMP" value="Sensor required" note="Add INA219 for actual power and a temperature sensor for temperature." color="var(--amber)" icon={<IconWarning size={10} color="var(--t3)" />} />
    </div>
  )
}