import { useCallback, useRef, useState } from 'react'
import { api } from '../api'
import type {
  ImportJob,
  InspectResult,
  MapRecord,
  Mapping,
  OSMPreprocessOptions,
  ReferenceGeoJSON,
  ReferenceLayerRecord,
  ReferenceLayerStyle,
} from '../types'

export type IntakeStage = 'idle' | 'uploading' | 'inspected' | 'importing' | 'ready' | 'error'

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

export interface MapIntakeOptions {
  hasActiveMap: boolean
  onMapImported(record: MapRecord): void
  onReferencePublished(record: ReferenceLayerRecord, data: ReferenceGeoJSON): void
  markServiceOnline(): void
}

export interface MapIntakeApi {
  stage: IntakeStage
  files: File[]
  inspect: InspectResult | null
  mapping: Mapping
  osmPreprocess: OSMPreprocessOptions
  mapName: string
  importJob: ImportJob | null
  /** Pre-publish draft style for reference intake (point data defaults amber). */
  referenceDraftStyle: ReferenceLayerStyle
  referenceImporting: boolean
  error: string
  dismissError(): void
  handleFiles(files: File[]): Promise<void>
  setMapName(name: string): void
  setMappingField<K extends keyof Mapping>(field: K, value: Mapping[K]): void
  setOSMPreprocess(next: OSMPreprocessOptions): void
  setReferenceDraftStyle(style: ReferenceLayerStyle): void
  importMap(): Promise<void>
  cancelImport(): Promise<void>
  importReferenceLayer(): Promise<void>
}

/** Upload → inspect → (reference publish | mapping + OSM profile) → compile. */
export function useMapIntake(options: MapIntakeOptions): MapIntakeApi {
  // Latest-ref so handleFiles stays stable like the original useCallback([]).
  const optionsRef = useRef(options)
  optionsRef.current = options

  const [stage, setStage] = useState<IntakeStage>('idle')
  const [files, setFiles] = useState<File[]>([])
  const [inspect, setInspect] = useState<InspectResult | null>(null)
  const [mapping, setMapping] = useState<Mapping>(emptyMapping)
  const [osmPreprocess, setOSMPreprocess] = useState<OSMPreprocessOptions>(defaultOSMPreprocess)
  const [mapName, setMapName] = useState('')
  const [importJob, setImportJob] = useState<ImportJob | null>(null)
  const [referenceDraftStyle, setReferenceDraftStyle] =
    useState<ReferenceLayerStyle>(defaultReferenceStyle)
  const [referenceImporting, setReferenceImporting] = useState(false)
  const [error, setError] = useState('')

  const handleFiles = useCallback(async (incoming: File[]) => {
    const supported = incoming.filter((file) =>
      /\.(shp|shx|dbf|prj|cpg|geojson|json|csv)$/i.test(file.name))
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
      setReferenceDraftStyle(result.geometry.toLowerCase().includes('point')
        ? { color: '#ffb24a', opacity: 0.9 }
        : defaultReferenceStyle)
      optionsRef.current.markServiceOnline()
      setStage('inspected')
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : '无法检查地图数据。')
      setStage('error')
    }
  }, [])

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
      optionsRef.current.onMapImported(completed.map)
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
        style: referenceDraftStyle,
      })
      const data = await api.getReferenceGeoJSON(record.id)
      optionsRef.current.onReferencePublished(record, data)
      setInspect(null)
      setFiles([])
      setStage(optionsRef.current.hasActiveMap ? 'ready' : 'idle')
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : '参考图层发布失败。')
    } finally {
      setReferenceImporting(false)
    }
  }

  const setMappingField = <K extends keyof Mapping>(field: K, value: Mapping[K]) => {
    setMapping((current) => ({ ...current, [field]: value }))
  }

  const dismissError = () => setError('')

  return {
    stage, files, inspect, mapping, osmPreprocess, mapName, importJob,
    referenceDraftStyle, referenceImporting, error,
    dismissError, handleFiles, setMapName, setMappingField, setOSMPreprocess,
    setReferenceDraftStyle, importMap, cancelImport, importReferenceLayer,
  }
}
