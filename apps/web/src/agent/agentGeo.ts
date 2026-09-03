import type {
  RoadGeoJSON,
  RouteAlgorithm,
  RouteGeoJSON,
  RouteProperties,
} from '../types'

export const algorithmLabels: Record<RouteAlgorithm, string> = {
  dijkstra: 'Dijkstra',
  astar: 'A*',
  bidijkstra: 'Bi-Dijkstra',
  biastar: 'Bi-A*',
}

export function formatTime(seconds: number): string {
  const total = Math.max(0, Math.round(seconds))
  const minutes = Math.floor(total / 60)
  return `${String(minutes).padStart(2, '0')}:${String(total % 60).padStart(2, '0')}`
}

export function formatCoordinate(point: [number, number] | null): string {
  return point ? `${point[0].toFixed(5)}, ${point[1].toFixed(5)}` : '点击地图设置'
}

/** Collects the road features carrying any of the given directed edges. */
export function routeForEdges(
  data: RoadGeoJSON,
  edges: number[] | undefined,
): RouteGeoJSON | null {
  if (!edges?.length) return null
  const selected = new Set(edges)
  const features = data.features.flatMap((feature) => {
    const ids = parseEdgeIds(feature.properties.EDGE_IDS)
    if (!ids.some((edge) => selected.has(edge))) return []
    const properties: RouteProperties = {
      ROAD_ID: feature.properties.ROAD_ID,
      SOURCE_ID: feature.properties.SOURCE_ID,
      CLASS: feature.properties.CLASS,
      LENGTH_M: feature.properties.LENGTH_M,
      EDGE_INDEX: ids.find((edge) => selected.has(edge)) ?? ids[0] ?? 0,
    }
    return [{ ...feature, properties }]
  })
  return { type: 'FeatureCollection', features }
}

function parseEdgeIds(raw: string | undefined): number[] {
  return String(raw ?? '')
    .split(',')
    .map((value) => Number(value.trim()))
    .filter(Number.isFinite)
}

// ---------------------------------------------------------------------------
// Directed-edge geometry lookup: converts an agent observation's
// (edgeId, offsetM) into a lon/lat map position. offsetM is METERS from the
// directed edge start (the engine stores route start offsets in meters).
//
// One roads.geojson feature is one road piece; its EDGE_IDS list directed
// edges in index order and DIRECTION tells how the two twins map onto the
// polyline. The builder appends the forward edge before the reverse twin, so
// a target edge runs polyline start->end unless it is the reverse twin
// (see map_builder.cc appendEdge and geojson_exporter.cc EDGE_IDS).
// ---------------------------------------------------------------------------

export interface RoadEdgeLocation {
  /** lon/lat vertices of the road feature polyline */
  coordinates: [number, number][]
  /** cumulative geodesic length in meters at each vertex */
  cumulativeM: number[]
  lengthM: number
  /** true when the directed edge runs polyline end -> start */
  reversed: boolean
}

export type RoadEdgeIndex = Map<number, RoadEdgeLocation>

const EARTH_RADIUS_M = 6371008.8

function haversineM(a: [number, number], b: [number, number]): number {
  const toRad = Math.PI / 180
  const dLat = (b[1] - a[1]) * toRad
  const dLon = (b[0] - a[0]) * toRad
  const sinLat = Math.sin(dLat / 2)
  const sinLon = Math.sin(dLon / 2)
  const h = sinLat * sinLat +
    Math.cos(a[1] * toRad) * Math.cos(b[1] * toRad) * sinLon * sinLon
  return 2 * EARTH_RADIUS_M * Math.asin(Math.min(1, Math.sqrt(h)))
}

export function buildRoadEdgeIndex(data: RoadGeoJSON): RoadEdgeIndex {
  const index: RoadEdgeIndex = new Map()
  for (const feature of data.features) {
    const line = feature.geometry
    if (line.type !== 'LineString' || line.coordinates.length < 2) continue
    const coordinates = line.coordinates.map(
      (point): [number, number] => [point[0], point[1]],
    )
    const cumulativeM: number[] = [0]
    for (let i = 1; i < coordinates.length; ++i) {
      cumulativeM.push(
        cumulativeM[i - 1] + haversineM(coordinates[i - 1], coordinates[i]),
      )
    }
    const lengthM = cumulativeM[cumulativeM.length - 1]
    if (!(lengthM > 0)) continue
    const ids = parseEdgeIds(feature.properties.EDGE_IDS)
    const direction = feature.properties.DIRECTION
    ids.forEach((id, position) => {
      if (index.has(id)) return // first write wins on duplicates
      index.set(id, {
        coordinates,
        cumulativeM,
        lengthM,
        reversed: direction === 'reverse' ||
          (direction === 'both' && position !== 0),
      })
    })
  }
  return index
}

/** Interpolates meters-along-edge into a lon/lat point; null when unknown. */
export function positionOnRoad(
  index: RoadEdgeIndex,
  edgeId: number,
  offsetM: number,
): [number, number] | null {
  const edge = index.get(edgeId)
  if (!edge) return null
  const distance = Math.min(Math.max(offsetM, 0), edge.lengthM)
  const along = edge.reversed ? edge.lengthM - distance : distance
  let segment = 1
  while (segment < edge.cumulativeM.length - 1 &&
         edge.cumulativeM[segment] < along) {
    ++segment
  }
  const start = edge.coordinates[segment - 1]
  const end = edge.coordinates[segment]
  const span = edge.cumulativeM[segment] - edge.cumulativeM[segment - 1]
  const t = span > 0 ? (along - edge.cumulativeM[segment - 1]) / span : 0
  return [
    start[0] + (end[0] - start[0]) * t,
    start[1] + (end[1] - start[1]) * t,
  ]
}
