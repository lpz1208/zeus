import { useEffect, useMemo, useState } from 'react'
import { CircleDot } from 'lucide-react'
import { emptyVehicleFrame } from '../types'
import type { MapLibraryApi } from '../hooks/useMapLibrary'
import type { ReferenceLibraryApi } from '../hooks/useReferenceLayers'
import { MapCanvas } from '../MapCanvas'
import { buildVehicleFrame } from '../playback'
import { LeftRail } from './LeftRail'
import { Inspector, type InspectorPanel } from './Inspector'
import { WorkbenchTopbar } from './WorkbenchTopbar'
import { useMapIntake } from './useMapIntake'
import { useMapInteraction } from './useMapInteraction'
import { usePlayback } from './usePlayback'
import { useRouteSimulation } from './useRouteSimulation'
import './workbench.css'
import { useAlgorithmLab } from '../algorithm/useAlgorithmLab'
import { AlgorithmConsole } from '../algorithm/AlgorithmConsole'

interface MapWorkbenchProps {
  library: MapLibraryApi
  references: ReferenceLibraryApi
  onEnterAgent(): void
  onEnterBenchmark(): void
}

/**
 * Main map workbench shell: composes the workbench-local hooks, owns the
 * inspector panel and the reset-on-map-change choreography, and wires the
 * full MapCanvas surface.
 */
export function MapWorkbench({
  library, references, onEnterAgent, onEnterBenchmark,
}: MapWorkbenchProps) {
  const [panel, setPanel] = useState<InspectorPanel>('details')
  const intake = useMapIntake({
    hasActiveMap: Boolean(library.activeMap),
    onMapImported: library.registerImportedMap,
    onReferencePublished: references.addPublished,
    markServiceOnline: library.markOnline,
  })
  const interaction = useMapInteraction(library.activeMap)
  const routeSim = useRouteSimulation(library.activeMap, library.roadData, {
    onRouteReady: () => setPanel('route'),
  })
  const playback = usePlayback(routeSim.simResult)
  const lab = useAlgorithmLab(library.activeMap?.id)

  // Map switch: drop route/simulation/selection state (simulation params
  // survive in sessionStorage inside useRouteSimulation).
  const activeMapId = library.activeMap?.id
  useEffect(() => {
    if (!activeMapId) return
    routeSim.resetForMapChange()
    interaction.clearSelection()
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [activeMapId])

  const vehicleFrame = useMemo(
    () => routeSim.simResult?.playback
      ? buildVehicleFrame(routeSim.simResult.playback, playback.time)
      : emptyVehicleFrame,
    [playback.time, routeSim.simResult],
  )

  const onVisibilityToggle = (id: string) => {
    if (!references.toggleVisibility(id)) interaction.clearReferenceFeatureIfLayer(id)
  }

  const onLayerDeleted = (id: string) => {
    interaction.clearReferenceFeatureIfLayer(id)
  }

  return (
    <main className={`wb-shell ${lab.opened ? 'is-algorithm' : ''}`}>
      <WorkbenchTopbar
        intakeStage={intake.stage}
        serviceOnline={library.serviceOnline}
        agentEnabled={Boolean(library.activeMap)}
        algorithmOpen={lab.opened}
        onToggleAlgorithm={() => lab.opened ? lab.close() : lab.open(routeSim.routeStart, routeSim.routeEnd)}
        onEnterAgent={onEnterAgent}
        onEnterBenchmark={onEnterBenchmark}
      />

      {!lab.opened && <LeftRail
        intake={intake}
        library={library}
        references={references}
        onVisibilityToggle={onVisibilityToggle}
        onLayerDeleted={onLayerDeleted}
      />}

      <MapCanvas
        data={library.roadData}
        nodeData={library.nodeData}
        issueData={library.issueData}
        referenceLayers={references.views}
        referenceFocus={references.focusRequest}
        mapName={library.activeMap?.name
          ?? references.views.find((layer) => layer.visible)?.record.name
          ?? 'BEIJING / SYNTHETIC GRID'}
        isDemo={!library.activeMap}
        selectedMatch={interaction.selectedMatch}
        selectedRoad={interaction.selectedRoad}
        selectedReferenceFeature={interaction.selectedReferenceFeature}
        focusedIssueIndex={interaction.focusedIssueIndex}
        routeMode={lab.opened ? lab.picking : routeSim.routeMode}
        routeStart={lab.opened ? lab.from : routeSim.routeStart}
        routeEnd={lab.opened ? lab.to : routeSim.routeEnd}
        routeData={lab.opened ? lab.routeData : routeSim.routeMainData}
        routeAlternativesData={lab.opened ? null : routeSim.routeGhostData}
        searchTrace={lab.opened ? lab.trace : routeSim.routeResult?.searchTrace ?? null}
        searchTraceProgress={lab.opened ? lab.playback.progress : routeSim.tracePlayback.progress}
        trajectoryData={lab.opened ? null : routeSim.simResult?.geojson ?? null}
        vehicleFrame={lab.opened ? emptyVehicleFrame : vehicleFrame}
        junctionPickMode={!lab.opened && routeSim.junctionPickMode}
        selectedControlNodeId={routeSim.selectedControlNodeId}
        onQuery={interaction.query}
        onRoutePoint={lab.opened ? lab.pick : routeSim.handleRoutePoint}
        onIssueSelect={interaction.selectIssue}
        onRoadSelect={(road) => {
          interaction.selectRoad(road)
          if (panel !== 'route') setPanel('details')
        }}
        onJunctionSelect={routeSim.selectJunction}
        onReferenceFeatureSelect={(feature) => {
          interaction.selectReferenceFeature(feature)
          if (feature && panel !== 'details') setPanel('details')
        }}
        onPointerMove={interaction.setPointer}
      />

      {lab.opened ? <AlgorithmConsole lab={lab} mapName={library.activeMap?.name ?? '未选择地图'} /> : <Inspector
        panel={panel}
        onPanelChange={setPanel}
        library={library}
        interaction={interaction}
        routeSim={routeSim}
        playback={playback}
        vehicleFrame={vehicleFrame}
      />}

      <footer className="wb-statusbar">
        <span><i className="wb-statusbar__live" /> ZEUS MAP KERNEL</span>
        <span>CRS <strong>{intake.inspect?.crs || 'WGS84 DISPLAY'}</strong></span>
        <span>POINTER <strong>{interaction.pointer[0].toFixed(5)}, {interaction.pointer[1].toFixed(5)}</strong></span>
        <span className="wb-statusbar__right">IMMUTABLE RUNTIME MAP <CircleDot size={9} /></span>
      </footer>
    </main>
  )
}
