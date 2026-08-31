import { useEffect, useState, useRef } from 'react'

export default function LatencyHistory({ events }) {
  const canvasRef = useRef(null)

  useEffect(() => {
    const canvas = canvasRef.current
    if (!canvas) return
    const ctx = canvas.getContext('2d')
    const w = canvas.width = canvas.offsetWidth
    const h = canvas.height = canvas.offsetHeight

    // Clear
    ctx.clearRect(0, 0, w, h)

    // Draw Grid
    ctx.strokeStyle = 'rgba(255,255,255,0.05)'
    ctx.lineWidth = 1
    for (let i = 0; i < 5; i++) {
      const y = i * (h / 4)
      ctx.beginPath()
      ctx.moveTo(0, y)
      ctx.lineTo(w, y)
      ctx.stroke()
    }

    if (events.length === 0) {
      ctx.fillStyle = 'rgba(255,255,255,0.5)'
      ctx.font = '10px Inter'
      ctx.textAlign = 'center'
      ctx.fillText('NO LATENCY DATA YET', w/2, h/2)
      return
    }

    // Extract latency values
    // We only plot the last 20 events to keep the chart legible
    const recentEvents = events.slice(-20)
    const data = recentEvents.map(e => (e.kw_to_connect_ms||0)+(e.receive_gap_ms||0)+(e.transcribe_ms||0))
    const maxVal = Math.max(1000, ...data)

    // Plot Line
    ctx.beginPath()
    data.forEach((val, i) => {
      const x = i * (w / Math.max(1, data.length - 1))
      const y = h - (val / maxVal) * h
      if (i === 0) ctx.moveTo(x, y)
      else ctx.lineTo(x, y)
    })
    
    ctx.strokeStyle = 'var(--accent-magenta)'
    ctx.lineWidth = 2
    ctx.stroke()

    // Draw area under line
    ctx.lineTo(w, h)
    ctx.lineTo(0, h)
    const gradient = ctx.createLinearGradient(0, 0, 0, h)
    gradient.addColorStop(0, 'rgba(255, 0, 85, 0.4)')
    gradient.addColorStop(1, 'transparent')
    ctx.fillStyle = gradient
    ctx.fill()

    // Draw Points
    data.forEach((val, i) => {
      const x = i * (w / Math.max(1, data.length - 1))
      const y = h - (val / maxVal) * h
      ctx.beginPath()
      ctx.arc(x, y, 3, 0, Math.PI * 2)
      ctx.fillStyle = val > 500 ? 'var(--accent-magenta)' : 'var(--accent-cyan)'
      ctx.fill()
    })

  }, [events])

  return (
    <div style={{ width: '100%', height: '100%', padding: '16px', display: 'flex', flexDirection: 'column' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: 16 }}>
        <div style={{ fontSize: 10, color: 'var(--text-muted)' }}>HISTORICAL E2E LATENCY (LAST 20 EVENTS)</div>
        <div style={{ fontSize: 10, color: 'var(--accent-magenta)' }}>PEAK: {Math.max(0, ...events.map(e => (e.kw_to_connect_ms||0)+(e.receive_gap_ms||0)+(e.transcribe_ms||0)))}ms</div>
      </div>
      <div style={{ flex: 1, position: 'relative' }}>
        <canvas ref={canvasRef} style={{ width: '100%', height: '100%', display: 'block' }}></canvas>
      </div>
    </div>
  )
}
