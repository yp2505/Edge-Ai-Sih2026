import { useEffect, useRef } from 'react'

export default function TopoBackground({ isActive }) {
  const canvasRef = useRef(null)

  useEffect(() => {
    const canvas = canvasRef.current
    const ctx = canvas.getContext('2d')
    let width, height
    
    // Mesh configuration
    const cols = 50
    const rows = 40
    let scale = 0
    let terrain = []
    let flying = 0

    const init = () => {
      width = canvas.width = window.innerWidth
      height = canvas.height = window.innerHeight
      scale = width / cols
      
      // Initialize terrain array
      terrain = new Array(cols).fill(0).map(() => new Array(rows).fill(0))
    }

    // Perlin-noise-like simple wave generator
    const generateTerrain = () => {
      flying -= 0.05 // Scroll speed
      let yoff = flying
      
      const maxHeight = isActive ? 150 : 20 // Spike height based on state
      
      for (let y = 0; y < rows; y++) {
        let xoff = 0
        for (let x = 0; x < cols; x++) {
          // Simple sine wave interference pattern for "organic" noise
          const val = Math.sin(xoff) * Math.cos(yoff) + Math.sin(xoff * 0.5 + yoff * 0.5)
          terrain[x][y] = val * maxHeight
          xoff += 0.2
        }
        yoff += 0.2
      }
    }

    const draw = () => {
      ctx.fillStyle = '#02040a'
      ctx.fillRect(0, 0, width, height)

      generateTerrain()

      // Isometric projection and rendering
      ctx.translate(width / 2, height / 3)
      ctx.rotate(Math.PI / 3)

      for (let y = 0; y < rows - 1; y++) {
        ctx.beginPath()
        for (let x = 0; x < cols; x++) {
          const xPos = (x - cols / 2) * scale
          const yPos = (y - rows / 2) * scale
          const zPos = terrain[x][y]

          // Project to 2D
          const screenX = xPos - yPos
          const screenY = (xPos + yPos) / 2 - zPos

          if (x === 0) ctx.moveTo(screenX, screenY)
          else ctx.lineTo(screenX, screenY)
        }
        
        // Dynamic coloring
        const gradient = ctx.createLinearGradient(-width/2, 0, width/2, 0)
        gradient.addColorStop(0, isActive ? 'rgba(0,240,255,0.0)' : 'rgba(100,100,150,0.0)')
        gradient.addColorStop(0.5, isActive ? 'rgba(0,240,255,0.8)' : 'rgba(100,100,150,0.3)')
        gradient.addColorStop(1, isActive ? 'rgba(0,240,255,0.0)' : 'rgba(100,100,150,0.0)')
        
        ctx.strokeStyle = gradient
        ctx.lineWidth = isActive ? 1.5 : 1
        ctx.stroke()
      }

      // Reset transform for next frame
      ctx.setTransform(1, 0, 0, 1, 0, 0)
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
