import { useEffect, useRef, useState } from 'react'

const API = 'http://localhost:8080'
const colors = { I: '#00d9ff', W: '#ffb020', E: '#ff5a5a', D: '#a7b4c2' }

export default function LogTerminal() {
  const [entries, setEntries] = useState([])
  const [open, setOpen] = useState(true)
  const endRef = useRef(null)
  const clearedAt = useRef(0)

  useEffect(() => {
    let cancelled = false
    const load = async () => {
      try {
        const response = await fetch(`${API}/api/esp-logs`, { signal: AbortSignal.timeout(2000) })
        if (!response.ok) return
        const data = await response.json()
        if (!cancelled) {
          const next = Array.isArray(data.entries) ? data.entries : []
          setEntries(next.filter(entry => !entry.ts || new Date(entry.ts).getTime() > clearedAt.current))
        }
      } catch {}
    }
    load()
    const timer = setInterval(load, 2000)
    return () => { cancelled = true; clearInterval(timer) }
  }, [])

  useEffect(() => { if (open) endRef.current?.scrollIntoView({ block: 'end' }) }, [entries, open])

  return (
    <div className="log-terminal">
      <div className="log-terminal-head">
        <button onClick={() => setOpen(value => !value)}>{open ? '[-]' : '[+]'} ESP32 WIRELESS LOG</button>
        <div><span>{entries.length} LINES</span><button className="log-clear" onClick={() => { clearedAt.current = Date.now(); setEntries([]) }}>CLEAR</button></div>
      </div>
      {open && <div className="log-terminal-body">
        {entries.length === 0 && <span className="log-muted">[I] TERMINAL: awaiting ESP32 or server events...</span>}
        {entries.map((entry, index) => <div className="log-line" key={`${entry.ts || index}-${index}`} style={{ color: colors[entry.level] || colors.D }}>
          <span className="log-time">{entry.ts ? new Date(entry.ts).toLocaleTimeString() : '--:--:--'}</span> [{entry.level || 'I'}] {entry.tag || 'ESP32'}: {entry.msg || ''}
        </div>)}
        <div ref={endRef} />
      </div>}
    </div>
  )
}