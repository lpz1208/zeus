import { useEffect, useMemo, useState } from 'react'
import {
  Ban, CarFront, Copy, FlaskConical, Plus, Route, Trash2,
} from 'lucide-react'
import type {
  BenchmarkManifest,
  BenchmarkRoadControl,
  BenchmarkScenario,
  BenchmarkStrategy,
  BenchmarkStrategyKind,
  BenchmarkVehicleControl,
  MapRecord,
  RoadControlAction,
  RouteAlgorithm,
  VehicleControlAction,
} from '../types'

interface StrategyDraft extends BenchmarkStrategy {
  enabled: boolean
  label: string
  note: string
}

interface BenchmarkComposerProps {
  maps: MapRecord[]
  activeMap: MapRecord | null
  submitting: boolean
  serviceOnline: boolean
  onSubmit(manifest: BenchmarkManifest): Promise<unknown>
}

const DEFAULT_ORIGIN: [number, number] = [114.4911555, 30.9567005]
const DEFAULT_DESTINATION: [number, number] = [114.8064655, 30.8130008]

const STRATEGIES: StrategyDraft[] = [
  { id: 'fixed-astar', kind: 'fixed', algorithm: 'astar', enabled: true, label: '固定算法', note: '仅初始规划，不动态切换' },
  { id: 'reactive-astar', kind: 'reactive', algorithm: 'astar', enabled: true, label: '反应式算法', note: '路线失效后使用单算法' },
  { id: 'rule-agent', kind: 'rule_agent', algorithm: 'astar', enabled: true, label: '规则 Agent', note: '比较完整工具注册表' },
  { id: 'model-agent', kind: 'model_agent', algorithm: 'astar', enabled: false, label: '模型 Agent', note: 'LangGraph + LLM 决策' },
]

function defaultScenario(mapId: string, index = 1): BenchmarkScenario {
  return {
    id: `scenario-${String(index).padStart(2, '0')}`,
    mapId,
    origin: DEFAULT_ORIGIN,
    destination: DEFAULT_DESTINATION,
    durationSeconds: 5000,
    stepSeconds: 1,
    rerouteIntervalSeconds: 60,
    rerouteCostRatio: 1.25,
    sampleIntervalSeconds: 10,
    maxDecisions: 200,
    seed: 20260904 + index - 1,
    roadControls: [],
    vehicleControls: [],
  }
}

function NumberField({
  label, value, onChange, step = 1, min,
}: {
  label: string
  value: number
  onChange(value: number): void
  step?: number
  min?: number
}) {
  return (
    <label className="bench-field">
      <span>{label}</span>
      <input type="number" value={value} step={step} min={min} onChange={(event) => onChange(Number(event.target.value))} />
    </label>
  )
}

