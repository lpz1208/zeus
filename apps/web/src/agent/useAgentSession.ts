import { useEffect, useMemo, useState } from 'react'
import { api } from '../api'
import type {
  AgentRouteCandidate,
  AgentSessionCreated,
  AgentSessionState,
  AgentSnapshot,
  AgentToolRegistry,
  AgentVehicleObservation,
  MapRecord,
  RouteAlgorithm,
  RouteGeoJSON,
} from '../types'
import { algorithmLabels, formatTime } from './agentGeo'

export type InspectorTab = 'observation' | 'tools' | 'trace'
export type AgentBusy =
  | 'create'
  | 'step'
  | 'event'
  | 'plan'
  | 'action'
  | 'snapshot'
  | 'restore'
  | 'snapshot-delete'
  | 'close'
  | null

export type TimelineKind = 'environment' | 'observation' | 'tool' | 'action' | 'guard'

export interface TimelineEvent {
  id: number
  kind: TimelineKind
  title: string
  detail: string
  simulationTime: number
  stateVersion?: number
}

const FALLBACK_ALGORITHMS: RouteAlgorithm[] = [
  'dijkstra', 'astar', 'bidijkstra', 'biastar',
]

/**
 * Owns every agent-session flow of the workbench: OD picking, environment
 * creation, event-driven stepping, tool comparison, the decision barrier,
 * snapshots and the trace timeline. Wire shapes are documented in types.ts.
 */
