import { useState } from 'react'
import { IconSettings, IconX, IconCpu, IconWifi } from './Icons.jsx'
import '../App.css' // Import global styles just in case, but we rely on classes

export default function SettingsModal({ onClose }) {
  const [conf, setConf] = useState(85)
  const [gain, setGain] = useState(50)
  const [mode, setMode] = useState('PERFORMANCE')

  return (
    <div style={{
      position: 'fixed', top: 0, left: 0, width: '100vw', height: '100vh',
      background: 'rgba(0,0,0,0.5)',
      backdropFilter: 'blur(8px)',
      zIndex: 100,
      display: 'flex', alignItems: 'center', justifyContent: 'center',
      animation: 'modal-fade-in 0.2s ease-out'
    }}>
      <div className="panel" style={{
        width: '100%', maxWidth: 440,
        padding: 32,
        position: 'relative',
        boxShadow: '0 24px 48px rgba(0,0,0,0.6), inset 0 1px 0 rgba(255,255,255,0.05)',
        animation: 'modal-slide-up 0.3s cubic-bezier(0.16, 1, 0.3, 1)'
      }}>
        
        {/* ── Header ── */}
        <div style={{ 
          display: 'flex', justifyContent: 'space-between', alignItems: 'center', 
          borderBottom: '1px solid var(--border)', paddingBottom: 20, marginBottom: 24 
        }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 12 }}>
            <div style={{
              display: 'flex', alignItems: 'center', justifyContent: 'center',
              width: 32, height: 32, borderRadius: 8,
              background: 'var(--primary-dim)', color: 'var(--primary)'
            }}>
              <IconSettings size={18} />
            </div>
            <div>
              <div style={{ fontSize: 16, fontWeight: 700, color: 'var(--t1)', letterSpacing: '-0.2px' }}>
                System Settings
              </div>
              <div style={{ fontSize: 11, color: 'var(--t3)' }}>
                Configure hardware profiles & audio parameters
              </div>
            </div>
          </div>
          <button 
            onClick={onClose} 
            style={{ 
              background: 'transparent', border: 'none', color: 'var(--t3)', 
              display: 'flex', alignItems: 'center', justifyContent: 'center',
              width: 32, height: 32, borderRadius: 8, cursor: 'pointer', transition: '0.2s' 
            }} 
            onMouseOver={e => { e.currentTarget.style.color = 'var(--t1)'; e.currentTarget.style.background = 'var(--border)'; }} 
            onMouseOut={e => { e.currentTarget.style.color = 'var(--t3)'; e.currentTarget.style.background = 'transparent'; }}
          >
            <IconX size={16} />
          </button>
        </div>

        {/* ── Settings Content ── */}
        <div style={{ display: 'flex', flexDirection: 'column', gap: 24 }}>
          
          {/* Sliders */}
          <div style={{ display: 'flex', flexDirection: 'column', gap: 16 }}>
            <div>
              <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: 8, alignItems: 'center' }}>
                <span style={{ fontSize: 11, fontWeight: 600, color: 'var(--t2)', textTransform: 'uppercase', letterSpacing: '0.5px' }}>
                  Wake-Word Confidence
                </span>
                <span style={{ fontFamily: 'var(--font-mono)', fontSize: 13, color: 'var(--t1)', fontWeight: 600 }}>{conf}%</span>
              </div>
              <input type="range" min="50" max="100" value={conf} onChange={e => setConf(e.target.value)} className="slider" style={{ '--accent': 'var(--primary)' }} />
            </div>

            <div>
              <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: 8, alignItems: 'center' }}>
                <span style={{ fontSize: 11, fontWeight: 600, color: 'var(--t2)', textTransform: 'uppercase', letterSpacing: '0.5px' }}>
                  Microphone I2S Gain
                </span>
                <span style={{ fontFamily: 'var(--font-mono)', fontSize: 13, color: 'var(--t1)', fontWeight: 600 }}>{gain}dB</span>
              </div>
              <input type="range" min="0" max="100" value={gain} onChange={e => setGain(e.target.value)} className="slider" style={{ '--accent': 'var(--sky)' }} />
            </div>
          </div>

          <div style={{ height: 1, background: 'var(--border)', margin: '4px 0' }} />

          {/* Mode Toggles */}
          <div>
            <div style={{ fontSize: 11, fontWeight: 600, color: 'var(--t2)', textTransform: 'uppercase', letterSpacing: '0.5px', marginBottom: 12 }}>
              Hardware Power Profile
            </div>
            <div style={{ display: 'flex', gap: 12 }}>
              <button 
                onClick={() => setMode('ECO')}
                style={{ 
                  flex: 1, padding: '12px', display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 6,
                  background: mode === 'ECO' ? 'var(--green-dim)' : 'transparent', 
                  color: mode === 'ECO' ? 'var(--green)' : 'var(--t3)', 
                  border: `1px solid ${mode === 'ECO' ? 'var(--green)' : 'var(--border)'}`, 
                  borderRadius: 12, cursor: 'pointer', transition: 'all 0.2s',
                  boxShadow: mode === 'ECO' ? '0 0 16px rgba(74,222,128,0.1)' : 'none'
                }}
              >
                <IconWifi size={18} />
                <span style={{ fontSize: 11, fontWeight: 700, letterSpacing: '0.5px' }}>ECO MODE</span>
              </button>
              <button 
                onClick={() => setMode('PERFORMANCE')}
                style={{ 
                  flex: 1, padding: '12px', display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 6,
                  background: mode === 'PERFORMANCE' ? 'var(--primary-dim)' : 'transparent', 
                  color: mode === 'PERFORMANCE' ? 'var(--primary)' : 'var(--t3)', 
                  border: `1px solid ${mode === 'PERFORMANCE' ? 'var(--primary)' : 'var(--border)'}`, 
                  borderRadius: 12, cursor: 'pointer', transition: 'all 0.2s',
                  boxShadow: mode === 'PERFORMANCE' ? '0 0 16px rgba(124,106,247,0.1)' : 'none'
                }}
              >
                <IconCpu size={18} />
                <span style={{ fontSize: 11, fontWeight: 700, letterSpacing: '0.5px' }}>PERFORMANCE</span>
              </button>
            </div>
          </div>

          {/* Danger Zone */}
          <div style={{ marginTop: 8 }}>
            <button style={{ 
              width: '100%',
              display: 'flex', alignItems: 'center', justifyContent: 'center', gap: 8,
              background: 'var(--red-dim)', color: 'var(--red)', border: '1px solid rgba(248,113,113,0.3)', 
              padding: '14px', borderRadius: 12, fontSize: 12, fontWeight: 700, letterSpacing: '1px', 
              cursor: 'pointer', transition: '0.2s', textTransform: 'uppercase'
            }} onMouseOver={e => { e.currentTarget.style.background = 'var(--red)'; e.currentTarget.style.color = '#fff'; e.currentTarget.style.boxShadow = '0 0 16px rgba(248,113,113,0.4)'; }} onMouseOut={e => { e.currentTarget.style.background = 'var(--red-dim)'; e.currentTarget.style.color = 'var(--red)'; e.currentTarget.style.boxShadow = 'none'; }}>
              Remote Reboot Node
            </button>
          </div>

        </div>

        <style>{`
          @keyframes modal-fade-in {
            from { opacity: 0; }
            to { opacity: 1; }
          }
          @keyframes modal-slide-up {
            from { opacity: 0; transform: translateY(20px) scale(0.95); }
            to { opacity: 1; transform: translateY(0) scale(1); }
          }
          
          .slider {
            -webkit-appearance: none; 
            width: 100%; 
            background: transparent; 
            height: 6px; 
            border-radius: 3px;
          }
          .slider::-webkit-slider-runnable-track {
            width: 100%; height: 6px; background: rgba(255,255,255,0.05); border-radius: 3px;
            box-shadow: inset 0 1px 2px rgba(0,0,0,0.3);
          }
          .slider::-webkit-slider-thumb {
            -webkit-appearance: none; height: 18px; width: 18px; border-radius: 50%;
            background: var(--accent); box-shadow: 0 0 12px var(--accent);
            margin-top: -6px; cursor: pointer; border: 2px solid #fff;
            transition: transform 0.1s;
          }
          .slider::-webkit-slider-thumb:hover {
            transform: scale(1.15);
          }
        `}</style>
      </div>
    </div>
  )
}
