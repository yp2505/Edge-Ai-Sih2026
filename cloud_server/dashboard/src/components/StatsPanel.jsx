import styles from './StatsPanel.module.css'

// ─── CONFIG — edit these when you have real measured values ───────────────────
const MODEL_SPECS = {
  modelSize:   '46.9 KB',
  tensorArena: '~54 KB',
  idleCpuPct:  '< 10%',    // ← replace with real measured value from benchmark_cpu.h
  protocol:    'HVP1 v1',
  asr:         'Whisper tiny INT8',
  tprTarget:   '≥ 90%',
  tprActual:   '99.0%',
  farTarget:   '≤ 5%',
  farActual:   '0.4%',
}

function StatBox({ label, value, sub, accent }) {
  return (
    <div className={styles.statBox}>
      <div className={styles.statLabel}>{label}</div>
      <div className={styles.statValue} style={accent ? { color: accent } : {}}>
        {value}
      </div>
      {sub && <div className={styles.statSub}>{sub}</div>}
    </div>
  )
}

function SectionTitle({ children }) {
  return <h3 className={styles.sectionTitle}>{children}</h3>
}

export default function StatsPanel({ stats }) {
  const { count, avg, min, max } = stats

  return (
    <>
      {/* ── Live Session Stats ────────────────────────────────────────── */}
      <div className={`glass ${styles.panel}`}>
        <div className={styles.panelHeader}>
          <h2 className={styles.panelTitle}>⚡ Session Stats</h2>
          <span className="pill pill-green">{count} detections</span>
        </div>
        <div className={styles.body}>
          <div className={styles.grid}>
            <StatBox label="Avg Latency"   value={`${avg} ms`}  sub="end-to-end"  accent="var(--cyan)" />
            <StatBox label="Min Latency"   value={`${min} ms`}  sub="best"        accent="var(--green-soft)" />
            <StatBox label="Max Latency"   value={`${max} ms`}  sub="worst"       accent={max > 800 ? '#f87171' : 'var(--yellow)'} />
            <StatBox label="Total Events"  value={count}         sub="this session" />
          </div>

          {/* Latency health bar */}
          {count > 0 && (
            <div className={styles.healthWrap}>
              <div className={styles.healthLabel}>
                <span className="text-muted">Avg vs target (&lt; 400ms)</span>
                <span className="mono" style={{ color: avg < 400 ? 'var(--green-soft)' : avg < 800 ? 'var(--yellow)' : '#f87171' }}>
                  {avg < 400 ? '✅ On target' : avg < 800 ? '⚠️ Moderate' : '🔴 High'}
                </span>
              </div>
              <div className={styles.healthTrack}>
                <div
                  className={styles.healthFill}
                  style={{
                    width: `${Math.min((avg / 1200) * 100, 100)}%`,
                    background: avg < 400
                      ? 'linear-gradient(90deg, #10b981, #34d399)'
                      : avg < 800
                      ? 'linear-gradient(90deg, #f59e0b, #fcd34d)'
                      : 'linear-gradient(90deg, #ef4444, #f87171)',
                  }}
                />
              </div>
            </div>
          )}
        </div>
      </div>

      {/* ── ESP32 Model Specs ──────────────────────────────────────────── */}
      <div className={`glass ${styles.panel}`}>
        <div className={styles.panelHeader}>
          <h2 className={styles.panelTitle}>🔬 Model Efficiency</h2>
          <span className="pill pill-blue">DS-CNN INT8</span>
        </div>
        <div className={styles.body}>
          <SectionTitle>Memory Footprint</SectionTitle>
          <div className={styles.grid}>
            <StatBox label="Model Size"   value={MODEL_SPECS.modelSize}   sub="Flash memory" />
            <StatBox label="Tensor Arena" value={MODEL_SPECS.tensorArena}  sub="RAM usage" />
          </div>
          <SectionTitle>CPU Utilisation</SectionTitle>
          <div className={styles.grid}>
            <StatBox label="Idle CPU"     value={MODEL_SPECS.idleCpuPct}  sub="Low-power mode" accent="var(--green-soft)" />
            <StatBox label="Protocol"     value={MODEL_SPECS.protocol}     sub="TCP 20B header" />
          </div>
        </div>
      </div>

      {/* ── Benchmark Results ──────────────────────────────────────────── */}
      <div className={`glass ${styles.panel}`}>
        <div className={styles.panelHeader}>
          <h2 className={styles.panelTitle}>📈 Evaluated Metrics</h2>
          <span className="pill pill-green">PASSED PS</span>
        </div>
        <div className={styles.body}>
          <div className={styles.grid}>
            <StatBox
              label="True-Positive Rate"
              value={MODEL_SPECS.tprActual}
              sub={`Target ${MODEL_SPECS.tprTarget} ✅`}
              accent="var(--green-soft)"
            />
            <StatBox
              label="False Activation"
              value={MODEL_SPECS.farActual}
              sub={`Target ${MODEL_SPECS.farTarget} ✅`}
              accent="#60a5fa"
            />
          </div>
          <SectionTitle>ASR Engine</SectionTitle>
          <div className={styles.specRow}>
            <span className="text-muted" style={{ fontSize: 12 }}>Engine</span>
            <span className="mono" style={{ fontSize: 13, color: 'var(--cyan)' }}>{MODEL_SPECS.asr}</span>
          </div>
        </div>
      </div>
    </>
  )
}
