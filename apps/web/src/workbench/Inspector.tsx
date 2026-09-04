import { Activity, Route } from 'lucide-react'
import type { MapLibraryApi } from '../hooks/useMapLibrary'
import type { VehicleFrameGeoJSON } from '../types'
import type { MapInteractionApi } from './useMapInteraction'
import type { PlaybackApi } from './usePlayback'
import type { RouteSimApi } from './useRouteSimulation'
import { DetailInspector } from './DetailInspector'
import { QualityInspector } from './QualityInspector'
import { RouteSimInspector } from './RouteSimInspector'

export type InspectorPanel = 'details' | 'quality' | 'route'

interface InspectorProps {
  panel: InspectorPanel
  onPanelChange(panel: InspectorPanel): void
  library: MapLibraryApi
  interaction: MapInteractionApi
  routeSim: RouteSimApi
  playback: PlaybackApi
  vehicleFrame: VehicleFrameGeoJSON
}

export function Inspector({
  panel, onPanelChange, library, interaction, routeSim, playback, vehicleFrame,
}: InspectorProps) {
  return (
    <aside className="wb-inspector">
      <div className="wb-inspector-head">
        <div>
          <span className="eyebrow">INSPECTOR</span>
          <h2>地图信息</h2>
        </div>
        <Activity size={18} />
      </div>

      <div className="wb-tabs" role="tablist" aria-label="地图信息切换">
        <button type="button" role="tab" aria-selected={panel === 'details'} className={panel === 'details' ? 'is-active' : ''} onClick={() => onPanelChange('details')}>详情</button>
        <button type="button" role="tab" aria-selected={panel === 'quality'} className={panel === 'quality' ? 'is-active' : ''} onClick={() => onPanelChange('quality')}>质检 <span>{library.activeMap?.issues.length ?? 0}</span></button>
        <button type="button" role="tab" aria-selected={panel === 'route'} className={panel === 'route' ? 'is-active' : ''} onClick={() => onPanelChange('route')}><Route size={13} /> 路由 / 仿真</button>
      </div>

      <div className="wb-inspector-scroll">
        {panel === 'details' && (
          <DetailInspector interaction={interaction} library={library} />
        )}
        {panel === 'quality' && (
          <QualityInspector library={library} interaction={interaction} />
        )}
        {panel === 'route' && (
          <RouteSimInspector
            library={library}
            routeSim={routeSim}
            playback={playback}
            interaction={interaction}
            vehicleFrame={vehicleFrame}
          />
        )}
      </div>

      <div className="wb-runtime">
        <span><i /> MAP RUNTIME</span>
        <strong>{library.activeMap ? 'INDEX READY' : 'STANDBY'}</strong>
      </div>
    </aside>
  )
}
