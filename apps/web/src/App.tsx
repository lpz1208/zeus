import { ChangeEvent, DragEvent, useCallback, useEffect, useMemo, useRef, useState } from 'react'
import {
  Activity,
  AlertTriangle,
  BrainCircuit,
  CarFront,
  Check,
  ChevronRight,
  CircleDot,
  Cpu,
  Database,
  Eye,
  EyeOff,
  FileCode2,
  FileStack,
  Gauge,
  Layers3,
  LoaderCircle,
  LocateFixed,
  MapPinned,
  MousePointer2,
  Network,
  Palette,
  Pause,
  Play,
  Route,
  Save,
  Search,
  Settings2,
  ShieldCheck,
  SlidersHorizontal,
  Trash2,
  UploadCloud,
  Waypoints,
  X,
  Zap,
} from 'lucide-react'
import { api } from './api'
import { emptyVehicleFrame } from './types'
import { AgentWorkbench } from './agent/AgentWorkbench'
import { demoNetwork } from './demo'
import { MapCanvas } from './MapCanvas'
import { buildVehicleFrame } from './playback'
import { SimulationControlPanel } from './SimulationControlPanel'
import { SignalPlanPanel } from './SignalPlanPanel'
import type {
  ImportJob,
  InspectResult,
  IssueGeoJSON,
  MapRecord,
  Mapping,
  MatchCandidate,
  NodeGeoJSON,
  OSMPreprocessOptions,
  QueryResponse,
  ReferenceFeatureSelection,
  ReferenceGeoJSON,
  ReferenceLayerRecord,
  ReferenceLayerStyle,
  RoadGeoJSON,
  RoadProperties,
  RouteAlgorithm,
  RouteResponse,
  Severity,
  SimulateResponse,
  SimulationControls,
  VehicleFrameGeoJSON,
} from './types'

type Stage = 'idle' | 'uploading' | 'inspected' | 'importing' | 'ready' | 'error'

const emptyMapping: Mapping = {
  idField: '',
  onewayField: '',
  speedField: '',
  lanesField: '',
  roadClassField: '',
  zLevelField: '',
  bridgeField: '',
  tunnelField: '',
  targetCrs: '',
  defaultSpeedKph: 40,
  snapToleranceMeters: 0.5,
  defaultBidirectional: true,
}

const emptyIssues: IssueGeoJSON = { type: 'FeatureCollection', features: [] }
const emptyNodes: NodeGeoJSON = { type: 'FeatureCollection', features: [] }
const emptyReference: ReferenceGeoJSON = { type: 'FeatureCollection', features: [] }
const emptySimulationControls: SimulationControls = {
  vehicleControls: [],
  roadControls: [],
  junctionControls: [],
  signalPlans: [],
}
const defaultReferenceStyle: ReferenceLayerStyle = { color: '#55c7b2', opacity: 0.24 }
const defaultOSMPreprocess: OSMPreprocessOptions = {
  enabled: false,
  includeService: false,
  includeTrack: false,
  includePrivate: false,
  minLengthMeters: 2,
}

function chooseField(fields: string[], terms: string[]): string {
  const normalized = fields.map((field) => ({ raw: field, normalized: field.toLowerCase() }))
  for (const term of terms) {
    const exact = normalized.find((field) => field.normalized === term)
    if (exact) return exact.raw
  }
  for (const term of terms) {
    const partial = normalized.find((field) => field.normalized.includes(term))
    if (partial) return partial.raw
  }
  return ''
}

function guessMapping(fields: string[]): Mapping {
  return {
    ...emptyMapping,
    idField: chooseField(fields, ['road_id', 'link_id', 'id', 'fid']),
    onewayField: chooseField(fields, ['oneway', 'one_way', 'direction', 'dir']),
    speedField: chooseField(fields, ['speed_kph', 'maxspeed', 'speed']),
    lanesField: chooseField(fields, ['lanes', 'lane_count', 'num_lanes']),
    roadClassField: chooseField(fields, ['road_class', 'fclass', 'class', 'type']),
    zLevelField: chooseField(fields, ['z_level', 'level', 'layer']),
    bridgeField: chooseField(fields, ['bridge']),
    tunnelField: chooseField(fields, ['tunnel']),
  }
}

function formatNumber(value: number): string {
  return new Intl.NumberFormat('zh-CN').format(value)
}

function severityLabel(severity: Severity): string {
  return { fatal: '致命', error: '错误', warning: '警告', info: '信息' }[severity]
}

function formatDuration(seconds: number): string {
  const total = Math.round(seconds)
  const hours = Math.floor(total / 3600)
  const minutes = Math.floor((total % 3600) / 60)
  const rest = total % 60
  const mm = String(minutes).padStart(2, '0')
  const ss = String(rest).padStart(2, '0')
  return hours > 0 ? `${hours}:${mm}:${ss}` : `${mm}:${ss}`
}

function routeFailureLabel(reason?: string): string {
  return {
    unmatched_origin: '起点未能吸附到道路',
    unmatched_destination: '终点未能吸附到道路',
    unreachable: '起终点之间不可达',
    empty_map: '地图为空',
  }[reason ?? ''] ?? '路径规划失败'
}

function cleaningReasonLabel(reason: string): string {
  return {
    access_restricted: '访问受限',
    non_drivable_class: '非机动车道路',
    service_disabled: '服务道路',
    track_disabled: '土路',
    too_short: '短小几何',
    duplicate_geometry: '重复几何',
    empty_geometry: '空几何',
    unsupported_geometry: '不支持的几何',
    missing_highway: '缺少 highway',
  }[reason] ?? reason.replaceAll('_', ' ')
}

function formatReferenceValue(value: unknown): string {
  if (value === null || value === undefined) return 'NULL'
  const text = typeof value === 'object' ? JSON.stringify(value) : String(value)
  return text.length > 160 ? `${text.slice(0, 157)}…` : text
}

interface FieldSelectProps {
  label: string
  value: string
  fields: string[]
  onChange: (value: string) => void
  optional?: boolean
}

function FieldSelect({ label, value, fields, onChange, optional = true }: FieldSelectProps) {
  return (
    <label className="field-control">
      <span>{label}</span>
      <select value={value} onChange={(event) => onChange(event.target.value)}>
        <option value="">{optional ? '未映射' : '请选择字段'}</option>
        {fields.map((field) => <option key={field} value={field}>{field}</option>)}
      </select>
    </label>
  )
}

function Pipeline({ stage }: { stage: Stage }) {
  const active = stage === 'idle' ? 0 : stage === 'uploading' ? 1 : stage === 'inspected' ? 2 : stage === 'importing' ? 3 : 4
  const steps = ['SOURCE', 'SCHEMA', 'TOPOLOGY', 'RUNTIME']
  return (
    <div className="pipeline" aria-label="地图编译进度">
      {steps.map((step, index) => (
        <div className={`pipeline__step ${index < active ? 'is-done' : ''} ${index === active ? 'is-active' : ''}`} key={step}>
          <span>{index < active ? <Check size={12} /> : String(index + 1).padStart(2, '0')}</span>
          <strong>{step}</strong>
          {index < steps.length - 1 && <ChevronRight size={13} />}
        </div>
      ))}
    </div>
  )
}

