import { useState } from 'react'
import type { MapLibraryApi } from '../hooks/useMapLibrary'
import type { VehicleFrameGeoJSON } from '../types'
import type { MapInteractionApi } from './useMapInteraction'
import type { PlaybackApi } from './usePlayback'
import type { RouteSimApi } from './useRouteSimulation'
import { RouteStage } from './stages/RouteStage'
import { ScenarioStage } from './stages/ScenarioStage'
import { ResultStage } from './stages/ResultStage'
import { PlaybackStage } from './stages/PlaybackStage'
import { StageSection, type StageStatus } from './stages/StageSection'

type StageId = 'route' | 'scenario' | 'run' | 'playback'

interface RouteSimInspectorProps {
  library: MapLibraryApi
  routeSim: RouteSimApi
  playback: PlaybackApi
  interaction: MapInteractionApi
  vehicleFrame: VehicleFrameGeoJSON
}

const STAGE_ORDER: { id: StageId; index: 1 | 2 | 3 | 4; title: string; subtitle: string }[] = [
  { id: 'route', index: 1, title: '路线', subtitle: 'ROUTE · OD 与算法' },
  { id: 'scenario', index: 2, title: '场景', subtitle: 'SCENARIO · 参数 · 控制 · 信号' },
  { id: 'run', index: 3, title: '运行与结果', subtitle: 'RUN · 确定性车流' },
  { id: 'playback', index: 4, title: '回放', subtitle: 'PLAYBACK · 轨迹时间轴' },
]

/**
 * Staged 路由/仿真 flow. Open state is purely derived: the first failed or
 * active stage auto-expands unless the user pinned a stage; a completed
 * earlier stage collapses while its successors open.
 */
export function RouteSimInspector({
  library, routeSim, playback, interaction, vehicleFrame,
}: RouteSimInspectorProps) {
  const [userOpen, setUserOpen] = useState<Partial<Record<StageId, boolean>>>({})

  const statusOf = (id: StageId): StageStatus => {
    const hasMap = Boolean(library.activeMap)
    const odLocked = Boolean(routeSim.routeStart && routeSim.routeEnd)
    switch (id) {
      case 'route':
        if (!hasMap) return 'idle'
        if (routeSim.routeResult) return routeSim.routeResult.ok ? 'done' : 'failed'
        return 'active'
      case 'scenario':
        if (!odLocked) return 'idle'
        if (routeSim.simResult?.ok && !routeSim.scenarioDirty) return 'done'
        return 'active'
      case 'run':
        if (!odLocked) return 'idle'
        if (routeSim.simResult) return routeSim.simResult.ok ? 'done' : 'failed'
        return 'active'
      case 'playback':
        if (!routeSim.simResult?.playback) return 'idle'
        return playback.time > 0 ? 'done' : 'active'
    }
  }

  const statuses = STAGE_ORDER.map((stage) => statusOf(stage.id))
  const autoStage = STAGE_ORDER.find((_, index) =>
    statuses[index] === 'failed' || statuses[index] === 'active')?.id

  const effectiveOpen = (id: StageId) => userOpen[id] ?? id === autoStage

  const toggle = (id: StageId) => {
    setUserOpen((current) => ({ ...current, [id]: !effectiveOpen(id) }))
  }

  return (
    <div className="wb-stages">
      {STAGE_ORDER.map(({ id, index, title, subtitle }) => (
        <section className={`wb-stage is-${statuses[index - 1]}`} key={id}>
          <StageSection
            index={index}
            title={title}
            subtitle={subtitle}
            status={statuses[index - 1]}
            open={effectiveOpen(id)}
            onToggle={() => toggle(id)}
          />
          {effectiveOpen(id) && (
            <div className="wb-stage__body">
              {id === 'route' && <RouteStage library={library} routeSim={routeSim} />}
              {id === 'scenario' && <ScenarioStage routeSim={routeSim} interaction={interaction} />}
              {id === 'run' && <ResultStage library={library} routeSim={routeSim} />}
              {id === 'playback' && (
                <PlaybackStage
                  playback={playback}
                  routeSim={routeSim}
                  vehicleFrame={vehicleFrame}
                />
              )}
            </div>
          )}
        </section>
      ))}
    </div>
  )
}
