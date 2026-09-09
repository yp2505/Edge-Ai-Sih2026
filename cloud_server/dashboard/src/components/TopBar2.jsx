import { useState, useEffect } from 'react'
import { VaaniLogo, IconSettings } from './Icons.jsx'

function Clock() {
  const [t, setT] = useState(new Date())
  useEffect(() => { const i = setInterval(() => setT(new Date()), 1000); return () => clearInterval(i) }, [])
  const pad = n => String(n).padStart(2, '0')
  const days = ['SUN', 'MON', 'TUE', 'WED', 'THU', 'FRI', 'SAT']
  const months = ['JAN', 'FEB', 'MAR', 'APR', 'MAY', 'JUN', 'JUL', 'AUG', 'SEP', 'OCT', 'NOV', 'DEC']
  const day = days[t.getDay()]
  const month = months[t.getMonth()]
  const date = pad(t.getDate())
  const year = t.getFullYear()
  const h12 = pad(t.getHours() % 12 || 12)
  const ampm = t.getHours() >= 12 ? 'PM' : 'AM'
  const timeStr = `${h12}:${pad(t.getMinutes())}:${pad(t.getSeconds())}`
  const dateStr = `${day}, ${date} ${month} ${year}`

  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: 14 }}>
      {/* Date */}
      <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'flex-end', justifyContent: 'center' }}>
        <div style={{ fontSize: 9, fontWeight: 800, color: 'var(--t4)', letterSpacing: 1.5, marginBottom: 2 }}>DATE</div>
        <div style={{ fontFamily: 'var(--mono)', fontSize: 12, fontWeight: 700, color: 'var(--t2)', whiteSpace: 'nowrap', letterSpacing: 0.5 }}>
          {dateStr}
        </div>
      </div>

      <div style={{ width: 1, height: 20, background: 'rgba(255,255,255,0.08)' }} />

      {/* Time */}
      <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'flex-end', justifyContent: 'center' }}>
        <div style={{ fontSize: 9, fontWeight: 800, color: 'var(--t4)', letterSpacing: 1.5, marginBottom: 2 }}>TIME</div>
        <div style={{ display: 'flex', alignItems: 'baseline', gap: 4 }}>
          <span style={{ fontFamily: 'var(--mono)', fontSize: 13, fontWeight: 700, color: 'var(--t1)' }}>{timeStr}</span>
          <span style={{ fontFamily: 'var(--mono)', fontSize: 9, fontWeight: 800, color: 'var(--c1)', letterSpacing: 1 }}>
            {ampm}
          </span>
        </div>
      </div>
    </div>
  )
}

export default function TopBar({ health, up, total, telemetryStale, onSettings }) {
  const u = health?.uptime_seconds || 0
  const h = Math.floor(u / 3600), m = Math.floor((u % 3600) / 60), s = u % 60
  const upStr = h > 0 ? `${h}h ${m}m` : m > 0 ? `${m}m` : `${s}s`
  
  const espStateText = !up ? 'Offline' : telemetryStale ? 'Disconnected' : 'Connected'
  const espStateColor = !up ? 'var(--t4)' : telemetryStale ? 'var(--amber)' : 'var(--green)'

  return (
    <div className="tb-wrap">
      
      {/* Brand */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 20 }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 12 }}>
          <div style={{ width: 34, height: 34, borderRadius: 10, background: 'rgba(0,229,255,0.08)', border: '1px solid rgba(0,229,255,0.2)', display: 'flex', alignItems: 'center', justifyContent: 'center', boxShadow: '0 0 15px rgba(0,229,255,0.1)' }}>
            <VaaniLogo size={20} />
          </div>
          <div style={{ display: 'flex', flexDirection: 'column', justifyContent: 'center' }}>
            <div style={{ fontFamily: 'var(--title)', fontSize: 15, fontWeight: 900, color: 'var(--t1)', letterSpacing: 2, lineHeight: 1.1 }}>HEY VAANI</div>
            <div style={{ fontSize: 9, fontWeight: 800, color: 'var(--c1)', letterSpacing: 1.5 }}>ISRO • EDGE AI • SIH 2026</div>
          </div>
        </div>

        <div className="hide-on-mobile" style={{ width: 1, height: 24, background: 'rgba(255,255,255,0.08)' }} />

        <div style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '6px 12px', background: up ? 'rgba(0,255,136,0.05)' : 'rgba(255,60,60,0.05)', borderRadius: 20, border: `1px solid ${up ? 'rgba(0,255,136,0.15)' : 'rgba(255,60,60,0.15)'}` }}>
          <div style={{ width: 6, height: 6, borderRadius: '50%', background: up ? 'var(--green)' : '#ff3c3c', boxShadow: up ? '0 0 8px var(--green)' : 'none' }} />
          <span style={{ fontSize: 10, fontWeight: 800, letterSpacing: 1.5, color: up ? 'var(--green)' : '#ff3c3c' }}>{up ? 'SYSTEM ONLINE' : 'SYSTEM OFFLINE'}</span>
        </div>
      </div>

      {/* Stats */}
      <div className="tb-stats-group">
        {[
          { l: 'QUERIES',       v: total,                  c: 'var(--c1)' },
          { l: 'SERVER UPTIME', v: up ? upStr : '--',      c: 'var(--c3)', hideM: true },
          { l: 'ESP32',         v: espStateText,           c: espStateColor },
          { l: 'DATA',          v: !up ? '--' : telemetryStale ? 'STALE' : 'LIVE', c: !up ? 'var(--t4)' : telemetryStale ? 'var(--amber)' : 'var(--c1)', hideM: true },
        ].map((s, i) => (
          <div key={s.l} className={s.hideM ? "hide-on-mobile" : ""} style={{ display: 'flex', alignItems: 'center' }}>
            <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'flex-start', justifyContent: 'center' }}>
              <div style={{ fontSize: 9, fontWeight: 800, color: 'var(--t4)', letterSpacing: 1.5, marginBottom: 2 }}>{s.l}</div>
              <div style={{ fontFamily: s.v === '--' ? 'inherit' : 'var(--mono)', fontSize: 13, fontWeight: 700, color: s.c }}>{s.v}</div>
            </div>
            {i < 3 && <div style={{ width: 1, height: 20, background: 'rgba(255,255,255,0.06)', marginLeft: 30 }} />}
          </div>
        ))}
      </div>

      {/* Right */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 16 }}>
        <div className="hide-on-mobile"><Clock /></div>
        <button onClick={onSettings} style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '8px 12px', background: 'rgba(255,255,255,0.03)', border: '1px solid rgba(255,255,255,0.08)', borderRadius: 12, color: 'var(--t2)', fontSize: 11, fontWeight: 700, letterSpacing: 1, transition: 'all 0.2s' }}>
          <IconSettings size={14} color="var(--c1)" /> <span className="hide-on-mobile">SETTINGS</span>
        </button>
      </div>
    </div>
  )
}
