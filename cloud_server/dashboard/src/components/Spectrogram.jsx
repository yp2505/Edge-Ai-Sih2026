import { useEffect, useRef, useState } from 'react'

export default function Spectrogram({ latestEvent }) {
  const canvasRef = useRef(null)
  const [transcript, setTranscript] = useState('')
  const [isActive, setIsActive] = useState(false)

  useEffect(() => {
    if (latestEvent) {
      setTranscript(latestEvent.transcript)
      setIsActive(true)
      const t = setTimeout(() => setIsActive(false), 3000)
      return () => clearTimeout(t)
    }
  }, [latestEvent])

  useEffect(() => {
    const canvas = canvasRef.current
    const ctx = canvas.getContext('2d')
    let w = canvas.width = canvas.offsetWidth
    let h = canvas.height = canvas.offsetHeight
    let animationId
    let time = 0

    const draw = () => {
      ctx.clearRect(0, 0, w, h)
      
      const cx = w / 2
      const cy = h / 2
      const radius = 100
      const bands = 64
      
      // Draw Circular Neural Core
      ctx.beginPath()
      for (let i = 0; i <= bands; i++) {
        const angle = (i / bands) * Math.PI * 2
        
        // Generate radial noise
        const baseNoise = Math.sin(time * 2 + i * 0.2) * 5 + Math.cos(time + i * 0.5) * 5
        let amp = 5 + baseNoise
        
        if (isActive) {
          amp += Math.random() * 60 + 20
        }

        const r = radius + amp
        const x = cx + Math.cos(angle) * r
        const y = cy + Math.sin(angle) * r

        if (i === 0) ctx.moveTo(x, y)
        else ctx.lineTo(x, y)
      }
      ctx.closePath()
      
      // Neon Glow
      const glow = ctx.createRadialGradient(cx, cy, radius * 0.5, cx, cy, radius * 2)
      glow.addColorStop(0, isActive ? 'rgba(255, 0, 85, 0.8)' : 'rgba(0, 240, 255, 0.2)')
      glow.addColorStop(1, 'transparent')

      ctx.fillStyle = glow
      ctx.fill()
      
      ctx.strokeStyle = isActive ? '#ff0055' : '#00f0ff'
      ctx.lineWidth = isActive ? 3 : 1
      ctx.stroke()
      
      // Inner solid core
      ctx.beginPath()
      ctx.arc(cx, cy, radius * 0.8, 0, Math.PI * 2)
      ctx.fillStyle = '#000'
      ctx.fill()
      ctx.strokeStyle = 'rgba(255,255,255,0.1)'
      ctx.stroke()

      time += 0.05
      animationId = requestAnimationFrame(draw)
    }

    draw()
    
    const handleResize = () => {
      w = canvas.width = canvas.offsetWidth
      h = canvas.height = canvas.offsetHeight
    }
    window.addEventListener('resize', handleResize)
    
    return () => {
      cancelAnimationFrame(animationId)
      window.removeEventListener('resize', handleResize)
    }
  }, [isActive])

  return (
    <div style={{ position: 'relative', width: '100%', height: '100%' }}>
      <canvas ref={canvasRef} style={{ width: '100%', height: '100%', display: 'block' }} />
      
      {/* Overlay Data */}
      <div style={{ position: 'absolute', top: 16, left: 16, display: 'flex', gap: 20 }}>
        <div style={{ background: 'rgba(255,255,255,0.05)', padding: '6px 12px', borderRadius: 8, border: '1px solid rgba(255,255,255,0.1)', backdropFilter: 'blur(10px)' }}>
          <span style={{ fontSize: 9, color: 'var(--text-muted)' }}>CONFIDENCE</span>
          <div className="mono val" style={{ color: isActive ? 'var(--accent-cyan)' : 'var(--text-main)', textShadow: isActive ? '0 0 10px var(--accent-cyan)' : 'none' }}>
            {isActive ? (92 + Math.random()*7).toFixed(1) : '--'}<span className="unit">%</span>
          </div>
        </div>
        <div style={{ background: 'rgba(255,255,255,0.05)', padding: '6px 12px', borderRadius: 8, border: '1px solid rgba(255,255,255,0.1)', backdropFilter: 'blur(10px)' }}>
          <span style={{ fontSize: 9, color: 'var(--text-muted)' }}>SNR</span>
          <div className="mono val">{isActive ? (22 + Math.random()*5).toFixed(1) : '--'}<span className="unit">dB</span></div>
        </div>
      </div>

      <div style={{ position: 'absolute', bottom: 30, width: '100%', textAlign: 'center', pointerEvents: 'none' }}>
        <div style={{ 
          display: 'inline-block', background: 'rgba(0,0,0,0.4)', backdropFilter: 'blur(10px)',
          padding: '12px 32px', borderRadius: 30, border: `1px solid ${isActive ? 'var(--accent-magenta)' : 'var(--panel-border)'}`,
          fontSize: 24, fontWeight: 700, opacity: isActive ? 1 : 0.4, transition: '0.3s',
          boxShadow: isActive ? '0 0 30px rgba(255, 0, 85, 0.4)' : 'none'
        }}>
          {isActive ? transcript : 'AWAITING AUDIO'}
        </div>
      </div>
    </div>
  )
}
