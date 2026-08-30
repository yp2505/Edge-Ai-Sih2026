export default function AsciiHardware({ latestEvent }) {
  const isProcessing = latestEvent && (Date.now() - new Date(latestEvent.timestamp).getTime() < 3000)
  
  const mcu = isProcessing ? '<!!!>' : '[MCU]'
  const wifi = isProcessing ? '<TX>' : '[TX]'
  const stat = isProcessing ? 'ACTIVE' : 'IDLE  '
  const clr = isProcessing ? 'var(--text-main)' : 'var(--text-muted)'

  return (
    <pre style={{ height: '100%', display: 'flex', flexDirection: 'column' }}>
{`
--- HARDWARE NODE ---
TYPE:  ESP32-S3 INT8
STAT:  `}<span style={{ color: clr }} className={isProcessing ? "blink" : ""}>{stat}</span>{`

      [ANTENNA]
          |
  .-------+-------.
  |       |       |
  |     ${wifi}      |
  |               |
  |  ...........  |
  |  : ${mcu} :  |
  |  :.......:  |
  |               |
  |   [ MIC ]     |
  '---------------'
       | | | |
      P I N S
      
> SENSOR_TEMP: 42.1C
> VRAM_USED:   54.2K
> UPLINK_MODE: TCP
`}
    </pre>
  )
}
