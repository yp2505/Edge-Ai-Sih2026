import { useState } from 'react'

export default function SettingsModal({ onClose }) {
  const [conf, setConf] = useState(85)
  const [gain, setGain] = useState(50)
  const [mode, setMode] = useState('PERFORMANCE')

  return (
    <div style={{
      position: 'fixed', top: 0, left: 0, width: '100vw', height: '100vh',
      background: 'rgba(0,0,0,0.6)', backdropFilter: 'blur(10px)', zIndex: 100,
      display: 'flex', alignItems: 'center', justifyContent: 'center'
    }}>
      <div style={{
        width: 500, background: 'var(--panel-bg)', borderRadius: 16,
        border: '1px solid var(--accent-cyan)', boxShadow: '0 0 40px rgba(0, 240, 255, 0.2)',
        padding: 30, position: 'relative'
      }}>
        
        {/* Header */}
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', borderBottom: '1px solid rgba(255,255,255,0.1)', paddingBottom: 16, marginBottom: 20 }}>
          <div style={{ fontSize: 16, fontWeight: 700, letterSpacing: 2, color: 'var(--accent-cyan)' }}>COMMAND LINK // UPLINK SETTINGS</div>
          <button onClick={onClose} style={{ 
            background: 'transparent', border: 'none', color: 'var(--text-muted)', fontSize: 24, cursor: 'pointer', transition: '0.2s' 
          }} onMouseOver={e => e.target.style.color = '#fff'} onMouseOut={e => e.target.style.color = 'var(--text-muted)'}>×</button>
        </div>

        {/* Sliders */}
        <div style={{ display: 'flex', flexDirection: 'column', gap: 24 }}>
          
          <div>
            <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: 8 }}>
              <span style={{ fontSize: 10, color: 'var(--text-muted)' }}>WAKE-WORD CONFIDENCE THRESHOLD</span>
              <span className="mono val">{conf}%</span>
            </div>
            <input type="range" min="50" max="100" value={conf} onChange={e => setConf(e.target.value)} style={{ width: '100%', cursor: 'pointer' }} />
          </div>

          <div>
            <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: 8 }}>
              <span style={{ fontSize: 10, color: 'var(--text-muted)' }}>MICROPHONE I2S GAIN</span>
              <span className="mono val">{gain}dB</span>
            </div>
            <input type="range" min="0" max="100" value={gain} onChange={e => setGain(e.target.value)} style={{ width: '100%', cursor: 'pointer' }} />
          </div>

          {/* Mode Toggles */}
          <div>
            <div style={{ fontSize: 10, color: 'var(--text-muted)', marginBottom: 8 }}>HARDWARE POWER PROFILE</div>
            <div style={{ display: 'flex', gap: 10 }}>
              <button 
                onClick={() => setMode('ECO')}
                style={{ flex: 1, padding: '10px', background: mode === 'ECO' ? 'var(--accent-green)' : 'rgba(255,255,255,0.05)', color: mode === 'ECO' ? '#000' : 'var(--text-main)', border: mode === 'ECO' ? 'none' : '1px solid rgba(255,255,255,0.2)', borderRadius: 8, cursor: 'pointer', fontWeight: 600, transition: '0.2s' }}
              >ECO MODE</button>
              <button 
                onClick={() => setMode('PERFORMANCE')}
                style={{ flex: 1, padding: '10px', background: mode === 'PERFORMANCE' ? 'var(--accent-magenta)' : 'rgba(255,255,255,0.05)', color: mode === 'PERFORMANCE' ? '#000' : 'var(--text-main)', border: mode === 'PERFORMANCE' ? 'none' : '1px solid rgba(255,255,255,0.2)', borderRadius: 8, cursor: 'pointer', fontWeight: 600, transition: '0.2s' }}
              >PERFORMANCE</button>
            </div>
          </div>

          {/* Danger Zone */}
          <div style={{ marginTop: 20, paddingTop: 20, borderTop: '1px solid rgba(255,0,85,0.2)', textAlign: 'center' }}>
            <button style={{ 
              background: 'rgba(255,0,85,0.1)', color: 'var(--accent-magenta)', border: '1px solid var(--accent-magenta)', 
              padding: '10px 30px', borderRadius: 8, fontWeight: 700, letterSpacing: 2, cursor: 'pointer', transition: '0.2s'
            }} onMouseOver={e => { e.target.style.background = 'var(--accent-magenta)'; e.target.style.color = '#000'; }} onMouseOut={e => { e.target.style.background = 'rgba(255,0,85,0.1)'; e.target.style.color = 'var(--accent-magenta)'; }}>
              REMOTE REBOOT NODE
            </button>
          </div>

        </div>

        <style>{`
          input[type=range] {
            -webkit-appearance: none; background: transparent; height: 6px; border-radius: 3px;
          }
          input[type=range]::-webkit-slider-runnable-track {
            width: 100%; height: 6px; background: rgba(255,255,255,0.1); border-radius: 3px;
          }
          input[type=range]::-webkit-slider-thumb {
            -webkit-appearance: none; height: 16px; width: 16px; border-radius: 50%;
            background: var(--accent-cyan); box-shadow: 0 0 10px var(--accent-cyan);
            margin-top: -5px; cursor: pointer;
          }
        `}</style>
      </div>
    </div>
  )
}
