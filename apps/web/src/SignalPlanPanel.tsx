import { useEffect, useState } from 'react'
import { Ban, GitBranch, MapPin, Plus, RadioTower, Trash2 } from 'lucide-react'
import type { JunctionSignalPlan, SignalMovement } from './types'

interface SignalPlanPanelProps {
  plans: JunctionSignalPlan[]
  selectedNodeId: number | null
  pickingJunction: boolean
  onPickingJunctionChange: (active: boolean) => void
  onChange: (plans: JunctionSignalPlan[]) => void
}

interface PhaseDraft {
  greenSeconds: number
  saturationFlowVph: number
  movements: string
}

const initialPhases = (): PhaseDraft[] => [
  { greenSeconds: 30, saturationFlowVph: 1800, movements: '' },
  { greenSeconds: 30, saturationFlowVph: 1800, movements: '' },
]

function parseMovements(value: string): SignalMovement[] | null {
  const result: SignalMovement[] = []
  const seen = new Set<string>()
  for (const raw of value.split(/[,;\n]+/)) {
    const token = raw.trim()
    if (!token) continue
    const match = token.match(/^(\d+)\s*(?:>|->|→)\s*(\d+)$/)
    if (!match) return null
    const movement = { fromEdgeId: Number(match[1]), toEdgeId: Number(match[2]) }
    const key = `${movement.fromEdgeId}>${movement.toEdgeId}`
    if (!seen.has(key)) {
      seen.add(key)
      result.push(movement)
    }
  }
  return result.length > 0 ? result : null
}

