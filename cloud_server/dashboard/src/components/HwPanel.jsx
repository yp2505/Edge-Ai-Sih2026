import { useState, useEffect } from 'react'

function Gauge({ label, value, color, unit = '%' }) {
  const ok = Number.isFinite(value)
  const pct = ok ? Math.max(0, Math.min(100, value)) : 0
  const r = 26, circ = 2 * Math.PI * r, dash = circ * (pct / 100)
  const strokeColor = ok ? color : 'rgba(255,255,255,0.06)'
  const valText = ok ? `${value.toFixed(0)}${unit}` : (label === 'TEMP' ? '--' : 'OFFLINE')
  return (
    <div className="hw-g" style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 6 }}>
      <div style={{ position: 'relative', width: 64, height: 64 }}>
        <svg width="64" height="64" viewBox="0 0 64 64" style={{ transform: 'rotate(-90deg)' }}>
          <circle cx="32" cy="32" r={r} fill="none" stroke="rgba(255,255,255,0.02)" strokeWidth="4" />
          <circle cx="32" cy="32" r={r} fill="none" stroke={strokeColor} strokeWidth="4" strokeLinecap="round"
            strokeDasharray={`${dash} ${circ - dash}`}
            style={{ transition: 'stroke-dasharray 0.6s ease', filter: ok ? `drop-shadow(0 0 4px ${color})` : 'none' }} />
        </svg>
        <div style={{ position: 'absolute', inset: 0, display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
          <span style={{ fontSize: ok ? 13 : 8, fontWeight: 800, fontFamily: 'var(--mono)', color: ok ? color : 'var(--t4)', letterSpacing: ok ? 0 : 1 }}>
            {valText}
          </span>
        </div>
      </div>
      <div style={{ fontSize: 10, fontWeight: 800, color: 'var(--t3)', letterSpacing: 1.5 }}>{label}</div>
    </div>
  )
}

export default function HwPanel({ telemetry, stale, serverUp }) {
  const conn = serverUp && telemetry && !stale
  
  const cpu = conn && telemetry.cpu != null ? telemetry.cpu : NaN
  const npu = conn && telemetry.npu != null ? telemetry.npu : NaN
  const temp = conn && telemetry.temp != null ? telemetry.temp : NaN
  const vcore = conn && telemetry.vcore != null ? `${telemetry.vcore.toFixed(2)} V` : 'N/A'
  const latency = conn && telemetry.latency_ms != null ? `${telemetry.latency_ms} ms` : 'N/A'
  const pktLoss = conn && telemetry.pkt_loss != null ? `${telemetry.pkt_loss} %` : 'N/A'
  const rssi = conn && telemetry.wifi_rssi_dbm != null ? `${telemetry.wifi_rssi_dbm} dBm` : 'N/A'
  const micStr = conn && telemetry.mic_rms != null ? (telemetry.mic_rms > 0 ? `${(20 * Math.log10(telemetry.mic_rms)).toFixed(1)} dB` : '-inf dB') : 'N/A'

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 20, padding: '16px 20px', height: '100%' }}>
      
      {/* Gauges */}
      <div style={{ display: 'flex', justifyContent: 'space-between', padding: '0 10px' }}>
        <Gauge label="CPU" value={cpu} color="var(--c1)" />
        <Gauge label="NPU" value={npu} color="var(--c3)" />
        <Gauge label="TEMP" value={temp} color="var(--amber)" unit="°" />
      </div>

      <div style={{ height: 1, background: 'rgba(255,255,255,0.06)', margin: '4px 0' }} />

      {/* Metrics List */}
      <div style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
        {[
          { l: 'MIC RMS',    v: micStr,  col: conn && telemetry.mic_rms != null ? 'var(--c1)' : 'var(--t4)' },
          { l: 'CORE VOLT',  v: vcore,   col: conn && telemetry.vcore != null ? 'var(--c3)' : 'var(--t4)' },
          { l: 'LATENCY',    v: latency, col: conn && telemetry.latency_ms != null ? 'var(--c1)' : 'var(--t4)' },
          { l: 'PKT LOSS',   v: pktLoss, col: conn && telemetry.pkt_loss != null ? 'var(--green)' : 'var(--t4)' },
          { l: 'WIFI RSSI',  v: rssi,    col: conn && telemetry.wifi_rssi_dbm != null ? 'var(--green)' : 'var(--t4)' },
        ].map(row => (
          <div key={row.l} style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
            <span style={{ fontSize: 10, fontWeight: 700, color: 'var(--t3)', letterSpacing: 1.5 }}>{row.l}</span>
            <span style={{ fontSize: 13, fontWeight: 600, color: row.col, fontFamily: 'var(--mono)', textAlign: 'right', minWidth: 80 }}>{row.v}</span>
          </div>
        ))}
      </div>
    </div>
  )
}

