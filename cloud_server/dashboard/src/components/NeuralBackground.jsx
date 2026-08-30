import { useEffect, useRef } from 'react'

export default function NeuralBackground({ isActive }) {
  const canvasRef = useRef(null)

  useEffect(() => {
    const canvas = canvasRef.current
    const ctx = canvas.getContext('2d')
    let w, h
    let particles = []

    const init = () => {
      w = canvas.width = window.innerWidth
      h = canvas.height = window.innerHeight
      particles = Array.from({ length: 80 }).map(() => ({
        x: Math.random() * w,
        y: Math.random() * h,
        vx: (Math.random() - 0.5) * 0.5,
        vy: (Math.random() - 0.5) * 0.5,
        size: Math.random() * 2 + 1
      }))
    }

    const draw = () => {
      ctx.clearRect(0, 0, w, h)
      
      const speedMult = isActive ? 5 : 1
      const connectDist = isActive ? 200 : 120
      
      particles.forEach((p, i) => {
        p.x += p.vx * speedMult
        p.y += p.vy * speedMult
        
        if (p.x < 0 || p.x > w) p.vx *= -1
        if (p.y < 0 || p.y > h) p.vy *= -1

        ctx.beginPath()
        ctx.arc(p.x, p.y, p.size, 0, Math.PI * 2)
        ctx.fillStyle = isActive ? 'rgba(0, 122, 255, 0.8)' : 'rgba(255, 255, 255, 0.2)'
        ctx.fill()

        for (let j = i + 1; j < particles.length; j++) {
          const p2 = particles[j]
          const dx = p.x - p2.x
          const dy = p.y - p2.y
          const dist = Math.sqrt(dx * dx + dy * dy)

          if (dist < connectDist) {
            ctx.beginPath()
            ctx.moveTo(p.x, p.y)
            ctx.lineTo(p2.x, p2.y)
            const alpha = 1 - (dist / connectDist)
            ctx.strokeStyle = isActive ? `rgba(0, 122, 255, ${alpha * 0.5})` : `rgba(255, 255, 255, ${alpha * 0.1})`
            ctx.lineWidth = 1
            ctx.stroke()
          }
        }
      })
      requestAnimationFrame(draw)
    }

    init()
    const animId = requestAnimationFrame(draw)
    
    window.addEventListener('resize', init)
    return () => {
      cancelAnimationFrame(animId)
      window.removeEventListener('resize', init)
    }
  }, [isActive])

  return (
    <canvas 
      ref={canvasRef} 
      style={{ position: 'absolute', top: 0, left: 0, width: '100%', height: '100%', zIndex: 0 }}
    />
  )
}
