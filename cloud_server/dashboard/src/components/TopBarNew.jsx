import { useState, useEffect } from 'react'
import { VaaniLogo, IconSettings } from './Icons.jsx'

function Clock() {
  const [t, setT] = useState(new Date())
  useEffect(() => {
    const i = setInterval(() => setT(new Date()), 1000)
    return () => clearInterval(i)
  }, [])
  return (
    <span style={{ fontFamily: 'var(--font-mono)', fontSize: 13, color: 'var(--t2)', letterSpacing: '0.5px' }}>
      {t.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })}
    </span>
  )
}

export default function TopBarNew({ health, serverUp, totalEvents, onSettingsClick }) {
  const uptime = health?.uptime_seconds || 0
  const h = Math.floor(uptime / 3600)
  const m = Math.floor((uptime % 3600) / 60)
  const upStr = h > 0 ? `${h}h ${m}m` : `${m}m`

  return (
    <div style={{
      display: 'flex',
      justifyContent: 'space-between',
      alignItems: 'center',
      height: '100%',
      padding: '0 20px',
      background: 'linear-gradient(90deg, rgba(255,255,255,0.02) 0%, transparent 100%)',
    }}>

      {/* ── Left: Brand ── */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 14 }}>
        {/* Animated logo SVG */}
        <VaaniLogo size={32} />

        <div style={{ display: 'flex', flexDirection: 'column', gap: 2 }}>
          <div style={{ fontSize: 16, fontWeight: 700, letterSpacing: '-0.2px', color: 'var(--t1)' }}>
            Hey Vaani
          </div>
          <div style={{ fontSize: 9, color: 'var(--t3)', letterSpacing: '2px', textTransform: 'uppercase', fontWeight: 600 }}>
            Edge AI · SIH 2026
          </div>
        </div>

        {/* Status Pill */}
        <div style={{
          display: 'flex', alignItems: 'center', gap: 6,
          padding: '4px 12px',
          background: serverUp ? 'rgba(74,222,128,0.1)' : 'rgba(248,113,113,0.1)',
          border: `1px solid ${serverUp ? 'rgba(74,222,128,0.2)' : 'rgba(248,113,113,0.2)'}`,
          borderRadius: 100,
          marginLeft: 12,
          boxShadow: serverUp ? '0 0 12px rgba(74,222,128,0.15)' : 'none',
        }}>
          <div style={{
            width: 6, height: 6, borderRadius: '50%',
            background: serverUp ? 'var(--green)' : 'var(--red)',
            boxShadow: serverUp ? '0 0 8px var(--green)' : 'none',
            animation: serverUp ? 'pulse-dot 2s infinite' : 'none',
          }} />
          <span style={{
            fontSize: 10, fontWeight: 700, letterSpacing: '0.5px', textTransform: 'uppercase',
            color: serverUp ? 'var(--green)' : 'var(--red)',
          }}>
            {serverUp ? 'Online' : 'Offline'}
          </span>
        </div>
      </div>

      {/* ── Center: Floating Stats ── */}
      <div style={{
        display: 'flex', gap: 16,
        padding: '6px 20px',
        background: 'rgba(255,255,255,0.02)',
        border: '1px solid var(--border)',
        borderRadius: 100,
        boxShadow: 'inset 0 1px 1px rgba(255,255,255,0.05)',
      }}>
        {[
          { label: 'Queries', value: totalEvents, mono: true, color: 'var(--primary)' },
          { label: 'Uptime', value: serverUp ? upStr : '—', mono: true, color: 'var(--sky)' },
          { label: 'Model', value: 'Whisper tiny', mono: false, color: 'var(--t1)' },
        ].map((s, i) => (
          <div key={s.label} style={{ display: 'flex', alignItems: 'center', gap: 16 }}>
            {i > 0 && <div style={{ width: 1, height: 16, background: 'var(--border)' }} />}
            <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 2 }}>
              <div style={{ fontSize: 9, color: 'var(--t3)', letterSpacing: '1px', textTransform: 'uppercase', fontWeight: 600 }}>{s.label}</div>
              <div style={{
                fontSize: 13, fontWeight: 700,
                fontFamily: s.mono ? 'var(--font-mono)' : 'var(--font-base)',
                color: s.color,
              }}>{s.value}</div>
            </div>
          </div>
        ))}
      </div>

      {/* ── Right: Time & Settings ── */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 20 }}>
        <Clock />

        <button
          onClick={onSettingsClick}
          style={{
            display: 'flex', alignItems: 'center', gap: 8,
            padding: '8px 16px',
            background: 'var(--primary-dim)',
            border: '1px solid rgba(124,106,247,0.3)',
            borderRadius: 100,
            color: 'var(--primary)',
            fontSize: 12, fontWeight: 600, letterSpacing: '0.5px',
            cursor: 'pointer',
            transition: 'all 0.2s',
            boxShadow: '0 0 12px rgba(124,106,247,0.15)',
          }}
          onMouseOver={e => {
            e.currentTarget.style.background = 'rgba(124,106,247,0.2)'
            e.currentTarget.style.boxShadow = '0 0 16px rgba(124,106,247,0.25)'
          }}
          onMouseOut={e => {
            e.currentTarget.style.background = 'var(--primary-dim)'
            e.currentTarget.style.boxShadow = '0 0 12px rgba(124,106,247,0.15)'
          }}
        >
          <IconSettings size={14} color="var(--primary)" />
          Settings
        </button>
      </div>
    </div>
  )
}
