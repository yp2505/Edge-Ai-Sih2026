import { useState, useEffect } from 'react'
import { IconUsb } from './Icons.jsx'

function Gauge({ label, value, color, unit = '%' }) {
  const ok = Number.isFinite(value)
  const pct = ok ? Math.max(0, Math.min(100, value)) : 0
  const r = 26, circ = 2 * Math.PI * r, dash = circ * (pct / 100)
  const strokeColor = ok ? color : 'rgba(255,255,255,0.08)'
  const valText = ok ? `${value.toFixed(0)}${unit}` : '--'
  return (
    <div className="hw-g" style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 5 }}>
      <div style={{ position: 'relative', width: 62, height: 62 }}>
        <svg width="62" height="62" viewBox="0 0 64 64" style={{ transform: 'rotate(-90deg)' }}>
          <circle className="hw-g-track" cx="32" cy="32" r={r} fill="none" stroke="rgba(255,255,255,0.05)" strokeWidth="4" />
          <circle cx="32" cy="32" r={r} fill="none" stroke={strokeColor} strokeWidth="4" strokeLinecap="round"
            strokeDasharray={`${dash} ${circ - dash}`}
            style={{ transition: 'stroke-dasharray 0.6s ease', filter: ok ? `drop-shadow(0 0 5px ${color})` : 'none' }} />
        </svg>
        <div style={{ position: 'absolute', inset: 0, display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
          <span style={{ fontSize: ok ? 13 : 10, fontWeight: 800, fontFamily: 'var(--mono)', color: ok ? color : 'rgba(255,255,255,0.3)', letterSpacing: ok ? 0 : 0.5 }}>
            {valText}
          </span>
        </div>
      </div>
      <div style={{ fontSize: 9.5, fontWeight: 800, color: ok ? 'var(--t2)' : 'rgba(255,255,255,0.4)', letterSpacing: 1.2 }}>{label}</div>
    </div>
  )
}

