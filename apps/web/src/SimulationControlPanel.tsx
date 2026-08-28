import { useEffect, useMemo, useState } from 'react'
import { Ban, CarFront, Gauge, MapPin, Plus, Trash2, Waypoints } from 'lucide-react'
import type {
  JunctionControlAction,
  RoadControlAction,
  RoadProperties,
  SimulationControls,
  VehicleControlAction,
} from './types'

interface SimulationControlPanelProps {
  controls: SimulationControls
  durationSeconds: number
  vehicleCount: number
  selectedRoad: RoadProperties | null
  selectedNodeId: number | null
  pickingJunction: boolean
  onPickingJunctionChange: (active: boolean) => void
  onChange: (controls: SimulationControls) => void
}

type Scope = 'vehicle' | 'road' | 'junction'

function edgeIdsFromRoad(road: RoadProperties | null): number[] {
  if (!road) return []
  return String(road.EDGE_IDS ?? '')
    .split(/[;,\s]+/)
    .map(Number)
    .filter((value) => Number.isInteger(value) && value >= 0)
}

const actionLabels: Record<string, string> = {
  hold: '暂停车辆',
  release: '恢复车辆',
  close: '封闭',
  open: '开放',
  speedFactor: '速度系数',
  capacityFactor: '容量系数',
}