export function useAgentSession(activeMap: MapRecord | null) {
  const [origin, setOrigin] = useState<[number, number] | null>(null)
  const [destination, setDestination] = useState<[number, number] | null>(null)
  const [algorithm, setAlgorithm] = useState<RouteAlgorithm>('astar')
  const [durationSeconds, setDurationSeconds] = useState(900)
  const [decisionIntervalSeconds, setDecisionIntervalSeconds] = useState(30)

  const [registry, setRegistry] = useState<AgentToolRegistry | null>(null)
  const [session, setSession] = useState<string | null>(null)
  const [state, setState] = useState<AgentSessionState | null>(null)
  const [observation, setObservation] = useState<AgentVehicleObservation | null>(null)
  const [decisionId, setDecisionId] = useState<string | null>(null)
  const [candidates, setCandidates] = useState<AgentRouteCandidate[]>([])
  const [selectedCandidate, setSelectedCandidate] = useState<string | null>(null)
  const [snapshots, setSnapshots] = useState<AgentSnapshot[]>([])
  const [timeline, setTimeline] = useState<TimelineEvent[]>([])
  const [tab, setTab] = useState<InspectorTab>('observation')
  const [busy, setBusy] = useState<AgentBusy>(null)
  const [error, setError] = useState('')
  const [previewRoute, setPreviewRoute] = useState<RouteGeoJSON | null>(null)

  const agentVehicleId = state?.agentVehicleIds?.[0] ?? 0

  const appendTimeline = (
    kind: TimelineKind,
    title: string,
    detail: string,
    nextState: AgentSessionState | null = state,
  ) => {
    setTimeline((current) => [{
      id: Date.now() + current.length,
      kind,
      title,
      detail,
      simulationTime: nextState?.simulationTimeS ?? 0,
      stateVersion: nextState?.stateVersion,
    }, ...current].slice(0, 80))
  }

  useEffect(() => {
    setRegistry(null)
    setError('')
    if (!activeMap) return
    api.getAgentTools(activeMap.id)
      .then(setRegistry)
      .catch((reason: Error) => setError(reason.message))
  }, [activeMap])

  useEffect(() => {
    setPreviewRoute(null)
    if (!activeMap || !origin || !destination || session) return
    let cancelled = false
    api.routeMap(activeMap.id, {
      fromLon: origin[0],
      fromLat: origin[1],
      toLon: destination[0],
      toLat: destination[1],
      algorithm,
      maxDistance: 100,
    }).then((result) => {
      if (!cancelled) setPreviewRoute(result.ok ? result.geojson ?? null : null)
    }).catch(() => {
      if (!cancelled) setPreviewRoute(null)
    })
    return () => { cancelled = true }
  }, [activeMap, algorithm, destination, origin, session])

  const selectableAlgorithms = useMemo(
    () => registry?.algorithms.map((item) => item.algorithmId) ?? FALLBACK_ALGORITHMS,
    [registry],
  )

  const handleRoutePoint = (longitude: number, latitude: number) => {
    if (session) return
    if (!origin || destination) {
      setOrigin([longitude, latitude])
      setDestination(null)
      setPreviewRoute(null)
    } else {
      setDestination([longitude, latitude])
    }
  }

  const resetOD = () => {
    setOrigin(null)
    setDestination(null)
    setPreviewRoute(null)
  }

  const refreshObservation = async (
    nextSession = session,
    vehicleId = agentVehicleId,
  ) => {
    if (!activeMap || !nextSession) return null
    const [sessionObservation, vehicleObservation] = await Promise.all([
      api.observeAgentSession(activeMap.id, nextSession),
      api.observeAgentVehicle(activeMap.id, nextSession, vehicleId),
    ])
    setState(sessionObservation)
    setObservation(vehicleObservation)
    return vehicleObservation
  }

  const startSession = async () => {
    if (!activeMap || !origin || !destination) return
    setBusy('create')
    setError('')
    try {
      const created = await api.createAgentSession(activeMap.id, {
        vehicles: [{
          fromLon: origin[0],
          fromLat: origin[1],
          toLon: destination[0],
          toLat: destination[1],
          departSeconds: 0,
          algorithm,
          agent: true,
        }],
        durationSeconds,
        stepSeconds: 1,
        sampleIntervalSeconds: 10,
        exitHeadwayFfSeconds: 1.4,
        exitHeadwayJamSeconds: 2,
        rerouteIntervalSeconds: decisionIntervalSeconds,
        rerouteCostRatio: 1.25,
        minSpeedRatio: 0,
      })
      if (!created.sessionId) throw new Error('环境没有返回 sessionId')
      setSession(created.sessionId)
      setState(created)
      setCandidates([])
      setSelectedCandidate(null)
      setDecisionId(null)
      setTimeline([])
      await refreshObservation(created.sessionId, created.agents?.[0] ?? 0)
      appendTimeline('environment', 'Environment reset', `${created.sessionId} · Agent vehicle ${created.agents?.[0] ?? 0}`, created)
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : '无法创建 Agent 会话')
    } finally {
      setBusy(null)
    }
  }

  const stepSession = async (untilEvent: boolean) => {
    if (!activeMap || !session || decisionId) return
    setBusy(untilEvent ? 'event' : 'step')
    setError('')
    try {
      const result = await api.stepAgentSession(activeMap.id, session, untilEvent
        ? { untilEvent: true, maxTicks: 100000 }
        : { ticks: 1 })
      setState(result.state)
      setDecisionId(result.decisionId ?? null)
      setCandidates([])
      setSelectedCandidate(null)
      const latest = await refreshObservation()
      if (latest) setObservation(latest)
      appendTimeline(
        result.decisionId ? 'observation' : 'environment',
        result.decisionId ? 'Decision boundary' : 'Environment step',
        result.decisionId
          ? `${result.state.decisionReason || 'periodic'} · observation published`
          : `tick ${result.state.tick} committed`,
        result.state,
      )
      if (result.decisionId) setTab('observation')
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : '环境推进失败')
    } finally {
      setBusy(null)
    }
  }

  const planWithAlgorithms = async (algorithms: RouteAlgorithm[]) => {
    if (!activeMap || !session) return
    setBusy('plan')
    setError('')
    try {
      const planned = await Promise.all(algorithms.map((item) => (
        api.planAgentRoute(activeMap.id, session, agentVehicleId, item)
      )))
      setCandidates(planned)
      const best = planned
        .filter((item) => item.ok && item.timeS !== undefined)
        .sort((left, right) => (left.timeS ?? Infinity) - (right.timeS ?? Infinity))[0]
      setSelectedCandidate(best?.candidateId ?? null)
      appendTimeline(
        'tool',
        algorithms.length > 1 ? 'Tool comparison' : `Tool · ${algorithmLabels[algorithms[0]]}`,
        best
          ? `${planned.length} candidates · best ${algorithmLabels[best.algorithm]} / ${formatTime(best.timeS ?? 0)}`
          : 'no valid route candidate',
      )
      setTab('tools')
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : '路线工具调用失败')
    } finally {
      setBusy(null)
    }
  }

  const submitAction = async (kind: 'keep_route' | 'commit_route') => {
    if (!activeMap || !session || !state || !decisionId) return
    if (kind === 'commit_route' && !selectedCandidate) return
    setBusy('action')
    setError('')
    try {
      const result = await api.submitAgentAction(activeMap.id, session, {
        decisionId,
        agentId: 'default',
        vehicleId: agentVehicleId,
        kind,
        candidateId: kind === 'commit_route' ? selectedCandidate ?? undefined : undefined,
        basedOnStateVersion: state.stateVersion,
        reasonCode: kind === 'commit_route' ? 'operator_selected_candidate' : 'operator_keep_route',
      })
      appendTimeline(
        result.accepted ? 'action' : 'guard',
        kind === 'commit_route' ? 'Commit route' : 'Keep route',
        result.accepted ? 'Action Guard accepted · applies at next tick' : result.reason,
      )
      setDecisionId(null)
      setCandidates([])
      setSelectedCandidate(null)
      await refreshObservation()
      setTab('trace')
    } catch (reason) {
      appendTimeline('guard', 'Action rejected', reason instanceof Error ? reason.message : 'invalid action')
      setError(reason instanceof Error ? reason.message : '动作提交失败')
    } finally {
      setBusy(null)
    }
  }

  const createSnapshot = async () => {
    if (!activeMap || !session) return
    setBusy('snapshot')
    setError('')
    try {
      const snapshot = await api.createAgentSnapshot(activeMap.id, session)
      setSnapshots((current) => [snapshot, ...current])
      appendTimeline('environment', 'Snapshot created', `${snapshot.snapshotId} · tick ${snapshot.tick}`)
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : '快照创建失败')
    } finally {
      setBusy(null)
    }
  }

  const restoreSnapshot = async (snapshot: AgentSnapshot) => {
    if (!activeMap) return
    setBusy('restore')
    setError('')
    const previous = session
    try {
      const restored = await api.restoreAgentSnapshot(activeMap.id, snapshot.snapshotId)
      if (!restored.state.sessionId) throw new Error('恢复结果缺少 sessionId')
      setSession(restored.state.sessionId)
      setState(restored.state)
      setDecisionId(restored.decisionId ?? null)
      setCandidates([])
      setSelectedCandidate(null)
      await refreshObservation(restored.state.sessionId,
        restored.state.agentVehicleIds?.[0] ?? 0)
      if (previous) await api.closeAgentSession(activeMap.id, previous).catch(() => undefined)
      appendTimeline('environment', 'Snapshot restored', `${snapshot.snapshotId} → ${restored.state.sessionId}`, restored.state)
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : '快照恢复失败')
    } finally {
      setBusy(null)
    }
  }

  const deleteSnapshot = async (snapshot: AgentSnapshot) => {
    if (!activeMap) return
    setBusy('snapshot-delete')
    setError('')
    try {
      await api.deleteAgentSnapshot(activeMap.id, snapshot.snapshotId)
      setSnapshots((current) => current.filter((item) => item.snapshotId !== snapshot.snapshotId))
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : '快照删除失败')
    } finally {
      setBusy(null)
    }
  }

  const closeSession = async () => {
    if (!activeMap || !session) return
    setBusy('close')
    try {
      await api.closeAgentSession(activeMap.id, session)
      setSession(null)
      setState(null)
      setObservation(null)
      setDecisionId(null)
      setCandidates([])
      setSelectedCandidate(null)
      setTimeline([])
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : '关闭会话失败')
    } finally {
      setBusy(null)
    }
  }

  return {
    // mission setup
    origin, destination, algorithm, durationSeconds, decisionIntervalSeconds,
    setAlgorithm, setDurationSeconds, setDecisionIntervalSeconds,
    handleRoutePoint, resetOD, previewRoute,
    registry, selectableAlgorithms,
    // live session
    session, state, observation, decisionId, agentVehicleId,
    candidates, selectedCandidate,
    selectCandidate: setSelectedCandidate,
    snapshots, timeline, tab, setTab,
    busy, error,
    dismissError: () => setError(''),
    // flows
    startSession, stepSession, planWithAlgorithms, submitAction,
    createSnapshot, restoreSnapshot, deleteSnapshot, closeSession,
  }
}

export type AgentSessionApi = ReturnType<typeof useAgentSession>
