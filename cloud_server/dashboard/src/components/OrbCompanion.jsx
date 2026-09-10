import { useEffect, useRef, useState } from 'react'

export default function OrbCompanion({ latest, serverUp, pingCount }) {
  const [state, setState] = useState('sleeping') // sleeping | listening | processing | detected
  const [label, setLabel] = useState('Offline')
  const [particles, setParticles] = useState([])
  const prevPing = useRef(pingCount)
  const timers = useRef([])

  const clearTimers = () => { timers.current.forEach(clearTimeout); timers.current = [] }
  const addTimer = (fn, ms) => { const t = setTimeout(fn, ms); timers.current.push(t); return t }

  const burst = () => {
    const p = Array.from({ length: 12 }, (_, i) => ({
      id: Date.now() + i,
      angle: (i / 12) * 360,
      dist: 80 + Math.random() * 50,
      size: 4 + Math.random() * 5,
      color: ['var(--cyan)', 'var(--teal)', 'var(--ice)', 'var(--green)'][i % 4],
    }))
    setParticles(p)
    addTimer(() => setParticles([]), 1400)
  }

  useEffect(() => {
    const isNew = pingCount !== prevPing.current
    if (isNew) prevPing.current = pingCount

    if (!serverUp) { clearTimers(); setState('sleeping'); setLabel('Offline'); return }

    if (isNew) {
      clearTimers()
      setState('processing')
      setLabel('Processing…')
      addTimer(() => {
        setState('detected')
        setLabel('Got it!')
        burst()
        addTimer(() => {
          setState('listening')
          setLabel('Listening…')
        }, 2800)
      }, 1000)
      return
    }

    if (state !== 'detected' && state !== 'processing') {
      setState('listening')
      setLabel('Listening…')
    }

    return clearTimers
  }, [serverUp, pingCount, latest])

  const isDetected = state === 'detected'
  const isListening = state === 'listening'
  const isProcessing = state === 'processing'
  const isSleeping = state === 'sleeping'

  const orbGradient = isSleeping
    ? 'radial-gradient(circle at 35% 30%, #2a3a5a 0%, #0f1b2b 50%, #050a10 100%)'
    : isDetected
    ? 'radial-gradient(circle at 35% 30%, #00ffff 0%, #00bfff 40%, #004488 100%)'
    : isProcessing
    ? 'radial-gradient(circle at 35% 30%, #ff00ff 0%, #aa00ff 40%, #440088 100%)' // Purplish when processing
    : 'radial-gradient(circle at 35% 30%, #00d4ff 0%, #0088cc 45%, #002255 100%)'

  const orbShadow = isSleeping
    ? '0 0 20px rgba(0,40,80,0.4), inset -10px -10px 20px rgba(0,0,0,0.8)'
    : isDetected
    ? '0 0 50px rgba(0,212,255,0.8), 0 0 100px rgba(0,212,255,0.4), inset -15px -15px 30px rgba(0,60,120,0.8)'
    : isProcessing
    ? '0 0 40px rgba(170,0,255,0.6), inset -12px -12px 25px rgba(50,0,100,0.8)'
    : '0 0 30px rgba(0,212,255,0.5), inset -12px -12px 25px rgba(0,40,80,0.7)'

  const slimeAnim = isSleeping
    ? 'slime-wobble 6s ease-in-out infinite'
    : isProcessing
    ? 'slime-wobble-fast 1s ease-in-out infinite'
    : isDetected
    ? 'slime-wobble-fast 0.6s ease-in-out infinite'
    : 'slime-wobble 2.5s ease-in-out infinite'

  return (
    <div className="orb-container" style={{ flex: 1, minHeight: 0 }}>

      {/* Rings */}
      {(isListening || isDetected || isProcessing) && (
        <div className="orb-rings" style={{
          position: 'absolute',
          width: 160, height: 160,
        }}>
          {[0, 1, 2].map(i => (
            <div key={i} className="orb-ring" style={{
              width: 110 + i * 45,
              height: 110 + i * 45,
              borderColor: isDetected ? 'rgba(0,212,255,0.3)' : 'rgba(0,212,255,0.12)',
              animationDelay: `${i * 0.7}s`,
              animationDuration: isDetected ? '1.8s' : '3s',
            }} />
          ))}
        </div>
      )}

      {/* Particles burst */}
      {particles.map(p => (
        <div key={p.id} style={{
          position: 'absolute',
          width: p.size, height: p.size,
          borderRadius: '50%',
          background: p.color,
          boxShadow: `0 0 ${p.size * 2}px ${p.color}`,
          transform: `rotate(${p.angle}deg) translateX(${p.dist}px)`,
          animation: 'fade-in 0.1s ease, fade-out 1.3s ease forwards',
          opacity: 0,
          animationFillMode: 'forwards',
        }} />
      ))}

      {/* Orb */}
      <div className="orb-sphere" style={{ background: orbGradient, boxShadow: orbShadow, animation: slimeAnim }}>
        <div className="orb-sphere-shine" />
        <div className="orb-face">
          <div className="orb-eyes">
            {isSleeping ? (
              <>
                <div style={{ width: 12, height: 3, background: 'rgba(255,255,255,0.2)', borderRadius: 3 }} />
                <div style={{ width: 12, height: 3, background: 'rgba(255,255,255,0.2)', borderRadius: 3 }} />
              </>
            ) : isProcessing ? (
              <>
                <div style={{ width: 10, height: 10, background: '#001a30', borderRadius: '50%', position: 'relative', animation: 'spin 1s linear infinite' }}>
                  <div style={{ position: 'absolute', top: 2, left: 2, width: 3, height: 3, background: 'rgba(255,255,255,0.9)', borderRadius: '50%' }} />
                </div>
                <div style={{ width: 10, height: 10, background: '#001a30', borderRadius: '50%', position: 'relative', animation: 'spin 1s linear infinite' }}>
                  <div style={{ position: 'absolute', top: 2, left: 2, width: 3, height: 3, background: 'rgba(255,255,255,0.9)', borderRadius: '50%' }} />
                </div>
              </>
            ) : (
              <>
                <div className="orb-eye" />
                <div className="orb-eye" />
              </>
            )}
          </div>
          {isDetected && (
            <div style={{ width: 22, height: 6, border: '2px solid rgba(0,0,0,0.4)', borderTop: 'none', borderRadius: '0 0 12px 12px', marginTop: 4 }} />
          )}
        </div>
      </div>

      {/* ZZZs */}
      {isSleeping && (
        <div style={{ position: 'absolute', top: '25%', right: '28%', display: 'flex', flexDirection: 'column', gap: 4, alignItems: 'flex-end' }}>
          {['z','z','Z'].map((c, i) => (
            <span key={i} style={{
              fontSize: 10 + i * 4, fontWeight: 700,
              color: 'var(--t4)',
              animation: `fade-in 0.5s ease ${i * 0.4}s infinite alternate`,
            }}>{c}</span>
          ))}
        </div>
      )}

      {/* Status Pill */}
      <div className="orb-status" style={{
        background: isDetected ? 'rgba(0,212,255,0.12)' : isListening ? 'rgba(0,212,255,0.06)' : 'rgba(255,255,255,0.04)',
        borderColor: isDetected ? 'rgba(0,212,255,0.3)' : isListening ? 'rgba(0,212,255,0.15)' : 'rgba(255,255,255,0.06)',
        color: isSleeping ? 'var(--t4)' : 'var(--cyan)',
      }}>
        <div className="orb-status-dot" style={{
          background: isSleeping ? 'var(--t4)' : isDetected ? 'var(--green)' : 'var(--cyan)',
          boxShadow: isSleeping ? 'none' : `0 0 6px ${isDetected ? 'var(--green)' : 'var(--cyan)'}`,
          animation: isSleeping ? 'none' : 'live-pulse 1.5s infinite',
        }} />
        {label}
      </div>
    </div>
  )
}
