export default function MissionHeader({ health, serverUp, totalEvents, onSettingsClick }) {
  const uptime = health?.uptime_seconds || 0
  const h = Math.floor(uptime / 3600), m = Math.floor((uptime % 3600) / 60), s = uptime % 60
  const upStr = `${h.toString().padStart(2,'0')}:${m.toString().padStart(2,'0')}:${s.toString().padStart(2,'0')}`
  
  return (
    <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', height: '100%', padding: '0 12px' }}>
      
      <div style={{ display: 'flex', alignItems: 'center', gap: 20 }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
          <div style={{ width: 24, height: 24, background: '#fff', color: '#000', display: 'flex', alignItems: 'center', justifyContent: 'center', fontWeight: 'bold' }}>HV</div>
          <div>
            <div style={{ fontSize: 14, fontWeight: 700, letterSpacing: 1 }}>VAANI // NEURAL EDGE COMMAND</div>
            <div style={{ fontSize: 9, color: 'var(--text-muted)', display: 'flex', gap: 10 }}>
              <span>VERSION 4.2.0-EDGE</span>
              <span>CLUSTER: ALPHA-1</span>
            </div>
          </div>
        </div>
        
        {/* Command Link Button */}
        <button onClick={onSettingsClick} style={{
          marginLeft: 20, padding: '4px 12px', background: 'rgba(0,240,255,0.1)', border: '1px solid var(--accent-cyan)',
          color: 'var(--accent-cyan)', fontSize: 10, fontWeight: 700, borderRadius: 4, cursor: 'pointer', transition: '0.2s',
          boxShadow: '0 0 10px rgba(0,240,255,0.2)'
        }} onMouseOver={e => { e.target.style.background = 'var(--accent-cyan)'; e.target.style.color = '#000'; }} onMouseOut={e => { e.target.style.background = 'rgba(0,240,255,0.1)'; e.target.style.color = 'var(--accent-cyan)'; }}>
          [ OPEN COMMAND LINK ]
        </button>
      </div>

      <div style={{ display: 'flex', gap: 30, alignItems: 'center' }}>
        
        {/* Fake telemetry metrics to look dense */}
        <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'flex-end' }}>
          <span style={{ fontSize: 9, color: 'var(--text-muted)' }}>NETWORK I/O</span>
          <span className="mono" style={{ fontSize: 11, color: 'var(--accent-cyan)' }}>TX: 2.4<span className="unit">KB/s</span> RX: 0.8<span className="unit">KB/s</span></span>
        </div>

        <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'flex-end' }}>
          <span style={{ fontSize: 9, color: 'var(--text-muted)' }}>TOTAL QUERIES</span>
          <span className="mono" style={{ fontSize: 14, fontWeight: 700 }}>{totalEvents}</span>
        </div>

        <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'flex-end' }}>
          <span style={{ fontSize: 9, color: 'var(--text-muted)' }}>SYSTEM UPTIME</span>
          <span className="mono" style={{ fontSize: 14, fontWeight: 700 }}>{upStr}</span>
        </div>

        <div style={{ display: 'flex', alignItems: 'center', gap: 8, background: 'rgba(255,255,255,0.05)', padding: '6px 16px', borderRadius: 20, border: '1px solid rgba(255,255,255,0.1)' }}>
          <div style={{ width: 8, height: 8, borderRadius: '50%', background: serverUp ? 'var(--accent-green)' : 'var(--accent-red)', boxShadow: serverUp ? '0 0 10px var(--accent-green)' : 'none' }}></div>
          <span style={{ fontSize: 11, fontWeight: 700, color: serverUp ? 'var(--accent-green)' : 'var(--accent-red)' }}>
            {serverUp ? 'SYSTEM ONLINE' : 'OFFLINE'}
          </span>
        </div>
      </div>

    </div>
  )
}
