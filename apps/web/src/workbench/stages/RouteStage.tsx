import {
  AlertTriangle,
  EyeOff,
  LoaderCircle,
  MapPinned,
  Route,
  X,
  Zap,
} from 'lucide-react'
import type { MapLibraryApi } from '../../hooks/useMapLibrary'
import type { RouteSimApi } from '../useRouteSimulation'
import { formatDuration, formatNumber, routeFailureLabel } from '../format'

interface RouteStageProps {
  library: MapLibraryApi
  routeSim: RouteSimApi
}

export function RouteStage({ library, routeSim }: RouteStageProps) {
  const { activeMap } = library
  const { routeMode, routeAlgorithm, routeStart, routeEnd, routeResult, routeBusy } = routeSim
  return (
    <section className="wb-section wb-route">
      <div className="section-label">
        <Route size={14} />
        <span>路径规划</span>
        <small>{activeMap ? 'ROUTING' : 'NO MAP'}</small>
      </div>
      <div className="wb-route-controls">
        <button
          type="button"
          className={`wb-route-toggle ${routeMode ? 'is-active' : ''}`}
          onClick={routeSim.toggleRouteMode}
          disabled={!activeMap}
        >
          {routeMode ? <><EyeOff size={13} /> 退出路由模式</> : <><Zap size={13} /> 激活路由模式</>}
        </button>
        <label className="wb-algo">
          <span>算法</span>
          <select
            value={routeAlgorithm}
            onChange={(event) => routeSim.setRouteAlgorithm(event.target.value as typeof routeAlgorithm)}
          >
            <option value="dijkstra">DIJKSTRA</option>
            <option value="astar">A*</option>
            <option value="bidijkstra">BI-DIJKSTRA</option>
            <option value="biastar">BI-A*</option>
          </select>
        </label>
        {(routeStart || routeEnd || routeResult) && (
          <button type="button" className="wb-route-clear" onClick={routeSim.clearRoute}>
            <X size={13} /> 清除
          </button>
        )}
      </div>
      {!activeMap ? (
        <div className="wb-route-state"><Route size={18} /><span>发布道路地图后可用路径规划。</span></div>
      ) : routeBusy ? (
        <div className="wb-route-state is-loading"><LoaderCircle className="spin" size={18} /><span>正在计算路径…</span></div>
      ) : routeResult ? (
        routeResult.ok ? (
          <>
            <div className="metric-matrix">
              <article><span>距离</span><strong>{(routeResult.lengthM / 1000).toFixed(2)}<small>km</small></strong></article>
              <article><span>预计时长</span><strong>{formatDuration(routeResult.timeS)}</strong></article>
              <article><span>扩展节点</span><strong>{formatNumber(routeResult.expandedNodes)}</strong></article>
              <article><span>计算耗时</span><strong>{routeResult.computeMs.toFixed(1)}<small>ms</small></strong></article>
              <article><span>途经边</span><strong>{formatNumber(routeResult.edges)}</strong></article>
              <article>
                <span>算法</span>
                <strong title={routeResult.effectiveAlgorithm && routeResult.effectiveAlgorithm !== routeResult.algorithm ? '该地图含转向限制，双向算法退化为前向搜索' : undefined}>
                  {routeResult.effectiveAlgorithm && routeResult.effectiveAlgorithm !== routeResult.algorithm
                    ? `${routeResult.algorithm.toUpperCase()} → ${routeResult.effectiveAlgorithm.toUpperCase()}`
                    : routeResult.algorithm.toUpperCase()}
                </strong>
              </article>
            </div>
            <div className="wb-snaps">
              <div className="wb-snap">
                <span>起点吸附</span>
                <strong>{routeResult.origin.source || `EDGE ${routeResult.origin.edge}`}</strong>
                <small>偏移 {routeResult.origin.offsetS.toFixed(1)} m · 距路 {routeResult.origin.distance.toFixed(1)} m · 置信 {Math.round(routeResult.origin.confidence * 100)}%</small>
              </div>
              <div className="wb-snap">
                <span>终点吸附</span>
                <strong>{routeResult.destination.source || `EDGE ${routeResult.destination.edge}`}</strong>
                <small>偏移 {routeResult.destination.offsetS.toFixed(1)} m · 距路 {routeResult.destination.distance.toFixed(1)} m · 置信 {Math.round(routeResult.destination.confidence * 100)}%</small>
              </div>
            </div>
          </>
        ) : (
          <div className="wb-route-state is-failed">
            <AlertTriangle size={18} />
            <strong>{routeFailureLabel(routeResult.reason)}</strong>
            <span>{routeResult.message}</span>
          </div>
        )
      ) : (
        <div className="wb-route-state">
          <MapPinned size={18} />
          <span>{routeMode
            ? routeStart
              ? '已设定起点，在地图上点击终点。'
              : '在地图上点击起点。'
            : '激活路由模式后，在地图上依次点击起点和终点。'}</span>
        </div>
      )}
    </section>
  )
}
