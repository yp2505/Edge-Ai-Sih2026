import { useState, useEffect } from 'react'
import { IconSettings, IconX, IconCpu, IconWifi, IconWaveform, IconReport, IconFlash } from './Icons.jsx'
import { encryptPassword } from '../crypto.js'

const API = import.meta.env.VITE_API_URL || ''

export default function SettingsModal({ onClose, telemetry, serverUp, health }) {
  const [conf, setConf] = useState(85)
  const [gain, setGain] = useState(50)
  const [mode, setMode] = useState('BALANCED')
  const [saving, setSaving] = useState(false)
  const [saveStatus, setSaveStatus] = useState(null) // 'success' | 'error' | null
  const [isModified, setIsModified] = useState(false)
  const [network, setNetwork] = useState({ ssid: '', password: '', server_ip: '', server_port: '5000' })
  const [networkStatus, setNetworkStatus] = useState(null)
  const [pushingNetwork, setPushingNetwork] = useState(false)

  // Mark modified whenever a setting changes
  useEffect(() => {
    setIsModified(true)
  }, [conf, gain, mode])

  // Clear modified on initial mount (hacky but works since defaults are set first)
  useEffect(() => setIsModified(false), [])

  useEffect(() => {
    fetch(`${API}/api/wifi-config`).then(r => r.ok ? r.json() : {}).then(data => {
      setNetwork(current => ({ ...current, ssid: data.ssid || '', server_ip: data.server_ip || '', server_port: String(data.server_port || 5000) }))
    }).catch(() => {})
  }, [])

  const updateNetwork = (field, value) => setNetwork(current => ({ ...current, [field]: value }))
  const pushNetwork = async () => {
    setPushingNetwork(true)
    setNetworkStatus(null)
    try {
      const payload = { ...network }
      if (payload.password) {
        payload.password = await encryptPassword(payload.password)
      }
      const r = await fetch(`${API}/api/wifi-config`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) })
      const data = await r.json()
      if (!r.ok) throw new Error(data.error || 'Unable to save network configuration')
      setNetwork(current => ({ ...current, password: '' }))
      setNetworkStatus('success')
    } catch (error) { setNetworkStatus(error.message || 'Unable to save network configuration') }
    finally { setPushingNetwork(false) }
  }

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
    label: { fontSize: 8.5, fontWeight: 700, letterSpacing: 1.2, textTransform: 'uppercase', marginBottom: 5, display: 'block', fontFamily: 'var(--mono)' },
    row: { marginBottom: 12 },
    divider: { height: 1, margin: '12px 0' },
  }

  const modeDescriptions = {
    PERFORMANCE: 'Fast',
    BALANCED: 'Default',
    ACCURACY: 'Max Acc'
  }

  const saveControls = (
    <div className="settings-actions">
      {isModified && !saveStatus && !saving && (
        <div className="settings-unsaved"><span>●</span> UNSAVED CHANGES</div>
      )}
      {saveStatus && (
        <div className={`settings-save-status ${saveStatus === 'success' ? 'success' : 'error'}`}>
          {saveStatus === 'success' ? '✓ SAVED' : saveStatus.toUpperCase()}
        </div>
      )}
      <button className={`settings-save-button ${isModified ? 'modified' : ''}`} onClick={handleSave} disabled={saving}>
        {saving ? 'SAVING...' : (isModified ? 'SAVE CHANGES' : 'SAVE CONFIGURATION')}
      </button>
      <button onClick={handleReset} className="settings-reset">Reset Defaults</button>
    </div>
  )

  return (
    <div className="modal-overlay" onClick={e => { if (e.target === e.currentTarget) onClose() }}>
      <div className="modal-box glass settings-modal">
        
        {/* Header */}
        <div className="settings-header">
          <div style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
            <div className="settings-icon-box">
              <IconSettings size={16} />
            </div>
            <div>
              <div className="settings-title">SYSTEM SETTINGS</div>
              <div className="settings-subtitle">Hardware & Network config</div>
            </div>
          </div>
          <button className="settings-close-btn" onClick={onClose} aria-label="Close settings"><IconX size={12} /></button>
        </div>

        <div className="settings-divider" style={S.divider} />

        {/* 2-Column Grid */}
        <div className="modal-body settings-grid-body">
          
          {/* COLUMN 1: Performance */}
          <div style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
            <div className="settings-section">
              <div style={S.row}>
                <label className="settings-lbl" style={S.label}>DETECTION MODE</label>
                <div className="settings-mode-group">
                  {['PERFORMANCE', 'BALANCED', 'ACCURACY'].map(m => (
                    <button key={m} type="button" className={`settings-mode-btn ${mode === m ? 'active' : ''}`} onClick={() => setMode(m)}>
                      <span className="mode-btn-title">{m}</span>
                      <span className="mode-btn-desc">{modeDescriptions[m]}</span>
                    </button>
                  ))}
                </div>
              </div>

              <div style={S.row}>
                <label className="settings-lbl" style={S.label}>CONFIDENCE <span className="settings-val" style={{ float: 'right', fontSize: 10 }}>{conf}%</span></label>
                <input type="range" min="50" max="99" value={conf} onChange={e => setConf(+e.target.value)} className="settings-slider" />
              </div>

              <div style={{ marginBottom: 0 }}>
                <label className="settings-lbl" style={S.label}>MIC GAIN <span className="settings-val" style={{ float: 'right', fontSize: 10 }}>{gain}%</span></label>
                <input type="range" min="0" max="100" value={gain} onChange={e => setGain(+e.target.value)} className="settings-slider" />
              </div>
            </div>

            {saveControls}
          </div>

          {/* COLUMN 2: Network & Status */}
          <div style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
            <div className="settings-section">
              <label className="settings-lbl" style={S.label}>NETWORK CONFIG</label>
              <div className="settings-network-grid">
                <label className="settings-field"><span className="settings-lbl" style={S.label}>SSID</span><input className="settings-input" value={network.ssid} placeholder="Network Name" onChange={e => updateNetwork('ssid', e.target.value)} /></label>
                <label className="settings-field"><span className="settings-lbl" style={S.label}>Password</span><input className="settings-input" type="password" value={network.password} placeholder="Keep existing" onChange={e => updateNetwork('password', e.target.value)} /></label>
                <label className="settings-field"><span className="settings-lbl" style={S.label}>Server IP</span><input className="settings-input" value={network.server_ip} placeholder="192.168.x.x" onChange={e => updateNetwork('server_ip', e.target.value)} /></label>
                <label className="settings-field"><span className="settings-lbl" style={S.label}>Port</span><input className="settings-input" type="number" value={network.server_port} placeholder="5000" onChange={e => updateNetwork('server_port', e.target.value)} /></label>
              </div>
              {networkStatus && <div className={`settings-net-status ${networkStatus === 'success' ? 'ok' : 'err'}`}>{networkStatus === 'success' ? '✓ QUEUED FOR ESP32' : networkStatus}</div>}
              <button type="button" className="settings-push-btn" onClick={pushNetwork} disabled={pushingNetwork}>{pushingNetwork ? 'PUSHING...' : 'PUSH TO ESP32'}</button>
            </div>

            {/* Status Cards Condensed */}
            <div style={{ display: 'flex', gap: 8 }}>
              <div className="settings-status-card">
                <div style={{ display: 'flex', alignItems: 'center', gap: 5, marginBottom: 4 }}><IconCpu size={11} className="settings-icon" /><span className="settings-lbl-bright">ESP32</span></div>
                <div className="settings-status-text">{telemetry ? 'ONLINE' : 'OFFLINE'}</div>
              </div>
              <div className="settings-status-card">
                <div style={{ display: 'flex', alignItems: 'center', gap: 5, marginBottom: 4 }}><IconReport size={11} className="settings-icon" /><span className="settings-lbl-bright">ASR ENGINE</span></div>
                <div className="settings-status-text">{serverUp ? 'READY' : 'OFFLINE'}</div>
              </div>
            </div>

          </div>
        </div>
      </div>
    </div>
  )
}


