import { useEffect, useRef, useState } from 'react'
import styles from './HeroOrb.module.css'

const BARS = 28   // number of waveform bars

export default function HeroOrb({ latest, serverUp, pingCount }) {
  const [pulse, setPulse] = useState(false)
  const prevPing = useRef(pingCount)

  // Trigger pulse animation on new detection
  useEffect(() => {
    if (pingCount !== prevPing.current) {
      prevPing.current = pingCount
      setPulse(true)
      const t = setTimeout(() => setPulse(false), 1200)
      return () => clearTimeout(t)
    }
  }, [pingCount])

  const transcript = latest?.transcript ?? null
  const total = latest
    ? (latest.kw_to_connect_ms ?? 0) + (latest.receive_gap_ms ?? 0) + (latest.transcribe_ms ?? 0)
    : null

  const latencyColor = total === null ? 'var(--cyan)'
    : total < 400 ? 'var(--green)'
    : total < 800 ? 'var(--yellow)'
    : 'var(--red)'

  return (
    <section className={styles.hero}>

      {/* Orb container */}
      <div className={styles.orbWrap}>
        {/* Outer spinning ring (Siri-style conic gradient) */}
        <div className={`${styles.siriRing} ${pulse ? styles.ringPulse : ''}`} />

        {/* Middle pulse ring */}
        <div className={`${styles.pulseRing} ${pulse ? styles.pulseActive : ''}`} />

        {/* Core orb */}
        <div className={styles.orb}>
          {/* Waveform bars inside orb */}
          <div className={styles.waveform} aria-hidden="true">
            {Array.from({ length: BARS }).map((_, i) => (
              <div
                key={i}
                className={`${styles.bar} ${serverUp && pulse ? styles.barActive : ''}`}
                style={{
                  animationDelay: `${(i / BARS) * 1.4}s`,
                  animationDuration: `${0.6 + (i % 5) * 0.12}s`,
                }}
              />
            ))}
          </div>
        </div>
      </div>

      {/* Transcript pill below orb */}
      <div className={styles.transcriptWrap}>
        {!serverUp ? (
          <div className={`${styles.transcriptPill} ${styles.pillOffline} pill`}>
            <span className={styles.pillDot} style={{ background: 'var(--red)' }} />
            <span>Server offline — start server.py to begin</span>
          </div>
        ) : transcript ? (
          <div
            className={`${styles.transcriptPill} ${pulse ? styles.pillNew : ''} pill`}
            key={latest?.session_id}
          >
            <span className={styles.pillDot} style={{ background: 'var(--green)' }} />
            <span className={styles.transcriptText}>&ldquo;{transcript}&rdquo;</span>
          </div>
        ) : (
          <div className={`${styles.transcriptPill} ${styles.pillWaiting} pill`}>
            <span className={styles.pillDot} style={{ background: 'var(--cyan)', animation: 'blink-dot 1.5s infinite' }} />
            <span>Listening for &ldquo;Hey Vaani&rdquo;…</span>
          </div>
        )}
      </div>

      {/* Latency strip — pill shaped, only shown if data exists */}
      {latest && (
        <div className={styles.latencyStrip}>
          {[
            { label: 'kw→connect', value: latest.kw_to_connect_ms ?? 0, color: 'var(--purple)' },
            { label: 'rcv gap',    value: latest.receive_gap_ms    ?? 0, color: 'var(--cyan)' },
            { label: 'transcribe', value: latest.transcribe_ms     ?? 0, color: 'var(--blue)' },
          ].map(({ label, value, color }) => (
            <div key={label} className={`${styles.latPill} glass pill`}>
              <span className={styles.latLabel}>{label}</span>
              <span className={`${styles.latValue} mono`} style={{ color }}>{value} ms</span>
            </div>
          ))}
          <div className={`${styles.latPillTotal} glass pill`}>
            <span className={styles.latLabel}>end-to-end</span>
            <span className={`${styles.latValueXl} mono`} style={{ color: latencyColor }}>{total} ms</span>
          </div>
        </div>
      )}

    </section>
  )
}
