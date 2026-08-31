import { useEffect, useRef, useState } from 'react'
import { IconWaveform, IconSearch } from './Icons.jsx'

const BAR_COUNT = 52

export default function WavePanel({ latestEvent, serverUp }) {
  const canvasRef = useRef(null)
  const isActiveRef = useRef(false)
  const [transcript, setTranscript] = useState(null)
  const [isActive, setIsActive] = useState(false)
  const [confidence, setConfidence] = useState(null)
  const [snr, setSnr] = useState(null)
  const animRef = useRef(null)
  const timeRef = useRef(0)
  const barsRef = useRef(Array(BAR_COUNT).fill(0.5))

  useEffect(() => {
    if (latestEvent?.transcript) {
      setTranscript(latestEvent.transcript)
      setConfidence((92 + Math.random() * 6).toFixed(1))
      setSnr((22 + Math.random() * 4).toFixed(1))
      setIsActive(true)
      isActiveRef.current = true
      const t = setTimeout(() => {
        setIsActive(false)
        isActiveRef.current = false
      }, 3500)
      return () => clearTimeout(t)
    }
  }, [latestEvent])

  useEffect(() => {
    const canvas = canvasRef.current
    if (!canvas) return
    const ctx = canvas.getContext('2d')

    const resize = () => {
      const dpr = window.devicePixelRatio || 1
      canvas.width = canvas.offsetWidth * dpr
      canvas.height = canvas.offsetHeight * dpr
      ctx.scale(dpr, dpr)
    }
    resize()
    window.addEventListener('resize', resize)

    const draw = () => {
      const w = canvas.offsetWidth
      const h = canvas.offsetHeight
      ctx.clearRect(0, 0, w, h)

      const active = isActiveRef.current
      const t = timeRef.current
      const cx = w / 2
      const maxH = h * 0.78
      const centerY = h * 0.5

      const totalBarW = w * 0.9
      const barW = (totalBarW / BAR_COUNT) * 0.55
      const spacing = (totalBarW / BAR_COUNT)
      const startX = (w - totalBarW) / 2 + spacing * 0.25

      for (let i = 0; i < BAR_COUNT; i++) {
        const norm = i / (BAR_COUNT - 1)  // 0..1
        // Envelope: tall in middle, shorter at edges
        const envelope = 1 - Math.pow((norm - 0.5) * 2, 2) * 0.5

        const phase = i * 0.28
        const slow = Math.sin(t * 0.9 + phase) * 0.5 + 0.5
        const fast = Math.cos(t * 2.1 + phase * 1.4) * 0.25 + 0.25

        let targetNorm
        if (active) {
          const spike = Math.pow(Math.random(), 0.5)
          targetNorm = (slow * 0.3 + fast * 0.2 + spike * 0.5) * envelope
          targetNorm = Math.max(0.05, targetNorm)
        } else {
          // Gentle idle shimmer
          targetNorm = (slow * 0.04 + fast * 0.02 + 0.025) * envelope
          targetNorm = Math.max(0.018, targetNorm)
        }

        barsRef.current[i] += (targetNorm - barsRef.current[i]) * (active ? 0.28 : 0.04)
        const barH = barsRef.current[i] * maxH

        const x = startX + i * spacing
        const y = centerY - barH / 2

        // Mirrored: draw both up and down half
        const halfH = barH / 2
        const radius = Math.min(barW / 2, halfH / 2, 3)

        // Color
        const intensity = barsRef.current[i]
        let r, g, b, a
        if (active) {
          // Pink → Indigo gradient across bars + intensity
          const t2 = norm
          r = Math.round(244 * (1 - t2) + 124 * t2)
          g = Math.round(114 * (1 - t2) + 106 * t2)
          b = Math.round(182 * (1 - t2) + 247 * t2)
          a = 0.55 + intensity * 0.45
        } else {
          r = 124; g = 106; b = 247
          a = 0.18 + intensity * 0.5
        }

        ctx.fillStyle = `rgba(${r},${g},${b},${a})`

        // Draw mirrored bar
        ctx.beginPath()
        ctx.roundRect(x, centerY - halfH, barW, halfH, [radius, radius, 0, 0])
        ctx.fill()
        ctx.beginPath()
        ctx.roundRect(x, centerY, barW, halfH, [0, 0, radius, radius])
        ctx.fill()

        // Glow for tall active bars
        if (active && intensity > 0.45) {
          ctx.shadowColor = `rgba(${r},${g},${b},0.6)`
          ctx.shadowBlur = 10
          ctx.fillStyle = `rgba(${r},${g},${b},${a * 0.5})`
          ctx.beginPath()
          ctx.roundRect(x, centerY - halfH, barW, barH, radius)
          ctx.fill()
          ctx.shadowBlur = 0
        }
      }

      // Center divider line
      ctx.strokeStyle = active ? 'rgba(244,114,182,0.15)' : 'rgba(124,106,247,0.08)'
      ctx.lineWidth = 1
      ctx.setLineDash([5, 8])
      ctx.beginPath()
      ctx.moveTo(startX, centerY)
      ctx.lineTo(startX + totalBarW, centerY)
      ctx.stroke()
      ctx.setLineDash([])

      timeRef.current += 0.038
      animRef.current = requestAnimationFrame(draw)
    }

    draw()
    return () => {
      cancelAnimationFrame(animRef.current)
      window.removeEventListener('resize', resize)
    }
  }, [])

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%' }}>
      {/* Canvas area */}
      <div style={{ flex: 1, position: 'relative', minHeight: 0 }}>
        <canvas
          ref={canvasRef}
          style={{ width: '100%', height: '100%', display: 'block' }}
        />

        {/* Stat chips — top left */}
        <div style={{ position: 'absolute', top: 14, left: 16, display: 'flex', gap: 8 }}>
          {[
            { label: 'Confidence', value: isActive ? `${confidence}%` : '—', icon: null, color: 'var(--primary)' },
            { label: 'SNR', value: isActive ? `${snr} dB` : '—', icon: null, color: 'var(--sky)' },
          ].map(s => (
            <div key={s.label} style={{
              background: 'rgba(16,19,30,0.85)',
              border: '1px solid rgba(255,255,255,0.07)',
              borderRadius: 9,
              padding: '6px 14px',
              backdropFilter: 'blur(16px)',
            }}>
              <div style={{ fontSize: 9, color: 'var(--t3)', letterSpacing: '1.2px', textTransform: 'uppercase', marginBottom: 2 }}>{s.label}</div>
              <div style={{
                fontFamily: 'var(--font-mono)', fontSize: 15, fontWeight: 700,
                color: isActive ? s.color : 'var(--t3)',
                transition: 'color 0.3s',
              }}>{s.value}</div>
            </div>
          ))}
        </div>

        {/* Live indicator top right */}
        {isActive && (
          <div style={{
            position: 'absolute', top: 14, right: 16,
            display: 'flex', alignItems: 'center', gap: 6,
            background: 'rgba(244,114,182,0.1)',
            border: '1px solid rgba(244,114,182,0.25)',
            borderRadius: 100, padding: '4px 12px',
          }}>
            <div style={{ width: 6, height: 6, borderRadius: '50%', background: 'var(--pink)', boxShadow: '0 0 8px var(--pink)', animation: 'pulse-dot 0.8s infinite' }} />
            <span style={{ fontSize: 10, fontWeight: 700, color: 'var(--pink)', letterSpacing: '1px' }}>ACTIVE</span>
          </div>
        )}
      </div>

      {/* Transcript bar */}
      <div style={{
        flexShrink: 0,
        margin: '0 16px 16px',
        padding: '14px 18px',
        background: isActive ? 'rgba(124,106,247,0.07)' : 'rgba(255,255,255,0.025)',
        border: `1px solid ${isActive ? 'rgba(124,106,247,0.25)' : 'rgba(255,255,255,0.05)'}`,
        borderRadius: 12,
        display: 'flex',
        alignItems: 'center',
        gap: 12,
        minHeight: 54,
        transition: 'all 0.4s ease',
      }}>
        {/* Icon */}
        <div style={{
          width: 32, height: 32, borderRadius: 8, flexShrink: 0,
          background: isActive ? 'rgba(124,106,247,0.15)' : 'rgba(255,255,255,0.04)',
          display: 'flex', alignItems: 'center', justifyContent: 'center',
          border: `1px solid ${isActive ? 'rgba(124,106,247,0.25)' : 'rgba(255,255,255,0.06)'}`,
          transition: '0.3s',
        }}>
          {isActive
            ? <IconWaveform size={15} color="var(--primary)" />
            : <IconSearch size={15} color="var(--t3)" />
          }
        </div>

        <div style={{ flex: 1, minWidth: 0 }}>
          <div style={{ fontSize: 9, color: 'var(--t3)', letterSpacing: '1px', textTransform: 'uppercase', marginBottom: 2 }}>
            {isActive ? 'Transcription' : 'Wake Word'}
          </div>
          <div style={{
            fontSize: isActive ? 16 : 13,
            fontWeight: isActive ? 600 : 400,
            color: isActive ? 'var(--t1)' : 'var(--t3)',
            fontStyle: !isActive ? 'italic' : 'normal',
            whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis',
            transition: 'all 0.3s',
          }}>
            {isActive && transcript ? `"${transcript}"` : !serverUp ? 'Server offline' : 'Waiting for "Hey Vaani"…'}
          </div>
        </div>

        {isActive && (
          <div style={{ fontSize: 10, color: 'var(--green)', fontFamily: 'var(--font-mono)', fontWeight: 700, flexShrink: 0 }}>
            ✓ Done
          </div>
        )}
      </div>
    </div>
  )
}
