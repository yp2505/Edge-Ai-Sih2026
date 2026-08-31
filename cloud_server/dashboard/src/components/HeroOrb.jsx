import { useEffect, useRef, useState } from 'react'
import styles from './HeroOrb.module.css'

export default function HeroOrb({ latest, serverUp, pingCount }) {
  const [faceState, setFaceState] = useState('sleeping')
  const [statusLabel, setStatusLabel] = useState('Offline')
  const [particles, setParticles] = useState([])
  const prevPing = useRef(pingCount)
  const happyTimeout = useRef(null)
  const particleId = useRef(0)

  const spawnParticles = () => {
    const p = Array.from({ length: 10 }, (_, i) => ({
      id: particleId.current++,
      angle: (i / 10) * 360 + Math.random() * 15,
      dist: 100 + Math.random() * 50,
      size: 5 + Math.random() * 6,
      color: i % 3 === 0 ? 'var(--pink)' : i % 3 === 1 ? 'var(--amber)' : 'var(--primary)',
    }))
    setParticles(p)
    setTimeout(() => setParticles([]), 1300)
  }

  useEffect(() => {
    const isNewPing = pingCount !== prevPing.current
    if (isNewPing) prevPing.current = pingCount

    if (!serverUp) {
      setFaceState('sleeping')
      setStatusLabel('Offline')
      if (happyTimeout.current) clearTimeout(happyTimeout.current)
      return
    }

    if (isNewPing) {
      setFaceState('thinking')
      setStatusLabel('Processing…')
      if (happyTimeout.current) clearTimeout(happyTimeout.current)
      happyTimeout.current = setTimeout(() => {
        setFaceState('happy')
        setStatusLabel('Got it!')
        spawnParticles()
        happyTimeout.current = setTimeout(() => {
          setFaceState('searching')
          setStatusLabel('Listening…')
        }, 3000)
      }, 1200)
      return
    }

    if (faceState !== 'happy' && faceState !== 'thinking') {
      setFaceState('searching')
      setStatusLabel('Listening…')
    }

    return () => { if (happyTimeout.current) clearTimeout(happyTimeout.current) }
  }, [serverUp, pingCount, latest])

  return (
    <div className={styles.heroWrap}>
      {/* Ambient glow disc */}
      <div className={`${styles.glowDisc} ${styles['glow_' + faceState]}`} />

      {/* Sparkle particles */}
      {particles.map(p => (
        <div
          key={p.id}
          className={styles.sparkle}
          style={{
            '--angle': `${p.angle}deg`,
            '--dist': `${p.dist}px`,
            width: p.size, height: p.size,
            background: p.color,
          }}
        />
      ))}

      {/* Slime blob */}
      <div className={`${styles.slime} ${styles['slime_' + faceState]}`}>
        {/* Inner shine */}
        <div className={styles.shine} />
        <div className={styles.shine2} />

        {/* Face layer */}
        <div className={styles.face}>
          {/* Eyes */}
          <div className={`${styles.eyeGroup} ${styles['eyes_' + faceState]}`}>
            <div className={`${styles.eye} ${styles.eyeL}`}>
              <div className={styles.pupil} />
              <div className={styles.eyeShine} />
            </div>
            <div className={`${styles.eye} ${styles.eyeR}`}>
              <div className={styles.pupil} />
              <div className={styles.eyeShine} />
            </div>
          </div>

          {/* Blush */}
          <div className={`${styles.blushRow} ${styles['blush_' + faceState]}`}>
            <div className={styles.blush} />
            <div className={styles.blush} />
          </div>

          {/* Mouth */}
          <div className={`${styles.mouth} ${styles['mouth_' + faceState]}`} />
        </div>
      </div>

      {/* Sleeping ZZZ overlay */}
      {faceState === 'sleeping' && (
        <div className={styles.zzzWrap} aria-hidden="true">
          <span className={styles.z1}>z</span>
          <span className={styles.z2}>z</span>
          <span className={styles.z3}>Z</span>
        </div>
      )}

      {/* Searching pulse ring */}
      {faceState === 'searching' && (
        <>
          <div className={styles.scanRing} />
          <div className={styles.scanRing2} />
        </>
      )}

      {/* Status pill */}
      <div className={`${styles.statusPill} ${styles['status_' + faceState]}`}>
        <div className={styles.statusDot} />
        <span className={styles.statusText}>{statusLabel}</span>
      </div>
    </div>
  )
}
