export default function TopoHeader({ health, serverUp, totalEvents }) {
  const uptime = health?.uptime_seconds || 0
  const h = Math.floor(uptime / 3600), m = Math.floor((uptime % 3600) / 60), s = uptime % 60
  const upStr = `${h.toString().padStart(2,'0')}:${m.toString().padStart(2,'0')}:${s.toString().padStart(2,'0')}`
  
  return (
    <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
      <div>
        <h1 style={{ fontSize: 24, fontWeight: 300, letterSpacing: 8, margin: 0 }}>HEY VAANI</h1>
        <div className="title-abstract" style={{ color: 'var(--accent)' }}>GENERATIVE.UI_V1</div>
      </div>
      
      <div style={{ display: 'flex', gap: 40, textAlign: 'right' }}>
        <div>
          <div className="title-abstract">SYS.UPTIME</div>
          <div className="mono">{upStr}</div>
        </div>
        <div>
          <div className="title-abstract">TOTAL.EVT</div>
          <div className="mono">{totalEvents}</div>
        </div>
        <div>
          <div className="title-abstract">LINK.STAT</div>
          <div className="mono" style={{ color: serverUp ? 'var(--accent)' : '#ff3b30' }}>
            {serverUp ? 'CONNECTED' : 'DISCONNECTED'}
          </div>
        </div>
      </div>
    </div>
  )
}
