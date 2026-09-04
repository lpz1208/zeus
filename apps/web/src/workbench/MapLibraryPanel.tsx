import { ChevronRight, Layers3, MapPinned, Network } from 'lucide-react'
import type { MapLibraryApi } from '../hooks/useMapLibrary'
import { formatNumber } from './format'

export function MapLibraryPanel({ library }: { library: MapLibraryApi }) {
  const { maps, activeMap, loadError } = library
  return (
    <section className="wb-lib">
      <div className="section-label">
        <Layers3 size={14} />
        <span>地图版本</span>
        <small>{maps.length}</small>
      </div>
      {loadError && <div className="wb-error is-compact" role="alert">{loadError}</div>}
      {maps.length === 0 ? (
        <div className="wb-lib-empty">
          <Network size={21} />
          <p>尚未发布地图</p>
          <span>上传道路数据以创建第一个版本</span>
        </div>
      ) : maps.map((record) => (
        <button
          type="button"
          className={`wb-map-row ${activeMap?.id === record.id ? 'is-active' : ''}`}
          key={record.id}
          onClick={() => library.activateMap(record)}
        >
          <MapPinned size={16} />
          <span><strong>{record.name}</strong><small>{formatNumber(record.summary.directedEdges)} EDGES · {record.id.slice(-6)}</small></span>
          <ChevronRight size={14} />
        </button>
      ))}
    </section>
  )
}
