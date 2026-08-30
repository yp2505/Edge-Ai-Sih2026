/** SVG Icon library — no emojis, all pure SVG */
export function IconGrid({ size = 16, color = 'currentColor' }) {
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke={color} strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round">
      <rect x="3" y="3" width="7" height="7" rx="1.5" />
      <rect x="14" y="3" width="7" height="7" rx="1.5" />
      <rect x="3" y="14" width="7" height="7" rx="1.5" />
      <rect x="14" y="14" width="7" height="7" rx="1.5" />
    </svg>
  )
}

export function IconRadio({ size = 16, color = 'currentColor' }) {
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke={color} strokeWidth="1.8" strokeLinecap="round">
      <path d="M2 12 C2 6.5 6.5 2 12 2" opacity="0.3"/>
      <path d="M2 12 C2 7.8 5.4 4.4 9.5 4" opacity="0.5"/>
      <circle cx="12" cy="12" r="3" fill={color} stroke="none"/>
      <path d="M22 12 C22 17.5 17.5 22 12 22" opacity="0.3"/>
      <path d="M22 12 C22 16.2 18.6 19.6 14.5 20" opacity="0.5"/>
      <path d="M5.5 5.5 C3.2 7.8 2 10 2 12" />
      <path d="M18.5 18.5 C20.8 16.2 22 14 22 12" />
    </svg>
  )
}

export function IconWaveform({ size = 16, color = 'currentColor' }) {
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke={color} strokeWidth="1.8" strokeLinecap="round">
      <polyline points="2,12 5,12 6,6 8,18 10,9 12,15 14,4 16,20 18,10 19,12 22,12" />
    </svg>
  )
}

export function IconClock({ size = 16, color = 'currentColor' }) {
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke={color} strokeWidth="1.8" strokeLinecap="round">
      <circle cx="12" cy="12" r="9" />
      <polyline points="12,7 12,12 15.5,14.5" />
    </svg>
  )
}

export function IconCpu({ size = 16, color = 'currentColor' }) {
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke={color} strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round">
      <rect x="7" y="7" width="10" height="10" rx="1" />
      <line x1="9" y1="7" x2="9" y2="4" /><line x1="12" y1="7" x2="12" y2="4" /><line x1="15" y1="7" x2="15" y2="4" />
      <line x1="9" y1="20" x2="9" y2="17" /><line x1="12" y1="20" x2="12" y2="17" /><line x1="15" y1="20" x2="15" y2="17" />
      <line x1="7" y1="9" x2="4" y2="9" /><line x1="7" y1="12" x2="4" y2="12" /><line x1="7" y1="15" x2="4" y2="15" />
      <line x1="20" y1="9" x2="17" y2="9" /><line x1="20" y1="12" x2="17" y2="12" /><line x1="20" y1="15" x2="17" y2="15" />
    </svg>
  )
}

export function IconBarChart({ size = 16, color = 'currentColor' }) {
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke={color} strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round">
      <line x1="12" y1="20" x2="12" y2="10" />
      <line x1="18" y1="20" x2="18" y2="4" />
      <line x1="6"  y1="20" x2="6"  y2="16" />
      <line x1="3"  y1="20" x2="21" y2="20" />
    </svg>
  )
}

export function IconReport({ size = 16, color = 'currentColor' }) {
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke={color} strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round">
      <path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z" />
      <polyline points="14,2 14,8 20,8" />
      <line x1="8" y1="13" x2="16" y2="13" />
      <line x1="8" y1="17" x2="13" y2="17" />
    </svg>
  )
}

export function IconSettings({ size = 16, color = 'currentColor' }) {
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke={color} strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round">
      <circle cx="12" cy="12" r="3" />
      <path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z" />
    </svg>
  )
}

export function IconWifi({ size = 16, color = 'currentColor' }) {
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke={color} strokeWidth="1.8" strokeLinecap="round">
      <path d="M5 12.55a11 11 0 0 1 14.08 0" />
      <path d="M1.42 9a16 16 0 0 1 21.16 0" />
      <path d="M8.53 16.11a6 6 0 0 1 6.95 0" />
      <circle cx="12" cy="20" r="1" fill={color} stroke="none"/>
    </svg>
  )
}

export function IconBell({ size = 16, color = 'currentColor' }) {
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke={color} strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round">
      <path d="M18 8A6 6 0 0 0 6 8c0 7-3 9-3 9h18s-3-2-3-9" />
      <path d="M13.73 21a2 2 0 0 1-3.46 0" />
    </svg>
  )
}

export function IconSearch({ size = 16, color = 'currentColor' }) {
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke={color} strokeWidth="1.8" strokeLinecap="round">
      <circle cx="11" cy="11" r="7" />
      <line x1="21" y1="21" x2="16.65" y2="16.65" />
    </svg>
  )
}

export function IconX({ size = 14, color = 'currentColor' }) {
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke={color} strokeWidth="2.2" strokeLinecap="round">
      <line x1="18" y1="6" x2="6" y2="18" /><line x1="6" y1="6" x2="18" y2="18" />
    </svg>
  )
}

export function IconDownload({ size = 16, color = 'currentColor' }) {
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke={color} strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round">
      <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
      <polyline points="7,10 12,15 17,10" />
      <line x1="12" y1="15" x2="12" y2="3" />
    </svg>
  )
}

/** Unique Hey Vaani logo SVG — stylised V with sound waves */
export function VaaniLogo({ size = 36 }) {
  return (
    <svg width={size} height={size} viewBox="0 0 40 40" fill="none">
      {/* Outer rounded square */}
      <rect width="40" height="40" rx="10" fill="url(#vaani-grad)" />
      {/* Stylised V shape */}
      <path d="M10 11 L20 29 L30 11" stroke="white" strokeWidth="3.2" strokeLinecap="round" strokeLinejoin="round" fill="none" />
      {/* Left sound arc */}
      <path d="M6.5 17 C5.5 19.5 5.5 22 6.5 24.5" stroke="rgba(255,255,255,0.55)" strokeWidth="2" strokeLinecap="round" fill="none"/>
      {/* Right sound arc */}
      <path d="M33.5 17 C34.5 19.5 34.5 22 33.5 24.5" stroke="rgba(255,255,255,0.55)" strokeWidth="2" strokeLinecap="round" fill="none"/>
      <defs>
        <linearGradient id="vaani-grad" x1="0" y1="0" x2="40" y2="40">
          <stop offset="0%" stopColor="#00c8ff"/>
          <stop offset="100%" stopColor="#0059ff"/>
        </linearGradient>
      </defs>
    </svg>
  )
}
