import { useEffect, useRef, useState } from 'react'

export default function OrbPanel({ latest, up, pings }) {
  const [state, setState] = useState('offline')
  const [label, setLabel] = useState('OFFLINE')
  const [sub, setSub] = useState('Server disconnected')
  const [burst, setBurst] = useState([])
  const prev = useRef(pings); const timers = useRef([])

  const clr = () => { timers.current.forEach(clearTimeout); timers.current = [] }
  const delay = (fn, ms) => { const t = setTimeout(fn, ms); timers.current.push(t) }

  const doBurst = () => {
    setBurst(Array.from({ length: 14 }, (_, i) => ({
      id: Date.now() + i, angle: (i / 14) * 360,
      dist: 65 + Math.random() * 45, size: 3.5 + Math.random() * 5,
      col: ['var(--c1)', 'var(--c2)', 'var(--c3)', 'var(--green)'][i % 4],
    })))
    delay(() => setBurst([]), 1300)
  }

  const prevSession = useRef(null)
  const prevStatus = useRef(null)

  useEffect(() => {
    if (!up) { clr(); setState('offline'); setLabel('OFFLINE'); setSub('Server disconnected'); return }

    const e = latest
    if (!e || (Date.now() - new Date(e.timestamp).getTime() > 6000)) {
       if (state !== 'idle') {
           setState('idle'); setLabel('ONLINE'); setSub('Waiting for wake word "Hey Vaani"')
       }
       return
    }

    if (e.session_id !== prevSession.current || e.status !== prevStatus.current) {
       prevSession.current = e.session_id
       prevStatus.current = e.status
       
       if (e.status === 'wake_word_detected') {
          clr(); setState('detect'); setLabel('LISTENING'); setSub('Streaming & transcribing...')
       } else if (e.status === 'complete') {
          clr(); setState('process'); setLabel('EXECUTING'); setSub(e.transcript && e.transcript.length < 20 ? `"${e.transcript}"` : 'Command finished'); doBurst()
          delay(() => { setState('idle'); setLabel('ONLINE'); setSub('Waiting for wake word "Hey Vaani"') }, 2500)
       }
    }
    return clr
  }, [up, latest, state])

  const off = state === 'offline'
  const det = state === 'detect'
  const proc = state === 'process'

  const grad = off
    ? 'radial-gradient(circle at 35% 30%, #0d1f30 0%, #060e18 50%, #020609 100%)'
    : det
    ? 'radial-gradient(circle at 35% 30%, #00f0ff 0%, #00b8e0 32%, #003c70 80%, #001830 100%)'
    : proc
    ? 'radial-gradient(circle at 35% 30%, #00d4f0 0%, #0090c0 35%, #002a60 80%, #001020 100%)'
    : 'radial-gradient(circle at 35% 30%, #00d4ff 0%, #0096cc 38%, #003060 80%, #00182e 100%)'

  const glow = off ? '0 0 20px rgba(0,0,0,0.6)' : det
    ? '0 0 50px rgba(0,229,255,0.65), 0 0 100px rgba(0,229,255,0.25)'
    : '0 0 32px rgba(0,229,255,0.38), 0 0 65px rgba(0,229,255,0.12)'

  return (
    <div style={{ flex: 1, display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'space-between', padding: '12px 10px', height: '100%', minHeight: 0 }}>
      {/* Top Labels */}
      <div style={{ textAlign: 'center', zIndex: 3 }}>
        <div style={{ fontFamily: 'var(--title)', fontSize: 16, fontWeight: 800, color: 'var(--t1)', letterSpacing: 2 }}>VAANI AI</div>
        <div style={{ fontSize: 11, fontWeight: 800, color: off ? 'var(--t4)' : det ? 'var(--green)' : proc ? 'var(--amber)' : 'var(--c1)', letterSpacing: 2, marginTop: 4 }}>{label}</div>
      </div>

      <div className="orb-wrap" style={{ flex: 'none', height: 86, margin: '6px 0' }}>
        {/* Sonar rings */}
        {!off && (
          <div className="orb-ring-wrap">
            {[0,1,2].map(i => (
              <div key={i} className="orb-ring" style={{
                width: 90 + i * 40, height: 90 + i * 40,
                borderColor: det ? 'rgba(0,229,255,0.3)' : 'rgba(0,229,255,0.1)',
                animationDelay: `${i * 0.75}s`,
                animationDuration: det ? '1.7s' : '3s',
              }} />
            ))}
          </div>
        )}

        {/* Burst particles */}
        {burst.map(p => (
          <div key={p.id} style={{
            position: 'absolute', width: p.size, height: p.size, borderRadius: '50%',
            background: p.col, boxShadow: `0 0 ${p.size * 2}px ${p.col}`,
            transform: `rotate(${p.angle}deg) translateX(${p.dist}px)`,
            animation: 'slide-up 0.1s ease forwards, fade-out 1.2s ease forwards',
            pointerEvents: 'none',
          }} />
        ))}

        {/* Sphere */}
        <div className="orb-sphere" style={{ width: 86, height: 86, borderRadius: '50%', background: grad, boxShadow: glow, animation: off ? 'none' : proc ? 'slime-wobble-fast 1s ease-in-out infinite' : 'slime-wobble 3s ease-in-out infinite' }}>
          <div className="orb-shine" />
          <div className="orb-face">
            <div className="orb-eyes">
              {off ? (
                <><div style={{ width:10, height:2, background:'rgba(255,255,255,0.15)', borderRadius:4 }}/>
                  <div style={{ width:10, height:2, background:'rgba(255,255,255,0.15)', borderRadius:4 }}/></>
              ) : proc ? (
                <>{[0,1].map(i => (
                  <div key={i} style={{ width:9, height:9, background:'rgba(0,5,20,0.9)', borderRadius:'50%', position:'relative', animation:'spin 0.9s linear infinite', animationDelay:`${i*0.1}s` }}>
                    <div style={{ position:'absolute', top:2, left:2, width:3, height:3, background:'rgba(255,255,255,0.9)', borderRadius:'50%' }} />
                  </div>
                ))}</>
              ) : (
                <><div className="orb-eye" style={{ width:8, height:10 }}/><div className="orb-eye" style={{ width:8, height:10 }}/></>
              )}
            </div>
            {det && <div style={{ width:18, height:5, border:'2px solid rgba(0,0,0,0.45)', borderTop:'none', borderRadius:'0 0 10px 10px', marginTop:4 }}/>}
          </div>
        </div>
      </div>

      {/* Subtext & Indicators */}
      <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 6, zIndex: 3, width: '100%' }}>
        <div style={{ fontSize: 11, color: 'var(--t3)', textAlign: 'center', minHeight: 14 }}>{sub}</div>
        
        <div style={{ display: 'flex', flexDirection: 'column', gap: 6, alignItems: 'flex-start', background: 'rgba(255,255,255,0.03)', padding: '8px 16px', borderRadius: 12, border: '1px solid rgba(255,255,255,0.06)', width: '90%', maxWidth: 200 }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 8, fontSize: 10, fontWeight: 700, color: up ? 'var(--t1)' : 'var(--t4)', letterSpacing: 0.5 }}>
            <div style={{ width: 6, height: 6, borderRadius: '50%', background: up ? 'var(--green)' : 'var(--t4)', boxShadow: up ? '0 0 6px var(--green)' : 'none' }} />
            Microphone Ready
          </div>
          <div style={{ display: 'flex', alignItems: 'center', gap: 8, fontSize: 10, fontWeight: 700, color: up ? 'var(--t1)' : 'var(--t4)', letterSpacing: 0.5 }}>
            <div style={{ width: 6, height: 6, borderRadius: '50%', background: up ? 'var(--green)' : 'var(--t4)', boxShadow: up ? '0 0 6px var(--green)' : 'none' }} />
            AI Engine Ready
          </div>
        </div>
      </div>
    </div>
  )
}
