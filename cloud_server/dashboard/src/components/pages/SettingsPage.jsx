import { useState } from 'react'
import styles from './Pages.module.css'

export default function SettingsPage() {
  const [apiHost, setApiHost]     = useState('localhost')
  const [apiPort, setApiPort]     = useState('8080')
  const [tcpPort, setTcpPort]     = useState('5000')
  const [idleCpu, setIdleCpu]     = useState('< 10%')
  const [pollMs,  setPollMs]      = useState('1500')
  const [saved,   setSaved]       = useState(false)

  const save = () => { setSaved(true); setTimeout(() => setSaved(false), 2000) }

  return (
    <div className={styles.page}>
      <div className={`card ${styles.settingsCard}`}>
        <div className={styles.settingsTitle}>Connection Settings</div>
        <div className={styles.settingsGrid}>
          {[
            { label: 'API Host', id:'api-host', val:apiHost, set:setApiHost, hint:'Hostname where server.py runs' },
            { label: 'API Port (HTTP)', id:'api-port', val:apiPort, set:setApiPort, hint:'HTTP dashboard API port (default 8080)' },
            { label: 'TCP Port (ASR)', id:'tcp-port', val:tcpPort, set:setTcpPort, hint:'ESP32 connects to this port (default 5000)' },
            { label: 'Poll Interval (ms)', id:'poll-ms', val:pollMs, set:setPollMs, hint:'How often the dashboard polls /api/events' },
          ].map(({ label, id, val, set, hint }) => (
            <div key={id} className={styles.settingField}>
              <label className={styles.settingLabel} htmlFor={id}>{label}</label>
              <input id={id} className={styles.settingInput} value={val} onChange={e => set(e.target.value)} />
              <div className={styles.settingHint}>{hint}</div>
            </div>
          ))}
        </div>
      </div>

      <div className={`card ${styles.settingsCard}`}>
        <div className={styles.settingsTitle}>Hardware Config  <span className={styles.settingNote}>(edit when you measure real values from serial)</span></div>
        <div className={styles.settingsGrid}>
          {[
            { label: 'Idle CPU %', id:'idle-cpu', val:idleCpu, set:setIdleCpu, hint:'From: [CPU] Idle: X% in benchmark_cpu.h serial output' },
          ].map(({ label, id, val, set, hint }) => (
            <div key={id} className={styles.settingField}>
              <label className={styles.settingLabel} htmlFor={id}>{label}</label>
              <input id={id} className={styles.settingInput} value={val} onChange={e => set(e.target.value)} />
              <div className={styles.settingHint}>{hint}</div>
            </div>
          ))}
        </div>
      </div>

      <div className={`card ${styles.settingsCard}`}>
        <div className={styles.settingsTitle}>Wireless Connectivity</div>
        <div className={styles.wifiInfo}>
          <div className={styles.wifiRow}><span className={styles.wifiLabel}>How it works</span></div>
          {[
            'ESP32 connects to your WiFi network (configured in firmware)',
            'ESP32 opens a TCP socket to server.py at its LAN IP on port 5000',
            'server.py transcribes audio and logs to server_log.json',
            'This dashboard polls /api/events on port 8080 every 1.5 seconds',
            'Fully wireless — no USB, no serial cable needed during demos',
          ].map((t, i) => (
            <div key={i} className={styles.wifiStep}>
              <span className={styles.wifiNum}>{i+1}</span>
              <span className={styles.wifiText}>{t}</span>
            </div>
          ))}
          <div className={styles.wifiNote}>
            To configure: set <code>CONFIG_SERVER_IP</code> and <code>CONFIG_SERVER_PORT</code> in ESP32 firmware to match your laptop's LAN IP. Server prints the correct values on startup.
          </div>
        </div>
      </div>

      <button id="btn-save-settings" className={styles.saveBtn} onClick={save}>
        {saved ? '✓ Saved!' : 'Save Settings'}
      </button>
    </div>
  )
}
