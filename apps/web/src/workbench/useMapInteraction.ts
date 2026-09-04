import { useCallback, useState } from 'react'
import { api } from '../api'
import type {
  MapRecord,
  MatchCandidate,
  QueryResponse,
  ReferenceFeatureSelection,
  RoadProperties,
} from '../types'

export interface MapInteractionApi {
  pointer: [number, number]
  queryResult: QueryResponse | null
  selectedMatch: MatchCandidate | null
  selectedRoad: RoadProperties | null
  selectedReferenceFeature: ReferenceFeatureSelection | null
  focusedIssueIndex: number | null
  bestMatch: MatchCandidate | null
  error: string
  dismissError(): void
  setPointer(longitude: number, latitude: number): void
  query(longitude: number, latitude: number): Promise<QueryResponse | null>
  /** Selects a road and clears any selected reference feature. */
  selectRoad(road: RoadProperties | null): void
  selectReferenceFeature(feature: ReferenceFeatureSelection | null): void
  selectIssue(index: number | null): void
  /** Drops every selection (map switch). */
  clearSelection(): void
  /** Drops a selected reference feature when its layer disappears or hides. */
  clearReferenceFeatureIfLayer(layerId: string): void
  /** Keeps a selected feature's layer label in sync after a rename. */
  syncLayerName(layerId: string, name: string): void
}

/** Click-driven inspection state: pointer, R-tree query, element selection. */
export function useMapInteraction(activeMap: MapRecord | null): MapInteractionApi {
  const [pointer, setPointerState] = useState<[number, number]>([116.391, 39.907])
  const [queryResult, setQueryResult] = useState<QueryResponse | null>(null)
  const [selectedMatch, setSelectedMatch] = useState<MatchCandidate | null>(null)
  const [selectedRoad, setSelectedRoad] = useState<RoadProperties | null>(null)
  const [selectedReferenceFeature, setSelectedReferenceFeature] =
    useState<ReferenceFeatureSelection | null>(null)
  const [focusedIssueIndex, setFocusedIssueIndex] = useState<number | null>(null)
  const [error, setError] = useState('')

  const query = useCallback(async (longitude: number, latitude: number) => {
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

  const selectRoad = (road: RoadProperties | null) => {
    setSelectedRoad(road)
    if (road) setSelectedReferenceFeature(null)
  }

  const selectReferenceFeature = (feature: ReferenceFeatureSelection | null) => {
    setSelectedReferenceFeature(feature)
    if (feature) setSelectedRoad(null)
  }

  const clearSelection = () => {
    setQueryResult(null)
    setSelectedMatch(null)
    setSelectedRoad(null)
    setSelectedReferenceFeature(null)
    setFocusedIssueIndex(null)
  }

  const clearReferenceFeatureIfLayer = (layerId: string) => {
    setSelectedReferenceFeature((current) =>
      current?.layerId === layerId ? null : current)
  }

  const syncLayerName = (layerId: string, name: string) => {
    setSelectedReferenceFeature((current) =>
      current?.layerId === layerId ? { ...current, layerName: name } : current)
  }

  const setPointer = (longitude: number, latitude: number) =>
    setPointerState([longitude, latitude])

  return {
    pointer, queryResult, selectedMatch, selectedRoad, selectedReferenceFeature,
    focusedIssueIndex,
    bestMatch: selectedMatch ?? queryResult?.matches[0] ?? null,
    error,
    dismissError: () => setError(''),
    setPointer, query, selectRoad, selectReferenceFeature,
    selectIssue: setFocusedIssueIndex,
    clearSelection, clearReferenceFeatureIfLayer, syncLayerName,
  }
}
