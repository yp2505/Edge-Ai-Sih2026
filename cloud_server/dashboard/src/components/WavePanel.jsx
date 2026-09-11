import { useEffect, useRef } from 'react'
import { IconWaveform, IconSearch } from './Icons.jsx'

const N = 58

export default function WavePanel({ latestEvent, serverUp, telemetry, stale }) {
  const ref = useRef(null)
  const animRef = useRef(null)
  const timeRef = useRef(0)
  const bars = useRef(Array(N).fill(0.02))

  const isStreaming = !stale && telemetry && telemetry.streaming === true
  const isRecentEvent = serverUp && latestEvent && (Date.now() - new Date(latestEvent.timestamp).getTime() < 8000)
  const isWake = isRecentEvent && latestEvent.status === 'wake_word_detected'
  const isProcessing = isRecentEvent && latestEvent.status === 'complete' && latestEvent.transcript !== '[silence]'
  const isRejected = isRecentEvent && latestEvent.verification_status === 'REJECTED'

  const conf = !stale && telemetry && telemetry.keyword_confidence != null
    ? (telemetry.keyword_confidence * 100).toFixed(1) : null
  const snr = !stale && telemetry && telemetry.snr != null ? telemetry.snr.toFixed(1) : null

  let statusText = 'Waiting for voice input...'
  let statusColor = 'var(--t4)'
  let isActive = false
  if (isStreaming) {
    statusText = 'Listening for your command...'
    statusColor = 'var(--c1)'
    isActive = true
  } else if (isWake) {
    statusText = 'HEY VAANI DETECTED — processing...'
    statusColor = 'var(--green)'
    isActive = true
  } else if (isProcessing) {
    statusText = latestEvent.transcript
    statusColor = 'var(--c3)'
    isActive = true
  } else if (isRejected) {
    statusText = ` "${latestEvent.matched_variant}" rejected (score ${(latestEvent.match_score * 100).toFixed(0)}%)`
    statusColor = 'var(--t4)'
  } else if (!serverUp) {
    statusText = 'Server offline...'
  }

  const activeRef = useRef(isActive)
  const micRef = useRef(0)

  useEffect(() => { activeRef.current = isActive }, [isActive])
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

      <div style={{ display: 'flex', gap: 12, marginBottom: 16 }}>
        {[{ l: 'CONFIDENCE', v: conf ? `${conf}%` : '--', c: 'var(--c1)' }, { l: 'SNR', v: snr ? `${snr} dB` : '--', c: 'var(--c3)' }].map(s => (
          <div key={s.l} style={{ flex: 1, background: 'rgba(255,255,255,0.02)', border: '1px solid rgba(255,255,255,0.05)', borderRadius: 12, padding: '10px 14px' }}>
            <div style={{ fontSize: 9, fontWeight: 800, color: 'var(--t4)', letterSpacing: 1.5, marginBottom: 4 }}>{s.l}</div>
            <div style={{ fontSize: 16, fontWeight: 700, color: isActive ? s.c : 'var(--t4)', fontFamily: 'var(--mono)' }}>{s.v}</div>
          </div>
        ))}
      </div>

      <div className="waveform-area" style={{ flex: 1, minHeight: 0, display: 'flex', flexDirection: 'column', position: 'relative', marginBottom: 20 }}>
        <div style={{ fontSize: 9, fontWeight: 800, color: 'var(--t4)', letterSpacing: 1.5, marginBottom: 8, paddingLeft: 4 }}>LIVE AUDIO</div>
        <div style={{ flex: 1, position: 'relative', background: 'rgba(255,255,255,0.015)', borderRadius: 12, border: '1px solid rgba(255,255,255,0.04)', overflow: 'hidden', opacity: isActive ? 1 : 0.25, transition: 'opacity 0.5s' }}>
          <canvas ref={ref} style={{ width: '100%', height: '100%', display: 'block' }} />
          {isActive && (
            <div style={{ position: 'absolute', top: 10, right: 12, display: 'flex', alignItems: 'center', gap: 6 }}>
              <div style={{ width: 6, height: 6, borderRadius: '50%', background: statusColor, boxShadow: `0 0 8px ${statusColor}`, animation: 'blink 0.8s infinite' }} />
              <span style={{ fontSize: 9, fontWeight: 700, color: statusColor, letterSpacing: '1.5px', fontFamily: 'var(--mono)' }}>{isStreaming ? 'LISTENING' : 'ACTIVE'}</span>
            </div>
          )}
        </div>
      </div>

      <div className="wave-status-card" style={{
        padding: 16, borderRadius: 14,
        background: isActive ? 'rgba(0,229,255,0.06)' : 'rgba(255,255,255,0.03)',
        border: `1px solid ${isActive ? 'rgba(0,229,255,0.2)' : 'rgba(255,255,255,0.05)'}`,
        boxShadow: isActive ? '0 0 20px rgba(0,229,255,0.05)' : 'none',
        display: 'flex', flexDirection: 'column', transition: 'all 0.3s'
      }}>
        <div style={{ fontSize: 9, fontWeight: 800, color: 'var(--t3)', letterSpacing: 1.5, marginBottom: 8 }}>WAKE WORD / AUDIO STATUS</div>
        <div style={{ display: 'flex', alignItems: 'center', color: statusColor, fontSize: 13, gap: 8, padding: '4px 0' }}>
          {isStreaming && (
            <span style={{ fontSize: 14, textShadow: `0 0 8px ${statusColor}` }}>&#9679;</span>
          )}
          {isWake && (
            <span style={{ fontSize: 14, textShadow: '0 0 8px var(--green)' }}>&#10003;</span>
          )}
          {!isActive && <IconSearch size={14} color="var(--t4)" />}
          {statusText}
        </div>
      </div>
    </div>
  )
}
