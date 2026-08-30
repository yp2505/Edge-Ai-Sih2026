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
      
      {/* Liquid 3D Sphere */}
      <div className={`${styles.orb} ${isActive ? styles.orbActive : styles.orbIdle}`}>
        <div className={styles.liquidCore}></div>
        <div className={styles.glowAura}></div>
      </div>

      {/* Brutalist Typography */}
      <div className={styles.textWrap}>
        {isActive ? (
          <h1 className={styles.activeText}>"{text}"</h1>
        ) : (
          <h1 className={styles.idleText}>LISTENING</h1>
        )}
      </div>

    </div>
  )
}
