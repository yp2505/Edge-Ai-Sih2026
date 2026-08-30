export default function AsciiHeader({ health, serverUp, totalEvents }) {
  const uptime = health?.uptime_seconds || 0
  const h = Math.floor(uptime / 3600), m = Math.floor((uptime % 3600) / 60), s = uptime % 60
  const upStr = `${h.toString().padStart(2,'0')}:${m.toString().padStart(2,'0')}:${s.toString().padStart(2,'0')}`
  
  const status = serverUp ? "[OK]" : "[FAIL]"
  const statusColor = serverUp ? "var(--text-main)" : "var(--text-alert)"

  return (
    <pre style={{ display: 'flex', justifyContent: 'space-between', padding: '10px 0' }}>
{`
╔════════════════════════════════════════════╗
║ HEY VAANI v2.0 // EDGE AI TERMINAL OS      ║
╚════════════════════════════════════════════╝
`}
<div style={{ display: 'flex', flexDirection: 'column', justifyContent: 'center', textAlign: 'right' }}>
{`CONNECTION: `}<span style={{ color: statusColor }}>{status}</span>{`
UPTIME:     ${upStr}
LOGS_RECV:  ${totalEvents.toString().padStart(4, '0')}
`}
</div>
    </pre>
  )
}
