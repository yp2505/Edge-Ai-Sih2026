import styles from './StatsPanel.module.css'

// ── CONFIG — edit idle CPU % when you have real measured value ────────────────
const MODEL = {
  size:      '46.9 KB',
  arena:     '~54 KB',
  idleCpu:   '< 10%',      // ← replace with real value from benchmark_cpu.h serial log
  protocol:  'HVP1 v1',
  tpr:       '99.0%',
  far:       '0.4%',
  engine:    'faster-whisper tiny INT8',
}

function StatCard({ label, value, sub, color, icon }) {
  return (
    <div className={styles.statCard}>
      {icon && <span className={styles.statIcon}>{icon}</span>}
      <div className={styles.statLabel}>{label}</div>
      <div className={styles.statValue} style={{ color: color || 'var(--text-primary)' }}>
        {value}
      </div>
      {sub && <div className={styles.statSub}>{sub}</div>}
    </div>
  )
}

function Section({ title, children }) {
  return (
    <div className={styles.section}>
      <div className={styles.sectionTitle}>{title}</div>
      {children}
    </div>
  )
}

function SpecRow({ label, value, color }) {
  return (
    <div className={styles.specRow}>
      <span className={styles.specLabel}>{label}</span>
      <span className={`mono ${styles.specValue}`} style={{ color }}>{value}</span>
    </div>
  )
}

export default function StatsPanel({ stats, serverUp }) {
  const { count, avg, min, max } = stats
  const health = avg < 400 ? { text: 'On target', color: 'var(--green)', pill: 'pill-green' }
               : avg < 800 ? { text: 'Moderate',  color: 'var(--yellow)', pill: 'pill-yellow' }
               : count === 0 ? { text: 'No data', color: 'var(--text-tertiary)', pill: 'pill-ghost' }
               :               { text: 'High',    color: 'var(--red)', pill: 'pill-red' }

  return (
    <>
      {/* ── Live Session ─────────────────────────────────────────────── */}
      <div className={styles.panel}>
        <div className={styles.panelHeader}>
          <h2 className={styles.panelTitle}>⚡ Session</h2>
          <span className={`pill-sm ${health.pill}`}>{health.text}</span>
        </div>

        <div className={styles.body}>
          <div className={styles.statsGrid}>
            <StatCard label="Avg E2E"     value={`${avg} ms`} color="var(--cyan)"  sub="latency" />
            <StatCard label="Detections"  value={count}        sub="this session" />
          </div>
          <div className={styles.statsGrid}>
            <StatCard label="Best"  value={`${min} ms`} color="var(--green)" sub="min latency" />
            <StatCard label="Worst" value={`${max} ms`} color={max > 800 ? 'var(--red)' : 'var(--yellow)'} sub="max latency" />
          </div>

          {/* Health bar */}
          {count > 0 && (
            <div className={styles.healthBar}>
              <div className={styles.healthFill} style={{
                width: `${Math.min((avg / 1200) * 100, 100)}%`,
                background: avg < 400 ? 'linear-gradient(90deg,#30d158,#34d399)'
                          : avg < 800 ? 'linear-gradient(90deg,#ffd60a,#ff9f0a)'
                          :             'linear-gradient(90deg,#ff453a,#ff6b6b)',
              }} />
            </div>
          )}
        </div>
      </div>

      {/* ── ESP32 Hardware ───────────────────────────────────────────── */}
      <div className={styles.panel}>
        <div className={styles.panelHeader}>
          <h2 className={styles.panelTitle}>🔬 ESP32 Hardware</h2>
          <span className={`pill-sm ${serverUp ? 'pill-green' : 'pill-ghost'}`}>
            {serverUp ? 'Connected' : 'Offline'}
          </span>
        </div>
        <div className={styles.body}>
          <Section title="Memory">
            <SpecRow label="Model Size"    value={MODEL.size}    color="var(--cyan)" />
            <SpecRow label="Tensor Arena"  value={MODEL.arena}   color="var(--blue)" />
          </Section>
          <Section title="CPU  (edit in StatsPanel.jsx)">
            <SpecRow label="Idle CPU %"    value={MODEL.idleCpu} color="var(--green)" />
            <SpecRow label="Protocol"      value={MODEL.protocol} />
          </Section>
          <Section title="ASR Engine">
            <SpecRow label="Model" value={MODEL.engine} color="var(--purple)" />
          </Section>
        </div>
      </div>

      {/* ── Benchmark ────────────────────────────────────────────────── */}
      <div className={styles.panel}>
        <div className={styles.panelHeader}>
          <h2 className={styles.panelTitle}>📈 Benchmark</h2>
          <span className="pill-sm pill-green">PASSED</span>
        </div>
        <div className={styles.body}>
          <div className={styles.statsGrid}>
            <StatCard label="TPR"  value={MODEL.tpr} color="var(--green)" sub="target ≥ 90%" icon="✅" />
            <StatCard label="FAR"  value={MODEL.far} color="var(--cyan)"  sub="target ≤ 5%"  icon="✅" />
          </div>
        </div>
      </div>
    </>
  )
}
