import { useEffect, useState } from 'react'

export default function NodeTopology({ serverUp, latestEvent }) {
  const [blip, setBlip] = useState(false)
  
  useEffect(() => {
    if (latestEvent) {
      setBlip(true)
      const t = setTimeout(() => setBlip(false), 2000)
      return () => clearTimeout(t)
    }
  }, [latestEvent])

  return (
    <div style={{ height: '100%', display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center' }}>
      
      {/* Radar Container */}
      <div className="radar-container" style={{ position: 'relative', width: 200, height: 200, borderRadius: '50%', border: '1px solid var(--accent-cyan)', background: 'rgba(0, 240, 255, 0.05)', boxShadow: '0 0 20px rgba(0, 240, 255, 0.2), inset 0 0 20px rgba(0, 240, 255, 0.1)' }}>
        
        {/* Grid lines */}
        <div style={{ position: 'absolute', top: '50%', width: '100%', height: 1, background: 'rgba(0, 240, 255, 0.2)' }}></div>
        <div style={{ position: 'absolute', left: '50%', width: 1, height: '100%', background: 'rgba(0, 240, 255, 0.2)' }}></div>
        <div style={{ position: 'absolute', top: '25%', left: '25%', width: '50%', height: '50%', borderRadius: '50%', border: '1px solid rgba(0, 240, 255, 0.2)' }}></div>

        {/* Sweeper */}
        <div className="radar-sweep" style={{
          position: 'absolute', top: '50%', left: '50%', width: '50%', height: 2, 
          background: 'linear-gradient(90deg, rgba(0,240,255,1) 0%, rgba(0,240,255,0) 100%)',
          transformOrigin: 'left center', animation: 'spin 3s linear infinite'
        }}>
          {/* Sweep trailing glow */}
          <div style={{ position: 'absolute', top: 0, left: 0, width: '100%', height: 60, background: 'conic-gradient(from 180deg at 0 0, rgba(0,240,255,0.4) 0%, transparent 40%)', transformOrigin: '0 0', transform: 'rotate(90deg)' }}></div>
        </div>

        {/* Center Node (Cloud) */}
        <div style={{ position: 'absolute', top: '50%', left: '50%', transform: 'translate(-50%, -50%)', width: 10, height: 10, background: 'var(--accent-magenta)', borderRadius: '50%', boxShadow: '0 0 10px var(--accent-magenta)' }}></div>

        {/* Blip (Edge Node) */}
        {blip && (
          <div className="blip" style={{ 
            position: 'absolute', top: '30%', left: '70%', width: 8, height: 8, 
            background: 'var(--text-main)', borderRadius: '50%', 
            boxShadow: '0 0 10px #fff', animation: 'ping 1s ease-out infinite' 
          }}></div>
        )}
      </div>

      <style>{`
        @keyframes spin { 100% { transform: rotate(360deg); } }
        @keyframes ping { 0% { transform: scale(1); opacity: 1; } 100% { transform: scale(3); opacity: 0; } }
      `}</style>

    </div>
  )
}
