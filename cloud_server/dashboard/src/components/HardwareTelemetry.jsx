import { useState, useEffect } from 'react'

const Dial = ({ label, value, color }) => {
  const dash = 250
  const offset = dash - (dash * value) / 100
  
  return (
    <div style={{ position: 'relative', width: 80, height: 80 }}>
      <svg width="80" height="80" viewBox="0 0 100 100" style={{ transform: 'rotate(-90deg)' }}>
        <circle cx="50" cy="50" r="40" fill="none" stroke="rgba(255,255,255,0.1)" strokeWidth="8" />
        <circle cx="50" cy="50" r="40" fill="none" stroke={color} strokeWidth="8" 
          strokeDasharray={dash} strokeDashoffset={offset} strokeLinecap="round" 
          style={{ transition: 'stroke-dashoffset 0.3s ease', filter: `drop-shadow(0 0 8px ${color})` }} />
      </svg>
      <div style={{ position: 'absolute', top: 0, left: 0, width: '100%', height: '100%', display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center' }}>
        <div className="mono" style={{ fontSize: 16, fontWeight: 700, color: 'var(--text-main)', textShadow: `0 0 10px ${color}` }}>{value.toFixed(0)}</div>
      </div>
      <div style={{ textAlign: 'center', fontSize: 9, color: 'var(--text-muted)', marginTop: 8 }}>{label}</div>
    </div>
  )
}

export default function HardwareTelemetry({ latestEvent }) {
  const [load, setLoad] = useState({ core0: 12, core1: 8, temp: 42.4, ram: 45 })
  
  useEffect(() => {
    const int = setInterval(() => {
      setLoad(prev => {
        const active = latestEvent && (Date.now() - new Date(latestEvent.timestamp).getTime() < 2000)
        return {
          core0: prev.core0 + ((active ? 85+Math.random()*10 : 10+Math.random()*5) - prev.core0) * 0.2,
          core1: prev.core1 + ((active ? 90+Math.random()*10 : 5+Math.random()*5) - prev.core1) * 0.2,
          temp: prev.temp + ((active ? 48+Math.random()*2 : 42+Math.random()) - prev.temp) * 0.05,
          ram: prev.ram + ((active ? 75 : 45) - prev.ram) * 0.1
        }
      })
    }, 200)
    return () => clearInterval(int)
  }, [latestEvent])

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 20, height: '100%', justifyContent: 'center' }}>
      
      <div style={{ display: 'flex', justifyContent: 'space-around' }}>
        <Dial label="CORE 0" value={load.core0} color="var(--accent-cyan)" />
        <Dial label="NPU" value={load.core1} color="var(--accent-magenta)" />
        <Dial label="VRAM" value={load.ram} color="var(--accent-cyan)" />
      </div>

      <div style={{ display: 'flex', justifyContent: 'space-around', background: 'rgba(255,255,255,0.05)', padding: '12px', borderRadius: 8, border: '1px solid rgba(255,255,255,0.1)' }}>
        <div style={{ textAlign: 'center' }}>
          <div style={{ fontSize: 9, color: 'var(--text-muted)' }}>THERMAL SENSOR</div>
          <div className="mono val" style={{ color: load.temp > 45 ? 'var(--accent-magenta)' : 'var(--text-main)' }}>
            {load.temp.toFixed(1)}<span className="unit">°C</span>
          </div>
        </div>
        <div style={{ textAlign: 'center' }}>
          <div style={{ fontSize: 9, color: 'var(--text-muted)' }}>POWER DRAW</div>
          <div className="mono val">
            {(load.core0 > 50 ? 1.2 : 0.4).toFixed(2)}<span className="unit">W</span>
          </div>
        </div>
      </div>

    </div>
  )
}
