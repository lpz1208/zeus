import { useCallback, useEffect, useState } from 'react'
import { api } from '../api'
import { demoNetwork } from '../demo'
import type {
  IssueGeoJSON,
  MapRecord,
  NodeGeoJSON,
  RoadGeoJSON,
} from '../types'

const emptyIssues: IssueGeoJSON = { type: 'FeatureCollection', features: [] }
const emptyNodes: NodeGeoJSON = { type: 'FeatureCollection', features: [] }

export interface MapLibraryApi {
  maps: MapRecord[]
  activeMap: MapRecord | null
  roadData: RoadGeoJSON
  issueData: IssueGeoJSON
  nodeData: NodeGeoJSON
  mapsLoaded: boolean
  serviceOnline: boolean
  loadError: string
  dismissLoadError(): void
  activateMap(map: MapRecord): void
  /** Prepends an imported map version and activates it (intake flow). */
  registerImportedMap(record: MapRecord): void
  /** Flags the control plane online after a successful direct command. */
  markOnline(): void
}

/**
 * Shared map-version state consumed by both workbenches: the version list,
 * the active version, its geo trio (roads/issues/nodes) and service status.
 * Selection/route/simulation state is workbench-local and resets there.
 */
export function useMapLibrary(): MapLibraryApi {
  const [maps, setMaps] = useState<MapRecord[]>([])
  const [activeMap, setActiveMap] = useState<MapRecord | null>(null)
  const [roadData, setRoadData] = useState<RoadGeoJSON>(demoNetwork)
  const [issueData, setIssueData] = useState<IssueGeoJSON>(emptyIssues)
  const [nodeData, setNodeData] = useState<NodeGeoJSON>(emptyNodes)
  const [mapsLoaded, setMapsLoaded] = useState(false)
  const [serviceOnline, setServiceOnline] = useState(false)
  const [loadError, setLoadError] = useState('')

  useEffect(() => {
    api.listMaps()
      .then((records) => {
        setServiceOnline(true)
        setMaps(records)
        if (records.length > 0) setActiveMap(records[0])
      })
      .catch(() => setServiceOnline(false))
      .finally(() => setMapsLoaded(true))
  }, [])

  useEffect(() => {
    if (!activeMap) {
      setRoadData(demoNetwork)
      setIssueData(emptyIssues)
      setNodeData(emptyNodes)
      return
    }
    Promise.all([
      api.getGeoJSON(activeMap.id),
      api.getIssueGeoJSON(activeMap.id).catch(() => emptyIssues),
      api.getNodeGeoJSON(activeMap.id).catch(() => emptyNodes),
    ])
      .then(([roads, issues, nodes]) => {
        setRoadData(roads)
        setIssueData(issues)
        setNodeData(nodes)
      })
      .catch((reason: Error) => setLoadError(reason.message))
  }, [activeMap])

  const activateMap = useCallback((map: MapRecord) => setActiveMap(map), [])

  const registerImportedMap = useCallback((record: MapRecord) => {
    setMaps((current) => [record, ...current.filter((item) => item.id !== record.id)])
    setActiveMap(record)
  }, [])

  const markOnline = useCallback(() => setServiceOnline(true), [])
  const dismissLoadError = useCallback(() => setLoadError(''), [])

  return {
    maps, activeMap, roadData, issueData, nodeData,
    mapsLoaded, serviceOnline, loadError,
    dismissLoadError, activateMap, registerImportedMap, markOnline,
  }
}
