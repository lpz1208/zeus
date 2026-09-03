import type {
  AgentActionRequest,
  AgentSessionCreated,
  AgentActionResult,
  AgentRouteCandidate,
  AgentSessionObservation,
  AgentSessionRequest,
  AgentSessionState,
  AgentSnapshot,
  AgentSnapshotRestore,
  AgentStepResponse,
  AgentToolRegistry,
  AgentVehicleObservation,
  ImportJob,
  InspectResult,
  IssueGeoJSON,
  MapRecord,
  Mapping,
  NodeGeoJSON,
  OSMPreprocessOptions,
  QueryRequest,
  QueryResponse,
  ReferenceGeoJSON,
  ReferenceLayerRecord,
  ReferenceLayerStyle,
  RoadGeoJSON,
  RouteRequest,
  RouteResponse,
  SimulateRequest,
  SimulateResponse,
} from './types'

async function request<T>(input: RequestInfo | URL, init?: RequestInit): Promise<T> {
  const response = await fetch(input, init)
  if (!response.ok) {
    let message = `${response.status} ${response.statusText}`
    try {
      const body = await response.json() as { error?: string }
      if (body.error) message = body.error
    } catch {
      // Preserve the HTTP status when the response is not JSON.
    }
    throw new Error(message)
  }
  if (response.status === 204) return undefined as T
  return response.json() as Promise<T>
}