export function BenchmarkComposer({
  maps, activeMap, submitting, serviceOnline, onSubmit,
}: BenchmarkComposerProps) {
  const initialMapId = activeMap?.id ?? maps[0]?.id ?? ''
  const [name, setName] = useState('navigation-comparison')
  const [repetitions, setRepetitions] = useState(3)
  const [congestionThreshold, setCongestionThreshold] = useState(5)
  const [inputPrice, setInputPrice] = useState(0)
  const [outputPrice, setOutputPrice] = useState(0)
  const [scenarios, setScenarios] = useState<BenchmarkScenario[]>([
    defaultScenario(initialMapId),
  ])
  const [activeIndex, setActiveIndex] = useState(0)
  const [strategies, setStrategies] = useState<StrategyDraft[]>(STRATEGIES)

  useEffect(() => {
    if (!activeMap) return
    setScenarios((current) => current.map((scenario) => (
      scenario.mapId ? scenario : { ...scenario, mapId: activeMap.id }
    )))
  }, [activeMap])

  const scenario = scenarios[activeIndex]
  const enabledStrategies = strategies.filter((item) => item.enabled)
  const totalRuns = scenarios.length * enabledStrategies.length * repetitions
  const valid = Boolean(
    serviceOnline && name.trim() && scenario && scenarios.every((item) => item.mapId)
    && enabledStrategies.length && repetitions > 0,
  )

  const selectedMapName = useMemo(() => (
    maps.find((item) => item.id === scenario?.mapId)?.name ?? '未选择地图'
  ), [maps, scenario?.mapId])

  const updateScenario = (patch: Partial<BenchmarkScenario>) => {
    setScenarios((current) => current.map((item, index) => (
      index === activeIndex ? { ...item, ...patch } : item
    )))
  }

  const updateRoadControl = (index: number, patch: Partial<BenchmarkRoadControl>) => {
    if (!scenario) return
    updateScenario({
      roadControls: scenario.roadControls.map((item, itemIndex) => (
        itemIndex === index ? { ...item, ...patch } : item
      )),
    })
  }

  const updateVehicleControl = (index: number, patch: Partial<BenchmarkVehicleControl>) => {
    if (!scenario) return
    updateScenario({
      vehicleControls: scenario.vehicleControls.map((item, itemIndex) => (
        itemIndex === index ? { ...item, ...patch } : item
      )),
    })
  }

  const addScenario = () => {
    const next = defaultScenario(activeMap?.id ?? maps[0]?.id ?? '', scenarios.length + 1)
    setScenarios((current) => [...current, next])
    setActiveIndex(scenarios.length)
  }

  const duplicateScenario = () => {
    if (!scenario) return
    const copy = structuredClone(scenario)
    copy.id = `${scenario.id}-copy`
    copy.seed += 1
    setScenarios((current) => [...current, copy])
    setActiveIndex(scenarios.length)
  }

  const removeScenario = () => {
    if (scenarios.length === 1) return
    setScenarios((current) => current.filter((_, index) => index !== activeIndex))
    setActiveIndex((current) => Math.max(0, current - 1))
  }

  const submit = () => {
    if (!valid) return
    void onSubmit({
      name: name.trim(),
      repetitions,
      congestionSpeedThresholdMps: congestionThreshold,
      modelInputUsdPerMillionTokens: inputPrice,
      modelOutputUsdPerMillionTokens: outputPrice,
      scenarios,
      strategies: enabledStrategies.map(({ id, kind, algorithm }) => ({ id, kind, algorithm })),
    }).catch(() => undefined)
  }

  return (
    <aside className="bench-composer">
      <div className="bench-heading">
        <div><span className="eyebrow">EXPERIMENT DESIGN</span><h1>实验编排</h1></div>
        <FlaskConical size={19} />
      </div>

      <label className="bench-field">
        <span>实验名称</span>
        <input value={name} onChange={(event) => setName(event.target.value)} />
      </label>
      <div className="bench-grid-2">
        <NumberField label="重复次数" value={repetitions} min={1} onChange={setRepetitions} />
        <NumberField label="拥堵阈值 m/s" value={congestionThreshold} min={0} step={0.5} onChange={setCongestionThreshold} />
      </div>

      <section className="bench-block">
        <div className="bench-block__head">
          <span>SCENARIOS / {scenarios.length}</span>
          <div>
            <button title="复制场景" type="button" onClick={duplicateScenario}><Copy size={12} /></button>
            <button title="新增场景" type="button" onClick={addScenario}><Plus size={12} /></button>
            <button title="删除场景" type="button" disabled={scenarios.length === 1} onClick={removeScenario}><Trash2 size={12} /></button>
          </div>
        </div>
        <div className="bench-scenario-tabs">
          {scenarios.map((item, index) => (
            <button type="button" className={index === activeIndex ? 'is-active' : ''} onClick={() => setActiveIndex(index)} key={`${item.id}-${index}`}>
              {String(index + 1).padStart(2, '0')}
            </button>
          ))}
        </div>
        {scenario && <div className="bench-scenario-form">
          <label className="bench-field"><span>场景 ID</span><input value={scenario.id} onChange={(event) => updateScenario({ id: event.target.value })} /></label>
          <label className="bench-field">
            <span>运行地图 · {selectedMapName}</span>
            <select value={scenario.mapId} onChange={(event) => updateScenario({ mapId: event.target.value })}>
              <option value="">选择地图版本</option>
              {maps.map((map) => <option value={map.id} key={map.id}>{map.name}</option>)}
            </select>
          </label>
          <div className="bench-coordinate-pair">
            <span>ORIGIN</span>
            <input aria-label="起点经度" type="number" step="0.000001" value={scenario.origin[0]} onChange={(event) => updateScenario({ origin: [Number(event.target.value), scenario.origin[1]] })} />
            <input aria-label="起点纬度" type="number" step="0.000001" value={scenario.origin[1]} onChange={(event) => updateScenario({ origin: [scenario.origin[0], Number(event.target.value)] })} />
          </div>
          <div className="bench-coordinate-pair is-destination">
            <span>GOAL</span>
            <input aria-label="终点经度" type="number" step="0.000001" value={scenario.destination[0]} onChange={(event) => updateScenario({ destination: [Number(event.target.value), scenario.destination[1]] })} />
            <input aria-label="终点纬度" type="number" step="0.000001" value={scenario.destination[1]} onChange={(event) => updateScenario({ destination: [scenario.destination[0], Number(event.target.value)] })} />
          </div>
          <div className="bench-grid-2">
            <NumberField label="仿真时长 s" value={scenario.durationSeconds} min={1} onChange={(value) => updateScenario({ durationSeconds: value })} />
            <NumberField label="固定种子" value={scenario.seed} min={0} onChange={(value) => updateScenario({ seed: value })} />
          </div>

          <details className="bench-advanced">
            <summary>ADVANCED SCENARIO</summary>
            <div className="bench-grid-2">
              <NumberField label="步长 s" value={scenario.stepSeconds} min={0.01} step={0.1} onChange={(value) => updateScenario({ stepSeconds: value })} />
              <NumberField label="采样间隔 s" value={scenario.sampleIntervalSeconds} min={0.1} onChange={(value) => updateScenario({ sampleIntervalSeconds: value })} />
              <NumberField label="重规划间隔 s" value={scenario.rerouteIntervalSeconds} min={0} onChange={(value) => updateScenario({ rerouteIntervalSeconds: value })} />
              <NumberField label="重规划代价比" value={scenario.rerouteCostRatio} min={1.01} step={0.01} onChange={(value) => updateScenario({ rerouteCostRatio: value })} />
              <NumberField label="最大决策数" value={scenario.maxDecisions} min={1} onChange={(value) => updateScenario({ maxDecisions: value })} />
            </div>
          </details>

          <div className="bench-event-group">
            <div className="bench-event-head"><span><Ban size={12} /> 道路事件</span><button type="button" onClick={() => updateScenario({ roadControls: [...scenario.roadControls, { timeSeconds: 300, edgeIds: [], action: 'close', value: 1 }] })}><Plus size={11} /> ADD</button></div>
            {scenario.roadControls.map((control, index) => <div className="bench-event" key={index}>
              <input title="触发秒数" type="number" value={control.timeSeconds} onChange={(event) => updateRoadControl(index, { timeSeconds: Number(event.target.value) })} />
              <input title="Edge IDs，逗号分隔" placeholder="EDGE IDS" value={control.edgeIds.join(',')} onChange={(event) => updateRoadControl(index, { edgeIds: event.target.value.split(',').map((item) => item.trim()).filter(Boolean).map(Number).filter(Number.isFinite) })} />
              <select value={control.action} onChange={(event) => updateRoadControl(index, { action: event.target.value as RoadControlAction })}>
                <option value="close">CLOSE</option><option value="open">OPEN</option><option value="speedFactor">SPEED</option><option value="capacityFactor">CAPACITY</option>
              </select>
              <button type="button" onClick={() => updateScenario({ roadControls: scenario.roadControls.filter((_, itemIndex) => itemIndex !== index) })}><Trash2 size={11} /></button>
            </div>)}
            {!scenario.roadControls.length && <p>没有预编排道路事件。</p>}
          </div>

          <div className="bench-event-group">
            <div className="bench-event-head"><span><CarFront size={12} /> 车辆事件</span><button type="button" onClick={() => updateScenario({ vehicleControls: [...scenario.vehicleControls, { timeSeconds: 300, vehicleId: 0, action: 'hold', value: 1 }] })}><Plus size={11} /> ADD</button></div>
            {scenario.vehicleControls.map((control, index) => <div className="bench-event" key={index}>
              <input title="触发秒数" type="number" value={control.timeSeconds} onChange={(event) => updateVehicleControl(index, { timeSeconds: Number(event.target.value) })} />
              <input title="Vehicle ID" type="number" value={control.vehicleId} onChange={(event) => updateVehicleControl(index, { vehicleId: Number(event.target.value) })} />
              <select value={control.action} onChange={(event) => updateVehicleControl(index, { action: event.target.value as VehicleControlAction })}>
                <option value="hold">HOLD</option><option value="release">RELEASE</option><option value="speedFactor">SPEED</option>
              </select>
              <button type="button" onClick={() => updateScenario({ vehicleControls: scenario.vehicleControls.filter((_, itemIndex) => itemIndex !== index) })}><Trash2 size={11} /></button>
            </div>)}
            {!scenario.vehicleControls.length && <p>没有预编排车辆事件。</p>}
          </div>
        </div>}
      </section>

      <section className="bench-block">
        <div className="bench-block__head"><span>STRATEGY MATRIX</span><strong>{enabledStrategies.length} ACTIVE</strong></div>
        <div className="bench-strategies">
          {strategies.map((strategy, index) => <article className={strategy.enabled ? 'is-enabled' : ''} key={strategy.kind}>
            <button type="button" aria-label={`${strategy.label}${strategy.enabled ? '已启用' : '未启用'}`} onClick={() => setStrategies((current) => current.map((item, itemIndex) => itemIndex === index ? { ...item, enabled: !item.enabled } : item))}><i /></button>
            <div><strong>{strategy.label}</strong><small>{strategy.note}</small></div>
            <select value={strategy.algorithm} onChange={(event) => setStrategies((current) => current.map((item, itemIndex) => itemIndex === index ? { ...item, algorithm: event.target.value as RouteAlgorithm, id: `${item.kind.replace('_', '-')}-${event.target.value}` } : item))}>
              {(['astar', 'dijkstra', 'biastar', 'bidijkstra'] as RouteAlgorithm[]).map((algorithm) => <option key={algorithm}>{algorithm}</option>)}
            </select>
          </article>)}
        </div>
      </section>

      <details className="bench-pricing">
        <summary>MODEL COST PROFILE</summary>
        <div className="bench-grid-2">
          <NumberField label="输入 $/1M" value={inputPrice} min={0} step={0.01} onChange={setInputPrice} />
          <NumberField label="输出 $/1M" value={outputPrice} min={0} step={0.01} onChange={setOutputPrice} />
        </div>
      </details>

      <button className="bench-submit" type="button" disabled={!valid || submitting} onClick={submit}>
        {submitting ? <span className="spin" /> : <Route size={14} />}
        {submitting ? '提交实验…' : `运行 ${totalRuns} 个 EPISODES`}
      </button>
      {!serviceOnline && <p className="bench-service-note">启动 `make agent-benchmark-service` 后可提交任务。</p>}
    </aside>
  )
}
