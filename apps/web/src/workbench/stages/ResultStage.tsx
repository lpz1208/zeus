import { AlertTriangle, LoaderCircle, Play, X } from 'lucide-react'
import type { MapLibraryApi } from '../../hooks/useMapLibrary'
import type { RouteSimApi } from '../useRouteSimulation'
import { formatDuration, formatNumber } from '../format'

interface ResultStageProps {
  library: MapLibraryApi
  routeSim: RouteSimApi
}

export function ResultStage({ library, routeSim }: ResultStageProps) {
  const { activeMap } = library
  const { routeStart, routeEnd, routeAlgorithm, simBusy, simResult, simConfig } = routeSim
  return (
    <section className="wb-section">
      <button
        className="wb-sim-run"
        type="button"
        disabled={!activeMap || !routeStart || !routeEnd || simBusy}
        onClick={() => void routeSim.runSimulation()}
      >
        {simBusy ? <LoaderCircle className="spin" size={15} /> : <Play size={14} fill="currentColor" />}
        <span>{simBusy ? 'C++ 内核计算中' : `运行 ${formatNumber(simConfig.count)} 辆车`}</span>
        <small>{routeAlgorithm.toUpperCase()}</small>
      </button>

      {routeSim.error && (
        <div className="wb-error" role="alert">
          <AlertTriangle size={14} />
          <span>{routeSim.error}</span>
          <button type="button" aria-label="关闭错误" onClick={routeSim.dismissError}><X size={12} /></button>
        </div>
      )}

      {simResult?.ok && (
        <div className="wb-result">
          <div className="wb-result__headline">
            <div><span>到达率</span><strong>{Math.round(simResult.arrived / Math.max(1, simResult.vehicles) * 100)}<small>%</small></strong></div>
            <div className="wb-result__status"><i /><span>{simResult.deadlock ? 'DEADLOCK' : 'COMPLETE'}</span><small>{simResult.computeMs.toFixed(1)} ms</small></div>
          </div>
          <div className="wb-result-grid">
            <div><span>已到达</span><strong>{formatNumber(simResult.arrived)}</strong></div>
            <div><span>平均耗时</span><strong>{formatDuration(simResult.avgTravelS)}</strong></div>
            <div><span>行驶距离</span><strong>{(simResult.totalDistanceM / 1000).toFixed(1)} km</strong></div>
            <div><span>路线计算</span><strong>{formatNumber(simResult.routePlans)}</strong></div>
          </div>
          {simResult.controlEvents > 0 && (
            <div className="wb-result-note is-control">
              <span>已应用 {simResult.controlEvents} 条控制</span>
              <small>车辆 {simResult.vehicleControls} · 道路 {simResult.roadControls} · 路口 {simResult.junctionControls}</small>
            </div>
          )}
          {simResult.rerouteAttempts > 0 && (
            <div className="wb-result-note is-reroute">
              <span>动态重规划 {simResult.rerouteAttempts} 次</span>
              <small>成功 {simResult.rerouteSucceeded} · 失败 {simResult.rerouteFailed}</small>
            </div>
          )}
          {simResult.signalPlans > 0 && (
            <div className="wb-result-note is-signal">
              <span>信号控制 {simResult.signalPlans} 个路口</span>
              <small>{simResult.signalPhases} 相位 · 红灯 {simResult.signalRedWaitEvents} · 饱和 {simResult.signalSaturationWaitEvents} · 放行 {simResult.signalMovementsPassed}</small>
            </div>
          )}
          {(simResult.waitingAtEnd > 0 || simResult.drivingAtEnd > 0 || simResult.unroutable > 0) && (
            <div className="wb-result__residual">
              <span>等待 {simResult.waitingAtEnd}</span>
              <span>行驶中 {simResult.drivingAtEnd}</span>
              <span>不可规划 {simResult.unroutable}</span>
            </div>
          )}
        </div>
      )}
    </section>
  )
}
