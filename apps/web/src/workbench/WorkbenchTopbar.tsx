import { BrainCircuit, Check, ChevronRight, FlaskConical, Waypoints } from 'lucide-react'
import type { IntakeStage } from './useMapIntake'

interface WorkbenchTopbarProps {
  intakeStage: IntakeStage
  serviceOnline: boolean
  agentEnabled: boolean
  onEnterAgent(): void
  onEnterBenchmark(): void
}

function Pipeline({ stage }: { stage: IntakeStage }) {
  const active = stage === 'idle' ? 0
    : stage === 'uploading' ? 1
    : stage === 'inspected' ? 2
    : stage === 'importing' ? 3
    : 4
  const steps = ['SOURCE', 'SCHEMA', 'TOPOLOGY', 'RUNTIME']
  return (
    <div className="wb-pipeline" aria-label="地图编译进度">
      {steps.map((step, index) => (
        <div
          className={`wb-pipeline__step ${index < active ? 'is-done' : ''} ${index === active ? 'is-active' : ''}`}
          key={step}
        >
          <span>{index < active ? <Check size={12} /> : String(index + 1).padStart(2, '0')}</span>
          <strong>{step}</strong>
          {index < steps.length - 1 && <ChevronRight size={13} />}
        </div>
      ))}
    </div>
  )
}

export function WorkbenchTopbar({
  intakeStage, serviceOnline, agentEnabled, onEnterAgent, onEnterBenchmark,
}: WorkbenchTopbarProps) {
  return (
    <header className="wb-topbar">
      <div className="wb-brand">
        <div className="wb-brand__mark"><Waypoints size={23} strokeWidth={1.7} /></div>
        <div>
          <span>ZEUS</span>
          <strong>MAP LAB</strong>
        </div>
      </div>
      <Pipeline stage={intakeStage} />
      <div className="wb-status">
        <span className={`wb-status__dot ${serviceOnline ? '' : 'is-offline'}`} />
        <div>
          <small>CONTROL PLANE</small>
          <strong>{serviceOnline ? 'ONLINE' : 'DEMO MODE'}</strong>
        </div>
        <button className="wb-agent-launch" type="button" onClick={onEnterAgent} disabled={!agentEnabled}>
          <BrainCircuit size={14} /> AGENT
        </button>
        <button className="wb-agent-launch wb-benchmark-launch" type="button" onClick={onEnterBenchmark}>
          <FlaskConical size={13} /> BENCH
        </button>
      </div>
    </header>
  )
}
