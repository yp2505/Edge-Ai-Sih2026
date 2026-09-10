import { useState, useEffect } from 'react'
import { VaaniLogo, IconSettings } from './Icons.jsx'

function Clock() {
  const [t, setT] = useState(new Date())
  useEffect(() => {
    const i = setInterval(() => setT(new Date()), 1000)
    return () => clearInterval(i)
  }, [])
  const hh = String(t.getHours()).padStart(2, '0')
  const mm = String(t.getMinutes()).padStart(2, '0')
  const ss = String(t.getSeconds()).padStart(2, '0')
  const ampm = t.getHours() >= 12 ? 'pm' : 'am'
  return (
    <div style={{ display: 'flex', alignItems: 'baseline', gap: 3 }}>
      <span className="topbar-clock">{hh}:{mm}:{ss}</span>
      <span style={{ fontFamily: 'var(--font-mono)', fontSize: 9, color: 'var(--t4)', letterSpacing: 1 }}>{ampm}</span>
    </div>
  )
}

export default function TopBarNew({ health, serverUp, totalEvents, onSettingsClick }) {
  const uptime = health?.uptime_seconds || 0
  const h = Math.floor(uptime / 3600)
  const m = Math.floor((uptime % 3600) / 60)
  const s = uptime % 60
  const upStr = h > 0 ? `${h}h ${m}m` : m > 0 ? `${m}m` : `${s}s`

  const stats = [
    { label: 'Queries', value: totalEvents, color: 'var(--cyan)' },
    { label: 'Uptime', value: serverUp ? upStr : '—', color: 'var(--ice)' },
    { label: 'Model', value: 'Whisper tiny', color: 'var(--t1)' },
    { label: 'ESP32', value: serverUp ? 'Connected' : 'Waiting', color: serverUp ? 'var(--green)' : 'var(--t4)' },
  ]

  return (
    <div className="topbar">

      {/* Brand */}
      <div className="topbar-brand">
        <div className="topbar-logo">
          <VaaniLogo size={22} />
        </div>
        <div>
          <div className="topbar-title-main">HEY VAANI</div>
          <div className="topbar-title-sub">ISRO · Edge AI · SIH 2026</div>
        </div>

        {/* Status Pill */}
        <div className={`topbar-online-pill ${serverUp ? 'online' : 'offline'}`}>
          <div
            className="topbar-online-dot"
            style={{ background: serverUp ? 'var(--green)' : 'var(--red)', boxShadow: serverUp ? '0 0 8px var(--green)' : 'none' }}
          />
          {serverUp ? 'Online' : 'Offline'}
        </div>
      </div>

      {/* Stats */}
      <div className="topbar-stats">
        {stats.map((s, i) => (
          <div key={s.label} style={{ display: 'flex', alignItems: 'center', flex: 1 }}>
            {i > 0 && <div className="topbar-divider" />}
            <div className="topbar-stat">
              <div className="topbar-stat-label">{s.label}</div>
              <div className="topbar-stat-value" style={{ color: s.color }}>{s.value}</div>
            </div>
          </div>
        ))}
      </div>

      {/* Right */}
      <div className="topbar-right">
        <Clock />
        <button className="topbar-btn" onClick={onSettingsClick} id="settings-btn">
          <IconSettings size={13} color="var(--cyan)" />
          Settings
        </button>
      </div>

    </div>
  )
}