export function SignalPlanPanel({
  plans,
  selectedNodeId,
  pickingJunction,
  onPickingJunctionChange,
  onChange,
}: SignalPlanPanelProps) {
  const [expanded, setExpanded] = useState(false)
  const [nodeId, setNodeId] = useState(0)
  const [offsetSeconds, setOffsetSeconds] = useState(0)
  const [yellowSeconds, setYellowSeconds] = useState(3)
  const [allRedSeconds, setAllRedSeconds] = useState(1)
  const [phases, setPhases] = useState<PhaseDraft[]>(initialPhases)
  const [error, setError] = useState('')

  useEffect(() => {
    if (selectedNodeId !== null) setNodeId(selectedNodeId)
  }, [selectedNodeId])

  const savePlan = () => {
    setError('')
    if (!Number.isInteger(nodeId) || nodeId < 0) {
      setError('请输入有效的路口节点 ID')
      return
    }
    if (![offsetSeconds, yellowSeconds, allRedSeconds].every(Number.isFinite) ||
      offsetSeconds < 0 || yellowSeconds < 0 || allRedSeconds < 0) {
      setError('周期偏移和清空时间必须是非负数')
      return
    }
    const parsedPhases = phases.map((phase) => ({
      greenSeconds: phase.greenSeconds,
      saturationFlowVph: phase.saturationFlowVph,
      movements: parseMovements(phase.movements),
    }))
    if (parsedPhases.some((phase) => !Number.isFinite(phase.greenSeconds) ||
      phase.greenSeconds < 0.1 || phase.greenSeconds > 600 || !phase.movements)) {
      setError('每个相位需要 0.1–600 秒绿灯，并填写“入边>出边”转向')
      return
    }
    if (parsedPhases.some((phase) => !Number.isFinite(phase.saturationFlowVph) ||
      phase.saturationFlowVph < 60 || phase.saturationFlowVph > 7200)) {
      setError('每个相位的转向饱和流率须在 60–7200 veh/h 之间')
      return
    }
    const plan: JunctionSignalPlan = {
      nodeId,
      offsetSeconds,
      yellowSeconds,
      allRedSeconds,
      phases: parsedPhases.map((phase) => ({
        greenSeconds: phase.greenSeconds,
        saturationFlowVph: phase.saturationFlowVph,
        movements: phase.movements as SignalMovement[],
      })),
    }
    onChange([...plans.filter((current) => current.nodeId !== nodeId), plan])
    onPickingJunctionChange(false)
  }

  const updatePhase = (index: number, update: Partial<PhaseDraft>) => {
    setPhases(phases.map((phase, phaseIndex) => phaseIndex === index
      ? { ...phase, ...update }
      : phase))
  }

  return (
    <div className={`signal-editor ${expanded ? 'is-expanded' : ''}`}>
      <button className="signal-editor__head" type="button" onClick={() => setExpanded(!expanded)}>
        <div><RadioTower size={13} /><span>转向信号</span></div>
        <small>{plans.length} PLANS · {plans.reduce((sum, plan) => sum + plan.phases.length, 0)} PHASES</small>
      </button>
      {expanded && <>
        <div className="signal-editor__timing">
          <label><span>节点 ID<small>NODE</small></span><input type="number" min="0" value={nodeId} onChange={(event) => setNodeId(Number(event.target.value))} /></label>
          <button className={pickingJunction ? 'control-pick is-active' : 'control-pick'} type="button" onClick={() => onPickingJunctionChange(!pickingJunction)}><MapPin size={12} />{pickingJunction ? '点击节点…' : '地图拾取'}</button>
          <label><span>周期偏移<small>OFFSET · S</small></span><input type="number" min="0" step="0.5" value={offsetSeconds} onChange={(event) => setOffsetSeconds(Number(event.target.value))} /></label>
          <label><span>黄灯<small>YELLOW · S</small></span><input type="number" min="0" max="60" step="0.5" value={yellowSeconds} onChange={(event) => setYellowSeconds(Number(event.target.value))} /></label>
          <label><span>全红<small>ALL RED · S</small></span><input type="number" min="0" max="60" step="0.5" value={allRedSeconds} onChange={(event) => setAllRedSeconds(Number(event.target.value))} /></label>
        </div>
        <div className="signal-phases">
          {phases.map((phase, index) => <div className="signal-phase" key={index}>
            <div className="signal-phase__index"><i /><span>P{index + 1}</span></div>
            <label><span>绿灯秒数</span><input type="number" min="0.1" max="600" step="0.5" value={phase.greenSeconds} onChange={(event) => updatePhase(index, { greenSeconds: Number(event.target.value) })} /></label>
            <label><span>饱和流率 <small>VEH/H</small></span><input type="number" min="60" max="7200" step="60" value={phase.saturationFlowVph} onChange={(event) => updatePhase(index, { saturationFlowVph: Number(event.target.value) })} /></label>
            <label className="signal-phase__movements"><span>允许转向 <small>FROM &gt; TO</small></span><input value={phase.movements} placeholder="例如 12>18, 13>18" onChange={(event) => updatePhase(index, { movements: event.target.value })} /></label>
            {phases.length > 1 && <button type="button" aria-label="删除相位" onClick={() => setPhases(phases.filter((_, phaseIndex) => phaseIndex !== index))}><Trash2 size={11} /></button>}
          </div>)}
          <button className="signal-phase-add" type="button" onClick={() => setPhases([...phases, { greenSeconds: 30, saturationFlowVph: 1800, movements: '' }])}><Plus size={12} />增加相位</button>
        </div>
        {error && <div className="control-editor__error"><Ban size={11} />{error}</div>}
        <button className="signal-save" type="button" onClick={savePlan}><GitBranch size={13} />保存节点 #{nodeId} 信号方案</button>
        {plans.length > 0 && <div className="signal-plan-list">
          {plans.map((plan) => <div key={plan.nodeId}>
            <span><i />节点 #{plan.nodeId}</span>
            <small>{plan.phases.length} 相位 · 周期 {plan.phases.reduce((sum, phase) => sum + phase.greenSeconds + plan.yellowSeconds + plan.allRedSeconds, 0).toFixed(1)}s</small>
            <button type="button" aria-label="删除信号方案" onClick={() => onChange(plans.filter((current) => current.nodeId !== plan.nodeId))}><Trash2 size={11} /></button>
          </div>)}
        </div>}
        <p className="control-editor__note">每项转向写作“入边 ID &gt; 出边 ID”。相位内每项转向独立按饱和流率放行；1800 veh/h 对应最小 2 秒车头时距。黄灯和全红期间停止新的路口转移。</p>
      </>}
    </div>
  )
}
