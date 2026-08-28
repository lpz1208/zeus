import { useEffect, useRef } from 'react'
import {
  LngLatBounds,
  Map as MapLibreMap,
  Marker,
  NavigationControl,
  setWorkerUrl,
  type FilterSpecification,
  type GeoJSONSource,
  type MapMouseEvent,
} from 'maplibre-gl'
import maplibreWorkerUrl from 'maplibre-gl/dist/maplibre-gl-worker.mjs?worker&url'
import { Crosshair, MousePointer2, Route } from 'lucide-react'
import type {
  IssueGeoJSON,
  MatchCandidate,
  NodeGeoJSON,
  QueryResponse,
  ReferenceFeatureSelection,
  ReferenceLayerView,
  RoadGeoJSON,
  RoadProperties,
  RouteGeoJSON,
  TrajectoryGeoJSON,
  VehicleFrameGeoJSON,
} from './types'

setWorkerUrl(maplibreWorkerUrl)

interface MapCanvasProps {
  data: RoadGeoJSON
  nodeData: NodeGeoJSON
  issueData: IssueGeoJSON
  referenceLayers: ReferenceLayerView[]
  referenceFocus: { layerId: string; requestId: number } | null
  mapName: string
  isDemo: boolean
  selectedMatch: MatchCandidate | null
  selectedRoad: RoadProperties | null
  selectedReferenceFeature: ReferenceFeatureSelection | null
  focusedIssueIndex: number | null
  routeMode: boolean
  routeStart: [number, number] | null
  routeEnd: [number, number] | null
  routeData: RouteGeoJSON | null
  trajectoryData: TrajectoryGeoJSON | null
  vehicleFrame: VehicleFrameGeoJSON
  junctionPickMode: boolean
  selectedControlNodeId: number | null
  onQuery: (longitude: number, latitude: number) => Promise<QueryResponse | null>
  onIssueSelect: (index: number) => void
  onRoadSelect: (road: RoadProperties) => void
  onJunctionSelect: (nodeId: number) => void
  onReferenceFeatureSelect: (feature: ReferenceFeatureSelection | null) => void
  onPointerMove: (longitude: number, latitude: number) => void
  onRoutePoint: (longitude: number, latitude: number) => void
}

const emptyFilter: FilterSpecification = ['==', ['get', 'ROAD_ID'], '__none__']

interface StyledReferenceProperties {
  [key: string]: unknown
  __ZEUS_COLOR: string
  __ZEUS_OPACITY: number
  __ZEUS_LAYER_ID: string
  __ZEUS_LAYER_NAME: string
  __ZEUS_FEATURE_INDEX: number
}

type StyledReferenceGeoJSON = GeoJSON.FeatureCollection<
  GeoJSON.Geometry,
  StyledReferenceProperties
>

function combineReferences(layers: ReferenceLayerView[]): StyledReferenceGeoJSON {
  return {
    type: 'FeatureCollection',
    features: layers.flatMap(({ record, data, visible }) => visible
      ? data.features.map((feature, featureIndex) => ({
          ...feature,
          properties: {
            ...(feature.properties ?? {}),
            __ZEUS_COLOR: record.style.color,
            __ZEUS_OPACITY: record.style.opacity,
            __ZEUS_LAYER_ID: record.id,
            __ZEUS_LAYER_NAME: record.name,
            __ZEUS_FEATURE_INDEX: featureIndex,
          },
        }))
      : []),
  }
}

function directionArrow(): ImageData {
  const canvas = document.createElement('canvas')
  canvas.width = 32
  canvas.height = 16
  const context = canvas.getContext('2d')
  if (!context) return new ImageData(32, 16)
  context.strokeStyle = '#1d4ed8'
  context.lineWidth = 3
  context.lineCap = 'square'
  context.lineJoin = 'miter'
  context.beginPath()
  context.moveTo(5, 8)
  context.lineTo(25, 8)
  context.lineTo(18, 2)
  context.moveTo(25, 8)
  context.lineTo(18, 14)
  context.stroke()
  return context.getImageData(0, 0, 32, 16)
}

function extendCoordinates(bounds: LngLatBounds, coordinates: unknown): number {
  if (!Array.isArray(coordinates) || coordinates.length === 0) return 0
  if (typeof coordinates[0] === 'number' && typeof coordinates[1] === 'number') {
    bounds.extend([coordinates[0], coordinates[1]])
    return 1
  }
  return coordinates.reduce(
    (count: number, child: unknown) => count + extendCoordinates(bounds, child),
    0,
  )
}