export function SimulationControlPanel({
  controls,
  durationSeconds,
  vehicleCount,
  selectedRoad,
  selectedNodeId,
  pickingJunction,
  onPickingJunctionChange,
  onChange,
}: SimulationControlPanelProps) {
  const [scope, setScope] = useState<Scope>('vehicle')
  const [timeSeconds, setTimeSeconds] = useState(0)
  const [vehicleId, setVehicleId] = useState(0)
  const [roadEdgeText, setRoadEdgeText] = useState('')
  const [nodeId, setNodeId] = useState(0)
  const [vehicleAction, setVehicleAction] = useState<VehicleControlAction>('hold')
  const [roadAction, setRoadAction] = useState<RoadControlAction>('close')
  const [junctionAction, setJunctionAction] = useState<JunctionControlAction>('close')
  const [factor, setFactor] = useState(0.5)
  const [error, setError] = useState('')

  useEffect(() => {
    const edgeIds = edgeIdsFromRoad(selectedRoad)
    if (edgeIds.length > 0) setRoadEdgeText(edgeIds.join(', '))
  }, [selectedRoad])

  useEffect(() => {
    if (selectedNodeId !== null) setNodeId(selectedNodeId)
  }, [selectedNodeId])

  const eventCount = controls.vehicleControls.length + controls.roadControls.reduce(
    (sum, control) => sum + control.edgeIds.length,
    0,
  ) + controls.junctionControls.length

  const timeline = useMemo(() => [
    ...controls.vehicleControls.map((control, index) => ({
      scope: 'vehicle' as const,
      index,
      time: control.timeSeconds,
      target: `车辆 #${control.vehicleId}`,
      action: actionLabels[control.action],
      value: control.value,
    })),
    ...controls.roadControls.map((control, index) => ({
      scope: 'road' as const,
      index,
      time: control.timeSeconds,
      target: `边 ${control.edgeIds.join(', ')}`,
      action: actionLabels[control.action],
      value: control.value,
    })),
    ...controls.junctionControls.map((control, index) => ({
      scope: 'junction' as const,
      index,
      time: control.timeSeconds,
      target: `路口 #${control.nodeId}`,
      action: actionLabels[control.action],
      value: undefined,
    })),
  ].sort((left, right) => left.time - right.time), [controls])

  const addControl = () => {
    setError('')
    if (!Number.isFinite(timeSeconds) || timeSeconds < 0 || timeSeconds >= durationSeconds) {
      setError(`执行时刻须在 0–${durationSeconds} 秒之间`)
      return
    }
    if (scope === 'vehicle') {
      if (!Number.isInteger(vehicleId) || vehicleId < 0 || vehicleId >= vehicleCount) {
        setError(`车辆 ID 范围为 0–${Math.max(0, vehicleCount - 1)}`)
        return
      }
      if (vehicleAction === 'speedFactor' && (!Number.isFinite(factor) || factor < 0.05 || factor > 3)) {
        setError('车辆速度系数范围为 0.05–3')
        return
      }
      onChange({
        ...controls,
        vehicleControls: [...controls.vehicleControls, {
          timeSeconds,
          vehicleId,
          action: vehicleAction,
          ...(vehicleAction === 'speedFactor' ? { value: factor } : {}),
        }],
      })
      return
    }
    if (scope === 'road') {
      const edgeIds = [...new Set(roadEdgeText.split(/[,;\s]+/).map(Number)
        .filter((value) => Number.isInteger(value) && value >= 0))]
      if (edgeIds.length === 0) {
        setError('请在地图上选择道路，或输入至少一个有向边 ID')
        return
      }
      const maximum = roadAction === 'capacityFactor' ? 10 : 3
      if ((roadAction === 'speedFactor' || roadAction === 'capacityFactor') &&
        (!Number.isFinite(factor) || factor < 0.05 || factor > maximum)) {
        setError(`${actionLabels[roadAction]}范围为 0.05–${maximum}`)
        return
      }
      onChange({
        ...controls,
        roadControls: [...controls.roadControls, {
          timeSeconds,
          edgeIds,
          action: roadAction,
          ...(['speedFactor', 'capacityFactor'].includes(roadAction) ? { value: factor } : {}),
        }],
      })
      return
    }
    if (!Number.isInteger(nodeId) || nodeId < 0) {
      setError('请输入有效的路口节点 ID')
      return
    }
    onChange({
      ...controls,
      junctionControls: [...controls.junctionControls, {
        timeSeconds,
        nodeId,
        action: junctionAction,
      }],
    })
    onPickingJunctionChange(false)
  }

  const removeControl = (item: typeof timeline[number]) => {
    if (item.scope === 'vehicle') {
      onChange({ ...controls, vehicleControls: controls.vehicleControls.filter((_, index) => index !== item.index) })
    } else if (item.scope === 'road') {
      onChange({ ...controls, roadControls: controls.roadControls.filter((_, index) => index !== item.index) })
    } else {
      onChange({ ...controls, junctionControls: controls.junctionControls.filter((_, index) => index !== item.index) })
    }
  }

  return (
    <div className="control-editor">
      <div className="control-editor__head">
        <div><Waypoints size={13} /><span>场景控制</span></div>
        <small>{eventCount} EVENTS</small>
      </div>
      <div className="control-editor__scope" role="tablist" aria-label="控制对象">
        <button className={scope === 'vehicle' ? 'is-active' : ''} onClick={() => setScope('vehicle')} type="button"><CarFront size={12} />车辆</button>
        <button className={scope === 'road' ? 'is-active' : ''} onClick={() => setScope('road')} type="button"><Gauge size={12} />道路</button>
        <button className={scope === 'junction' ? 'is-active' : ''} onClick={() => setScope('junction')} type="button"><MapPin size={12} />路口</button>
      </div>
      <div className="control-editor__form">
        <label className="control-time"><span>执行时刻<small>TIME / S</small></span><input type="number" min="0" max={Math.max(0, durationSeconds - 0.1)} step="0.1" value={timeSeconds} onChange={(event) => setTimeSeconds(Number(event.target.value))} /></label>
        {scope === 'vehicle' && <>
          <label><span>车辆 ID<small>0–{Math.max(0, vehicleCount - 1)}</small></span><input type="number" min="0" max={Math.max(0, vehicleCount - 1)} value={vehicleId} onChange={(event) => setVehicleId(Number(event.target.value))} /></label>
          <label><span>控制命令</span><select value={vehicleAction} onChange={(event) => setVehicleAction(event.target.value as VehicleControlAction)}><option value="hold">暂停</option><option value="release">恢复</option><option value="speedFactor">速度系数</option></select></label>
        </>}
        {scope === 'road' && <>
          <label className="control-target-wide"><span>有向边 ID<small>{selectedRoad ? selectedRoad.ROAD_ID : 'SELECT ROAD / CSV'}</small></span><input value={roadEdgeText} placeholder="在地图选路，或输入 12, 13" onChange={(event) => setRoadEdgeText(event.target.value)} /></label>
          <label><span>控制命令</span><select value={roadAction} onChange={(event) => setRoadAction(event.target.value as RoadControlAction)}><option value="close">封闭</option><option value="open">开放</option><option value="speedFactor">速度系数</option><option value="capacityFactor">容量系数</option></select></label>
        </>}
        {scope === 'junction' && <>
          <label><span>节点 ID<small>NODE INDEX</small></span><input type="number" min="0" value={nodeId} onChange={(event) => setNodeId(Number(event.target.value))} /></label>
          <button className={pickingJunction ? 'control-pick is-active' : 'control-pick'} type="button" onClick={() => onPickingJunctionChange(!pickingJunction)}><MapPin size={12} />{pickingJunction ? '点击地图节点…' : '地图拾取'}</button>
          <label><span>控制命令</span><select value={junctionAction} onChange={(event) => setJunctionAction(event.target.value as JunctionControlAction)}><option value="close">封闭</option><option value="open">开放</option></select></label>
        </>}
        {((scope === 'vehicle' && vehicleAction === 'speedFactor') ||
          (scope === 'road' && ['speedFactor', 'capacityFactor'].includes(roadAction))) && (
          <label><span>控制系数<small>MULTIPLIER</small></span><input type="number" min="0.05" max={roadAction === 'capacityFactor' ? 10 : 3} step="0.05" value={factor} onChange={(event) => setFactor(Number(event.target.value))} /></label>
        )}
      </div>
      {error && <div className="control-editor__error"><Ban size={11} />{error}</div>}
      <button className="control-add" type="button" onClick={addControl}><Plus size={13} />加入控制时间线</button>
      {timeline.length > 0 && <div className="control-timeline">
        {timeline.map((item) => <div className={`control-event control-event--${item.scope}`} key={`${item.scope}-${item.index}`}>
          <time>{item.time.toFixed(1)}<small>s</small></time>
          <i />
          <div><strong>{item.target}</strong><span>{item.action}{item.value !== undefined ? ` · ${item.value}×` : ''}</span></div>
          <button type="button" aria-label="删除控制" onClick={() => removeControl(item)}><Trash2 size={11} /></button>
        </div>)}
      </div>}
      <p className="control-editor__note">命令在最近的仿真 tick 生效；同一时刻按列表顺序确定性执行。</p>
    </div>
  )
}
