import { useEffect, useState } from 'react'

export default function TopoTranscript({ latestEvent }) {
  const [isActive, setIsActive] = useState(false)
  const [text, setText] = useState('')

  useEffect(() => {
    if (latestEvent) {
      setIsActive(true)
      setText(latestEvent.transcript || 'PROCESSING...')
      const t = setTimeout(() => setIsActive(false), 4000)
      return () => clearTimeout(t)
    }
  }, [latestEvent])

  return (
    <div style={{ textAlign: 'center', maxWidth: 800 }}>
      {isActive ? (
        <h2 style={{ 
          fontSize: 64, 
          fontWeight: 300, 
          lineHeight: 1.1,
          letterSpacing: -2,
          textShadow: '0 20px 40px rgba(0,0,0,0.8)'
        }}>
          "{text}"
        </h2>
      ) : (
        <div className="title-abstract" style={{ fontSize: 16, letterSpacing: 8, opacity: 0.3 }}>
          AWAITING FREQUENCY
        </div>
      )}
    </div>
  )
}