function extendGeometry(bounds: LngLatBounds, geometry: GeoJSON.Geometry): number {
  if (geometry.type === 'GeometryCollection') {
    return geometry.geometries.reduce(
      (count, child) => count + extendGeometry(bounds, child),
      0,
    )
  }
  return extendCoordinates(bounds, geometry.coordinates)
}

function boundsFor(data: GeoJSON.FeatureCollection): LngLatBounds | null {
  const bounds = new LngLatBounds()
  const points = data.features.reduce(
    (count, feature) => count + (feature.geometry ? extendGeometry(bounds, feature.geometry) : 0),
    0,
  )
  return points > 0 ? bounds : null
}

export function MapCanvas({
  data,
  nodeData,
  issueData,
  referenceLayers,
  referenceFocus,
  mapName,
  isDemo,
  selectedMatch,
  selectedRoad,
  selectedReferenceFeature,
  focusedIssueIndex,
  routeMode,
  routeStart,
  routeEnd,
  routeData,
  trajectoryData,
  vehicleFrame,
  junctionPickMode,
  selectedControlNodeId,
  onQuery,
  onIssueSelect,
  onRoadSelect,
  onJunctionSelect,
  onReferenceFeatureSelect,
  onPointerMove,
  onRoutePoint,
}: MapCanvasProps) {
  const containerRef = useRef<HTMLDivElement>(null)
  const mapRef = useRef<MapLibreMap | null>(null)
  const markerRef = useRef<Marker | null>(null)
  const routeStartMarkerRef = useRef<Marker | null>(null)
  const routeEndMarkerRef = useRef<Marker | null>(null)
  const queryRef = useRef(onQuery)
  const pointerRef = useRef(onPointerMove)
  const issueSelectRef = useRef(onIssueSelect)
  const roadSelectRef = useRef(onRoadSelect)
  const junctionSelectRef = useRef(onJunctionSelect)
  const referenceFeatureSelectRef = useRef(onReferenceFeatureSelect)
  const routeModeRef = useRef(routeMode)
  const junctionPickModeRef = useRef(junctionPickMode)
  const routePointRef = useRef(onRoutePoint)
  const dataRef = useRef(data)
  const nodeDataRef = useRef(nodeData)
  const issueDataRef = useRef(issueData)
  const referenceLayersRef = useRef(referenceLayers)

  queryRef.current = onQuery
  pointerRef.current = onPointerMove
  issueSelectRef.current = onIssueSelect
  roadSelectRef.current = onRoadSelect
  junctionSelectRef.current = onJunctionSelect
  referenceFeatureSelectRef.current = onReferenceFeatureSelect
  routeModeRef.current = routeMode
  junctionPickModeRef.current = junctionPickMode
  routePointRef.current = onRoutePoint
  dataRef.current = data
  nodeDataRef.current = nodeData
  issueDataRef.current = issueData
  referenceLayersRef.current = referenceLayers

  useEffect(() => {
    if (!containerRef.current || mapRef.current) return

    const map = new MapLibreMap({
      container: containerRef.current,
      center: [116.391, 39.907],
      zoom: 12,
      minZoom: 2,
      maxZoom: 20,
      attributionControl: false,
      style: {
        version: 8,
        sources: {},
        layers: [
          {
            id: 'background',
            type: 'background',
            paint: { 'background-color': '#f1f5f9' },
          },
        ],
      },
    })

    map.addControl(new NavigationControl({ showCompass: true }), 'bottom-right')
    map.getCanvas().style.cursor = 'crosshair'

    map.on('load', () => {
      map.addSource('references', {
        type: 'geojson',
        data: combineReferences(referenceLayersRef.current),
      })
      map.addLayer({
        id: 'reference-fill',
        type: 'fill',
        source: 'references',
        filter: ['==', ['geometry-type'], 'Polygon'],
        paint: {
          'fill-color': ['get', '__ZEUS_COLOR'],
          'fill-opacity': ['get', '__ZEUS_OPACITY'],
        },
      })
      map.addLayer({
        id: 'reference-outline',
        type: 'line',
        source: 'references',
        filter: ['==', ['geometry-type'], 'Polygon'],
        paint: {
          'line-color': ['get', '__ZEUS_COLOR'],
          'line-width': ['interpolate', ['linear'], ['zoom'], 4, 1.4, 15, 3],
          'line-opacity': 1,
        },
      })
      map.addLayer({
        id: 'reference-points',
        type: 'circle',
        source: 'references',
        filter: ['==', ['geometry-type'], 'Point'],
        paint: {
          'circle-radius': ['interpolate', ['linear'], ['zoom'], 5, 3, 15, 7],
          'circle-color': ['get', '__ZEUS_COLOR'],
          'circle-opacity': ['get', '__ZEUS_OPACITY'],
          'circle-stroke-color': '#ffffff',
          'circle-stroke-width': 1.5,
        },
      })
      map.addLayer({
        id: 'reference-selected-polygon',
        type: 'line',
        source: 'references',
        filter: [
          'all',
          ['==', ['geometry-type'], 'Polygon'],
          ['==', ['get', '__ZEUS_LAYER_ID'], '__none__'],
        ],
        paint: {
          'line-color': '#0f172a',
          'line-width': ['interpolate', ['linear'], ['zoom'], 4, 2, 15, 5],
          'line-opacity': 1,
        },
      })
      map.addLayer({
        id: 'reference-selected-point',
        type: 'circle',
        source: 'references',
        filter: [
          'all',
          ['==', ['geometry-type'], 'Point'],
          ['==', ['get', '__ZEUS_LAYER_ID'], '__none__'],
        ],
        paint: {
          'circle-radius': ['interpolate', ['linear'], ['zoom'], 5, 7, 15, 13],
          'circle-color': 'rgba(0, 0, 0, 0)',
          'circle-stroke-color': '#0f172a',
          'circle-stroke-width': 2,
        },
      })
      map.addSource('roads', { type: 'geojson', data: dataRef.current })
      map.addLayer({
        id: 'road-halo',
        type: 'line',
        source: 'roads',
        paint: {
          'line-color': '#ffffff',
          'line-width': ['interpolate', ['linear'], ['zoom'], 7, 4, 15, 10],
          'line-opacity': 0.96,
        },
      })
      map.addSource('issues', { type: 'geojson', data: issueDataRef.current })
      map.addLayer({
        id: 'issues-halo',
        type: 'circle',
        source: 'issues',
        paint: {
          'circle-radius': ['interpolate', ['linear'], ['zoom'], 7, 5, 15, 11],
          'circle-color': [
            'match', ['get', 'SEVERITY'],
            'fatal', '#ff453a',
            'error', '#ff5d4d',
            'warning', '#ff8a24',
            '#8fb6a7',
          ],
          'circle-opacity': 0.18,
          'circle-blur': 0.25,
        },
      })
      map.addLayer({
        id: 'issues-point',
        type: 'circle',
        source: 'issues',
        paint: {
          'circle-radius': ['interpolate', ['linear'], ['zoom'], 7, 2, 15, 4.5],
          'circle-color': [
            'match', ['get', 'SEVERITY'],
            'fatal', '#ff453a',
            'error', '#ff5d4d',
            'warning', '#ff8a24',
            '#8fb6a7',
          ],
          'circle-stroke-color': '#ffffff',
          'circle-stroke-width': 1,
        },
      })
      map.addLayer({
        id: 'issues-selected',
        type: 'circle',
        source: 'issues',
        filter: ['==', ['get', 'ISSUE_INDEX'], -1],
        paint: {
          'circle-radius': ['interpolate', ['linear'], ['zoom'], 7, 8, 15, 15],
          'circle-color': 'rgba(0, 0, 0, 0)',
          'circle-stroke-color': '#0f172a',
          'circle-stroke-width': 2,
        },
      })
      map.addLayer({
        id: 'roads',
        type: 'line',
        source: 'roads',
        paint: {
          'line-color': [
            'match',
            ['get', 'CLASS'],
            ['arterial', 'primary', 'trunk'], '#2563eb',
            ['ring', 'motorway'], '#f97316',
            ['collector', 'secondary'], '#64748b',
            '#94a3b8',
          ],
          'line-width': ['interpolate', ['linear'], ['zoom'], 7, 0.8, 13, 2.2, 17, 5],
          'line-opacity': 0.9,
        },
      })
      map.addLayer({
        id: 'roads-highlight',
        type: 'line',
        source: 'roads',
        filter: emptyFilter,
        paint: {
          'line-color': '#e11d48',
          'line-width': ['interpolate', ['linear'], ['zoom'], 7, 3, 15, 10],
          'line-blur': 0.4,
          'line-opacity': 1,
        },
      })
      map.addSource('trajectories', { type: 'geojson', data: { type: 'FeatureCollection', features: [] } })
      map.addLayer({
        id: 'simulation-trajectories',
        type: 'line',
        source: 'trajectories',
        layout: { 'line-cap': 'round', 'line-join': 'round' },
        paint: {
          'line-color': '#f59e0b',
          'line-width': ['interpolate', ['linear'], ['zoom'], 8, 2, 16, 5],
          'line-opacity': 0.45,
        },
      })
      map.addSource('route', { type: 'geojson', data: { type: 'FeatureCollection', features: [] } })
      map.addLayer({
        id: 'route-casing',
        type: 'line',
        source: 'route',
        layout: { 'line-cap': 'round', 'line-join': 'round' },
        paint: {
          'line-color': '#ffffff',
          'line-width': ['interpolate', ['linear'], ['zoom'], 8, 7, 16, 13],
          'line-opacity': 0.9,
        },
      })
      map.addLayer({
        id: 'route-line',
        type: 'line',
        source: 'route',
        layout: { 'line-cap': 'round', 'line-join': 'round' },
        paint: {
          'line-color': '#7c3aed',
          'line-width': ['interpolate', ['linear'], ['zoom'], 8, 4, 16, 9],
          'line-opacity': 1,
        },
      })
      map.addImage('direction-arrow', directionArrow(), { pixelRatio: 2 })
      map.addLayer({
        id: 'road-direction',
        type: 'symbol',
        source: 'roads',
        minzoom: 13,
        filter: ['in', ['get', 'DIRECTION'], ['literal', ['forward', 'reverse']]],
        layout: {
          'symbol-placement': 'line',
          'symbol-spacing': 90,
          'icon-image': 'direction-arrow',
          'icon-size': 0.8,
          'icon-allow-overlap': true,
          'icon-ignore-placement': true,
          'icon-rotation-alignment': 'map',
          'icon-rotate': ['match', ['get', 'DIRECTION'], 'reverse', 180, 0],
        },
      })
      map.addSource('nodes', { type: 'geojson', data: nodeDataRef.current })
      map.addLayer({
        id: 'node-halo',
        type: 'circle',
        source: 'nodes',
        minzoom: 13,
        paint: {
          'circle-radius': ['interpolate', ['linear'], ['zoom'], 13, 3, 18, 8],
          'circle-color': '#ffffff',
          'circle-stroke-color': '#2563eb',
          'circle-stroke-width': 1,
          'circle-opacity': 0.92,
        },
      })
      map.addLayer({
        id: 'nodes',
        type: 'circle',
        source: 'nodes',
        minzoom: 13,
        paint: {
          'circle-radius': ['interpolate', ['linear'], ['zoom'], 13, 1, 18, 3],
          'circle-color': [
            'case',
            ['>', ['+', ['get', 'IN_DEGREE'], ['get', 'OUT_DEGREE']], 4],
            '#f97316',
            '#2563eb',
          ],
        },
      })
      map.addLayer({
        id: 'node-control-selected',
        type: 'circle',
        source: 'nodes',
        minzoom: 11,
        filter: ['==', ['get', 'NODE_INDEX'], -1],
        paint: {
          'circle-radius': ['interpolate', ['linear'], ['zoom'], 11, 6, 18, 13],
          'circle-color': 'rgba(255, 255, 255, 0.8)',
          'circle-stroke-color': '#dc2626',
          'circle-stroke-width': 2.5,
        },
      })
      map.addSource('simulation-vehicles', { type: 'geojson', data: { type: 'FeatureCollection', features: [] } })
      map.addLayer({
        id: 'simulation-vehicles',
        type: 'circle',
        source: 'simulation-vehicles',
        paint: {
          'circle-radius': ['interpolate', ['linear'], ['zoom'], 7, 2.5, 14, 4.5, 18, 7],
          'circle-color': [
            'case',
            ['==', ['get', 'HELD'], true], '#dc2626',
            ['<', ['get', 'SPEED_FACTOR'], 1], '#f97316',
            '#f59e0b',
          ],
          'circle-opacity': 0.96,
          'circle-stroke-color': '#ffffff',
          'circle-stroke-width': 1.5,
        },
      })
      map.moveLayer('issues-halo')
      map.moveLayer('issues-point')
      map.moveLayer('issues-selected')
      map.moveLayer('simulation-vehicles')

      const references = combineReferences(referenceLayersRef.current)
      const bounds = isDemo && references.features.length > 0
        ? boundsFor(references)
        : boundsFor(dataRef.current)
      if (bounds) map.fitBounds(bounds, { padding: 72, duration: 0, maxZoom: 15 })
    })

    map.on('mousemove', (event: MapMouseEvent) => {
      pointerRef.current(event.lngLat.lng, event.lngLat.lat)
      const layers = [
        'issues-point',
        'roads',
        'reference-points',
        'reference-fill',
        ...(junctionPickModeRef.current ? ['nodes'] : []),
      ]
        .filter((layer) => Boolean(map.getLayer(layer)))
      const interactive = layers.length > 0
        ? map.queryRenderedFeatures(event.point, { layers })
        : []
      map.getCanvas().style.cursor = interactive.length > 0 ? 'pointer' : 'crosshair'
    })

    map.on('click', async (event: MapMouseEvent) => {
      if (junctionPickModeRef.current) {
        const node = map.getLayer('nodes')
          ? map.queryRenderedFeatures(event.point, { layers: ['nodes'] })[0]
          : undefined
        const nodeId = Number(node?.properties?.NODE_INDEX)
        if (Number.isInteger(nodeId)) junctionSelectRef.current(nodeId)
        return
      }
      if (routeModeRef.current) {
        routePointRef.current(event.lngLat.lng, event.lngLat.lat)
        return
      }
      const issue = map.getLayer('issues-point')
        ? map.queryRenderedFeatures(event.point, { layers: ['issues-point'] })[0]
        : undefined
      const issueIndex = Number(issue?.properties?.ISSUE_INDEX)
      if (Number.isInteger(issueIndex)) {
        issueSelectRef.current(issueIndex)
        return
      }
      const road = map.getLayer('roads')
        ? map.queryRenderedFeatures(event.point, { layers: ['roads'] })[0]
        : undefined
      if (road?.properties) {
        referenceFeatureSelectRef.current(null)
        roadSelectRef.current(road.properties as RoadProperties)
      } else {
        const referenceLayers = ['reference-points', 'reference-fill']
          .filter((layer) => Boolean(map.getLayer(layer)))
        const reference = referenceLayers.length > 0
          ? map.queryRenderedFeatures(event.point, { layers: referenceLayers })[0]
          : undefined
        if (reference?.properties) {
          const layerId = String(reference.properties.__ZEUS_LAYER_ID)
          const layerName = String(reference.properties.__ZEUS_LAYER_NAME)
          const featureIndex = Number(reference.properties.__ZEUS_FEATURE_INDEX)
          const properties: Record<string, unknown> = { ...reference.properties }
          delete properties.__ZEUS_LAYER_ID
          delete properties.__ZEUS_LAYER_NAME
          delete properties.__ZEUS_FEATURE_INDEX
          delete properties.__ZEUS_COLOR
          delete properties.__ZEUS_OPACITY
          referenceFeatureSelectRef.current({
            layerId,
            layerName,
            featureIndex,
            geometryType: reference.geometry.type,
            properties,
          })
          return
        }
        referenceFeatureSelectRef.current(null)
      }
      const markerNode = document.createElement('div')
      markerNode.className = 'query-marker'
      markerRef.current?.remove()
      markerRef.current = new Marker({ element: markerNode, anchor: 'center' })
        .setLngLat(event.lngLat)
        .addTo(map)
      await queryRef.current(event.lngLat.lng, event.lngLat.lat)
    })

    mapRef.current = map
    return () => {
      markerRef.current?.remove()
      routeStartMarkerRef.current?.remove()
      routeEndMarkerRef.current?.remove()
      map.remove()
      mapRef.current = null
    }
  }, [])

  useEffect(() => {
    const map = mapRef.current
    if (!map) return
    const update = () => {
      const source = map.getSource('roads') as GeoJSONSource | undefined
      if (!source) return
      source.setData(data)
      if (map.getLayer('roads-highlight')) map.setFilter('roads-highlight', emptyFilter)
      const bounds = boundsFor(data)
      if (bounds) map.fitBounds(bounds, { padding: 72, duration: 700, maxZoom: 15 })
    }
    if (map.isStyleLoaded()) update()
    else map.once('load', update)
  }, [data])

  useEffect(() => {
    const map = mapRef.current
    if (!map) return
    const update = () => {
      const source = map.getSource('nodes') as GeoJSONSource | undefined
      source?.setData(nodeData)
    }
    if (map.isStyleLoaded()) update()
    else map.once('load', update)
  }, [nodeData])

  useEffect(() => {
    const map = mapRef.current
    if (!map) return
    const update = () => {
      const source = map.getSource('issues') as GeoJSONSource | undefined
      source?.setData(issueData)
    }
    if (map.isStyleLoaded()) update()
    else map.once('load', update)
  }, [issueData])

  useEffect(() => {
    const map = mapRef.current
    if (!map) return
    const combined = combineReferences(referenceLayers)
    const update = () => {
      const source = map.getSource('references') as GeoJSONSource | undefined
      source?.setData(combined)
      if (!isDemo || combined.features.length === 0) return
      const bounds = boundsFor(combined)
      if (bounds) map.fitBounds(bounds, { padding: 72, duration: 650, maxZoom: 14 })
    }
    if (map.isStyleLoaded()) update()
    else map.once('load', update)
  }, [isDemo, referenceLayers])

  useEffect(() => {
    if (!referenceFocus) return
    const map = mapRef.current
    const layer = referenceLayers.find(({ record }) => record.id === referenceFocus.layerId)
    if (!map || !layer || layer.data.features.length === 0) return
    const focus = () => {
      const bounds = boundsFor(layer.data)
      if (bounds) map.fitBounds(bounds, { padding: 68, duration: 700, maxZoom: 13 })
    }
    if (map.isStyleLoaded()) focus()
    else map.once('load', focus)
  }, [referenceFocus, referenceLayers])

  useEffect(() => {
    const map = mapRef.current
    if (!map || !map.getLayer('reference-selected-polygon')) return
    if (selectedReferenceFeature) {
      map.setFilter('reference-selected-polygon', [
        'all',
        ['==', ['geometry-type'], 'Polygon'],
        ['==', ['get', '__ZEUS_LAYER_ID'], selectedReferenceFeature.layerId],
        ['==', ['get', '__ZEUS_FEATURE_INDEX'], selectedReferenceFeature.featureIndex],
      ])
      map.setFilter('reference-selected-point', [
        'all',
        ['==', ['geometry-type'], 'Point'],
        ['==', ['get', '__ZEUS_LAYER_ID'], selectedReferenceFeature.layerId],
        ['==', ['get', '__ZEUS_FEATURE_INDEX'], selectedReferenceFeature.featureIndex],
      ])
      return
    }
    map.setFilter('reference-selected-polygon', [
      'all',
      ['==', ['geometry-type'], 'Polygon'],
      ['==', ['get', '__ZEUS_LAYER_ID'], '__none__'],
    ])
    map.setFilter('reference-selected-point', [
      'all',
      ['==', ['geometry-type'], 'Point'],
      ['==', ['get', '__ZEUS_LAYER_ID'], '__none__'],
    ])
  }, [selectedReferenceFeature])

  useEffect(() => {
    const map = mapRef.current
    if (!map || !map.getLayer('roads-highlight')) return
    map.setFilter(
      'roads-highlight',
      selectedRoad
        ? ['==', ['get', 'ROAD_ID'], selectedRoad.ROAD_ID]
        : selectedMatch
          ? ['==', ['get', 'ROAD_ID'], selectedMatch.roadId]
          : emptyFilter,
    )
  }, [selectedMatch, selectedRoad])

  useEffect(() => {
    const map = mapRef.current
    if (!map || !map.getLayer('issues-selected')) return
    map.setFilter(
      'issues-selected',
      ['==', ['get', 'ISSUE_INDEX'], focusedIssueIndex ?? -1],
    )
    if (focusedIssueIndex === null) return
    const feature = issueData.features.find(
      (candidate) => candidate.properties.ISSUE_INDEX === focusedIssueIndex,
    )
    if (feature) {
      map.flyTo({
        center: feature.geometry.coordinates as [number, number],
        zoom: Math.max(map.getZoom(), 16),
        duration: 650,
      })
    }
  }, [focusedIssueIndex, issueData])

  useEffect(() => {
    const map = mapRef.current
    if (!map) return
    const update = () => {
      const source = map.getSource('route') as GeoJSONSource | undefined
      source?.setData(routeData ?? { type: 'FeatureCollection', features: [] })
      if (!routeData || routeData.features.length === 0) return
      const bounds = boundsFor(routeData)
      if (bounds) map.fitBounds(bounds, { padding: 88, duration: 700, maxZoom: 16 })
    }
    if (map.isStyleLoaded()) update()
    else map.once('load', update)
  }, [routeData])

  useEffect(() => {
    const map = mapRef.current
    if (!map) return
    const update = () => {
      const source = map.getSource('trajectories') as GeoJSONSource | undefined
      source?.setData(trajectoryData ?? { type: 'FeatureCollection', features: [] })
    }
    if (map.isStyleLoaded()) update()
    else map.once('load', update)
  }, [trajectoryData])

  useEffect(() => {
    const map = mapRef.current
    if (!map) return
    const update = () => {
      const source = map.getSource('simulation-vehicles') as GeoJSONSource | undefined
      source?.setData(vehicleFrame)
    }
    if (map.isStyleLoaded()) update()
    else map.once('load', update)
  }, [vehicleFrame])

  useEffect(() => {
    const map = mapRef.current
    if (!map) return
    const update = () => {
      if (!map.getLayer('node-control-selected')) return
      map.setFilter('node-control-selected', selectedControlNodeId === null
        ? ['==', ['get', 'NODE_INDEX'], -1]
        : ['==', ['get', 'NODE_INDEX'], selectedControlNodeId])
    }
    if (map.isStyleLoaded()) update()
    else map.once('load', update)
  }, [selectedControlNodeId])

  useEffect(() => {
    const map = mapRef.current
    if (!map) return
    const place = (
      ref: { current: Marker | null },
      position: [number, number] | null,
      variant: string,
    ) => {
      ref.current?.remove()
      ref.current = null
      if (!position) return
      const element = document.createElement('div')
      element.className = `route-marker ${variant}`
      ref.current = new Marker({ element, anchor: 'center' })
        .setLngLat(position)
        .addTo(map)
    }
    place(routeStartMarkerRef, routeStart, 'route-marker--start')
    place(routeEndMarkerRef, routeEnd, 'route-marker--end')
  }, [routeStart, routeEnd])

  return (
    <section className="map-stage" aria-label="地图视图">
      <div className="map-stage__texture" aria-hidden="true" />
      <div ref={containerRef} className="map-canvas" />

      <div className="map-id-plate">
        <span className="eyebrow">ACTIVE MAP</span>
        <strong>{mapName}</strong>
        <span className={isDemo ? 'status-pill status-pill--demo' : 'status-pill'}>
          <i /> {isDemo ? 'DEMO GEOMETRY' : 'RUNTIME CONNECTED'}
        </span>
      </div>

      <div className="map-instruction">
        {routeMode ? <Route size={15} /> : isDemo ? <MousePointer2 size={15} /> : <Crosshair size={15} />}
        <span>{routeMode
          ? routeStart
            ? routeEnd
              ? '已设定起终点 · 再次点击地图重新开始'
              : '已设定起点 · 点击地图设定终点'
            : '路由模式 · 第一次点击设起点'
          : isDemo && referenceLayers.some((layer) => layer.visible)
            ? '已显示参考边界 · 可滚轮缩放并点击查看属性'
            : isDemo
              ? '导入 SHP 或 GEOJSON 后启用道路匹配'
              : '点击任意位置查询最近有向道路'}</span>
      </div>

      <div className="map-legend" aria-label="道路图例">
        <span><i className="legend-line legend-line--arterial" /> 主干路</span>
        <span><i className="legend-line legend-line--ring" /> 快速路</span>
        <span><i className="legend-line legend-line--local" /> 支路</span>
        <span><i className="legend-arrow">→</i> 单向</span>
        <span><i className="legend-node" /> 拓扑节点</span>
        <span><i className="legend-reference" /> 参考图层</span>
        <span><i className="legend-dot" /> 质检事件</span>
        <span><i className="legend-vehicle" /> 仿真车辆</span>
      </div>
    </section>
  )
}
