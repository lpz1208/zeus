import { CarFront } from 'lucide-react'
import type { MapInteractionApi } from '../useMapInteraction'
import type { RouteSimApi } from '../useRouteSimulation'
import { SimulationControlPanel } from '../../SimulationControlPanel'
import { SignalPlanPanel } from '../../SignalPlanPanel'

interface ScenarioStageProps {
  routeSim: RouteSimApi
  interaction: MapInteractionApi
}

export function ScenarioStage({ routeSim, interaction }: ScenarioStageProps) {
  const { simConfig, simControls, routeStart, routeEnd } = routeSim
  return (
    <>
      <section className="wb-section">
        <div className="section-label">
          <CarFront size={14} />
          <span>交通仿真</span>
          <small>MESOSCOPIC</small>
        </div>
        <div className={`wb-readiness ${routeStart && routeEnd ? 'is-ready' : ''}`}>
          <i />
          <span>{routeStart && routeEnd ? 'OD 已锁定，可生成确定性车流' : '先在地图上设置起点和终点'}</span>
        </div>
        <div className="wb-params">
          <label><span>车辆数<small>VEHICLES</small></span><input type="number" min="1" max="10000" value={simConfig.count} onChange={(event) => routeSim.patchSimConfig({ count: Number(event.target.value) })} /></label>
          <label><span>发车分布<small>SECONDS</small></span><input type="number" min="0" max={simConfig.durationSeconds} value={simConfig.spreadSeconds} onChange={(event) => routeSim.patchSimConfig({ spreadSeconds: Number(event.target.value) })} /></label>
          <label><span>仿真时长<small>SECONDS</small></span><input type="number" min="60" max="28800" value={simConfig.durationSeconds} onChange={(event) => routeSim.patchSimConfig({ durationSeconds: Number(event.target.value) })} /></label>
          <label><span>采样间隔<small>SECONDS</small></span><input type="number" min={Math.max(1, simConfig.stepSeconds)} value={simConfig.sampleIntervalSeconds} onChange={(event) => routeSim.patchSimConfig({ sampleIntervalSeconds: Number(event.target.value) })} /></label>
          <label title="同一路段车辆在自由流状态下连续驶出的最小时间间隔；0 表示关闭出口门控。"><span>自由流出车<small>HEADWAY · S</small></span><input type="number" min="0" max="60" step="0.1" value={simConfig.exitHeadwayFfSeconds} onChange={(event) => { const value = Number(event.target.value); routeSim.patchSimConfig({ exitHeadwayFfSeconds: value, exitHeadwayJamSeconds: simConfig.exitHeadwayJamSeconds === 0 ? 0 : Math.max(value, simConfig.exitHeadwayJamSeconds) }) }} /></label>
          <label title="路段拥堵时连续车辆驶出的最小时间间隔；0 表示沿用自由流值。"><span>拥堵出车<small>HEADWAY · S</small></span><input type="number" min={simConfig.exitHeadwayFfSeconds} max="60" step="0.1" value={simConfig.exitHeadwayJamSeconds} onChange={(event) => { const value = Number(event.target.value); routeSim.patchSimConfig({ exitHeadwayJamSeconds: value === 0 ? 0 : Math.max(value, simConfig.exitHeadwayFfSeconds) }) }} /></label>
          <label title="按此仿真时间间隔从实时路段占用率重建路由权重；0 表示关闭周期扫描。"><span>拥堵扫描<small>REROUTE · S</small></span><input type="number" min="0" max="3600" step="1" value={simConfig.rerouteIntervalSeconds} onChange={(event) => { const value = Number(event.target.value); routeSim.patchSimConfig({ rerouteIntervalSeconds: value === 0 ? 0 : Math.max(value, simConfig.stepSeconds) }) }} /></label>
          <label title="路段动态代价相对上次发布值达到该倍率时，才触发受影响车辆重规划。"><span>权重阈值<small>COST RATIO</small></span><input type="number" min="1.01" max="10" step="0.05" value={simConfig.rerouteCostRatio} onChange={(event) => routeSim.patchSimConfig({ rerouteCostRatio: Math.max(1.01, Number(event.target.value)) }) } /></label>
        </div>
        <p className="wb-note">出口间隔限制路段放行率；拥堵扫描按占用率生成动态权重。两项扫描间隔为 0 时关闭周期重规划，显式封路、限速和降容仍会立即评估。</p>
      </section>
      <SimulationControlPanel
        controls={simControls}
        durationSeconds={simConfig.durationSeconds}
        vehicleCount={simConfig.count}
        selectedRoad={interaction.selectedRoad}
        selectedNodeId={routeSim.selectedControlNodeId}
        pickingJunction={routeSim.junctionPickMode}
        onPickingJunctionChange={routeSim.setJunctionPickMode}
        onChange={routeSim.applyControls}
      />
      <SignalPlanPanel
        plans={simControls.signalPlans}
        selectedNodeId={routeSim.selectedControlNodeId}
        pickingJunction={routeSim.junctionPickMode}
        onPickingJunctionChange={routeSim.setJunctionPickMode}
        onChange={(signalPlans) =>
          routeSim.applyControls({ ...simControls, signalPlans })}
      />
    </>
  )
}
