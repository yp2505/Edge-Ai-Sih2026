import { useState, useEffect } from 'react'
import { IconCpu, IconFlash, IconWarning } from './Icons.jsx'

function RingGauge({ label, value, max = 100, color, icon }) {
  const r = 30
  const circ = 2 * Math.PI * r
  const pct = Math.min(100, Math.max(0, (value / max) * 100))
  const offset = circ * (1 - pct / 100)

  return (
    <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 6 }}>
      <div style={{ position: 'relative', width: 74, height: 74 }}>
        <svg width="74" height="74" viewBox="0 0 74 74" style={{ transform: 'rotate(-90deg)' }}>
          {/* Track */}
          <circle cx="37" cy="37" r={r} fill="none" stroke="rgba(255,255,255,0.05)" strokeWidth="6.5" />
          {/* Fill */}
          <circle
            cx="37" cy="37" r={r} fill="none"
            stroke={color}
            strokeWidth="6.5"
            strokeLinecap="round"
            strokeDasharray={circ}
            strokeDashoffset={offset}
            style={{ transition: 'stroke-dashoffset 0.45s ease', filter: `drop-shadow(0 0 5px ${color}55)` }}
          />
        </svg>
        {/* Center */}
        <div style={{
          position: 'absolute', inset: 0,
          display: 'flex', flexDirection: 'column',
          alignItems: 'center', justifyContent: 'center',
        }}>
          <span style={{ fontSize: 17, fontWeight: 700, fontFamily: 'var(--font-mono)', color: 'var(--t1)', lineHeight: 1 }}>
            {value.toFixed(0)}
          </span>
          <span style={{ fontSize: 8, color: 'var(--t3)' }}>%</span>
        </div>
      </div>
      <div style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
        {icon}
        <span style={{ fontSize: 10, color: 'var(--t3)', fontWeight: 600, letterSpacing: '0.8px', textTransform: 'uppercase' }}>{label}</span>
      </div>
    </div>
  )
}

function StatBar({ label, value, max, color, unit, icon, warn }) {
  const pct = Math.min(100, (value / max) * 100)
  const isWarn = warn && value > warn

  return (
    <div>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 5 }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 5 }}>
          {icon}
          <span style={{ fontSize: 10, color: 'var(--t3)', fontWeight: 600, letterSpacing: '0.8px', textTransform: 'uppercase' }}>{label}</span>
          {isWarn && <IconWarning size={11} color="var(--red)" />}
        </div>
        <span style={{ fontSize: 12, fontFamily: 'var(--font-mono)', fontWeight: 700, color: isWarn ? 'var(--red)' : color }}>
          {typeof value === 'number' ? value.toFixed(1) : value}{unit}
        </span>
      </div>
      <div style={{ height: 5, background: 'rgba(255,255,255,0.05)', borderRadius: 4, overflow: 'hidden' }}>
        <div style={{
          height: '100%',
          width: `${pct}%`,
          background: isWarn ? 'var(--red)' : color,
          borderRadius: 4,
          transition: 'width 0.45s ease',
          boxShadow: `0 0 8px ${isWarn ? 'var(--red)' : color}66`,
        }} />
      </div>
    </div>
  )
}

export default function HardwareTelemetryNew({ latestEvent }) {
  const [load, setLoad] = useState({ cpu: 12, npu: 8, vram: 42, temp: 42.4, power: 0.4 })

  useEffect(() => {
    const int = setInterval(() => {
      setLoad(prev => {
        const active = latestEvent && (Date.now() - new Date(latestEvent.timestamp).getTime() < 3000)
        return {
          cpu:   prev.cpu   + ((active ? 78+Math.random()*18 : 10+Math.random()*8)  - prev.cpu)  * 0.18,
          npu:   prev.npu   + ((active ? 85+Math.random()*12 : 5+Math.random()*7)   - prev.npu)  * 0.18,
          vram:  prev.vram  + ((active ? 70 : 38) - prev.vram)  * 0.1,
          temp:  prev.temp  + ((active ? 49+Math.random()*2 : 41+Math.random()) - prev.temp) * 0.05,
          power: prev.power + ((active ? 1.3 : 0.38) - prev.power) * 0.1,
        }
      })
    }, 180)
    return () => clearInterval(int)
  }, [latestEvent])

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 14, height: '100%', justifyContent: 'center' }}>
      {/* Gauge row */}
      <div style={{ display: 'flex', justifyContent: 'space-around', alignItems: 'center' }}>
        <RingGauge label="CPU" value={load.cpu} color="var(--primary)" icon={<IconCpu size={10} color="var(--t3)" />} />
        <RingGauge label="NPU" value={load.npu} color="var(--pink)" icon={<IconFlash size={10} color="var(--t3)" />} />
        <RingGauge label="VRAM" value={load.vram} color="var(--sky)" icon={<IconCpu size={10} color="var(--t3)" />} />
      </div>

      {/* Divider */}
      <div style={{ height: 1, background: 'var(--border)', margin: '0 0' }} />

      {/* Bar stats */}
      <div style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
        <StatBar
          label="Temperature" value={load.temp} max={80} unit="°C"
          color="var(--amber)" warn={47}
          icon={<IconWarning size={10} color="var(--t3)" />}
        />
        <StatBar
          label="Power Draw" value={load.power} max={2} unit="W"
          color="var(--green)"
          icon={<IconFlash size={10} color="var(--t3)" />}
        />
      </div>
    </div>
  )
}
