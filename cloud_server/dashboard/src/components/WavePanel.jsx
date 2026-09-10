import { useEffect, useRef, useState } from 'react'
import { IconWaveform, IconSearch } from './Icons.jsx'

const N = 58

export default function WavePanel({ latestEvent, serverUp, telemetry, stale }) {
  const ref = useRef(null)
  const animRef = useRef(null)
  const timeRef = useRef(0)
  const bars = useRef(Array(N).fill(0.02))

  const active = serverUp && latestEvent && (Date.now() - new Date(latestEvent.timestamp).getTime() < 4000)
  const isWake = active && latestEvent.status === 'wake_word_detected'
  const tx = active ? (isWake ? 'HEY VAANI DETECTED — listening for command...' : latestEvent.transcript) : null
  const conf = active && latestEvent.match_score ? (latestEvent.match_score * 100).toFixed(1) : null
  const snr = !stale && telemetry && telemetry.snr != null ? telemetry.snr.toFixed(1) : null

  const activeRef = useRef(active)
  const micRef = useRef(0)
  
  useEffect(() => { activeRef.current = active }, [active])
  useEffect(() => { micRef.current = (!stale && telemetry && telemetry.mic_rms > 0) ? telemetry.mic_rms : 0 }, [telemetry, stale])

  useEffect(() => {
    const canvas = ref.current; if (!canvas) return
    const ctx = canvas.getContext('2d')
    const resize = () => {
      const dpr = devicePixelRatio || 1
      canvas.width = canvas.offsetWidth * dpr; canvas.height = canvas.offsetHeight * dpr; ctx.scale(dpr, dpr)
    }
    resize(); window.addEventListener('resize', resize)

    const draw = () => {
      const w = canvas.offsetWidth, h = canvas.offsetHeight
      ctx.clearRect(0, 0, w, h)
      const on = activeRef.current, t = timeRef.current
      const cy = h * 0.5, maxH = h * 0.74, pad = w * 0.04
      const total = w - pad * 2, sp = total / N, bw = sp * 0.5
      const baseVol = Math.min(1.0, micRef.current * 8.0)

      for (let i = 0; i < N; i++) {
        const n = i / (N - 1)
        const env = 0.25 + 0.75 * (1 - Math.pow((n - 0.5) * 2, 2))
        const ph = i * 0.24
        const w1 = Math.sin(t * 1.1 + ph) * 0.5 + 0.5
        const w2 = Math.cos(t * 2.4 + ph * 1.3) * 0.3 + 0.3
        
        let tgt = baseVol > 0.01 
          ? (w1 * 0.4 + w2 * 0.2 + 0.4) * env * baseVol
          : Math.max(0.01, (w1 * 0.03 + w2 * 0.015 + 0.012) * env)
          
        bars.current[i] += (tgt - bars.current[i]) * 0.15

        const inten = bars.current[i]
        const hh = inten * maxH / 2
        const x = pad + i * sp + sp * 0.25
        const r = Math.min(bw / 2, hh / 2, 4)
        // Cyan → teal gradient
        const f = n
        const R = Math.round(0), G = Math.round(210 - f * 20), B = Math.round(255 - f * 60)
        const a = on ? 0.45 + inten * 0.55 : 0.1 + inten * 0.3
        ctx.fillStyle = `rgba(${R},${G},${B},${a})`
        ctx.beginPath(); ctx.roundRect(x, cy - hh, bw, hh, [r, r, 0, 0]); ctx.fill()
        ctx.beginPath(); ctx.roundRect(x, cy, bw, hh, [0, 0, r, r]); ctx.fill()

        if (on && inten > 0.38) {
          ctx.shadowColor = `rgba(${R},${G},${B},0.75)`; ctx.shadowBlur = 14
          ctx.fillStyle = `rgba(${R},${G},${B},${a * 0.35})`
          ctx.beginPath(); ctx.roundRect(x, cy - hh, bw, hh * 2, r); ctx.fill()
          ctx.shadowBlur = 0
        }
      }
      // Centre divider
      ctx.strokeStyle = on ? 'rgba(0,229,255,0.14)' : 'rgba(255,255,255,0.04)'
      ctx.lineWidth = 1; ctx.setLineDash([4, 9])
      ctx.beginPath(); ctx.moveTo(pad, cy); ctx.lineTo(w - pad, cy); ctx.stroke(); ctx.setLineDash([])
      timeRef.current += 0.04; animRef.current = requestAnimationFrame(draw)
    }
    draw()
    return () => { cancelAnimationFrame(animRef.current); window.removeEventListener('resize', resize) }
  }, [])

  return (
    <div className="wave-panel-content" style={{ display: 'flex', flexDirection: 'column', height: '100%', padding: '16px 20px 20px' }}>
      
      {/* Metrics Row */}
      <div style={{ display: 'flex', gap: 12, marginBottom: 16 }}>
        {[{ l: 'CONFIDENCE', v: active ? `${conf}%` : '--', c: 'var(--c1)' }, { l: 'SNR', v: active ? `${snr} dB` : '--', c: 'var(--c3)' }].map(s => (
          <div key={s.l} style={{ flex: 1, background: 'rgba(255,255,255,0.02)', border: '1px solid rgba(255,255,255,0.05)', borderRadius: 12, padding: '10px 14px' }}>
            <div style={{ fontSize: 9, fontWeight: 800, color: 'var(--t4)', letterSpacing: 1.5, marginBottom: 4 }}>{s.l}</div>
            <div style={{ fontSize: 16, fontWeight: 700, color: active ? s.c : 'var(--t4)', fontFamily: 'var(--mono)' }}>{s.v}</div>
          </div>
        ))}
      </div>

      {/* Waveform Area */}
      <div className="waveform-area" style={{ flex: 1, minHeight: 0, display: 'flex', flexDirection: 'column', position: 'relative', marginBottom: 20 }}>
        <div style={{ fontSize: 9, fontWeight: 800, color: 'var(--t4)', letterSpacing: 1.5, marginBottom: 8, paddingLeft: 4 }}>LIVE AUDIO</div>
        <div style={{ flex: 1, position: 'relative', background: 'rgba(255,255,255,0.015)', borderRadius: 12, border: '1px solid rgba(255,255,255,0.04)', overflow: 'hidden', opacity: active ? 1 : 0.25, transition: 'opacity 0.5s' }}>
          <canvas ref={ref} style={{ width: '100%', height: '100%', display: 'block' }} />
          {active && (
            <div style={{ position: 'absolute', top: 10, right: 12, display: 'flex', alignItems: 'center', gap: 6 }}>
              <div style={{ width: 6, height: 6, borderRadius: '50%', background: 'var(--c1)', boxShadow: '0 0 8px var(--c1)', animation: 'blink 0.8s infinite' }} />
              <span style={{ fontSize: 9, fontWeight: 700, color: 'var(--c1)', letterSpacing: '1.5px', fontFamily: 'var(--mono)' }}>ACTIVE</span>
            </div>
          )}
        </div>
      </div>

      {/* Transcript Card */}
      <div className="wave-status-card" style={{ 
        padding: 16, borderRadius: 14,
        background: active ? 'rgba(0,229,255,0.06)' : 'rgba(255,255,255,0.03)',
        border: `1px solid ${active ? 'rgba(0,229,255,0.2)' : 'rgba(255,255,255,0.05)'}`,
        boxShadow: active ? '0 0 20px rgba(0,229,255,0.05)' : 'none',
        display: 'flex', flexDirection: 'column', transition: 'all 0.3s'
      }}>
        <div style={{ fontSize: 9, fontWeight: 800, color: 'var(--t3)', letterSpacing: 1.5, marginBottom: 8 }}>WAKE WORD / AUDIO STATUS</div>
        {active ? (
          <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 6, color: 'var(--green)', fontSize: 13, fontWeight: 600 }}>
              <span style={{ fontSize: 14, textShadow: '0 0 8px var(--green)' }}>✓</span> "Hey Vaani" detected
            </div>
            <div style={{ color: 'var(--t1)', fontSize: 15, fontWeight: 500, fontStyle: 'italic' }}>
              {tx && tx.length > 5 && !tx.startsWith('[') ? `"${tx}"` : 'Processing...'}
            </div>
          </div>
        ) : (
          <div style={{ display: 'flex', alignItems: 'center', color: 'var(--t4)', fontSize: 13, gap: 8, padding: '4px 0' }}>
            {!serverUp ? 'Server offline...' : (
              <>
                <IconSearch size={14} color="var(--t4)" />
                Waiting for voice input...
              </>
            )}
          </div>
        )}
      </div>
    </div>
  )
}