export default function HwPanel({ telemetry, stale, serverUp, health, deviceInfo }) {
  const isPlugged = Boolean(deviceInfo && deviceInfo.connected)
  const conn = serverUp && (isPlugged || (telemetry && !stale))

  const cpu = conn && telemetry?.cpu != null ? telemetry.cpu : NaN
  const rssi = conn && telemetry?.wifi_rssi_dbm != null ? `${telemetry.wifi_rssi_dbm} dBm` : (isPlugged ? 'Direct USB' : '--')
  const micRms = conn && telemetry?.mic_rms != null ? telemetry.mic_rms : 0
  const micDbStr = micRms > 0 ? `${(20 * Math.log10(micRms)).toFixed(1)} dB` : '-inf dB'
  const micBarPct = Math.min(100, Math.max(0, (micRms / 0.04) * 100))
  
  const kwConf = conn && telemetry?.keyword_confidence != null ? telemetry.keyword_confidence : 0
  const kwConfPct = (kwConf * 100).toFixed(0)
  const isKwHigh = kwConf >= 0.70
  
  const ram = serverUp && health?.server_ram_pct != null ? health.server_ram_pct : NaN
  const sourceZone = conn && telemetry?.source_zone ? telemetry.source_zone.replaceAll('_', ' ') : 'UNKNOWN'
  const sourceKnown = sourceZone !== 'UNKNOWN'

  return (
    <div className="hw-panel-content" style={{ display: 'flex', flexDirection: 'column', width: '100%', height: '100%', justifyContent: 'space-between', padding: '10px 14px' }}>
      
      {/* ── USB Cable Hardware Status Card ── */}
      <div style={{
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'space-between',
        padding: '7px 11px',
        background: isPlugged ? 'rgba(0, 255, 136, 0.07)' : 'rgba(255, 255, 255, 0.02)',
        border: `1px solid ${isPlugged ? 'rgba(0, 255, 136, 0.28)' : 'rgba(255, 255, 255, 0.07)'}`,
        borderRadius: 8,
        boxShadow: isPlugged ? '0 0 10px rgba(0, 255, 136, 0.12)' : 'none',
      }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 9 }}>
          <div style={{
            width: 26,
            height: 26,
            borderRadius: 6,
            background: isPlugged ? 'rgba(0, 255, 136, 0.12)' : 'rgba(255, 255, 255, 0.04)',
            border: `1px solid ${isPlugged ? 'rgba(0, 255, 136, 0.32)' : 'rgba(255, 255, 255, 0.09)'}`,
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'center',
            flexShrink: 0
          }}>
            <IconUsb size={15} color={isPlugged ? 'var(--green)' : 'rgba(255, 255, 255, 0.45)'} />
          </div>
          <div style={{ display: 'flex', flexDirection: 'column' }}>
            <span style={{ fontSize: 8.5, fontWeight: 800, color: 'var(--t4)', letterSpacing: 1 }}>USB HARDWARE</span>
            <span style={{ fontSize: 11, fontWeight: 700, color: isPlugged ? 'var(--green)' : 'rgba(255, 255, 255, 0.45)', fontFamily: 'var(--mono)' }}>
              {isPlugged ? `${deviceInfo?.port || 'Device'} • CONNECTED` : 'DISCONNECTED'}
            </span>
          </div>
        </div>
        <div style={{
          width: 7,
          height: 7,
          borderRadius: '50%',
          background: isPlugged ? 'var(--green)' : 'rgba(255, 255, 255, 0.25)',
          boxShadow: isPlugged ? '0 0 6px var(--green)' : 'none',
          animation: isPlugged ? 'pulse 2s infinite' : 'none'
        }} />
      </div>

      {/* ── Real-Time Voice Activity / Mic Meter ── */}
      <div style={{ display: 'flex', flexDirection: 'column', gap: 4 }}>
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
          <span style={{ fontSize: 8.5, fontWeight: 800, color: 'var(--t3)', letterSpacing: 1 }}>MIC AUDIO</span>
          <span style={{ fontSize: 10.5, fontWeight: 700, color: micRms > 0.005 ? 'var(--c1)' : 'rgba(255,255,255,0.4)', fontFamily: 'var(--mono)' }}>
            {conn ? micDbStr : 'OFFLINE'}
          </span>
        </div>
        <div style={{ width: '100%', height: 5, background: 'rgba(255,255,255,0.06)', borderRadius: 3, overflow: 'hidden' }}>
          <div style={{
            width: `${micBarPct}%`,
            height: '100%',
            background: micRms > 0.02 ? 'linear-gradient(90deg, var(--c1), var(--green))' : 'var(--c1)',
            boxShadow: micRms > 0.01 ? '0 0 6px var(--c1)' : 'none',
            borderRadius: 3,
            transition: 'width 0.15s ease-out'
          }} />
        </div>
      </div>

      {/* ── Real-Time Keyword Confidence Meter ── */}
      <div style={{ display: 'flex', flexDirection: 'column', gap: 4 }}>
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
          <span style={{ fontSize: 8.5, fontWeight: 800, color: 'var(--t3)', letterSpacing: 1 }}>WAKE CONFIDENCE</span>
          <span style={{ fontSize: 10.5, fontWeight: 700, color: isKwHigh ? 'var(--green)' : 'rgba(255,255,255,0.4)', fontFamily: 'var(--mono)' }}>
            {conn ? `${kwConfPct}% ${isKwHigh ? '⚡ WAKE' : ''}` : '0%'}
          </span>
        </div>
        <div style={{ position: 'relative', width: '100%', height: 5, background: 'rgba(255,255,255,0.06)', borderRadius: 3, overflow: 'hidden' }}>
          <div style={{
            width: `${kwConf * 100}%`,
            height: '100%',
            background: isKwHigh ? 'linear-gradient(90deg, var(--c1), var(--green))' : 'var(--c3)',
            boxShadow: isKwHigh ? '0 0 8px var(--green)' : 'none',
            borderRadius: 3,
            transition: 'width 0.2s ease-out'
          }} />
        </div>
      </div>

      {/* Gauges (ESP32 CPU & SERVER RAM) */}
      <div className="hw-gauges" style={{ display: 'flex', justifyContent: 'space-around', alignItems: 'center', padding: '2px 0' }}>
        <Gauge label="ESP32 CPU" value={cpu} color="var(--c1)" />
        <Gauge label="SERVER RAM" value={ram} color="var(--c3)" />
      </div>

      {/* ── Clean 2-Column Status Tiles (Fits perfectly without overflow) ── */}
      <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 8 }}>
        <div style={{
          padding: '6px 10px',
          background: 'rgba(255,255,255,0.02)',
          border: '1px solid rgba(255,255,255,0.06)',
          borderRadius: 6,
          display: 'flex',
          flexDirection: 'column',
          gap: 2
        }}>
          <span style={{ fontSize: 8, fontWeight: 800, color: 'var(--t4)', letterSpacing: 0.8 }}>DEVICE LINK</span>
          <span style={{ fontSize: 11, fontWeight: 700, color: isPlugged ? 'var(--green)' : conn ? 'var(--c1)' : 'rgba(255,255,255,0.4)', fontFamily: 'var(--mono)', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
            {isPlugged ? 'USB Cable' : conn ? 'Wi-Fi Link' : 'Offline'}
          </span>
        </div>

        <div style={{
          padding: '6px 10px',
          background: 'rgba(255,255,255,0.02)',
          border: '1px solid rgba(255,255,255,0.06)',
          borderRadius: 6,
          display: 'flex',
          flexDirection: 'column',
          gap: 2
        }}>
          <span style={{ fontSize: 8, fontWeight: 800, color: 'var(--t4)', letterSpacing: 0.8 }}>WIFI RSSI</span>
          <span style={{ fontSize: 11, fontWeight: 700, color: conn && telemetry?.wifi_rssi_dbm != null ? 'var(--green)' : 'rgba(255,255,255,0.4)', fontFamily: 'var(--mono)' }}>
            {rssi}
          </span>
        </div>

        <div style={{
          padding: '6px 10px',
          background: sourceKnown ? 'rgba(0, 229, 255, 0.05)' : 'rgba(255,255,255,0.02)',
          border: `1px solid ${sourceKnown ? 'rgba(0, 229, 255, 0.20)' : 'rgba(255,255,255,0.06)'}`,
          borderRadius: 6,
          display: 'flex',
          flexDirection: 'column',
          gap: 2
        }}>
          <span style={{ fontSize: 8, fontWeight: 800, color: 'var(--t4)', letterSpacing: 0.8 }}>SOURCE ZONE</span>
          <span style={{ fontSize: 11, fontWeight: 700, color: sourceKnown ? 'var(--c1)' : 'rgba(255,255,255,0.4)', fontFamily: 'var(--mono)', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
            {sourceZone}
          </span>
        </div>
      </div>

    </div>
  )
}
