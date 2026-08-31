import { useState } from 'react'
import DataGrid from './DataGrid'
import LatencyHistory from './LatencyHistory'

export default function RightPanel({ events }) {
  const [tab, setTab] = useState('LOGS')

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%' }}>
      {/* Tabs Header */}
      <div style={{ display: 'flex', borderBottom: '1px solid rgba(255,255,255,0.05)' }}>
        <button 
          onClick={() => setTab('LOGS')}
          style={{ 
            flex: 1, padding: '12px', background: 'transparent', border: 'none', 
            borderBottom: tab === 'LOGS' ? '2px solid var(--accent-cyan)' : '2px solid transparent',
            color: tab === 'LOGS' ? 'var(--accent-cyan)' : 'var(--text-muted)',
            fontSize: 10, fontWeight: 700, letterSpacing: 1, cursor: 'pointer', transition: '0.2s'
          }}
        >
          EVENT LOGS
        </button>
        <button 
          onClick={() => setTab('HISTORY')}
          style={{ 
            flex: 1, padding: '12px', background: 'transparent', border: 'none', 
            borderBottom: tab === 'HISTORY' ? '2px solid var(--accent-magenta)' : '2px solid transparent',
            color: tab === 'HISTORY' ? 'var(--accent-magenta)' : 'var(--text-muted)',
            fontSize: 10, fontWeight: 700, letterSpacing: 1, cursor: 'pointer', transition: '0.2s'
          }}
        >
          LATENCY HISTORY
        </button>
      </div>

      {/* Tab Content */}
      <div style={{ flex: 1, overflow: 'hidden' }}>
        {tab === 'LOGS' ? <DataGrid events={events} /> : <LatencyHistory events={events} />}
      </div>
    </div>
  )
}
