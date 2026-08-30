import { useEffect, useState } from 'react'
import styles from './CinematicBlob.module.css'

export default function CinematicBlob({ latestEvent }) {
  const [isActive, setIsActive] = useState(false)
  const [text, setText] = useState('')

  useEffect(() => {
    if (latestEvent) {
      setIsActive(true)
      setText(latestEvent.transcript || 'PROCESSING AUDIO...')
      const t = setTimeout(() => setIsActive(false), 4000)
      return () => clearTimeout(t)
    }
  }, [latestEvent])

  return (
    <div className={styles.container}>
      
      {/* The massive glowing blob */}
      <div className={`${styles.blob} ${isActive ? styles.blobActive : styles.blobIdle}`}>
        <div className={styles.core}></div>
        <div className={styles.ring1}></div>
        <div className={styles.ring2}></div>
        <div className={styles.ring3}></div>
      </div>

      {/* Typography */}
      <div className={styles.textWrap}>
        {isActive ? (
          <h1 className={styles.activeText}>"{text}"</h1>
        ) : (
          <h1 className={styles.idleText}>AWAITING WAKE-WORD</h1>
        )}
      </div>

    </div>
  )
}
