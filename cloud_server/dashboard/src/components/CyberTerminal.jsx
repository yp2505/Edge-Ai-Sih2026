import { useRef, useEffect } from 'react'
import styles from './CyberTerminal.module.css'
import { IconWarning, IconFlash } from './Icons.jsx'

export default function CyberTerminal({ events }) {
  const scrollRef = useRef(null)
  
  useEffect(() => {
    if (scrollRef.current) {
      scrollRef.current.scrollTop = scrollRef.current.scrollHeight
    }
  }, [events.length])

  return (
    <div className={styles.terminal}>
      <div className={styles.termHeader}>
        <span className={styles.termTitle}>SYS.LOGS // DATA FEED</span>
        <span className={styles.termCount}>TOTAL: {events.length}</span>
      </div>
      
      <div className={styles.termScroll} ref={scrollRef}>
        {events.length === 0 ? (
          <div className={styles.termEmpty}>
            <span className="mono glow-cyan">] AWAITING_DATA_STREAM...</span>
          </div>
        ) : (
          events.map((e, i) => {
            const total = (e.kw_to_connect_ms||0)+(e.receive_gap_ms||0)+(e.transcribe_ms||0)
            const slow = total > 500
            return (
              <div key={e.session_id} className={`${styles.termRow} ${i === events.length - 1 ? styles.termRowNew : ''}`}>
                <div className={styles.termTop}>
                  <span className={styles.tSession}>[ID:{e.session_id.toString().padStart(4,'0')}]</span>
                  {slow ? (
                    <span className={styles.tWarn}><IconWarning size={10} /> {total}ms</span>
                  ) : (
                    <span className={styles.tFast}><IconFlash size={10} /> {total}ms</span>
                  )}
                </div>
                <div className={styles.termText}>&gt; {e.transcript}</div>
                <div className={styles.termMetrics}>
                  <span>TX:{e.kw_to_connect_ms||0}</span>
                  <span>RCV:{e.receive_gap_ms||0}</span>
                  <span>INF:{e.transcribe_ms||0}</span>
                </div>
              </div>
            )
          })
        )}
      </div>
    </div>
  )
}
