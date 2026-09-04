import { useEffect, useMemo, useRef, useState } from 'react'
import { api } from '../api'
import type {
  MapRecord,
  ReferenceGeoJSON,
  ReferenceLayerRecord,
  ReferenceLayerStyle,
  ReferenceLayerView,
} from '../types'

const emptyReference: ReferenceGeoJSON = { type: 'FeatureCollection', features: [] }

export interface ReferenceSaveDraft {
  name: string
  style: ReferenceLayerStyle
}

export interface ReferenceLibraryApi {
  layers: ReferenceLayerRecord[]
  views: ReferenceLayerView[]
  referencesLoaded: boolean
  mutating: boolean
  focusRequest: { layerId: string; requestId: number } | null
  error: string
  dismissError(): void
  /** Returns the visibility AFTER the toggle (callers clear selections). */
  toggleVisibility(id: string): boolean
  focusLayer(id: string): void
  /** Resolves to the updated record, or null on failure. */
  saveLayer(id: string, draft: ReferenceSaveDraft): Promise<ReferenceLayerRecord | null>
  /** Resolves to true when the layer was actually deleted. */
  removeLayer(record: ReferenceLayerRecord): Promise<boolean>
  /** Prepends a freshly published layer, shows and focuses it (intake flow). */
  addPublished(record: ReferenceLayerRecord, data: ReferenceGeoJSON): void
}

/**
 * Shared reference-layer state: list, eager geojson cache, visibility, focus
 * requests and CRUD. Selection interplay (clearing a selected feature when
 * its layer hides or is deleted) stays with the selection owner.
 */
export function useReferenceLayers(
  maps: MapRecord[],
  mapsLoaded: boolean,
): ReferenceLibraryApi {
  const [layers, setLayers] = useState<ReferenceLayerRecord[]>([])
  const [data, setData] = useState<Record<string, ReferenceGeoJSON>>({})
  const [visibleIds, setVisibleIds] = useState<Set<string>>(new Set())
  const [focusRequest, setFocusRequest] = useState<{ layerId: string; requestId: number } | null>(null)
  const [mutating, setMutating] = useState(false)
  const [referencesLoaded, setReferencesLoaded] = useState(false)
  const [error, setError] = useState('')
  const hasAutoFocusedReference = useRef(false)

  useEffect(() => {
    api.listReferenceLayers()
      .then(async (records) => {
        const loaded = await Promise.all(records.map(async (record) => {
          const geo = await api.getReferenceGeoJSON(record.id).catch(() => emptyReference)
          return [record.id, geo] as const
        }))
        setLayers(records)
        setData(Object.fromEntries(loaded))
        setVisibleIds(new Set(records.map((record) => record.id)))
      })
      .catch(() => {
        // Road map operation remains available when an older server has no reference-layer API.
      })
      .finally(() => setReferencesLoaded(true))
  }, [])

  useEffect(() => {
    if (hasAutoFocusedReference.current || !mapsLoaded || !referencesLoaded ||
        maps.length > 0 || layers.length === 0) return
    hasAutoFocusedReference.current = true
    setFocusRequest({ layerId: layers[0].id, requestId: Date.now() })
  }, [maps.length, mapsLoaded, layers, referencesLoaded])

  const views = useMemo(
    () => layers.map((record) => ({
      record,
      data: data[record.id] ?? emptyReference,
      visible: visibleIds.has(record.id),
    })),
    [data, layers, visibleIds],
  )

  const toggleVisibility = (id: string): boolean => {
    const visibleAfter = !visibleIds.has(id)
    setVisibleIds((current) => {
      const next = new Set(current)
      if (next.has(id)) next.delete(id)
      else next.add(id)
      return next
    })
    return visibleAfter
  }

  const focusLayer = (id: string) => {
    setVisibleIds((current) => new Set(current).add(id))
    setFocusRequest({ layerId: id, requestId: Date.now() })
  }

  const saveLayer = async (id: string, draft: ReferenceSaveDraft) => {
    setMutating(true)
    setError('')
    try {
      const updated = await api.updateReferenceLayer(id, draft)
      setLayers((current) => current.map((record) => (
        record.id === updated.id ? updated : record
      )))
      return updated
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : '参考图层更新失败。')
      return null
    } finally {
      setMutating(false)
    }
  }

  const removeLayer = async (record: ReferenceLayerRecord) => {
    if (!window.confirm(`确定删除参考图层“${record.name}”？该操作无法撤销。`)) return false
    setMutating(true)
    setError('')
    try {
      await api.deleteReferenceLayer(record.id)
      setLayers((current) => current.filter((item) => item.id !== record.id))
      setData((current) => {
        const next = { ...current }
        delete next[record.id]
        return next
      })
      setVisibleIds((current) => {
        const next = new Set(current)
        next.delete(record.id)
        return next
      })
      return true
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : '参考图层删除失败。')
      return false
    } finally {
      setMutating(false)
    }
  }

  const addPublished = (record: ReferenceLayerRecord, geo: ReferenceGeoJSON) => {
    setLayers((current) => [record, ...current.filter((item) => item.id !== record.id)])
    setData((current) => ({ ...current, [record.id]: geo }))
    setVisibleIds((current) => new Set(current).add(record.id))
    setFocusRequest({ layerId: record.id, requestId: Date.now() })
  }

  const dismissError = () => setError('')

  return {
    layers, views, referencesLoaded, mutating, focusRequest, error,
    dismissError, toggleVisibility, focusLayer, saveLayer, removeLayer, addPublished,
  }
}
