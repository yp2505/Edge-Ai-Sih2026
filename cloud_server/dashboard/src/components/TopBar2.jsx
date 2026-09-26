import { useState, useEffect } from 'react'
import { VaaniLogo, IconSettings, IconUsb } from './Icons.jsx'

function Clock() {
  const [offset, setOffset] = useState(0)
  const [synced, setSynced] = useState(false)
  const [t, setT] = useState(new Date())

  // Network synchronization with real-world time
  useEffect(() => {
    async function syncTime() {
      try {
        const res = await fetch('/api/realtime')
        if (res.ok) {
          const data = await res.json()
          if (data.offsetMs !== undefined) {
            setOffset(data.offsetMs)
            setSynced(true)
            setT(new Date(Date.now() + data.offsetMs))
          }
        }
      } catch (err) {}
    }
    syncTime()
    const syncInterval = setInterval(syncTime, 60000)
    return () => clearInterval(syncInterval)
  }, [])

  useEffect(() => {
    const i = setInterval(() => {
      setT(new Date(Date.now() + offset))
    }, 1000)
    return () => clearInterval(i)
  }, [offset])

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
    <div style={{ display: 'flex', alignItems: 'center', gap: 14 }} title={synced ? "Network Synchronized Time (IST)" : "Local Time"}>
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
        <div style={{ display: 'flex', alignItems: 'center', gap: 4, marginBottom: 2 }}>
          {synced && <div style={{ width: 4, height: 4, borderRadius: '50%', background: 'var(--green)', boxShadow: '0 0 6px var(--green)' }} />}
          <div style={{ fontSize: 9, fontWeight: 800, color: 'var(--t4)', letterSpacing: 1.5 }}>TIME (IST)</div>
        </div>
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

export default function TopBar({ health, up, total, telemetryStale, onSettings, deviceInfo, theme, onToggleTheme }) {
  const u = health?.uptime_seconds || 0
  const h = Math.floor(u / 3600), m = Math.floor((u % 3600) / 60), s = u % 60
  const upStr = !up ? '--' : (h > 0 ? `${h}h ${m}m` : m > 0 ? `${m}m` : `${s}s`)
  
  const isPlugged = Boolean(deviceInfo && deviceInfo.connected)
  const espStateText = !up
    ? 'Offline'
    : isPlugged
    ? `Online (${deviceInfo.port || 'USB'})`
    : telemetryStale
    ? 'Disconnected'
    : 'Online (Wi-Fi)'
  const espStateColor = !up ? 'var(--t4)' : (isPlugged || !telemetryStale) ? 'var(--green)' : 'rgba(255,255,255,0.45)'

  const isAws = Boolean(health?.is_aws || (health?.target_server && (health.target_server.includes('13.233') || health.target_server.includes('aws'))))
  const awsOnline = Boolean(health?.aws_online)

  return (
    <div className="tb-wrap">
      
      {/* Brand */}
      <div className="tb-brand-container" style={{ display: 'flex', alignItems: 'center', gap: 14, flexWrap: 'wrap', justifyContent: 'center' }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 12 }}>
          <div className="tb-logo" style={{ width: 40, height: 40, borderRadius: '50%', background: 'rgba(0,0,0,0.6)', border: '1px solid rgba(0,229,255,0.25)', display: 'flex', alignItems: 'center', justifyContent: 'center', boxShadow: '0 0 16px rgba(0,229,255,0.18)', overflow: 'hidden' }}>
            <VaaniLogo size={40} />
          </div>
          <div style={{ display: 'flex', flexDirection: 'column', justifyContent: 'center' }}>
            <div style={{ fontFamily: 'var(--title)', fontSize: 15, fontWeight: 900, color: 'var(--t1)', letterSpacing: 2, lineHeight: 1.1, whiteSpace: 'nowrap' }}>HEY VAANI</div>
            <div style={{ fontSize: 9, fontWeight: 800, color: 'var(--c1)', letterSpacing: 1.5, whiteSpace: 'nowrap' }}>ISRO • EDGE AI • SIH 2026</div>
          </div>
        </div>

        <div className="hide-on-mobile" style={{ width: 1, height: 24, background: 'rgba(255,255,255,0.08)' }} />

        {/* Server Status (Real-time live ASR backend) */}
        <div
          title={up ? 'ASR Backend Server Online (Port 8080)' : 'ASR Backend Server Offline'}
          style={{
            display: 'flex',
            alignItems: 'center',
            gap: 7,
            height: 30,
            padding: '0 13px',
            background: up ? 'rgba(0,255,136,0.06)' : 'rgba(255,75,75,0.08)',
            borderRadius: 15,
            border: `1px solid ${up ? 'rgba(0,255,136,0.22)' : 'rgba(255,75,75,0.25)'}`,
            boxShadow: up ? '0 0 10px rgba(0,255,136,0.12)' : '0 0 8px rgba(255,75,75,0.12)',
            flexShrink: 0,
            whiteSpace: 'nowrap',
            boxSizing: 'border-box'
          }}
        >
          <div style={{
            width: 6,
            height: 6,
            borderRadius: '50%',
            background: up ? 'var(--green)' : '#ff5252',
            boxShadow: up ? '0 0 8px var(--green)' : '0 0 6px #ff5252',
            animation: up ? 'pulse 2s infinite' : 'none',
            flexShrink: 0
          }} />
          <span style={{
            fontSize: 10,
            fontWeight: 800,
            letterSpacing: 1.2,
            color: up ? 'var(--green)' : '#ff6b6b',
            whiteSpace: 'nowrap',
            lineHeight: 1
          }}>
            {up ? 'SERVER ONLINE' : 'SERVER OFFLINE'}
          </span>
        </div>

        {/* AWS Cloud Link Status (Real-time live AWS EC2 reachability) */}
        {isAws && (
          <div
            title={`AWS Cloud Target: ${health?.target_server || '13.233.154.18'} (${awsOnline ? 'Connected' : 'Offline / Unreachable'})`}
            style={{
              display: 'flex',
              alignItems: 'center',
              gap: 7,
              height: 30,
              padding: '0 12px',
              background: awsOnline ? 'rgba(0,255,136,0.06)' : 'rgba(255,255,255,0.03)',
              borderRadius: 15,
              border: `1px solid ${awsOnline ? 'rgba(0,255,136,0.22)' : 'rgba(255,255,255,0.08)'}`,
              boxShadow: awsOnline ? '0 0 10px rgba(0,255,136,0.12)' : 'none',
              flexShrink: 0,
              whiteSpace: 'nowrap',
              boxSizing: 'border-box',
              transition: 'all 0.3s'
            }}
          >
            <div style={{
              width: 6,
              height: 6,
              borderRadius: '50%',
              background: awsOnline ? 'var(--green)' : 'rgba(255,255,255,0.25)',
              boxShadow: awsOnline ? '0 0 8px var(--green)' : 'none',
              animation: awsOnline ? 'pulse 2s infinite' : 'none',
              flexShrink: 0
            }} />
            <span style={{
              fontSize: 10,
              fontWeight: 800,
              letterSpacing: 1.2,
              color: awsOnline ? 'var(--green)' : 'rgba(255,255,255,0.45)',
              whiteSpace: 'nowrap',
              lineHeight: 1
            }}>
              {awsOnline ? 'AWS: ONLINE' : 'AWS: OFFLINE'}
            </span>
          </div>
        )}

        {/* USB Cable Plug Status */}
        <div style={{
          display: 'flex',
          alignItems: 'center',
          gap: 7,
          height: 30,
          padding: '0 13px',
          background: isPlugged ? 'rgba(0,255,136,0.08)' : 'rgba(255,255,255,0.03)',
          borderRadius: 15,
          border: `1px solid ${isPlugged ? 'rgba(0,255,136,0.25)' : 'rgba(255,255,255,0.08)'}`,
          boxShadow: isPlugged ? '0 0 10px rgba(0,255,136,0.18)' : 'none',
          flexShrink: 0,
          whiteSpace: 'nowrap',
          boxSizing: 'border-box',
          transition: 'all 0.3s'
        }}>
          <IconUsb size={13} color={isPlugged ? 'var(--green)' : 'rgba(255,255,255,0.45)'} />
          <div style={{
            width: 6,
            height: 6,
            borderRadius: '50%',
            background: isPlugged ? 'var(--green)' : 'rgba(255,255,255,0.25)',
            boxShadow: isPlugged ? '0 0 6px var(--green)' : 'none',
            flexShrink: 0
          }} />
          <span style={{
            fontSize: 10,
            fontWeight: 800,
            letterSpacing: 1.2,
            color: isPlugged ? 'var(--green)' : 'rgba(255,255,255,0.45)',
            whiteSpace: 'nowrap',
            lineHeight: 1
          }}>
            {isPlugged ? `USB: ${deviceInfo?.port || 'CONNECTED'}` : 'USB: UNPLUGGED'}
          </span>
        </div>
      </div>

      {/* Stats */}
      <div className="tb-stats-group">
        {[
          { l: 'QUERIES',       v: total,                  c: 'var(--c1)' },
          { l: 'SERVER UPTIME', v: up ? upStr : '--',      c: 'var(--c3)', hideM: true },
          { l: 'ESP32',         v: espStateText,           c: espStateColor },
          { l: 'DATA',          v: !up ? '--' : isPlugged || !telemetryStale ? 'LIVE' : 'STALE', c: !up ? 'var(--t4)' : isPlugged || !telemetryStale ? 'var(--green)' : 'rgba(255,255,255,0.45)', hideM: true },
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
        <button aria-label="Toggle Theme" onClick={onToggleTheme} className="tb-action-btn" style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '8px 12px', background: 'rgba(255,255,255,0.03)', border: '1px solid rgba(255,255,255,0.08)', borderRadius: 12, color: 'var(--t2)', fontSize: 11, fontWeight: 700, letterSpacing: 1, transition: 'all 0.2s' }}>
          <span className="hide-on-mobile">{theme === 'space' ? 'LIGHT THEME' : 'SPACE THEME'}</span>
        </button>
        <button aria-label="Open settings" onClick={onSettings} className="tb-action-btn" style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '8px 12px', background: 'rgba(255,255,255,0.03)', border: '1px solid rgba(255,255,255,0.08)', borderRadius: 12, color: 'var(--t2)', fontSize: 11, fontWeight: 700, letterSpacing: 1, transition: 'all 0.2s' }}>
          <IconSettings size={14} color="var(--c1)" /> <span className="hide-on-mobile">SETTINGS</span>
        </button>
      </div>
    </div>
  )
}
