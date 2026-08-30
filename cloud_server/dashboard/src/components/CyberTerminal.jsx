import { useRef, useEffect } from 'react'
import styles from './CyberTerminal.module.css'

export default function CyberTerminal({ events, isGlass }) {
  const scrollRef = useRef(null)
  
  useEffect(() => {
    if (scrollRef.current) {
      scrollRef.current.scrollTop = scrollRef.current.scrollHeight
    }
  }, [events.length])

  return (
    <div className={`${styles.terminal} ${isGlass ? styles.glassMode : ''}`}>
      <div className={styles.termHeader}>
        <span className={styles.termTitle}>DATA LOG</span>
      </div>
      
      <div className={styles.termScroll} ref={scrollRef}>
        {events.length === 0 ? (
          <div className={styles.termEmpty}>
            <span className="mono">NO DATA YET</span>
          </div>
        ) : (
          events.map((e, i) => {
            const total = (e.kw_to_connect_ms||0)+(e.receive_gap_ms||0)+(e.transcribe_ms||0)
            return (
              <div key={e.session_id} className={`${styles.termRow} ${i === events.length - 1 ? styles.termRowNew : ''}`}>
                <div className={styles.termText}>{e.transcript}</div>
                <div className={styles.termMetrics}>
                  <span className="mono">ID {e.session_id.toString().padStart(4,'0')}</span>
                  <span className="mono" style={{ color: 'var(--accent-orange)' }}>{total}ms</span>
                </div>
              </div>
            )
          })
        )}
      </div>
    </div>
  )
}
