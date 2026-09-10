import { useState, useEffect } from 'react'
import { IconSettings, IconX, IconCpu, IconWifi, IconWaveform, IconReport, IconFlash } from './Icons.jsx'

const API = 'http://localhost:8080'

export default function SettingsModal({ onClose, telemetry, serverUp, health }) {
  const [conf, setConf] = useState(85)
  const [gain, setGain] = useState(50)
  const [mode, setMode] = useState('BALANCED')
  const [saving, setSaving] = useState(false)
  const [saveStatus, setSaveStatus] = useState(null) // 'success' | 'error' | null
  const [isModified, setIsModified] = useState(false)

  // Mark modified whenever a setting changes
  useEffect(() => {
    setIsModified(true)
  }, [conf, gain, mode])

  // Clear modified on initial mount (hacky but works since defaults are set first)
  useEffect(() => setIsModified(false), [])

  const handleSave = async () => {
    setSaving(true)
    setSaveStatus(null)
    try {
      // Simulate attempting to save to the real backend
      const r = await fetch(`${API}/api/config`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ conf, gain, mode })
      })
      if (!r.ok) {
        if (r.status === 404) throw new Error('Hardware control unavailable (Endpoint not found)')
        throw new Error('Failed to save configuration')
      }
      setSaveStatus('success')
      setIsModified(false)
    } catch (e) {
      setSaveStatus(e.message || 'Error saving')
    }
    setSaving(false)
    setTimeout(() => {
      setSaveStatus(null)
    }, 3000)
  }

  const handleReset = () => {
    setConf(85)
    setGain(50)
    setMode('BALANCED')
  }

  const S = {
    label: { fontSize: 9, fontWeight: 700, letterSpacing: 1.5, textTransform: 'uppercase', color: '#7F91A3', marginBottom: 6, display: 'block', fontFamily: 'var(--mono)' },
    input: { width: '100%', padding: '9px 14px', background: 'rgba(0,0,0,0.2)', border: '1px solid rgba(255,255,255,0.05)', borderRadius: 10, color: '#E8F1F7', fontSize: 13, outline: 'none', fontFamily: 'var(--mono)' },
    slider: { width: '100%', accentColor: '#00D9FF', cursor: 'pointer', marginTop: 3 },
    row: { marginBottom: 20 },
    divider: { height: 1, background: 'rgba(255,255,255,0.06)', margin: '18px 0' },
    modeBtn: a => ({ flex: 1, padding: '10px 0', background: a ? 'rgba(0,217,255,0.1)' : 'rgba(0,0,0,0.2)', border: `1px solid ${a ? 'rgba(0,217,255,0.3)' : 'rgba(255,255,255,0.05)'}`, borderRadius: 8, color: a ? '#00D9FF' : '#7F91A3', fontSize: 10, fontWeight: 700, letterSpacing: 1, cursor: 'pointer', transition: 'all 0.2s', display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 4, boxShadow: a ? '0 0 10px rgba(0,217,255,0.05)' : 'none' }),
    save: { width: '100%', padding: '12px 0', background: isModified ? 'rgba(0,217,255,0.15)' : 'rgba(0,0,0,0.2)', border: `1px solid ${isModified ? 'rgba(0,217,255,0.4)' : 'rgba(255,255,255,0.05)'}`, borderRadius: 12, color: isModified ? '#E8F1F7' : '#00D9FF', fontSize: 13, fontWeight: 700, cursor: 'pointer', letterSpacing: 0.5, transition: 'all 0.2s', boxShadow: isModified ? '0 0 20px rgba(0,217,255,0.1)' : 'none' },
    closeBtn: { width: 32, height: 32, borderRadius: 8, border: '1px solid rgba(255,255,255,0.05)', background: 'rgba(0,0,0,0.2)', display: 'flex', alignItems: 'center', justifyContent: 'center', cursor: 'pointer', color: '#7F91A3', transition: 'all 0.2s' },
    card: { flex: 1, padding: '12px', background: 'rgba(0,0,0,0.2)', border: '1px solid rgba(255,255,255,0.05)', borderRadius: 10 }
  }

  const modeDescriptions = {
    PERFORMANCE: 'Lowest processing latency',
    BALANCED: 'Recommended default',
    ACCURACY: 'Maximum recognition confidence'
  }

  return (
    <div className="modal-overlay" onClick={e => { if (e.target === e.currentTarget) onClose() }}>
      <div className="modal-box glass" style={S.modalBox}>
        {/* Header */}
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 20 }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 12 }}>
            <div style={{ width: 38, height: 38, borderRadius: 11, background: 'rgba(0,217,255,0.1)', border: '1px solid rgba(0,217,255,0.3)', display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
              <IconSettings size={18} color="#00D9FF" />
            </div>
            <div>
              <div style={{ fontSize: 15, fontWeight: 700, color: '#E8F1F7', letterSpacing: 0.3 }}>SYSTEM SETTINGS</div>
              <div style={{ fontSize: 10, color: '#7F91A3', marginTop: 2 }}>Hardware profiles & audio parameters</div>
            </div>
          </div>
          <button style={S.closeBtn} onClick={onClose}><IconX size={14} color="#7F91A3" /></button>
        </div>

        <div style={S.divider} />

        <div className="modal-body">
          {/* Mode */}
          <div style={S.row}>
            <label style={S.label}>VOICE DETECTION MODE</label>
            <div style={{ display: 'flex', gap: 6 }}>
              {['PERFORMANCE', 'BALANCED', 'ACCURACY'].map(m => (
                <button key={m} style={S.modeBtn(mode === m)} onClick={() => setMode(m)}>
                  {m}
                  {mode === m && <span style={{ fontSize: 8, color: '#00D9FF', opacity: 0.8, textTransform: 'none', fontWeight: 500, letterSpacing: 0 }}>{modeDescriptions[m]}</span>}
                </button>
              ))}
            </div>
          </div>

          {/* Confidence */}
          <div style={S.row}>
            <label style={S.label}>CONFIDENCE THRESHOLD <span style={{ float: 'right', color: '#00D9FF', fontSize: 11 }}>{conf}%</span></label>
            <input type="range" min="50" max="99" value={conf} onChange={e => setConf(+e.target.value)} style={S.slider} />
            <div style={{ display: 'flex', justifyContent: 'space-between', fontSize: 9, color: '#7F91A3', fontFamily: 'var(--mono)', marginTop: 4 }}>
              <span>50% (sensitive)</span><span>99% (precise)</span>
            </div>
          </div>

          {/* Gain */}
          <div style={S.row}>
            <label style={S.label}>MICROPHONE GAIN <span style={{ float: 'right', color: '#00D9FF', fontSize: 11 }}>{gain}%</span></label>
            <input type="range" min="0" max="100" value={gain} onChange={e => setGain(+e.target.value)} style={S.slider} />
            <div style={{ display: 'flex', justifyContent: 'space-between', fontSize: 9, color: '#7F91A3', fontFamily: 'var(--mono)', marginTop: 4 }}>
              <span>0% (mute)</span><span>100% (max)</span>
            </div>
          </div>

          <div style={{ display: 'flex', gap: 12, marginBottom: 20 }}>
            {/* Audio Info */}
            <div style={S.card}>
              <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginBottom: 8 }}><IconWaveform size={12} color="#00D9FF" /><IconFlash size={12} color="#00D9FF" style={{marginLeft: -4}} /><span style={{...S.label, margin: 0, color: '#E8F1F7'}}>AUDIO CONFIG</span></div>
              <div style={{ fontSize: 10, color: '#7F91A3', fontFamily: 'var(--mono)' }}>SOURCE <span style={{ color: '#E8F1F7', float: 'right' }}>ESP32 Microphone</span></div>
              <div style={{ fontSize: 10, color: '#7F91A3', fontFamily: 'var(--mono)', marginTop: 4 }}>SAMPLE RATE <span style={{ color: '#E8F1F7', float: 'right' }}>16 kHz</span></div>
              <div style={{ fontSize: 10, color: '#7F91A3', fontFamily: 'var(--mono)', marginTop: 4 }}>CHANNELS <span style={{ color: '#E8F1F7', float: 'right' }}>Mono</span></div>
            </div>
          </div>

          <div style={S.divider} />

          {/* Device Info */}
          <div style={{ display: 'flex', gap: 12, marginBottom: 12 }}>
            <div style={S.card}>
              <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginBottom: 8 }}><IconCpu size={12} color="#00D9FF" /><span style={{...S.label, margin: 0, color: '#E8F1F7'}}>DEVICE</span></div>
              <div style={{ fontSize: 11, fontWeight: 600, color: '#E8F1F7', fontFamily: 'var(--mono)', marginBottom: 6 }}>ESP32-WROOM-32</div>
              <div style={{ fontSize: 9, color: '#7F91A3', fontFamily: 'var(--mono)', marginBottom: 2 }}>STATUS <span style={{ color: telemetry ? '#00D37F' : '#FF453A', float: 'right' }}>● {telemetry ? 'ONLINE' : 'OFFLINE'}</span></div>
              {telemetry && <div style={{ fontSize: 9, color: '#7F91A3', fontFamily: 'var(--mono)' }}>DEVICE ID <span style={{ color: '#E8F1F7', float: 'right' }}>{telemetry.device_id || 'UNKNOWN'}</span></div>}
            </div>
            
            <div style={S.card}>
              <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginBottom: 8 }}><IconReport size={12} color="#00D9FF" /><span style={{...S.label, margin: 0, color: '#E8F1F7'}}>ASR ENGINE</span></div>
              <div style={{ fontSize: 11, fontWeight: 600, color: '#E8F1F7', fontFamily: 'var(--mono)', marginBottom: 6 }}>Whisper</div>
              <div style={{ fontSize: 9, color: '#7F91A3', fontFamily: 'var(--mono)', marginBottom: 2 }}>STATUS <span style={{ color: serverUp ? '#00D37F' : '#FF453A', float: 'right' }}>● {serverUp ? 'READY' : 'OFFLINE'}</span></div>
              {health && <div style={{ fontSize: 9, color: '#7F91A3', fontFamily: 'var(--mono)' }}>MODEL <span style={{ color: '#E8F1F7', float: 'right' }}>{health.asr_engine || 'tiny'}</span></div>}
            </div>
          </div>

          <div style={{ fontSize: 10, color: '#7F91A3', fontFamily: 'var(--mono)', padding: '8px 12px', background: 'rgba(0,0,0,0.2)', border: '1px solid rgba(255,255,255,0.05)', borderRadius: 8, marginBottom: 20, display: 'flex', justifyContent: 'space-between' }}>
            <span>ESP32 <span style={{ color: telemetry ? '#00D37F' : '#FF453A' }}>● {telemetry ? 'ONLINE' : 'OFFLINE'}</span></span>
            <span>Wi-Fi <span style={{ color: telemetry ? '#00D37F' : '#FF453A' }}>● {telemetry ? 'CONNECTED' : 'DISCONNECTED'}</span></span>
            <span>SERVER <span style={{ color: serverUp ? '#00D37F' : '#FF453A' }}>● {serverUp ? 'ONLINE' : 'OFFLINE'}</span></span>
          </div>

        </div>

        {/* Action Bar */}
        <div style={{ marginTop: 8 }}>
          {isModified && !saveStatus && !saving && (
            <div style={{ fontSize: 10, fontWeight: 700, fontFamily: 'var(--mono)', color: '#FFB020', textAlign: 'center', marginBottom: 12, display: 'flex', alignItems: 'center', justifyContent: 'center', gap: 6 }}>
              <span style={{ color: '#00D9FF' }}>●</span> UNSAVED CHANGES
            </div>
          )}
          {saveStatus && (
            <div style={{ fontSize: 10, fontWeight: 700, fontFamily: 'var(--mono)', color: saveStatus === 'success' ? '#00D37F' : '#FF453A', textAlign: 'center', marginBottom: 12 }}>
              {saveStatus === 'success' ? '✓ CONFIGURATION SAVED' : `✕ ${saveStatus.toUpperCase()}`}
            </div>
          )}
          
          <button style={S.save}
            onMouseOver={e => e.currentTarget.style.background = isModified ? 'rgba(0,217,255,0.2)' : 'rgba(0,0,0,0.4)'}
            onMouseOut={e => e.currentTarget.style.background = isModified ? 'rgba(0,217,255,0.15)' : 'rgba(0,0,0,0.2)'}
            onClick={handleSave}
            disabled={saving}>
            {saving ? 'SAVING...' : (isModified ? 'SAVE CHANGES' : 'SAVE CONFIGURATION')}
          </button>

          <div style={{ textAlign: 'center', marginTop: 12 }}>
            <button onClick={handleReset} style={{ background: 'none', border: 'none', color: '#7F91A3', fontSize: 10, fontFamily: 'var(--mono)', cursor: 'pointer', letterSpacing: 0.5 }}>
              Reset to Defaults
            </button>
          </div>
        </div>

      </div>
    </div>
  )
}
