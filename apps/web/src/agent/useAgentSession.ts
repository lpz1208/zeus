import { useEffect, useMemo, useRef, useState } from 'react'
import type { LabResult } from '../algorithm/useAlgorithmLab'
import { runAlgorithmNavigation, type NavigationRecord } from './algorithmNavigation'
import { navigationHistory, exportNavigationHistory, type NavigationHistory, type NavigationHistorySummary } from './navigationHistory'
import { api } from '../api'
import { useTracePlayback } from '../hooks/useTracePlayback'
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
  SearchTrace,
} from '../types'
import { algorithmLabels, formatTime } from './agentGeo'

export type InspectorTab = 'observation' | 'tools' | 'code' | 'trace'
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
  | 'navigation'
  | 'background'
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
  'dijkstra', 'astar', 'bidijkstra', 'biastar', 'kshortest',
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
  const [recoveryIntervalSeconds, setRecoveryIntervalSeconds] = useState(0)

  const [registry, setRegistry] = useState<AgentToolRegistry | null>(null)
  const [session, setSession] = useState<string | null>(null)
  const [state, setState] = useState<AgentSessionState | null>(null)
  const [observation, setObservation] = useState<AgentVehicleObservation | null>(null)
  const [decisionId, setDecisionId] = useState<string | null>(null)
  const [candidates, setCandidates] = useState<AgentRouteCandidate[]>([])
  const [selectedCandidate, setSelectedCandidate] = useState<string | null>(null)
  const [searchTrace, setSearchTrace] = useState<SearchTrace | null>(null)
  const [snapshots, setSnapshots] = useState<AgentSnapshot[]>([])
  const [timeline, setTimeline] = useState<TimelineEvent[]>([])
  const [tab, setTab] = useState<InspectorTab>('observation')
  const [busy, setBusy] = useState<AgentBusy>(null)
  const [error, setError] = useState('')
  const [previewRoute, setPreviewRoute] = useState<RouteGeoJSON | null>(null)
  const [source, setSource] = useState(() => {
    try { return localStorage.getItem('zeus.algorithm.draft.v1') ?? '' } catch { return '' }
  })
  const [codeResult, setCodeResult] = useState<LabResult | null>(null)
  const [navigation, setNavigation] = useState<{ status: 'idle' | 'running' | 'pausing' | 'paused' | 'stopped' | 'complete' | 'error'; phase: string }>({ status: 'idle', phase: '' })
  const [navigationRecords, setNavigationRecords] = useState<NavigationRecord[]>([])
  const [lastCheckpoint, setLastCheckpoint] = useState<NavigationHistorySummary | null>(null)
  const [historyVersion, setHistoryVersion] = useState(0)
  const viewToken = useRef<object>({})
  useEffect(() => {
    viewToken.current = {}
    return () => { viewToken.current = {} }
  }, [activeMap?.id])
  const navigationRun = useRef<{ active: boolean; stop: boolean } | null>(null)
  useEffect(() => () => {
    if (navigationRun.current) navigationRun.current.active = false
    navigationRun.current = null
  }, [activeMap?.id, session])
  const pauseNavigation = (stop = false) => {
    const run = navigationRun.current
    if (run) {
      run.active = false
      run.stop = stop
      setNavigation({ status: 'pausing', phase: '等待当前请求完成；不会发起下一步' })
    } else setNavigation({ status: stop ? 'stopped' : 'paused', phase: stop ? '自动导航已停止' : '自动导航已暂停' })
  }
  const codeRequest = useRef<AbortController | null>(null)
  useEffect(() => () => { codeRequest.current?.abort(); codeRequest.current = null }, [activeMap?.id, session])
  useEffect(() => { setCodeResult(null) }, [state?.stateVersion, session])

  const stopCode = () => {
    if (codeRequest.current) {
      codeRequest.current.abort()
      codeRequest.current = null
      setBusy(null)
    }
  }
  const updateSource = (value: string) => {
    if (navigationRun.current) pauseNavigation()
    stopCode()
    setSource(value)
    try { localStorage.setItem('zeus.algorithm.draft.v1', value) } catch { /* draft is optional */ }
    setCodeResult(null)
    setSearchTrace(null)
    setCandidates(current => current.filter(item => item.algorithm !== 'custom'))
    setSelectedCandidate(current => candidates.some(item => item.candidateId === current && item.algorithm === 'custom') ? null : current)
  }

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
        rerouteRecoveryIntervalSeconds: recoveryIntervalSeconds,
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
      setNavigationRecords([])
      setLastCheckpoint(null)
      setNavigation({ status: 'idle', phase: '' })
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

  const planWithSource = async () => {
    if (!activeMap || !session || !state || !decisionId || busy || !source.trim()) return
    const controller = new AbortController()
    codeRequest.current = controller
    setBusy('plan')
    setError('')
    setCodeResult(null)
    setCandidates(current => current.filter(item => item.algorithm !== 'custom'))
    setSelectedCandidate(null)
    try {
      const response = await fetch(`/api/maps/${encodeURIComponent(activeMap.id)}/agent/sessions/${encodeURIComponent(session)}/algorithms/plan`, {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, signal: controller.signal,
        body: JSON.stringify({ vehicleId: agentVehicleId, basedOnStateVersion: state.stateVersion, source, steps: 5000000 }),
      })
      const payload = await response.json() as { error?: string; result?: LabResult; candidate?: AgentRouteCandidate }
      if (!response.ok) throw new Error(payload.error ?? '车辆算法运行失败')
      if (codeRequest.current !== controller || controller.signal.aborted) return
      if (!payload.result) throw new Error('未收到算法执行结果')
      setCodeResult(payload.result)
      setSearchTrace(payload.result.searchTrace ?? null)
      if (payload.candidate?.ok) {
        const candidate = { ...payload.candidate, geojson: payload.result.geojson }
        setCandidates(current => [...current.filter(item => item.algorithm !== 'custom'), candidate])
        setSelectedCandidate(candidate.candidateId)
        appendTimeline('tool', 'Custom route verified', `${candidate.candidateId} · ${formatTime(candidate.timeS ?? 0)} · exact path`)
      }
    } catch (reason) {
      if (!controller.signal.aborted) setError(reason instanceof Error ? reason.message : '车辆算法运行失败')
    } finally {
      if (codeRequest.current === controller) { codeRequest.current = null; setBusy(null) }
    }
  }

  const startNavigation = async (once = false) => {
    if (!activeMap || !session || busy || navigationRun.current || !source.trim()) return
    const run = { active: true, stop: false }
    navigationRun.current = run
    setNavigation({ status: 'running', phase: '读取车辆状态' })
    setBusy('navigation')
    setError('')
    setCandidates([])
    setSelectedCandidate(null)
    setCodeResult(null)
    setNavigationRecords([])
    setLastCheckpoint(null)
    const records = new Map<string, NavigationRecord>()
    let saved: NavigationHistorySummary | null = null
    const attached = () => navigationRun.current === run
    const base = `/api/maps/${encodeURIComponent(activeMap.id)}/agent/sessions/${encodeURIComponent(session)}`
    try {
      const outcome = await runAlgorithmNavigation({
        runId: crypto.randomUUID(),
        source, vehicleId: agentVehicleId, once, maxDecisions: 200,
        active: () => attached() && run.active,
        request: async <T,>(path: string, body?: unknown): Promise<T> => {
          // Let resident worker commands finish. Aborting them kills the
          // shared map process and loses other sessions hosted in it.
          const response = await fetch(base + path, body === undefined ? {} : {
            method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
          })
          const payload = await response.json()
          if (!response.ok) throw new Error(payload.error ?? `请求失败 (${response.status})`)
          return payload as T
        },
        onObservation: async next => {
          if (!attached()) return
          setState(next)
          setDecisionId(next.decisionId || null)
          const vehicle = await api.observeAgentVehicle(activeMap.id, session, agentVehicleId)
          if (vehicle.stateVersion !== next.stateVersion) throw new Error('观察期间车辆状态发生变化，已暂停')
          if (attached()) setObservation(vehicle)
        },
        onPlan: value => {
          if (!attached() || !run.active) return
          setCodeResult(value.result)
          setSearchTrace(value.result.searchTrace ?? null)
          if (value.candidate) {
            setCandidates([{ ...value.candidate, geojson: value.result.geojson }])
            setSelectedCandidate(value.candidate.candidateId)
          }
        },
        onRecord: value => {
          const recorded = { ...value, sessionId: session, mapId: activeMap.id }
          records.set(recorded.id, recorded)
          if (attached()) setNavigationRecords([...records.values()])
        },
        checkpoint: async () => {
          // Finish persistence even after leaving the page. On reload during
          // an in-flight request, the last acknowledged checkpoint is the
          // recovery boundary; a new session is always restored from it.
          try {
            const current = await api.observeAgentSession(activeMap.id, session)
            saved = await navigationHistory.save(activeMap.id, session, {
              id: saved?.id, revision: saved?.revision ?? 0, basedOnStateVersion: current.stateVersion,
              source, records: [...records.values()],
            })
            if (attached()) setLastCheckpoint(saved)
          } catch (reason) {
            throw new Error(`导航记录保存失败，自动循环已暂停：${reason instanceof Error ? reason.message : String(reason)}`)
          }
        },
        onPhase: phase => { if (attached() && run.active) setNavigation({ status: 'running', phase }) },
        yieldBetween: () => new Promise(resolve => setTimeout(resolve, 250)),
      })
      if (attached()) {
        const phase = outcome === 'complete' ? '车辆任务已结束' : outcome === 'once' ? '单次决策已执行，车辆已暂停'
          : outcome === 'limit' ? '本次已保存 200 条记录；再次开始会创建新的导航历史' : run.stop ? '自动导航已停止' : '自动导航已暂停'
        setNavigation({ status: outcome === 'complete' ? 'complete' : run.stop ? 'stopped' : 'paused', phase })
      }
    } catch (reason) {
      if (attached()) {
        const message = reason instanceof Error ? reason.message : '自动导航失败'
        setError(message)
        setNavigation({ status: 'error', phase: '自动循环已停止，查看决策记录修正代码后可重试' })
      }
    } finally {
      if (attached()) {
        navigationRun.current = null
        setBusy(null)
        setCandidates([])
        setSelectedCandidate(null)
        setHistoryVersion(value => value + 1)
      }
    }
  }

  const exportNavigationRecords = () => {
    exportNavigationHistory({ version: 1, records: navigationRecords }, 'zeus-navigation-decisions.json')
  }

  const restoreNavigation = async (history: NavigationHistory) => {
    if (!activeMap || busy || navigationRun.current || history.mapId !== activeMap.id) return
    setBusy('restore')
    setError('')
    const previous = session
    const token = viewToken.current
    let restoredSession: string | undefined
    try {
      const restored = await navigationHistory.restore(activeMap.id, history.id, history.revision)
      restoredSession = restored.state.sessionId
      if (!restoredSession) throw new Error('恢复结果缺少 sessionId')
      const vehicleId = restored.state.agentVehicleIds?.[0] ?? 0
      const vehicle = await api.observeAgentVehicle(activeMap.id, restoredSession, vehicleId)
      if (viewToken.current !== token) {
        await api.closeAgentSession(activeMap.id, restoredSession).catch(() => undefined)
        return
      }
      const mission = history.snapshot.request
      const demand = mission.vehicles[vehicleId]
      if (demand) {
        setOrigin([demand.fromLon, demand.fromLat])
        setDestination([demand.toLon, demand.toLat])
        setAlgorithm(demand.algorithm)
      }
      setDurationSeconds(mission.durationSeconds)
      setDecisionIntervalSeconds(mission.rerouteIntervalSeconds)
      setRecoveryIntervalSeconds(mission.rerouteRecoveryIntervalSeconds ?? 0)
      setSession(restoredSession)
      setState(restored.state)
      setObservation(vehicle)
      setDecisionId(restored.decisionId ?? null)
      updateSource(history.source)
      setCandidates([])
      setSelectedCandidate(null)
      setCodeResult(null)
      setNavigationRecords([])
      setLastCheckpoint(null)
      setNavigation({ status: 'paused', phase: `已从历史 T${history.snapshot.tick} 恢复${restored.integrity === 'verified' ? '，内容与地图已校验' : '，旧版内容与地图未校验'}，等待手动继续` })
      setTimeline([])
      setTab('code')
      if (previous) await api.closeAgentSession(activeMap.id, previous).catch(() => undefined)
    } catch (reason) {
      if (restoredSession) await api.closeAgentSession(activeMap.id, restoredSession).catch(() => undefined)
      if (viewToken.current === token) setError(reason instanceof Error ? reason.message : '导航历史恢复失败')
    } finally { if (viewToken.current === token) setBusy(null) }
  }

  const attachBackgroundSession = async (sessionId: string, savedSource?: string) => {
    if (!activeMap || busy || navigationRun.current) return
    const token = viewToken.current
    const previousSession = session
    const current = await api.observeAgentSession(activeMap.id, sessionId)
    const vehicleId = current.agents[0]?.vehicleId ?? 0
    const vehicle = await api.observeAgentVehicle(activeMap.id, sessionId, vehicleId)
    if (viewToken.current !== token) return
    setSession(sessionId)
    setState(current)
    setObservation(vehicle)
    setDecisionId(current.decisionId ?? null)
    if (savedSource !== undefined) {
      updateSource(savedSource)
      setCandidates([])
      setSelectedCandidate(null)
      setNavigation({ status: 'paused', phase: '已连接后台导航，进度由后台任务更新' })
      setTab('code')
    }
    if (previousSession !== sessionId) {
      setCodeResult(null)
      setCandidates([])
      setSelectedCandidate(null)
      setSearchTrace(null)
      setNavigationRecords([])
      setLastCheckpoint(null)
      setTimeline([])
    }
  }

  const planWithAlgorithms = async (algorithms: RouteAlgorithm[]) => {
    if (!activeMap || !session) return
    setBusy('plan')
    setError('')
    try {
      const planned = await Promise.all(algorithms.map((item) => (
        api.planAgentRoute(activeMap.id, session, agentVehicleId, item, {
          kPaths: item === 'kshortest' ? 3 : 1,
          recordTrace: true,
        })
      )))
      // k-shortest responses flatten their alternatives into the shared
      // candidate list; every alternative is its own committable candidateId.
      const flattened = planned.flatMap((item) => [
        item,
        ...(item.alternatives?.slice(1) ?? []).map((alternative) => ({
          ...item,
          candidateId: alternative.candidateId,
          timeS: alternative.timeS,
          lengthM: alternative.lengthM,
          expandedNodes: alternative.expandedNodes,
          edges: alternative.edges,
          alternatives: undefined,
        })),
      ])
      setCandidates(flattened)
      setSearchTrace(planned.find((item) => item.searchTrace)?.searchTrace ?? null)
      const best = flattened
        .filter((item) => item.ok && item.timeS !== undefined)
        .sort((left, right) => (left.timeS ?? Infinity) - (right.timeS ?? Infinity))[0]
      setSelectedCandidate(best?.candidateId ?? null)
      appendTimeline(
        'tool',
        algorithms.length > 1 ? 'Tool comparison' : `Tool · ${algorithmLabels[algorithms[0]]}`,
        best
          ? `${flattened.length} candidates · best ${algorithmLabels[best.algorithm]} / ${formatTime(best.timeS ?? 0)}`
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
      appendTimeline('environment', 'Snapshot restored', `${snapshot.snapshotId} → ${restored.state.sessionId} · ${restored.integrity === 'verified' ? '内容与地图版本已校验' : '旧版快照：无法校验原始内容与地图版本'}`, restored.state)
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
      setNavigation({ status: 'idle', phase: '' })
      setLastCheckpoint(null)
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

  const tracePlayback = useTracePlayback(searchTrace)

  return {
    // mission setup
    origin, destination, algorithm, durationSeconds, decisionIntervalSeconds,
    recoveryIntervalSeconds, setRecoveryIntervalSeconds,
    setAlgorithm, setDurationSeconds, setDecisionIntervalSeconds,
    handleRoutePoint, resetOD, previewRoute,
    registry, selectableAlgorithms,
    // live session
    session, state, observation, decisionId, agentVehicleId,
    candidates, selectedCandidate, searchTrace, tracePlayback,
    selectCandidate: setSelectedCandidate,
    snapshots, timeline, tab, setTab,
    busy: busy ?? (state?.navigationJobId ? 'background' as const : null), error,
    source, updateSource, codeResult, planWithSource, stopCode,
    navigation, navigationRecords, startNavigation, pauseNavigation, exportNavigationRecords,
    historyMapId: activeMap?.id, historyVersion, lastCheckpoint, restoreNavigation, attachBackgroundSession,
    clearNavigationRecords: () => { if (!navigationRun.current) setNavigationRecords([]) },
    codeRunning: busy === 'plan' && codeRequest.current !== null,
    dismissError: () => setError(''),
    // flows
    startSession, stepSession, planWithAlgorithms, submitAction,
    createSnapshot, restoreSnapshot, deleteSnapshot, closeSession,
  }
}

export type AgentSessionApi = ReturnType<typeof useAgentSession>