export function App() {
  const fileInputRef = useRef<HTMLInputElement>(null)
  const hasAutoFocusedReference = useRef(false)
  const [stage, setStage] = useState<Stage>('idle')
  const [files, setFiles] = useState<File[]>([])
  const [inspect, setInspect] = useState<InspectResult | null>(null)
  const [mapping, setMapping] = useState<Mapping>(emptyMapping)
  const [osmPreprocess, setOSMPreprocess] = useState<OSMPreprocessOptions>(defaultOSMPreprocess)
  const [mapName, setMapName] = useState('')
  const [maps, setMaps] = useState<MapRecord[]>([])
  const [activeMap, setActiveMap] = useState<MapRecord | null>(null)
  const [geoJSON, setGeoJSON] = useState<RoadGeoJSON>(demoNetwork)
  const [issueGeoJSON, setIssueGeoJSON] = useState<IssueGeoJSON>(emptyIssues)
  const [nodeGeoJSON, setNodeGeoJSON] = useState<NodeGeoJSON>(emptyNodes)
  const [referenceLayers, setReferenceLayers] = useState<ReferenceLayerRecord[]>([])
  const [referenceData, setReferenceData] = useState<Record<string, ReferenceGeoJSON>>({})
  const [visibleReferenceIds, setVisibleReferenceIds] = useState<Set<string>>(new Set())
  const [referenceStyle, setReferenceStyle] = useState<ReferenceLayerStyle>(defaultReferenceStyle)
  const [referenceImporting, setReferenceImporting] = useState(false)
  const [editingReferenceId, setEditingReferenceId] = useState<string | null>(null)
  const [referenceDraft, setReferenceDraft] = useState<{ name: string; style: ReferenceLayerStyle } | null>(null)
  const [referenceMutating, setReferenceMutating] = useState(false)
  const [mapsLoaded, setMapsLoaded] = useState(false)
  const [referencesLoaded, setReferencesLoaded] = useState(false)
  const [referenceFocus, setReferenceFocus] = useState<{ layerId: string; requestId: number } | null>(null)
  const [rightPanel, setRightPanel] = useState<'details' | 'quality' | 'route'>('details')
  const [routeMode, setRouteMode] = useState(false)
  const [routeAlgorithm, setRouteAlgorithm] = useState<RouteAlgorithm>('dijkstra')
  const [routeStart, setRouteStart] = useState<[number, number] | null>(null)
  const [routeEnd, setRouteEnd] = useState<[number, number] | null>(null)
  const [routeResult, setRouteResult] = useState<RouteResponse | null>(null)
  const [routeBusy, setRouteBusy] = useState(false)
  const [simConfig, setSimConfig] = useState({
    count: 100,
    spreadSeconds: 600,
    durationSeconds: 900,
    stepSeconds: 1,
    sampleIntervalSeconds: 15,
    exitHeadwayFfSeconds: 0,
    exitHeadwayJamSeconds: 0,
    rerouteIntervalSeconds: 0,
    rerouteCostRatio: 1.25,
  })
  const [simResult, setSimResult] = useState<SimulateResponse | null>(null)
  const [simBusy, setSimBusy] = useState(false)
  const [simControls, setSimControls] = useState<SimulationControls>(emptySimulationControls)
  const [junctionPickMode, setJunctionPickMode] = useState(false)
  const [selectedControlNodeId, setSelectedControlNodeId] = useState<number | null>(null)
  const [playbackTime, setPlaybackTime] = useState(0)
  const [playing, setPlaying] = useState(false)
  const [playbackSpeed, setPlaybackSpeed] = useState(1)
  const [importJob, setImportJob] = useState<ImportJob | null>(null)
  const [focusedIssueIndex, setFocusedIssueIndex] = useState<number | null>(null)
  const [queryResult, setQueryResult] = useState<QueryResponse | null>(null)
  const [selectedMatch, setSelectedMatch] = useState<MatchCandidate | null>(null)
  const [selectedRoad, setSelectedRoad] = useState<RoadProperties | null>(null)
  const [selectedReferenceFeature, setSelectedReferenceFeature] = useState<ReferenceFeatureSelection | null>(null)
  const [pointer, setPointer] = useState<[number, number]>([116.391, 39.907])
  const [serviceOnline, setServiceOnline] = useState(false)
  const [error, setError] = useState('')
  const [workspace, setWorkspace] = useState<'map' | 'agent'>('map')

  useEffect(() => {
    api.listMaps()
      .then((records) => {
        setServiceOnline(true)
        setMaps(records)
        if (records.length > 0) setActiveMap(records[0])
      })
      .catch(() => setServiceOnline(false))
      .finally(() => setMapsLoaded(true))
    api.listReferenceLayers()
      .then(async (records) => {
        const loaded = await Promise.all(records.map(async (record) => {
          const data = await api.getReferenceGeoJSON(record.id).catch(() => emptyReference)
          return [record.id, data] as const
        }))
        setReferenceLayers(records)
        setReferenceData(Object.fromEntries(loaded))
        setVisibleReferenceIds(new Set(records.map((record) => record.id)))
      })
      .catch(() => {
        // Road map operation remains available when an older server has no reference-layer API.
      })
      .finally(() => setReferencesLoaded(true))
  }, [])

  useEffect(() => {
    if (hasAutoFocusedReference.current || !mapsLoaded || !referencesLoaded || maps.length > 0 || referenceLayers.length === 0) return
    hasAutoFocusedReference.current = true
    setReferenceFocus({ layerId: referenceLayers[0].id, requestId: Date.now() })
  }, [maps.length, mapsLoaded, referenceLayers, referencesLoaded])

  useEffect(() => {
    setRouteStart(null)
    setRouteEnd(null)
    setRouteResult(null)
    setSimResult(null)
    setPlaying(false)
    setPlaybackTime(0)
    if (!activeMap) {
      setGeoJSON(demoNetwork)
      setIssueGeoJSON(emptyIssues)
      setNodeGeoJSON(emptyNodes)
      setFocusedIssueIndex(null)
      setQueryResult(null)
      setSelectedMatch(null)
      setSelectedRoad(null)
      return
    }
    Promise.all([
      api.getGeoJSON(activeMap.id),
      api.getIssueGeoJSON(activeMap.id).catch(() => emptyIssues),
      api.getNodeGeoJSON(activeMap.id).catch(() => emptyNodes),
    ])
      .then(([roads, issues, nodes]) => {
        setGeoJSON(roads)
        setIssueGeoJSON(issues)
        setNodeGeoJSON(nodes)
        setFocusedIssueIndex(null)
        setSelectedRoad(null)
        setStage('ready')
        setError('')
      })
      .catch((reason: Error) => setError(reason.message))
  }, [activeMap])

  useEffect(() => {
    if (!playing || !simResult?.playback) return
    const duration = simResult.playback.duration_s
    const timer = window.setInterval(() => {
      setPlaybackTime((current) => Math.min(duration, current + playbackSpeed * 0.1))
    }, 100)
    return () => window.clearInterval(timer)
  }, [playing, playbackSpeed, simResult])

  useEffect(() => {
    if (simResult?.playback && playbackTime >= simResult.playback.duration_s) {
      setPlaying(false)
    }
  }, [playbackTime, simResult])

  const handleFiles = useCallback(async (incoming: File[]) => {
    const supported = incoming.filter((file) => /\.(shp|shx|dbf|prj|cpg|geojson|json|csv)$/i.test(file.name))
    if (supported.length === 0) {
      setError('请选择一个 GeoJSON，或完整 SHP 数据包；可同时附带一个 Zeus 转向 CSV。')
      setStage('error')
      return
    }
    setFiles(supported)
    setInspect(null)
    setImportJob(null)
    setError('')
    setStage('uploading')
    try {
      const result = await api.inspectFiles(supported)
      setInspect(result)
      setMapName(result.layer || result.sourceFile.replace(/\.(shp|geojson|json)$/i, ''))
      setMapping(guessMapping(result.fields))
      setOSMPreprocess({ ...defaultOSMPreprocess, enabled: result.osmRoadData })
      setReferenceStyle(result.geometry.toLowerCase().includes('point')
        ? { color: '#ffb24a', opacity: 0.9 }
        : defaultReferenceStyle)
      setServiceOnline(true)
      setStage('inspected')
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : '无法检查地图数据。')
      setStage('error')
    }
  }, [])

  const onFileChange = (event: ChangeEvent<HTMLInputElement>) => {
    if (event.target.files) void handleFiles(Array.from(event.target.files))
  }

  const onDrop = (event: DragEvent<HTMLButtonElement>) => {
    event.preventDefault()
    void handleFiles(Array.from(event.dataTransfer.files))
  }

  const importMap = async () => {
    if (!inspect) return
    setError('')
    setImportJob(null)
    setStage('importing')
    try {
      const started = await api.importMap({
        uploadId: inspect.uploadId,
        sourceFile: inspect.sourceFile,
        turnRestrictionsFile: inspect.turnRestrictionsFile,
        name: mapName,
        mapping,
        osmPreprocess,
      })
      setImportJob(started)
      const completed = await api.waitForJob(started.id, setImportJob)
      if (completed.status !== 'succeeded' || !completed.map) {
        throw new Error(completed.error || completed.message || '地图编译失败。')
      }
      const record = completed.map
      setMaps((current) => [record, ...current.filter((item) => item.id !== record.id)])
      setActiveMap(record)
      setStage('ready')
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : '地图编译失败。')
      setStage('error')
    }
  }

  const cancelImport = async () => {
    if (!importJob || !['queued', 'running'].includes(importJob.status)) return
    try {
      await api.cancelJob(importJob.id)
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : '无法取消地图编译任务。')
    }
  }

  const importReferenceLayer = async () => {
    if (!inspect || inspect.suggestedUsage !== 'reference-layer') return
    setReferenceImporting(true)
    setError('')
    try {
      const record = await api.createReferenceLayer({
        uploadId: inspect.uploadId,
        sourceFile: inspect.sourceFile,
        name: mapName,
        style: referenceStyle,
      })
      const data = await api.getReferenceGeoJSON(record.id)
      setReferenceLayers((current) => [record, ...current.filter((item) => item.id !== record.id)])
      setReferenceData((current) => ({ ...current, [record.id]: data }))
      setVisibleReferenceIds((current) => new Set(current).add(record.id))
      setReferenceFocus({ layerId: record.id, requestId: Date.now() })
      setInspect(null)
      setFiles([])
      setStage(activeMap ? 'ready' : 'idle')
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : '参考图层发布失败。')
    } finally {
      setReferenceImporting(false)
    }
  }

  const toggleReferenceLayer = (id: string) => {
    if (visibleReferenceIds.has(id) && selectedReferenceFeature?.layerId === id) {
      setSelectedReferenceFeature(null)
    }
    setVisibleReferenceIds((current) => {
      const next = new Set(current)
      if (next.has(id)) next.delete(id)
      else next.add(id)
      return next
    })
  }

  const focusReferenceLayer = (id: string) => {
    setVisibleReferenceIds((current) => new Set(current).add(id))
    setReferenceFocus({ layerId: id, requestId: Date.now() })
  }

  const editReferenceLayer = (record: ReferenceLayerRecord) => {
    setEditingReferenceId(record.id)
    setReferenceDraft({ name: record.name, style: { ...record.style } })
  }

  const toggleReferenceEditor = (record: ReferenceLayerRecord) => {
    if (editingReferenceId === record.id) {
      setEditingReferenceId(null)
      setReferenceDraft(null)
      return
    }
    editReferenceLayer(record)
  }

  const saveReferenceLayer = async () => {
    if (!editingReferenceId || !referenceDraft) return
    setReferenceMutating(true)
    setError('')
    try {
      const updated = await api.updateReferenceLayer(editingReferenceId, referenceDraft)
      setReferenceLayers((current) => current.map((record) => (
        record.id === updated.id ? updated : record
      )))
      setSelectedReferenceFeature((current) => current?.layerId === updated.id
        ? { ...current, layerName: updated.name }
        : current)
      setEditingReferenceId(null)
      setReferenceDraft(null)
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : '参考图层更新失败。')
    } finally {
      setReferenceMutating(false)
    }
  }

  const deleteReferenceLayer = async (record: ReferenceLayerRecord) => {
    if (!window.confirm(`确定删除参考图层“${record.name}”？该操作无法撤销。`)) return
    setReferenceMutating(true)
    setError('')
    try {
      await api.deleteReferenceLayer(record.id)
      setReferenceLayers((current) => current.filter((item) => item.id !== record.id))
      setReferenceData((current) => {
        const next = { ...current }
        delete next[record.id]
        return next
      })
      setVisibleReferenceIds((current) => {
        const next = new Set(current)
        next.delete(record.id)
        return next
      })
      if (selectedReferenceFeature?.layerId === record.id) setSelectedReferenceFeature(null)
      setEditingReferenceId(null)
      setReferenceDraft(null)
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : '参考图层删除失败。')
    } finally {
      setReferenceMutating(false)
    }
  }

  const queryMap = useCallback(async (longitude: number, latitude: number) => {
    if (!activeMap) return null
    try {
      const result = await api.queryMap(activeMap.id, {
        lon: longitude,
        lat: latitude,
        maxDistance: 100,
        limit: 5,
      })
      setQueryResult(result)
      setSelectedMatch(result.matches[0] ?? null)
      setError('')
      return result
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : '道路匹配失败。')
      return null
    }
  }, [activeMap])

  const computeRoute = useCallback(async (
    from: [number, number],
    to: [number, number],
    algorithm: RouteAlgorithm,
  ) => {
    if (!activeMap) return
    setRouteBusy(true)
    setError('')
    try {
      const result = await api.routeMap(activeMap.id, {
        fromLon: from[0],
        fromLat: from[1],
        toLon: to[0],
        toLat: to[1],
        algorithm,
        maxDistance: 100,
      })
      setRouteResult(result)
      setRightPanel('route')
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : '路径规划失败。')
    } finally {
      setRouteBusy(false)
    }
  }, [activeMap])

  const handleRoutePoint = useCallback((longitude: number, latitude: number) => {
    if (!routeStart) {
      setRouteStart([longitude, latitude])
      setRouteEnd(null)
      setRouteResult(null)
      setSimResult(null)
      setPlaying(false)
      setPlaybackTime(0)
      return
    }
    if (routeEnd) {
      setRouteStart([longitude, latitude])
      setRouteEnd(null)
      setRouteResult(null)
      setSimResult(null)
      setPlaying(false)
      setPlaybackTime(0)
      return
    }
    setRouteEnd([longitude, latitude])
    void computeRoute(routeStart, [longitude, latitude], routeAlgorithm)
  }, [computeRoute, routeAlgorithm, routeEnd, routeStart])

  const changeRouteAlgorithm = (algorithm: RouteAlgorithm) => {
    setRouteAlgorithm(algorithm)
    setSimResult(null)
    setPlaying(false)
    setPlaybackTime(0)
    if (routeStart && routeEnd) void computeRoute(routeStart, routeEnd, algorithm)
  }

  const runSimulation = async () => {
    if (!activeMap || !routeStart || !routeEnd) return
    setSimBusy(true)
    setPlaying(false)
    setPlaybackTime(0)
    setError('')
    try {
      const result = await api.simulateMap(activeMap.id, {
        fromLon: routeStart[0],
        fromLat: routeStart[1],
        toLon: routeEnd[0],
        toLat: routeEnd[1],
        algorithm: routeAlgorithm,
        ...simControls,
        ...simConfig,
      })
      setSimResult(result)
      if (!result.ok) setError(result.message || '交通仿真无法规划给定 OD。')
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : '交通仿真失败。')
    } finally {
      setSimBusy(false)
    }
  }

  const clearRoute = () => {
    setRouteStart(null)
    setRouteEnd(null)
    setRouteResult(null)
    setSimResult(null)
    setPlaying(false)
    setPlaybackTime(0)
    setSimControls(emptySimulationControls)
    setJunctionPickMode(false)
    setSelectedControlNodeId(null)
  }

  const toggleRouteMode = () => {
    setRouteMode((current) => !current)
    setRightPanel('route')
  }

  const summary = activeMap?.summary
  const qualityScore = useMemo(() => {
    if (!summary) return 98
    const penalty = summary.fatal * 30 + summary.errors * 8 + summary.warnings * 0.8
    return Math.max(0, Math.round(100 - penalty))
  }, [summary])

  const bestMatch = selectedMatch ?? queryResult?.matches[0] ?? null
  const locatedIssueIndices = useMemo(
    () => new Set(issueGeoJSON.features.map((feature) => feature.properties.ISSUE_INDEX)),
    [issueGeoJSON],
  )
  const referenceViews = useMemo(
    () => referenceLayers.map((record) => ({
      record,
      data: referenceData[record.id] ?? emptyReference,
      visible: visibleReferenceIds.has(record.id),
    })),
    [referenceData, referenceLayers, visibleReferenceIds],
  )
  const vehicleFrame = useMemo(
    () => simResult?.playback
      ? buildVehicleFrame(simResult.playback, playbackTime)
      : emptyVehicleFrame,
    [playbackTime, simResult],
  )

  if (workspace === 'agent') {
    return (
      <AgentWorkbench
        maps={maps}
        activeMap={activeMap}
        onMapChange={setActiveMap}
        data={geoJSON}
        nodeData={nodeGeoJSON}
        issueData={issueGeoJSON}
        referenceLayers={referenceViews}
        onExit={() => setWorkspace('map')}
      />
    )
  }

  return (
    <main className="app-shell">
      <header className="topbar">
        <div className="brand-lockup">
          <div className="brand-mark"><Waypoints size={23} strokeWidth={1.7} /></div>
          <div>
            <span>ZEUS</span>
            <strong>MAP LAB</strong>
          </div>
        </div>
        <Pipeline stage={stage} />
        <div className="system-status">
          <span className={serviceOnline ? 'pulse-dot' : 'pulse-dot pulse-dot--offline'} />
          <div>
            <small>CONTROL PLANE</small>
            <strong>{serviceOnline ? 'ONLINE' : 'DEMO MODE'}</strong>
          </div>
          <button className="agent-workspace-launch" type="button" onClick={() => setWorkspace('agent')} disabled={!activeMap}>
            <BrainCircuit size={14} /> AGENT
          </button>
          <span className="build-tag">BUILD 0.1.0</span>
        </div>
      </header>

      <aside className="left-rail panel-grid">
        <div className="rail-heading">
          <div>
            <span className="eyebrow">DATA INTAKE</span>
            <h1>地图编译台</h1>
          </div>
          <Database size={18} />
        </div>

        <button
          className={`drop-zone ${stage === 'uploading' ? 'is-loading' : ''}`}
          type="button"
          onClick={() => fileInputRef.current?.click()}
          onDragOver={(event) => event.preventDefault()}
          onDrop={onDrop}
          data-testid="upload-zone"
        >
          <input
            ref={fileInputRef}
            type="file"
            accept=".shp,.shx,.dbf,.prj,.cpg,.geojson,.json,.csv,application/geo+json,application/json,text/csv"
            multiple
            onChange={onFileChange}
            hidden
          />
          {stage === 'uploading'
            ? <LoaderCircle className="spin" size={27} />
            : <UploadCloud size={27} strokeWidth={1.4} />}
          <strong>{stage === 'uploading' ? '正在解析数据结构' : '拖入 SHP 数据包或 GEOJSON'}</strong>
          <span>道路文件 · 可附加一个 TURN RESTRICTIONS .CSV</span>
          <i>SELECT FILES</i>
        </button>

        {files.length > 0 && (
          <div className="file-strip">
            {files.map((file) => (
              <span key={`${file.name}-${file.size}`}><FileCode2 size={12} /> {file.name}</span>
            ))}
          </div>
        )}

        {inspect && (
          <>
            <div className="source-card">
              <div className="source-card__icon"><FileStack size={19} /></div>
              <div>
                <span>{inspect.driver}</span>
                <strong>{formatNumber(inspect.featureCount)} FEATURES</strong>
                <small>{inspect.geometry} · {inspect.crs || 'CRS UNKNOWN'}</small>
              </div>
              <Check size={15} className="source-card__check" />
            </div>

            {inspect.suggestedUsage === 'reference-layer' ? (
              <section className="reference-intake" aria-label="发布参考图层">
                <div className="reference-intake__head">
                  <Layers3 size={17} />
                  <div>
                    <strong>识别为参考图层</strong>
                    <span>将转换为 WGS84 后叠加显示，不参与导航拓扑与道路匹配。</span>
                  </div>
                  <small>NON-ROUTABLE</small>
                </div>
                <label className="field-control field-control--full">
                  <span>图层名称</span>
                  <input value={mapName} onChange={(event) => setMapName(event.target.value)} />
                </label>
                <div className="reference-style-grid">
                  <label className="reference-color-control">
                    <span>图层颜色</span>
                    <div>
                      <input
                        type="color"
                        value={referenceStyle.color}
                        onChange={(event) => setReferenceStyle({ ...referenceStyle, color: event.target.value })}
                      />
                      <strong>{referenceStyle.color.toUpperCase()}</strong>
                    </div>
                  </label>
                  <label className="reference-opacity-control">
                    <span>不透明度 <strong>{Math.round(referenceStyle.opacity * 100)}%</strong></span>
                    <input
                      type="range"
                      min="0.05"
                      max="1"
                      step="0.05"
                      value={referenceStyle.opacity}
                      onChange={(event) => setReferenceStyle({ ...referenceStyle, opacity: Number(event.target.value) })}
                    />
                  </label>
                </div>
                <button
                  className="reference-publish-button"
                  type="button"
                  disabled={referenceImporting}
                  onClick={() => void importReferenceLayer()}
                >
                  {referenceImporting ? <LoaderCircle className="spin" size={16} /> : <Palette size={16} />}
                  <span>{referenceImporting ? '正在转换并发布' : '发布为参考图层'}</span>
                  <ChevronRight size={16} />
                </button>
              </section>
            ) : !inspect.navigationCompatible ? (
              <div className="geometry-gate" role="alert">
                <AlertTriangle size={18} />
                <div>
                  <strong>{inspect.geometry} 不能构建导航拓扑</strong>
                  <span>没有识别到可用的线状道路或点面参考几何，请检查 GeoJSON 数据结构。</span>
                </div>
                <small>NON-ROUTABLE</small>
              </div>
            ) : (
              <>
            {inspect.osmRoadData && (
              <section className={`osm-profile ${osmPreprocess.enabled ? 'is-enabled' : ''}`} aria-label="OSM 机动车清洗">
                <div className="osm-profile__head">
                  <span className="osm-profile__mark"><CarFront size={16} /></span>
                  <div>
                    <strong>OSM 汽车画像</strong>
                    <small>HIGHWAY SCHEMA DETECTED</small>
                  </div>
                  <label className="profile-switch">
                    <input
                      type="checkbox"
                      checked={osmPreprocess.enabled}
                      onChange={(event) => setOSMPreprocess({ ...osmPreprocess, enabled: event.target.checked })}
                    />
                    <span aria-hidden="true" />
                    <em>{osmPreprocess.enabled ? '已启用' : '未启用'}</em>
                  </label>
                </div>
                <p>在服务器端过滤非机动车道路，并统一方向、限速、层级与访问权限。</p>
                {osmPreprocess.enabled && (
                  <div className="osm-profile__options">
                    <label>
                      <input type="checkbox" checked={osmPreprocess.includeService} onChange={(event) => setOSMPreprocess({ ...osmPreprocess, includeService: event.target.checked })} />
                      <span>服务道路<small>停车场 / 园区</small></span>
                    </label>
                    <label>
                      <input type="checkbox" checked={osmPreprocess.includeTrack} onChange={(event) => setOSMPreprocess({ ...osmPreprocess, includeTrack: event.target.checked })} />
                      <span>土路<small>乡村 / 林间</small></span>
                    </label>
                    <label>
                      <input type="checkbox" checked={osmPreprocess.includePrivate} onChange={(event) => setOSMPreprocess({ ...osmPreprocess, includePrivate: event.target.checked })} />
                      <span>受限道路<small>PRIVATE ACCESS</small></span>
                    </label>
                    <label className="osm-min-length">
                      <span>最短道路<small>METERS</small></span>
                      <input type="number" min="0.1" max="1000" step="0.5" value={osmPreprocess.minLengthMeters} onChange={(event) => setOSMPreprocess({ ...osmPreprocess, minLengthMeters: Number(event.target.value) })} />
                    </label>
                  </div>
                )}
              </section>
            )}
            <section className="mapping-section">
              <div className="section-label">
                <SlidersHorizontal size={14} />
                <span>{osmPreprocess.enabled ? '拓扑参数' : '字段映射'}</span>
                <small>{osmPreprocess.enabled ? 'CANONICAL' : `${inspect.fields.length} FIELDS`}</small>
              </div>
              <label className="field-control field-control--full">
                <span>地图名称</span>
                <input value={mapName} onChange={(event) => setMapName(event.target.value)} />
              </label>
              {osmPreprocess.enabled ? (
                <div className="canonical-note"><Check size={13} /><span>清洗结果自动使用 Zeus 标准字段映射</span><small>ROAD_ID · ONEWAY · SPEED · CLASS · Z</small></div>
              ) : (
                <div className="field-grid">
                  <FieldSelect label="道路 ID" value={mapping.idField} fields={inspect.fields} optional={false} onChange={(idField) => setMapping({ ...mapping, idField })} />
                  <FieldSelect label="单双向" value={mapping.onewayField} fields={inspect.fields} onChange={(onewayField) => setMapping({ ...mapping, onewayField })} />
                  <FieldSelect label="限速" value={mapping.speedField} fields={inspect.fields} onChange={(speedField) => setMapping({ ...mapping, speedField })} />
                  <FieldSelect label="车道数" value={mapping.lanesField} fields={inspect.fields} onChange={(lanesField) => setMapping({ ...mapping, lanesField })} />
                  <FieldSelect label="道路等级" value={mapping.roadClassField} fields={inspect.fields} onChange={(roadClassField) => setMapping({ ...mapping, roadClassField })} />
                  <FieldSelect label="高程层" value={mapping.zLevelField} fields={inspect.fields} onChange={(zLevelField) => setMapping({ ...mapping, zLevelField })} />
                  <FieldSelect label="桥梁" value={mapping.bridgeField} fields={inspect.fields} onChange={(bridgeField) => setMapping({ ...mapping, bridgeField })} />
                </div>
              )}
              <div className="numeric-grid">
                <label className="field-control">
                  <span>吸附容差 / m</span>
                  <input type="number" min="0.01" step="0.1" value={mapping.snapToleranceMeters} onChange={(event) => setMapping({ ...mapping, snapToleranceMeters: Number(event.target.value) })} />
                </label>
                {!osmPreprocess.enabled && (
                  <label className="field-control">
                    <span>默认限速 / km·h⁻¹</span>
                    <input type="number" min="1" value={mapping.defaultSpeedKph} onChange={(event) => setMapping({ ...mapping, defaultSpeedKph: Number(event.target.value) })} />
                  </label>
                )}
              </div>
            </section>

            <button className="compile-button" type="button" onClick={() => void importMap()} disabled={stage === 'importing'} data-testid="compile-map">
              {stage === 'importing' ? <LoaderCircle className="spin" size={17} /> : <Zap size={17} fill="currentColor" />}
              <span>{stage === 'importing' ? (importJob?.phase === 'preprocessing' ? '正在清洗 OSM 路网' : '正在构建拓扑') : (osmPreprocess.enabled ? '清洗并编译地图' : '编译运行时地图')}</span>
              <ChevronRight size={17} />
            </button>
            {stage === 'importing' && importJob && (
              <div className="import-progress" role="status" aria-live="polite">
                <div className="import-progress__meta">
                  <span>{importJob.phase.toUpperCase()}</span>
                  <strong>{importJob.progress}%</strong>
                </div>
                <div className="import-progress__track"><i style={{ width: `${importJob.progress}%` }} /></div>
                <div className="import-progress__message">
                  <span>{importJob.message}</span>
                  <button type="button" onClick={() => void cancelImport()}>取消</button>
                </div>
              </div>
            )}
              </>
            )}
          </>
        )}

        {error && (
          <div className="error-banner" role="alert">
            <AlertTriangle size={15} />
            <span>{error}</span>
            <button type="button" aria-label="关闭错误" onClick={() => setError('')}><X size={13} /></button>
          </div>
        )}

        <section className="map-library">
          <div className="section-label">
            <Layers3 size={14} />
            <span>地图版本</span>
            <small>{maps.length}</small>
          </div>
          {maps.length === 0 ? (
            <div className="empty-library">
              <Network size={21} />
              <p>尚未发布地图</p>
              <span>上传道路数据以创建第一个版本</span>
            </div>
          ) : maps.map((record) => (
            <button
              type="button"
              className={`map-version ${activeMap?.id === record.id ? 'is-active' : ''}`}
              key={record.id}
              onClick={() => setActiveMap(record)}
            >
              <MapPinned size={16} />
              <span><strong>{record.name}</strong><small>{record.summary.directedEdges} EDGES · {record.id.slice(-6)}</small></span>
              <ChevronRight size={14} />
            </button>
          ))}
        </section>

        <section className="reference-library">
          <div className="section-label">
            <Palette size={14} />
            <span>参考图层</span>
            <small>{referenceLayers.length}</small>
          </div>
          {referenceLayers.length === 0 ? (
            <div className="reference-library__empty">
              <Layers3 size={18} />
              <span>点或面数据将在这里作为地图上下文叠加</span>
            </div>
          ) : referenceLayers.map((record) => {
            const visible = visibleReferenceIds.has(record.id)
            return (
              <div className={`reference-layer-row ${visible ? 'is-visible' : ''}`} key={record.id}>
                <div className="reference-layer-row__main">
                  <button
                    type="button"
                    className="reference-layer-toggle"
                    aria-pressed={visible}
                    onClick={() => toggleReferenceLayer(record.id)}
                  >
                    <i style={{ '--layer-color': record.style.color } as React.CSSProperties} />
                    <span>
                      <strong>{record.name}</strong>
                      <small>{record.geometry} · {formatNumber(record.featureCount)} FEATURES</small>
                    </span>
                    {visible ? <Eye size={14} /> : <EyeOff size={14} />}
                  </button>
                  <button
                    type="button"
                    className="reference-layer-focus"
                    aria-label={`定位到${record.name}`}
                    title="定位到图层"
                    onClick={() => focusReferenceLayer(record.id)}
                  >
                    <LocateFixed size={14} />
                  </button>
                  <button
                    type="button"
                    className="reference-layer-settings"
                    aria-label={`编辑${record.name}`}
                    aria-expanded={editingReferenceId === record.id}
                    onClick={() => toggleReferenceEditor(record)}
                  >
                    <Settings2 size={14} />
                  </button>
                </div>
                {editingReferenceId === record.id && referenceDraft && (
                  <div className="reference-layer-editor">
                    <label className="field-control">
                      <span>图层名称</span>
                      <input
                        value={referenceDraft.name}
                        onChange={(event) => setReferenceDraft({ ...referenceDraft, name: event.target.value })}
                      />
                    </label>
                    <div className="reference-layer-editor__style">
                      <input
                        type="color"
                        aria-label="图层颜色"
                        value={referenceDraft.style.color}
                        onChange={(event) => setReferenceDraft({
                          ...referenceDraft,
                          style: { ...referenceDraft.style, color: event.target.value },
                        })}
                      />
                      <label>
                        <span>OPACITY {Math.round(referenceDraft.style.opacity * 100)}%</span>
                        <input
                          type="range"
                          min="0.05"
                          max="1"
                          step="0.05"
                          value={referenceDraft.style.opacity}
                          onChange={(event) => setReferenceDraft({
                            ...referenceDraft,
                            style: { ...referenceDraft.style, opacity: Number(event.target.value) },
                          })}
                        />
                      </label>
                    </div>
                    <div className="reference-layer-editor__actions">
                      <button
                        type="button"
                        className="reference-layer-save"
                        disabled={referenceMutating || !referenceDraft.name.trim()}
                        onClick={() => void saveReferenceLayer()}
                      ><Save size={12} /> 保存</button>
                      <button
                        type="button"
                        className="reference-layer-delete"
                        disabled={referenceMutating}
                        onClick={() => void deleteReferenceLayer(record)}
                      ><Trash2 size={12} /> 删除</button>
                    </div>
                  </div>
                )}
              </div>
            )
          })}
        </section>

        <div className="rail-footnote">
          <Cpu size={13} /> C++20 MAP CORE
          <span>GDAL / BOOST RTREE</span>
        </div>
      </aside>

      <MapCanvas
        data={geoJSON}
        nodeData={nodeGeoJSON}
        issueData={issueGeoJSON}
        referenceLayers={referenceViews}
        referenceFocus={referenceFocus}
        mapName={activeMap?.name ?? referenceViews.find((layer) => layer.visible)?.record.name ?? 'BEIJING / SYNTHETIC GRID'}
        isDemo={!activeMap}
        selectedMatch={selectedMatch}
        selectedRoad={selectedRoad}
        selectedReferenceFeature={selectedReferenceFeature}
        focusedIssueIndex={focusedIssueIndex}
        routeMode={routeMode}
        routeStart={routeStart}
        routeEnd={routeEnd}
        routeData={routeResult?.geojson ?? null}
        trajectoryData={simResult?.geojson ?? null}
        vehicleFrame={vehicleFrame}
        junctionPickMode={junctionPickMode}
        selectedControlNodeId={selectedControlNodeId}
        onQuery={queryMap}
        onRoutePoint={handleRoutePoint}
        onIssueSelect={setFocusedIssueIndex}
        onRoadSelect={(road) => {
          setSelectedRoad(road)
          setSelectedReferenceFeature(null)
          if (rightPanel !== 'route') setRightPanel('details')
        }}
        onJunctionSelect={(nodeId) => {
          setSelectedControlNodeId(nodeId)
          setJunctionPickMode(false)
        }}
        onReferenceFeatureSelect={(feature) => {
          setSelectedReferenceFeature(feature)
          if (feature) {
            setSelectedRoad(null)
            setRightPanel('details')
          }
        }}
        onPointerMove={(longitude, latitude) => setPointer([longitude, latitude])}
      />

      <aside className="right-rail panel-grid">
        <div className="inspector-header">
          <div>
            <span className="eyebrow">INSPECTOR</span>
            <h2>地图信息</h2>
          </div>
          <Activity size={18} />
        </div>

        <div className="inspector-tabs" role="tablist" aria-label="地图信息切换">
          <button type="button" role="tab" aria-selected={rightPanel === 'details'} className={rightPanel === 'details' ? 'is-active' : ''} onClick={() => setRightPanel('details')}>详情</button>
          <button type="button" role="tab" aria-selected={rightPanel === 'quality'} className={rightPanel === 'quality' ? 'is-active' : ''} onClick={() => setRightPanel('quality')}>质检 <span>{activeMap?.issues.length ?? 0}</span></button>
          <button type="button" role="tab" aria-selected={rightPanel === 'route'} className={rightPanel === 'route' ? 'is-active' : ''} onClick={() => setRightPanel('route')}><Route size={13} /> 路由 / 仿真</button>
        </div>

        {rightPanel === 'details' ? (
          <div className="inspector-content">
            {selectedReferenceFeature ? (
              <section className="monitor-section reference-feature-inspector">
                <div className="section-label"><Layers3 size={14} /><span>参考要素</span><small>SELECTED</small></div>
                <div className="reference-feature-card">
                  <div className="reference-feature-card__head">
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
              <section className="monitor-section road-inspector">
                <div className="section-label"><Route size={14} /><span>道路属性</span><small>SELECTED</small></div>
                <div className="road-attribute-card">
                  <div className="road-attribute-card__title">
                    <span>{selectedRoad.CLASS || 'UNCLASSIFIED'}</span>
                    <strong>{selectedRoad.SOURCE_ID || selectedRoad.ROAD_ID}</strong>
                  </div>
                  <dl>
                    <div><dt>方向</dt><dd>{{ both: '双向', forward: '正向单行', reverse: '反向单行' }[selectedRoad.DIRECTION]}</dd></div>
                    <div><dt>限速</dt><dd>{Number(selectedRoad.SPEED_KPH).toFixed(0)} km/h</dd></div>
                    <div><dt>长度</dt><dd>{Number(selectedRoad.LENGTH_M).toFixed(1)} m</dd></div>
                    <div><dt>Z LEVEL</dt><dd>{selectedRoad.Z_LEVEL}</dd></div>
                    <div className="road-attribute-card__wide"><dt>ROAD ID</dt><dd>{selectedRoad.ROAD_ID}</dd></div>
                    <div className="road-attribute-card__wide"><dt>EDGE IDS</dt><dd>{selectedRoad.EDGE_IDS}</dd></div>
                  </dl>
                </div>
              </section>
            ) : (
              <div className="selection-empty">
                <MousePointer2 size={22} />
                <strong>选择地图要素</strong>
                <span>点击道路、行政区或 POI，在这里查看详细属性。</span>
              </div>
            )}

            <section className="monitor-section locator-panel">
              <div className="section-label"><Search size={14} /><span>位置解析</span><small>RTREE</small></div>
              <div className="coordinate-readout">
                <span>LON <strong>{pointer[0].toFixed(6)}</strong></span>
                <span>LAT <strong>{pointer[1].toFixed(6)}</strong></span>
              </div>
              {bestMatch ? (
                <div className="match-card">
                  <div className="match-card__head"><span>BEST MATCH</span><strong>{Math.round(bestMatch.confidence * 100)}%</strong></div>
                  <div className="match-road"><Waypoints size={18} /><strong>{bestMatch.source}</strong><span>EDGE {bestMatch.edge}</span></div>
                  <dl>
                    <div><dt>道路偏移</dt><dd>{bestMatch.offsetS.toFixed(2)} m</dd></div>
                    <div><dt>横向距离</dt><dd>{bestMatch.distance.toFixed(2)} m</dd></div>
                    <div><dt>ROAD ID</dt><dd>{bestMatch.roadId.slice(-12)}</dd></div>
                  </dl>
                </div>
              ) : <div className="locator-empty"><Gauge size={18} /><span>{activeMap ? '点击地图开始道路匹配' : '发布道路地图后可用'}</span></div>}
            </section>
          </div>
        ) : rightPanel === 'route' ? (
          <div className="inspector-content">
            <section className="monitor-section route-panel">
              <div className="section-label"><Route size={14} /><span>路径规划</span><small>{activeMap ? 'ROUTING' : 'NO MAP'}</small></div>
              <div className="route-controls">
                <button
                  type="button"
                  className={`route-mode-toggle ${routeMode ? 'is-active' : ''}`}
                  onClick={toggleRouteMode}
                  disabled={!activeMap}
                >
                  {routeMode ? <><EyeOff size={13} /> 退出路由模式</> : <><Zap size={13} /> 激活路由模式</>}
                </button>
                <label className="route-algorithm-select">
                  <span>算法</span>
                  <select
                    value={routeAlgorithm}
                    onChange={(event) => changeRouteAlgorithm(event.target.value as RouteAlgorithm)}
                  >
                    <option value="dijkstra">DIJKSTRA</option>
                    <option value="astar">A*</option>
                    <option value="bidijkstra">BI-DIJKSTRA</option>
                    <option value="biastar">BI-A*</option>
                  </select>
                </label>
                {(routeStart || routeEnd || routeResult) && (
                  <button type="button" className="route-clear" onClick={clearRoute}>
                    <X size={13} /> 清除
                  </button>
                )}
              </div>
              {!activeMap ? (
                <div className="route-empty"><Route size={18} /><span>发布道路地图后可用路径规划。</span></div>
              ) : routeBusy ? (
                <div className="route-loading"><LoaderCircle className="spin" size={18} /><span>正在计算路径…</span></div>
              ) : routeResult ? (
                routeResult.ok ? (
                  <>
                    <div className="metric-matrix route-metrics">
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
                    <div className="route-matches">
                      <div className="route-endpoint">
                        <span>起点吸附</span>
                        <strong>{routeResult.origin.source || `EDGE ${routeResult.origin.edge}`}</strong>
                        <small>偏移 {routeResult.origin.offsetS.toFixed(1)} m · 距路 {routeResult.origin.distance.toFixed(1)} m · 置信 {Math.round(routeResult.origin.confidence * 100)}%</small>
                      </div>
                      <div className="route-endpoint">
                        <span>终点吸附</span>
                        <strong>{routeResult.destination.source || `EDGE ${routeResult.destination.edge}`}</strong>
                        <small>偏移 {routeResult.destination.offsetS.toFixed(1)} m · 距路 {routeResult.destination.distance.toFixed(1)} m · 置信 {Math.round(routeResult.destination.confidence * 100)}%</small>
                      </div>
                    </div>
                  </>
                ) : (
                  <div className="route-failed">
                    <AlertTriangle size={18} />
                    <strong>{routeFailureLabel(routeResult.reason)}</strong>
                    <span>{routeResult.message}</span>
                  </div>
                )
              ) : (
                <div className="route-empty">
                  <MapPinned size={18} />
                  <span>{routeMode
                    ? routeStart
                      ? '已设定起点，在地图上点击终点。'
                      : '在地图上点击起点。'
                    : '激活路由模式后，在地图上依次点击起点和终点。'}</span>
                </div>
              )}
            </section>

            <section className="monitor-section sim-panel">
              <div className="section-label"><CarFront size={14} /><span>交通仿真</span><small>MESOSCOPIC</small></div>
              <div className={`sim-readiness ${routeStart && routeEnd ? 'is-ready' : ''}`}>
                <i />
                <span>{routeStart && routeEnd ? 'OD 已锁定，可生成确定性车流' : '先在地图上设置起点和终点'}</span>
              </div>
              <div className="sim-parameter-grid">
                <label><span>车辆数<small>VEHICLES</small></span><input type="number" min="1" max="10000" value={simConfig.count} onChange={(event) => setSimConfig({ ...simConfig, count: Number(event.target.value) })} /></label>
                <label><span>发车分布<small>SECONDS</small></span><input type="number" min="0" max={simConfig.durationSeconds} value={simConfig.spreadSeconds} onChange={(event) => setSimConfig({ ...simConfig, spreadSeconds: Number(event.target.value) })} /></label>
                <label><span>仿真时长<small>SECONDS</small></span><input type="number" min="60" max="28800" value={simConfig.durationSeconds} onChange={(event) => setSimConfig({ ...simConfig, durationSeconds: Number(event.target.value) })} /></label>
                <label><span>采样间隔<small>SECONDS</small></span><input type="number" min={Math.max(1, simConfig.stepSeconds)} value={simConfig.sampleIntervalSeconds} onChange={(event) => setSimConfig({ ...simConfig, sampleIntervalSeconds: Number(event.target.value) })} /></label>
                <label title="同一路段车辆在自由流状态下连续驶出的最小时间间隔；0 表示关闭出口门控。"><span>自由流出车<small>HEADWAY · S</small></span><input type="number" min="0" max="60" step="0.1" value={simConfig.exitHeadwayFfSeconds} onChange={(event) => { const value = Number(event.target.value); setSimConfig({ ...simConfig, exitHeadwayFfSeconds: value, exitHeadwayJamSeconds: simConfig.exitHeadwayJamSeconds === 0 ? 0 : Math.max(value, simConfig.exitHeadwayJamSeconds) }) }} /></label>
                <label title="路段拥堵时连续车辆驶出的最小时间间隔；0 表示沿用自由流值。"><span>拥堵出车<small>HEADWAY · S</small></span><input type="number" min={simConfig.exitHeadwayFfSeconds} max="60" step="0.1" value={simConfig.exitHeadwayJamSeconds} onChange={(event) => { const value = Number(event.target.value); setSimConfig({ ...simConfig, exitHeadwayJamSeconds: value === 0 ? 0 : Math.max(value, simConfig.exitHeadwayFfSeconds) }) }} /></label>
                <label title="按此仿真时间间隔从实时路段占用率重建路由权重；0 表示关闭周期扫描。"><span>拥堵扫描<small>REROUTE · S</small></span><input type="number" min="0" max="3600" step="1" value={simConfig.rerouteIntervalSeconds} onChange={(event) => { const value = Number(event.target.value); setSimConfig({ ...simConfig, rerouteIntervalSeconds: value === 0 ? 0 : Math.max(value, simConfig.stepSeconds) }) }} /></label>
                <label title="路段动态代价相对上次发布值达到该倍率时，才触发受影响车辆重规划。"><span>权重阈值<small>COST RATIO</small></span><input type="number" min="1.01" max="10" step="0.05" value={simConfig.rerouteCostRatio} onChange={(event) => setSimConfig({ ...simConfig, rerouteCostRatio: Math.max(1.01, Number(event.target.value)) })} /></label>
              </div>
              <p className="sim-flow-note">出口间隔限制路段放行率；拥堵扫描按占用率生成动态权重。两项扫描间隔为 0 时关闭周期重规划，显式封路、限速和降容仍会立即评估。</p>
              <SimulationControlPanel
                controls={simControls}
                durationSeconds={simConfig.durationSeconds}
                vehicleCount={simConfig.count}
                selectedRoad={selectedRoad}
                selectedNodeId={selectedControlNodeId}
                pickingJunction={junctionPickMode}
                onPickingJunctionChange={(active) => {
                  setJunctionPickMode(active)
                  if (active) setRouteMode(false)
                }}
                onChange={(controls) => {
                  setSimControls(controls)
                  setSimResult(null)
                  setPlaying(false)
                  setPlaybackTime(0)
                }}
              />
              <SignalPlanPanel
                plans={simControls.signalPlans}
                selectedNodeId={selectedControlNodeId}
                pickingJunction={junctionPickMode}
                onPickingJunctionChange={(active) => {
                  setJunctionPickMode(active)
                  if (active) setRouteMode(false)
                }}
                onChange={(signalPlans) => {
                  setSimControls({ ...simControls, signalPlans })
                  setSimResult(null)
                  setPlaying(false)
                  setPlaybackTime(0)
                }}
              />
              <button className="sim-run" type="button" disabled={!activeMap || !routeStart || !routeEnd || simBusy} onClick={() => void runSimulation()}>
                {simBusy ? <LoaderCircle className="spin" size={15} /> : <Play size={14} fill="currentColor" />}
                <span>{simBusy ? 'C++ 内核计算中' : `运行 ${formatNumber(simConfig.count)} 辆车`}</span>
                <small>{routeAlgorithm.toUpperCase()}</small>
              </button>

              {simResult?.ok && (
                <div className="sim-result">
                  <div className="sim-result__headline">
                    <div><span>到达率</span><strong>{Math.round(simResult.arrived / Math.max(1, simResult.vehicles) * 100)}<small>%</small></strong></div>
                    <div className="sim-result__status"><i /><span>{simResult.deadlock ? 'DEADLOCK' : 'COMPLETE'}</span><small>{simResult.computeMs.toFixed(1)} ms</small></div>
                  </div>
                  <div className="sim-metrics">
                    <div><span>已到达</span><strong>{formatNumber(simResult.arrived)}</strong></div>
                    <div><span>平均耗时</span><strong>{formatDuration(simResult.avgTravelS)}</strong></div>
                    <div><span>行驶距离</span><strong>{(simResult.totalDistanceM / 1000).toFixed(1)} km</strong></div>
                    <div><span>路线计算</span><strong>{formatNumber(simResult.routePlans)}</strong></div>
                  </div>
                  {simResult.controlEvents > 0 && (
                    <div className="sim-control-summary">
                      <span>已应用 {simResult.controlEvents} 条控制</span>
                      <small>车辆 {simResult.vehicleControls} · 道路 {simResult.roadControls} · 路口 {simResult.junctionControls}</small>
                    </div>
                  )}
                  {simResult.rerouteAttempts > 0 && (
                    <div className="sim-reroute-summary">
                      <span>动态重规划 {simResult.rerouteAttempts} 次</span>
                      <small>成功 {simResult.rerouteSucceeded} · 失败 {simResult.rerouteFailed}</small>
                    </div>
                  )}
                  {simResult.signalPlans > 0 && (
                    <div className="sim-signal-summary">
                      <span>信号控制 {simResult.signalPlans} 个路口</span>
                      <small>{simResult.signalPhases} 相位 · 红灯 {simResult.signalRedWaitEvents} · 饱和 {simResult.signalSaturationWaitEvents} · 放行 {simResult.signalMovementsPassed}</small>
                    </div>
                  )}
                  {(simResult.waitingAtEnd > 0 || simResult.drivingAtEnd > 0 || simResult.unroutable > 0) && (
                    <div className="sim-result__residual">
                      <span>等待 {simResult.waitingAtEnd}</span>
                      <span>行驶中 {simResult.drivingAtEnd}</span>
                      <span>不可规划 {simResult.unroutable}</span>
                    </div>
                  )}
                  {simResult.playback && (
                    <div className="playback-console">
                      <div className="playback-console__time">
                        <span>PLAYBACK</span>
                        <strong>{formatDuration(playbackTime)}</strong>
                        <small>/ {formatDuration(simResult.playback.duration_s)}</small>
                      </div>
                      <input type="range" min="0" max={simResult.playback.duration_s} step="0.1" value={playbackTime} onChange={(event) => { setPlaybackTime(Number(event.target.value)); setPlaying(false) }} />
                      <div className="playback-console__controls">
                        <button type="button" aria-label={playing ? '暂停回放' : '播放回放'} onClick={() => { if (playbackTime >= simResult.playback!.duration_s) setPlaybackTime(0); setPlaying(!playing) }}>
                          {playing ? <Pause size={13} fill="currentColor" /> : <Play size={13} fill="currentColor" />}
                        </button>
                        {[1, 10, 30].map((speed) => (
                          <button type="button" className={playbackSpeed === speed ? 'is-active' : ''} key={speed} onClick={() => setPlaybackSpeed(speed)}>{speed}×</button>
                        ))}
                        <span title="当前时刻已发生的重规划次数">{(simResult.playback.reroutes ?? []).filter((reroute) => reroute.time_s <= playbackTime).length} REROUTE · {vehicleFrame.features.length} ACTIVE</span>
                      </div>
                    </div>
                  )}
                </div>
              )}
            </section>
          </div>
        ) : (
          <div className="inspector-content">
            <section className="quality-summary">
              <div><span>拓扑健康度</span><strong>{qualityScore}<small>/100</small></strong></div>
              <i className={qualityScore >= 90 ? 'is-good' : qualityScore >= 70 ? 'is-warning' : 'is-danger'}>{qualityScore >= 90 ? '结构稳定' : qualityScore >= 70 ? '需要复核' : '高风险'}</i>
            </section>
            <div className="metric-matrix">
              <article><span>拓扑节点</span><strong>{formatNumber(summary?.nodes ?? 28)}</strong></article>
              <article><span>有向边</span><strong>{formatNumber(summary?.directedEdges ?? 42)}</strong></article>
              <article><span>转向规则</span><strong>{formatNumber(summary?.turnTransitions ?? 0)}</strong></article>
              <article><span>致命错误</span><strong>{summary?.fatal ?? 0}</strong></article>
            </div>
            {activeMap?.cleaning && (
              <section className="monitor-section cleaning-report">
                <div className="section-label"><CarFront size={14} /><span>OSM 清洗报告</span><small>{activeMap.cleaning.profile.toUpperCase()}</small></div>
                <div className="cleaning-report__hero">
                  <div>
                    <span>可行车保留率</span>
                    <strong>{Math.round(activeMap.cleaning.outputFeatures / Math.max(1, activeMap.cleaning.inputFeatures) * 100)}<small>%</small></strong>
                  </div>
                  <div className="cleaning-report__counts">
                    <span><small>INPUT</small>{formatNumber(activeMap.cleaning.inputFeatures)}</span>
                    <span><small>OUTPUT</small>{formatNumber(activeMap.cleaning.outputFeatures)}</span>
                    <span><small>FILTERED</small>{formatNumber(activeMap.cleaning.filteredFeatures)}</span>
                  </div>
                </div>
                <div className="cleaning-report__bar"><i style={{ width: `${activeMap.cleaning.outputFeatures / Math.max(1, activeMap.cleaning.inputFeatures) * 100}%` }} /></div>
                <div className="cleaning-report__reasons">
                  {Object.entries(activeMap.cleaning.excludedByReason)
                    .sort(([, left], [, right]) => right - left)
                    .map(([reason, count]) => (
                      <div key={reason}><span>{cleaningReasonLabel(reason)}</span><strong>{formatNumber(count)}</strong></div>
                    ))}
                </div>
                <div className="cleaning-report__normalization">
                  <span>默认限速 <strong>{formatNumber(activeMap.cleaning.normalization.defaultSpeedApplied)}</strong></span>
                  <span>方向修正 <strong>{formatNumber(activeMap.cleaning.normalization.impliedOnewayApplied + activeMap.cleaning.normalization.reverseOnewayNormalized)}</strong></span>
                  <span>最短边 <strong>{activeMap.cleaning.options.minLengthMeters} m</strong></span>
                </div>
              </section>
            )}
            <section className="monitor-section">
              <div className="section-label"><ShieldCheck size={14} /><span>质检事件</span><small>{activeMap?.issues.length ?? 0}</small></div>
              <div className="issue-list">
                {!activeMap || activeMap.issues.length === 0 ? (
                  <div className="all-clear"><Check size={16} /><span>没有需要处理的拓扑事件</span></div>
                ) : activeMap.issues.slice(0, 12).map((issue, index) => (
                  <button
                    type="button"
                    className={`issue issue--${issue.severity} ${focusedIssueIndex === index ? 'is-focused' : ''}`}
                    key={`${issue.code}-${index}`}
                    disabled={!locatedIssueIndices.has(index)}
                    title={locatedIssueIndices.has(index) ? '在地图中定位' : '这是全局问题，没有单一位置'}
                    onClick={() => setFocusedIssueIndex(index)}
                  >
                    <CircleDot size={12} />
                    <div><strong>{issue.code}</strong><span>{issue.message}</span></div>
                    <small>{severityLabel(issue.severity)}</small>
                  </button>
                ))}
              </div>
            </section>
          </div>
        )}

        <div className="runtime-band"><span><i /> MAP RUNTIME</span><strong>{activeMap ? 'INDEX READY' : 'STANDBY'}</strong></div>
      </aside>

      <footer className="statusbar">
        <span><i className="statusbar__live" /> ZEUS MAP KERNEL</span>
        <span>CRS <strong>{inspect?.crs || 'WGS84 DISPLAY'}</strong></span>
        <span>POINTER <strong>{pointer[0].toFixed(5)}, {pointer[1].toFixed(5)}</strong></span>
        <span className="statusbar__right">IMMUTABLE RUNTIME MAP <CircleDot size={9} /></span>
      </footer>
    </main>
  )
}
