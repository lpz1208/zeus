import {
  Gauge,
  Layers3,
  MousePointer2,
  Route,
  Search,
  Waypoints,
} from 'lucide-react'
import type { MapLibraryApi } from '../hooks/useMapLibrary'
import type { MapInteractionApi } from './useMapInteraction'
import { formatReferenceValue } from './format'

interface DetailInspectorProps {
  interaction: MapInteractionApi
  library: MapLibraryApi
}

export function DetailInspector({ interaction, library }: DetailInspectorProps) {
  const { selectedReferenceFeature, selectedRoad, bestMatch, pointer } = interaction
  return (
    <>
      {selectedReferenceFeature ? (
        <section className="wb-section">
          <div className="section-label"><Layers3 size={14} /><span>参考要素</span><small>SELECTED</small></div>
          <div className="wb-feature-card">
            <div className="wb-feature-card__head">
              <span>{selectedReferenceFeature.geometryType}</span>
              <strong>{selectedReferenceFeature.layerName}</strong>
              <small>FEATURE {selectedReferenceFeature.featureIndex + 1}</small>
            </div>
            <dl>
              {Object.entries(selectedReferenceFeature.properties).length === 0 ? (
                <div><dt>属性</dt><dd>无业务属性</dd></div>
              ) : Object.entries(selectedReferenceFeature.properties).slice(0, 12).map(([key, value]) => (
                <div key={key}><dt>{key}</dt><dd title={formatReferenceValue(value)}>{formatReferenceValue(value)}</dd></div>
              ))}
            </dl>
          </div>
        </section>
      ) : selectedRoad ? (
        <section className="wb-section">
          <div className="section-label"><Route size={14} /><span>道路属性</span><small>SELECTED</small></div>
          <div className="wb-road-card">
            <div className="wb-road-card__title">
              <span>{selectedRoad.CLASS || 'UNCLASSIFIED'}</span>
              <strong>{selectedRoad.SOURCE_ID || selectedRoad.ROAD_ID}</strong>
            </div>
            <dl>
              <div><dt>方向</dt><dd>{{ both: '双向', forward: '正向单行', reverse: '反向单行' }[selectedRoad.DIRECTION]}</dd></div>
              <div><dt>限速</dt><dd>{Number(selectedRoad.SPEED_KPH).toFixed(0)} km/h</dd></div>
              <div><dt>长度</dt><dd>{Number(selectedRoad.LENGTH_M).toFixed(1)} m</dd></div>
              <div><dt>Z LEVEL</dt><dd>{selectedRoad.Z_LEVEL}</dd></div>
              <div className="wb-road-card__wide"><dt>ROAD ID</dt><dd>{selectedRoad.ROAD_ID}</dd></div>
              <div className="wb-road-card__wide"><dt>EDGE IDS</dt><dd>{selectedRoad.EDGE_IDS}</dd></div>
            </dl>
          </div>
        </section>
      ) : (
        <div className="wb-empty">
          <MousePointer2 size={22} />
          <strong>选择地图要素</strong>
          <span>点击道路、行政区或 POI，在这里查看详细属性。</span>
        </div>
      )}

      <section className="wb-section wb-locator">
        <div className="section-label"><Search size={14} /><span>位置解析</span><small>RTREE</small></div>
        <div className="wb-coords">
          <span>LON <strong>{pointer[0].toFixed(6)}</strong></span>
          <span>LAT <strong>{pointer[1].toFixed(6)}</strong></span>
        </div>
        {bestMatch ? (
          <div className="wb-match">
            <div className="wb-match__head"><span>BEST MATCH</span><strong>{Math.round(bestMatch.confidence * 100)}%</strong></div>
            <div className="wb-match-road"><Waypoints size={18} /><strong>{bestMatch.source}</strong><span>EDGE {bestMatch.edge}</span></div>
            <dl>
              <div><dt>道路偏移</dt><dd>{bestMatch.offsetS.toFixed(2)} m</dd></div>
              <div><dt>横向距离</dt><dd>{bestMatch.distance.toFixed(2)} m</dd></div>
              <div><dt>ROAD ID</dt><dd>{bestMatch.roadId.slice(-12)}</dd></div>
            </dl>
          </div>
        ) : <div className="wb-locator-empty"><Gauge size={18} /><span>{library.activeMap ? '点击地图开始道路匹配' : '发布道路地图后可用'}</span></div>}
      </section>
    </>
  )
}
