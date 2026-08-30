import styles from './Pages.module.css'

// ── Edit these when you have real measured values from benchmark_cpu.h ────────
const SPECS = {
  model: { name: 'DS-CNN', precision: 'INT8', size: '46.9 KB', framework: 'TensorFlow Lite Micro' },
  memory: { tensor_arena: '~54 KB', heap_free: '~200 KB', flash_total: '4 MB' },
  cpu: { idle: '< 10%', inference: '~25%', wifi: '~15%', note: 'Update from serial: [CPU] log' },
  hw: { mcu: 'ESP32-S3', mic: 'INMP441 (I2S)', freq: '240 MHz', cores: 'Dual-core Xtensa LX7' },
  benchmark: { tpr: '99.0%', far: '0.4%', tpr_target: '≥ 90%', far_target: '≤ 5%' },
  protocol: { name: 'HVP1 v1', transport: 'TCP', port: 5000, header: '20-byte binary header' },
}

function SpecGroup({ title, rows }) {
  return (
    <div className={`card ${styles.specGroup}`}>
      <div className={styles.specTitle}>{title}</div>
      <div className={styles.specRows}>
        {rows.map(({ label, value, accent }) => (
          <div key={label} className={styles.specRow}>
            <span className={styles.specLabel}>{label}</span>
            <span className={`mono ${styles.specValue}`} style={{ color: accent || 'var(--t1)' }}>{value}</span>
          </div>
        ))}
      </div>
    </div>
  )
}

export default function DeviceSpecsPage() {
  return (
    <div className={styles.page}>
      <div className={styles.specsGrid}>
        <SpecGroup title="Microcontroller" rows={[
          { label: 'MCU',    value: SPECS.hw.mcu,    accent: 'var(--accent)' },
          { label: 'CPU',    value: SPECS.hw.freq },
          { label: 'Cores',  value: SPECS.hw.cores },
          { label: 'Mic',    value: SPECS.hw.mic },
        ]} />
        <SpecGroup title="ML Model" rows={[
          { label: 'Architecture', value: SPECS.model.name },
          { label: 'Precision',    value: SPECS.model.precision, accent: 'var(--green)' },
          { label: 'Size',         value: SPECS.model.size, accent: 'var(--accent)' },
          { label: 'Framework',    value: SPECS.model.framework },
        ]} />
        <SpecGroup title="Memory" rows={[
          { label: 'Tensor Arena', value: SPECS.memory.tensor_arena, accent: 'var(--yellow)' },
          { label: 'Heap Free',    value: SPECS.memory.heap_free },
          { label: 'Flash Total',  value: SPECS.memory.flash_total },
        ]} />
        <SpecGroup title={`CPU Utilisation  (${SPECS.cpu.note})`} rows={[
          { label: 'Idle',      value: SPECS.cpu.idle,      accent: 'var(--green)' },
          { label: 'Inference', value: SPECS.cpu.inference, accent: 'var(--yellow)' },
          { label: 'WiFi',      value: SPECS.cpu.wifi },
        ]} />
        <SpecGroup title="Protocol" rows={[
          { label: 'Name',      value: SPECS.protocol.name, accent: 'var(--accent)' },
          { label: 'Transport', value: SPECS.protocol.transport },
          { label: 'Port',      value: SPECS.protocol.port },
          { label: 'Header',    value: SPECS.protocol.header },
        ]} />
        <SpecGroup title="Benchmark Results" rows={[
          { label: 'True-Positive Rate', value: `${SPECS.benchmark.tpr}  (target ${SPECS.benchmark.tpr_target})`, accent: 'var(--green)' },
          { label: 'False Alarm Rate',   value: `${SPECS.benchmark.far}  (target ${SPECS.benchmark.far_target})`, accent: 'var(--accent)' },
        ]} />
      </div>
    </div>
  )
}