export const api = {
  listMaps(): Promise<MapRecord[]> {
    return request('/api/maps')
  },

  async inspectFiles(files: File[]): Promise<InspectResult> {
    const form = new FormData()
    files.forEach((file) => form.append('files', file, file.name))
    return request('/api/maps/inspect', { method: 'POST', body: form })
  },

  importMap(payload: {
    uploadId: string
    sourceFile: string
    turnRestrictionsFile?: string
    name: string
    mapping: Mapping
    osmPreprocess: OSMPreprocessOptions
  }): Promise<ImportJob> {
    return request('/api/maps/import', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    })
  },

  listReferenceLayers(): Promise<ReferenceLayerRecord[]> {
    return request('/api/reference-layers')
  },

  createReferenceLayer(payload: {
    uploadId: string
    sourceFile: string
    name: string
    style: ReferenceLayerStyle
  }): Promise<ReferenceLayerRecord> {
    return request('/api/reference-layers', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    })
  },

  getReferenceGeoJSON(id: string): Promise<ReferenceGeoJSON> {
    return request(`/api/reference-layers/${encodeURIComponent(id)}/geojson`)
  },

  updateReferenceLayer(
    id: string,
    payload: { name?: string; style?: ReferenceLayerStyle },
  ): Promise<ReferenceLayerRecord> {
    return request(`/api/reference-layers/${encodeURIComponent(id)}`, {
      method: 'PATCH',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    })
  },

  deleteReferenceLayer(id: string): Promise<void> {
    return request(`/api/reference-layers/${encodeURIComponent(id)}`, { method: 'DELETE' })
  },

  getGeoJSON(id: string): Promise<RoadGeoJSON> {
    return request(`/api/maps/${encodeURIComponent(id)}/geojson`)
  },

  getIssueGeoJSON(id: string): Promise<IssueGeoJSON> {
    return request(`/api/maps/${encodeURIComponent(id)}/issues.geojson`)
  },

  getNodeGeoJSON(id: string): Promise<NodeGeoJSON> {
    return request(`/api/maps/${encodeURIComponent(id)}/nodes.geojson`)
  },

  getJob(id: string): Promise<ImportJob> {
    return request(`/api/jobs/${encodeURIComponent(id)}`)
  },

  waitForJob(id: string, onProgress: (job: ImportJob) => void): Promise<ImportJob> {
    return new Promise((resolve, reject) => {
      const source = new EventSource(`/api/jobs/${encodeURIComponent(id)}/events`)
      const timeout = window.setTimeout(() => {
        source.close()
        reject(new Error('地图编译进度等待超时，请在地图版本列表中检查结果。'))
      }, 15 * 60 * 1000)
      const finish = (job: ImportJob) => {
        onProgress(job)
        if (!['succeeded', 'failed', 'cancelled'].includes(job.status)) return
        window.clearTimeout(timeout)
        source.close()
        resolve(job)
      }
      source.addEventListener('progress', (event) => {
        try {
          finish(JSON.parse((event as MessageEvent<string>).data) as ImportJob)
        } catch {
          window.clearTimeout(timeout)
          source.close()
          reject(new Error('无法解析地图编译进度。'))
        }
      })
      source.onerror = () => {
        void api.getJob(id).then(finish).catch(() => {
          // EventSource will reconnect automatically while the job is still active.
        })
      }
    })
  },

  cancelJob(id: string): Promise<ImportJob> {
    return request(`/api/jobs/${encodeURIComponent(id)}/cancel`, { method: 'POST' })
  },

  queryMap(id: string, payload: QueryRequest): Promise<QueryResponse> {
    return request(`/api/maps/${encodeURIComponent(id)}/query`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    })
  },

  routeMap(id: string, payload: RouteRequest): Promise<RouteResponse> {
    return request(`/api/maps/${encodeURIComponent(id)}/route`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    })
  },

  simulateMap(id: string, payload: SimulateRequest): Promise<SimulateResponse> {
    return request(`/api/maps/${encodeURIComponent(id)}/simulate`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    })
  },

  getAgentTools(mapId: string): Promise<AgentToolRegistry> {
    return request(`/api/maps/${encodeURIComponent(mapId)}/agent/tools`)
  },

  createAgentSession(mapId: string, payload: AgentSessionRequest): Promise<AgentSessionCreated> {
    return request(`/api/maps/${encodeURIComponent(mapId)}/agent/sessions`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    })
  },

  observeAgentSession(mapId: string, sessionId: string): Promise<AgentSessionObservation> {
    return request(`/api/maps/${encodeURIComponent(mapId)}/agent/sessions/${encodeURIComponent(sessionId)}`)
  },

  observeAgentVehicle(
    mapId: string,
    sessionId: string,
    vehicleId: number,
  ): Promise<AgentVehicleObservation> {
    return request(`/api/maps/${encodeURIComponent(mapId)}/agent/sessions/${encodeURIComponent(sessionId)}/agent/${vehicleId}`)
  },

  planAgentRoute(
    mapId: string,
    sessionId: string,
    vehicleId: number,
    algorithm: string,
  ): Promise<AgentRouteCandidate> {
    return request(`/api/maps/${encodeURIComponent(mapId)}/agent/sessions/${encodeURIComponent(sessionId)}/plan`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ vehicleId, algorithm }),
    })
  },

  stepAgentSession(
    mapId: string,
    sessionId: string,
    payload: { ticks?: number; untilEvent?: boolean; maxTicks?: number },
  ): Promise<AgentStepResponse> {
    return request(`/api/maps/${encodeURIComponent(mapId)}/agent/sessions/${encodeURIComponent(sessionId)}/step`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    })
  },

  submitAgentAction(
    mapId: string,
    sessionId: string,
    payload: AgentActionRequest,
  ): Promise<AgentActionResult> {
    return request(`/api/maps/${encodeURIComponent(mapId)}/agent/sessions/${encodeURIComponent(sessionId)}/actions`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    })
  },

  pauseAgentSession(mapId: string, sessionId: string): Promise<AgentSessionState> {
    return request(`/api/maps/${encodeURIComponent(mapId)}/agent/sessions/${encodeURIComponent(sessionId)}/pause`, {
      method: 'POST',
    })
  },

  createAgentSnapshot(mapId: string, sessionId: string): Promise<AgentSnapshot> {
    return request(`/api/maps/${encodeURIComponent(mapId)}/agent/sessions/${encodeURIComponent(sessionId)}/snapshots`, {
      method: 'POST',
    })
  },

  restoreAgentSnapshot(mapId: string, snapshotId: string): Promise<AgentSnapshotRestore> {
    return request(`/api/maps/${encodeURIComponent(mapId)}/agent/snapshots/${encodeURIComponent(snapshotId)}/restore`, {
      method: 'POST',
    })
  },

  deleteAgentSnapshot(mapId: string, snapshotId: string): Promise<void> {
    return request(`/api/maps/${encodeURIComponent(mapId)}/agent/snapshots/${encodeURIComponent(snapshotId)}`, {
      method: 'DELETE',
    })
  },

  closeAgentSession(mapId: string, sessionId: string): Promise<void> {
    return request(`/api/maps/${encodeURIComponent(mapId)}/agent/sessions/${encodeURIComponent(sessionId)}`, {
      method: 'DELETE',
    })
  },
}
