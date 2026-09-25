import type { AgentSessionRequest, AgentSnapshotRestore } from '../types'
import type { NavigationRecord } from './algorithmNavigation'

export interface NavigationHistorySummary {
  id: string
  revision: number
  sessionId: string
  updatedAt: string
  tick: number
  records: number
  finished: boolean
}

export interface NavigationHistory {
  id: string
  revision: number
  mapId: string
  sessionId: string
  createdAt: string
  updatedAt: string
  source: string
  records: NavigationRecord[]
  finished: boolean
  snapshot: { tick: number; stateVersion: number; request: AgentSessionRequest }
}

async function request<T>(path: string, body?: unknown, method = body === undefined ? 'GET' : 'POST'): Promise<T> {
  const response = await fetch(path, {
    method, headers: { 'Content-Type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body),
  })
  const data = await response.json()
  if (!response.ok) throw new Error(data.error || `导航历史请求失败 (${response.status})`)
  return data as T
}
const base = (map: string) => `/api/maps/${encodeURIComponent(map)}/agent/navigation-history`
export const navigationHistory = {
  list: (map: string) => request<NavigationHistorySummary[]>(base(map)),
  get: (map: string, id: string) => request<NavigationHistory>(`${base(map)}/${encodeURIComponent(id)}`),
  remove: (map: string, id: string) => request(`${base(map)}/${encodeURIComponent(id)}`, undefined, 'DELETE'),
  restore: (map: string, id: string, revision: number) => request<AgentSnapshotRestore>(`${base(map)}/${encodeURIComponent(id)}/restore`, { revision }),
  save: (map: string, session: string, payload: {
    id?: string; revision: number; basedOnStateVersion: number; source: string; records: NavigationRecord[]
  }) => request<NavigationHistorySummary>(`/api/maps/${encodeURIComponent(map)}/agent/sessions/${encodeURIComponent(session)}/navigation-history`, payload),
}

export function exportNavigationHistory(value: unknown, filename: string) {
  const url = URL.createObjectURL(new Blob([JSON.stringify(value, null, 2)], { type: 'application/json' }))
  const anchor = document.createElement('a')
  anchor.href = url
  anchor.download = filename
  anchor.click()
  setTimeout(() => URL.revokeObjectURL(url), 1000)
}
