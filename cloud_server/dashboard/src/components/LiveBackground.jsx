import { useEffect, useRef } from 'react'

export default function LiveBackground() {
  const canvasRef = useRef(null)

  useEffect(() => {
    const canvas = canvasRef.current
    if (!canvas) return
    const ctx = canvas.getContext('2d')
    let animId
    let W, H

    let shootingStars = []
    let ambientStars = []
    let t = 0

    const resize = () => {
      W = canvas.width = window.innerWidth
      H = canvas.height = window.innerHeight
      initAmbientStars()
    }

    function initAmbientStars() {
      ambientStars = Array.from({ length: 400 }, () => ({
        x: Math.random() * W,
        y: Math.random() * H,
        r: 0.5 + Math.random() * 1.5,
        baseAlpha: 0.2 + Math.random() * 0.8,
        twinkleSpeed: 0.005 + Math.random() * 0.02,
        twinkleOffset: Math.random() * Math.PI * 2,
        color: Math.random() > 0.5 ? 'rgba(255,255,255,' : 'rgba(180,240,255,',
        vx: -0.05 - Math.random() * 0.15, // Drifting left
        vy: -0.02 - Math.random() * 0.05  // Drifting slightly up
      }))
    }

    function spawnShootingStar() {
      const angle = (Math.random() * 30 + 15) * (Math.PI / 180)
      const speed = 15 + Math.random() * 20
      shootingStars.push({
        x: Math.random() * W * 0.8,
        y: Math.random() * H * 0.3,
        vx: Math.cos(angle) * speed,
        vy: Math.sin(angle) * speed,
        len: 80 + Math.random() * 150,
        alpha: 1,
        width: 1 + Math.random() * 2,
        life: 1,
        decay: 0.015 + Math.random() * 0.02,
      })
    }

    function draw() {
      t++
      
      // Dark space background
      ctx.fillStyle = '#03030a'
      ctx.fillRect(0, 0, W, H)

      // Ambient Twinkling Stars
      ambientStars.forEach(s => {
        // Move stars
        s.x += s.vx
        s.y += s.vy
        
        // Wrap around edges
        if (s.x < 0) s.x = W
        if (s.x > W) s.x = 0
        if (s.y < 0) s.y = H
        if (s.y > H) s.y = 0

        const a = s.baseAlpha * (0.5 + 0.5 * Math.sin(t * s.twinkleSpeed + s.twinkleOffset))
        ctx.beginPath()
        ctx.arc(s.x, s.y, s.r, 0, Math.PI * 2)
        ctx.fillStyle = `${s.color}${a.toFixed(2)})`
        ctx.fill()
        
        if (s.r > 1.2 && a > 0.5) {
          ctx.shadowColor = s.color + '0.8)'
          ctx.shadowBlur = s.r * 4
          ctx.fill()
          ctx.shadowBlur = 0
        }
      })

      // Shooting Stars
      if (t % 180 === 0 && shootingStars.length < 2) spawnShootingStar()
      
      shootingStars.forEach((s, i) => {
        s.x += s.vx; s.y += s.vy
        s.life -= s.decay
        const a = Math.max(0, s.life)

        const grad = ctx.createLinearGradient(s.x, s.y, s.x - s.vx * s.len / s.vx, s.y - s.vy * s.len / s.vx)
        grad.addColorStop(0, `rgba(255,255,255,${a.toFixed(2)})`)
        grad.addColorStop(0.2, `rgba(0,220,255,${(a * 0.6).toFixed(2)})`)
        grad.addColorStop(1, 'rgba(0,100,255,0)')
        
        ctx.beginPath()
        ctx.moveTo(s.x, s.y)
        ctx.lineTo(s.x - (s.vx / Math.sqrt(s.vx ** 2 + s.vy ** 2)) * s.len,
                   s.y - (s.vy / Math.sqrt(s.vx ** 2 + s.vy ** 2)) * s.len)
        ctx.strokeStyle = grad
        ctx.lineWidth = s.width
        ctx.shadowColor = 'rgba(0,220,255,0.8)'
        ctx.shadowBlur = 12
        ctx.stroke()
        ctx.shadowBlur = 0

        ctx.beginPath()
        ctx.arc(s.x, s.y, s.width * 1.5, 0, Math.PI * 2)
        ctx.fillStyle = `rgba(255,255,255,${a.toFixed(2)})`
        ctx.fill()

        if (s.life <= 0 || s.x > W + 200 || s.y > H + 200) shootingStars.splice(i, 1)
      })

      animId = requestAnimationFrame(draw)
    }

    window.addEventListener('resize', resize)
    resize()
    draw()

    return () => {
      cancelAnimationFrame(animId)
      window.removeEventListener('resize', resize)
    }
  }, [])

  return (
    <canvas
      ref={canvasRef}
      style={{
        position: 'fixed',
        inset: 0,
        zIndex: 0,
        display: 'block',
        width: '100vw',
        height: '100vh',
      }}
      aria-hidden="true"
    />
  )
}
