import styles from './RecentDetections.module.css'

function timeAgo(iso) {
  if (!iso) return '—'
  const diff = (Date.now() - new Date(iso).getTime()) / 1000
  if (diff < 5)    return 'just now'
  if (diff < 60)   return `${Math.floor(diff)}s ago`
  if (diff < 3600) return `${Math.floor(diff/60)}m ago`
  return new Date(iso).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
}

function latencyBadge(total) {
  if (total === 0) return { cls: 'badge-muted',  text: '—' }
  if (total < 400) return { cls: 'badge-green',  text: `${total} ms` }
  if (total < 800) return { cls: 'badge-yellow', text: `${total} ms` }
  return               { cls: 'badge-red',    text: `${total} ms` }
}

export default function RecentDetections({ events }) {
  const show = events.slice(0, 10)

  return (
    <div className={styles.wrap}>
      {/* Table header */}
      <div className={styles.tableHeader}>
        <div>
          <h2 className={styles.title}>Recent Detections</h2>
          <p className={styles.sub}>Latest wake-word activations from the ESP32</p>
        </div>
        <button className={`badge badge-accent ${styles.viewAll}`} id="btn-view-all">
          View All ({events.length})
        </button>
      </div>

      {/* Table */}
      {events.length === 0 ? (
        <div className={styles.empty}>
          <span>No detections yet — say &ldquo;Hey Vaani&rdquo; to begin</span>
        </div>
      ) : (
        <div className={styles.tableWrap}>
          <table className={styles.table}>
            <thead>
              <tr>
                <th>SESSION</th>
                <th>TRANSCRIPT</th>
                <th>kw→connect</th>
                <th>rcv gap</th>
                <th>transcribe</th>
                <th>TOTAL E2E</th>
                <th>TIME</th>
                <th>STATUS</th>
              </tr>
            </thead>
            <tbody>
              {show.map((e, i) => {
                const total = (e.kw_to_connect_ms??0) + (e.receive_gap_ms??0) + (e.transcribe_ms??0)
                const { cls, text } = latencyBadge(total)
                const isNew = i === 0
                return (
                  <tr key={e.session_id} className={`${styles.row} ${isNew ? styles.rowNew : ''}`}>
                    <td>
                      <div className={styles.sessionCell}>
                        <div className={styles.sessionAvatar}>
                          #{e.session_id}
                        </div>
                        <span className="mono t3" style={{ fontSize: 10 }}>{e.client_ip ?? 'esp32'}</span>
                      </div>
                    </td>
                    <td>
                      <div className={styles.transcriptCell} title={e.transcript}>
                        {e.transcript ?? 'Processing…'}
                      </div>
                    </td>
                    <td><span className="mono t2">{e.kw_to_connect_ms  ?? '—'} ms</span></td>
                    <td><span className="mono t2">{e.receive_gap_ms    ?? '—'} ms</span></td>
                    <td><span className="mono t2">{e.transcribe_ms     ?? '—'} ms</span></td>
                    <td><span className={`badge ${cls}`}>{text}</span></td>
                    <td><span className="t3" style={{ fontSize: 12 }}>{timeAgo(e.timestamp)}</span></td>
                    <td>
                      <span className={`badge ${total < 800 ? 'badge-green' : 'badge-red'}`}>
                        {total < 800 ? 'Good' : 'Slow'}
                      </span>
                    </td>
                  </tr>
                )
              })}
            </tbody>
          </table>
        </div>
      )}
    </div>
  )
}
