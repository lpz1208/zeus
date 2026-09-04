import { Cpu, Database } from 'lucide-react'
import type { MapIntakeApi } from './useMapIntake'
import type { MapLibraryApi } from '../hooks/useMapLibrary'
import type { ReferenceLibraryApi } from '../hooks/useReferenceLayers'
import { IntakePanel } from './IntakePanel'
import { MapLibraryPanel } from './MapLibraryPanel'
import { ReferenceLibraryPanel } from './ReferenceLibraryPanel'

interface LeftRailProps {
  intake: MapIntakeApi
  library: MapLibraryApi
  references: ReferenceLibraryApi
  onVisibilityToggle(id: string): void
  onLayerDeleted(id: string): void
}

export function LeftRail({
  intake, library, references, onVisibilityToggle, onLayerDeleted,
}: LeftRailProps) {
  return (
    <aside className="wb-rail">
      <div className="wb-rail-head">
        <div>
          <span className="eyebrow">DATA INTAKE</span>
          <h1>地图编译台</h1>
        </div>
        <Database size={18} />
      </div>

      <IntakePanel intake={intake} />
      <MapLibraryPanel library={library} />
      <ReferenceLibraryPanel
        references={references}
        onVisibilityToggle={onVisibilityToggle}
        onLayerDeleted={onLayerDeleted}
      />

      <div className="wb-footnote">
        <Cpu size={13} /> C++20 MAP CORE
        <span>GDAL / BOOST RTREE</span>
      </div>
    </aside>
  )
}
