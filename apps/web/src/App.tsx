import { useState } from 'react'
import { AgentWorkbench } from './agent/AgentWorkbench'
import { useMapLibrary } from './hooks/useMapLibrary'
import { useReferenceLayers } from './hooks/useReferenceLayers'
import { MapWorkbench } from './workbench/MapWorkbench'

/**
 * Workspace switcher. Map-version and reference-layer state is shared so it
 * survives round-trips into the agent workbench; everything else is local to
 * the workbench that owns it.
 */
export function App() {
  const [workspace, setWorkspace] = useState<'map' | 'agent'>('map')
  const library = useMapLibrary()
  const references = useReferenceLayers(library.maps, library.mapsLoaded)

  if (workspace === 'agent') {
    return (
      <AgentWorkbench
        maps={library.maps}
        activeMap={library.activeMap}
        onMapChange={library.activateMap}
        data={library.roadData}
        nodeData={library.nodeData}
        issueData={library.issueData}
        referenceLayers={references.views}
        onExit={() => setWorkspace('map')}
      />
    )
  }

  return (
    <MapWorkbench
      library={library}
      references={references}
      onEnterAgent={() => setWorkspace('agent')}
    />
  )
}
