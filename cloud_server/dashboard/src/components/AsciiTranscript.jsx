import { useEffect, useState } from 'react'

export default function AsciiTranscript({ latestEvent }) {
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

  const drawVisualizer = () => {
    if (!isActive) return `
    _   _
   / \\ / \\
  ( - | - )
   \\_/ \\_/
    Z Z Z
    `
    // Random chaotic ascii bars for "processing"
    return Array(5).fill(0).map(() => 
      Array(20).fill(0).map(() => Math.random() > 0.5 ? '|' : '=').join('')
    ).join('\n')
  }

  return (
    <pre style={{ height: '100%', display: 'flex', flexDirection: 'column', justifyContent: 'center', alignItems: 'center', textAlign: 'center' }}>
{`
< WAKE_WORD_SENSOR >
======================================
`}
<span style={{ fontSize: 24, margin: '20px 0', color: isActive ? '#fff' : 'var(--text-muted)' }} className={isActive ? 'blink' : ''}>
  {isActive ? `> ${text} <` : 'WAITING FOR INPUT...'}
</span>
{`
======================================
`}
<span className={isActive ? 'alert' : 'muted'} style={{ margin: '20px 0' }}>
{drawVisualizer()}
</span>
    </pre>
  )
}
