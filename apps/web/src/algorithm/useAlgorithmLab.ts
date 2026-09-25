import { useCallback, useEffect, useRef, useState } from 'react'
import type { RouteGeoJSON, SearchTrace } from '../types'
import { useTracePlayback } from '../hooks/useTracePlayback'

export interface Capabilities {
  version: string
  language: string
  template: string
  methods: { signature: string; description: string }[]
  syntax: string
  limits: { seconds: number; memoryMiB: number; steps: number; maxSteps: number; trace: number; sourceBytes: number }
}
export interface LabResult {
  keepCurrentRoute?: boolean
  observation?: CodeObservation
  executionMode?: 'run' | 'debug' | 'vehicle'
  runId?: string
  storageError?: string
  ok: boolean
  phase: 'execution' | 'validation' | 'verified' | 'unreachable' | 'kept'
  error?: string
  line?: number
  logs: string[] | null
  steps: number
  expandedNodes: number
  computeMs: number
  timeS: number
  lengthM: number
  baseline: { ok: boolean; timeS: number; lengthM: number }
  geojson?: RouteGeoJSON
  searchTrace?: SearchTrace
  codeRevision: string
  snapshotId: string
  mapId: string
}
export interface CodeObservation {
  mode: 'static' | 'vehicle'
  tick?: number; stateVersion?: number; simulationTimeS?: number; decisionReason?: string
  vehicle?: {
    vehicleId: number; state: string; routeId: number
    position: { edgeId: number; offsetM: number } | null
    destinationEdgeId: number; destinationOffsetM: number; remainingEtaS: number
    routeInvalidated: boolean; held: boolean; remainingEdgeIds: number[]; remainingEdgesTruncated: boolean
  }
}
export interface ExperimentSummary {
  executionMode?: 'run' | 'debug'
  id: string; createdAt: string; ok: boolean; phase: LabResult['phase']; codeRevision: string; snapshotId: string
  timeS: number; lengthM: number; computeMs: number; expandedNodes: number
}
interface Experiment {
  id: string; createdAt: string; result: LabResult
  request: { fromLon: number; fromLat: number; toLon: number; toLat: number; source: string; closedEdges: number[] | null; steps: number }
}
export interface DebugEvent {
  sessionId: string; kind: 'paused' | 'finished'; line?: number; sequence?: number
  variables?: Record<string, string>; frames?: string[]; steps?: number; expandedNodes?: number; logs?: string[]
  result?: LabResult
}
const draftKey = 'zeus.algorithm.draft.v1'
type Point = [number, number]
export function useAlgorithmLab(mapId: string | undefined) {
  const [opened, setOpened] = useState(false)
  const [capabilities, setCapabilities] = useState<Capabilities | null>(null)
  const [source, setSource] = useState(() => {
    try { return localStorage.getItem(draftKey) ?? '' } catch { return '' }
  })
  const [from, setFrom] = useState<Point | null>(null)
  const [to, setTo] = useState<Point | null>(null)
  const [picking, setPicking] = useState(false)
  const [closedEdges, setClosedEdges] = useState('')
  const [steps, setSteps] = useState(2000000)
  const [busy, setBusy] = useState(false)
  const [message, setMessage] = useState('')
  const [result, setResult] = useState<LabResult | null>(null)
  const [resultInput, setResultInput] = useState('')
  const [experiments, setExperiments] = useState<ExperimentSummary[]>([])
  const [historyMessage, setHistoryMessage] = useState('')
  const [debug, setDebug] = useState<DebugEvent | null>(null)
  const [breakpoints, setBreakpoints] = useState('')
  const debugSession = useRef<{ url: string; input: string } | null>(null)
  const debugInput = useRef<string | null>(null)
  const historyRequest = useRef<AbortController | null>(null)
  const restoreRequest = useRef<AbortController | null>(null)
  const activeMap = useRef(mapId)
  activeMap.current = mapId
  const active = useRef<AbortController | null>(null)
  const generation = useRef(0)
  const signature = JSON.stringify([mapId, from, to, source, closedEdges, steps])
  const current = useRef(signature)
  current.current = signature
  const stale = Boolean(result && resultInput !== signature)
  const trace = !stale ? result?.searchTrace ?? null : null
  const playback = useTracePlayback(trace)
  const refreshHistory = useCallback(async () => {
    if (mapId !== activeMap.current) return
    historyRequest.current?.abort()
    if (!mapId) return
    const controller = new AbortController()
    historyRequest.current = controller
    try {
      const response = await fetch(`/api/maps/${encodeURIComponent(mapId)}/algorithms/experiments`, { signal: controller.signal })
      if (!response.ok) throw new Error('无法加载实验历史')
      const records = await response.json() as ExperimentSummary[]
      if (!controller.signal.aborted && mapId === activeMap.current) { setExperiments(records); setHistoryMessage('') }
    } catch (error) { if (!controller.signal.aborted) setHistoryMessage(error instanceof Error ? error.message : '无法加载实验历史') }
  }, [mapId])
  useEffect(() => {
    setExperiments([]); setHistoryMessage('')
    void refreshHistory()
    return () => { historyRequest.current?.abort(); restoreRequest.current?.abort() }
  }, [refreshHistory])

  const releaseDebugger = useCallback(() => {
    const session = debugSession.current
    debugSession.current = null; debugInput.current = null
    if (session) void fetch(session.url, { method: 'DELETE', keepalive: true }).catch(() => { /* server idle deadline remains enforced */ })
  }, [])
  const stop = useCallback(() => {
    generation.current++
    active.current?.abort()
    active.current = null
    setBusy(false)
    releaseDebugger(); setDebug(null)
  }, [releaseDebugger])
  useEffect(() => {
    if (debugInput.current && debugInput.current !== signature) { stop(); setMessage('实验输入已修改，调试已停止；重新开始调试以使用新代码和条件。') }
  }, [signature, stop])
  useEffect(() => {
    if (debug?.kind !== 'paused' || busy) return
    const timer = window.setTimeout(() => { stop(); setMessage('暂停等待超过 60 秒，调试会话已结束。') }, 60000)
    return () => window.clearTimeout(timer)
  }, [debug, busy, stop])
  useEffect(() => {
    const controller = new AbortController()
    fetch('/api/algorithms/capabilities', { signal: controller.signal })
      .then(async (r) => { if (!r.ok) throw new Error('无法加载算法方法说明'); return r.json() as Promise<Capabilities> })
      .then((value) => { setCapabilities(value); setSource((code) => code || value.template) })
      .catch((error: Error) => { if (error.name !== 'AbortError') setMessage(error.message) })
    return () => { controller.abort(); active.current?.abort(); generation.current++; releaseDebugger() }
  }, [releaseDebugger])
  useEffect(() => {
    const timer = window.setTimeout(() => { try { localStorage.setItem(draftKey, source) } catch { /* draft is still in memory */ } }, 250)
    return () => window.clearTimeout(timer)
  }, [source])
  useEffect(() => {
    stop(); setResult(null); setFrom(null); setTo(null); setClosedEdges(''); setMessage(''); setPicking(true)
  }, [mapId, stop])

  const run = async () => {
    if (!mapId || !from || !to || busy || debugSession.current) return
    restoreRequest.current?.abort()
    const edges = closedEdges.trim() ? closedEdges.split(',').map((value) => value.trim() ? Number(value.trim()) : NaN) : []
    if (edges.some((value) => !Number.isSafeInteger(value) || value < 0) || edges.length > 256) {
      setMessage('封闭道路请输入非负整数索引，以逗号分隔，最多 256 条。'); return
    }
    stop()
    const id = generation.current
    const input = signature
    const controller = new AbortController()
    active.current = controller
    setBusy(true); setResult(null); setMessage('正在创建快照、执行算法并校验路线…')
    try {
      const response = await fetch(`/api/maps/${encodeURIComponent(mapId)}/algorithms/run`, {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, signal: controller.signal,
        body: JSON.stringify({ fromLon: from[0], fromLat: from[1], toLon: to[0], toLat: to[1], source, closedEdges: edges, steps }),
      })
      const value = await response.json() as LabResult & { error?: string }
      if (!response.ok) throw new Error(value.error || '算法验证请求失败')
      if (id !== generation.current) return
      setResult(value); setResultInput(input)
      setMessage([input === current.current ? '' : '运行期间输入已修改，结果属于上一版本。', value.storageError ?? ''].filter(Boolean).join(' '))
      void refreshHistory()
    } catch (error) {
      if (id === generation.current) setMessage(error instanceof Error ? error.message : '运行失败')
    } finally {
      if (id === generation.current) { setBusy(false); active.current = null }
    }
  }
  const debugAction = async (action: 'start' | 'step' | 'continue') => {
    if (!mapId || !from || !to || busy || (action !== 'start' && !debugSession.current)) return
    const points = breakpoints.trim() ? breakpoints.split(',').map(value => value.trim() ? Number(value.trim()) : NaN) : []
    if (points.length > 64 || points.some(line => !Number.isSafeInteger(line) || line < 1 || line > source.split('\n').length)) {
      setMessage('断点请输入代码范围内的行号，以逗号分隔，最多 64 个。'); return
    }
    const edges = closedEdges.trim() ? closedEdges.split(',').map(value => value.trim() ? Number(value.trim()) : NaN) : []
    if (edges.length > 256 || edges.some(edge => !Number.isSafeInteger(edge) || edge < 0)) { setMessage('封闭道路请输入非负整数索引，以逗号分隔，最多 256 条。'); return }
    restoreRequest.current?.abort()
    if (action === 'start') { stop(); setResult(null); debugInput.current = signature }
    const id = generation.current, input = debugInput.current ?? signature
    const base = `/api/maps/${encodeURIComponent(mapId)}/algorithms/debug`
    const controller = new AbortController()
    active.current = controller
    setBusy(true); setMessage(action === 'start' ? '正在创建调试会话…' : '正在执行到下一处暂停位置…')
    try {
      const response = await fetch(action === 'start' ? base : debugSession.current!.url, {
        method: 'POST', signal: controller.signal, headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(action === 'start' ? { fromLon: from[0], fromLat: from[1], toLon: to[0], toLat: to[1], source, closedEdges: edges, steps } : { action, sequence: debug?.sequence, breakpoints: points }),
      })
      const event = await response.json() as DebugEvent & { error?: string }
      if (!response.ok) throw new Error(event.error ?? '调试请求失败')
      if (id !== generation.current) {
        if (action === 'start' && event.kind === 'paused') void fetch(`${base}/${encodeURIComponent(event.sessionId)}`, { method: 'DELETE', keepalive: true }).catch(() => {})
        return
      }
      if (event.kind === 'paused') {
        debugSession.current = { url: `${base}/${encodeURIComponent(event.sessionId)}`, input }
        setDebug(event); setMessage('进程已暂停。单步执行下一条语句，或继续运行到断点。60 秒无操作将自动结束。')
      } else if (event.result) {
        debugSession.current = null; debugInput.current = null; setDebug(null)
        setResult(event.result); setResultInput(input); setMessage(event.result.storageError ?? '调试已完成，结果经过同一套路线校验。'); void refreshHistory()
      } else { throw new Error('调试器没有返回有效结果') }
    } catch (error) {
      if (id === generation.current) { releaseDebugger(); setDebug(null); setMessage(error instanceof Error ? error.message : '调试失败') }
    } finally {
      if (id === generation.current) { setBusy(false); active.current = null }
    }
  }
  const restoreExperiment = async (id: string) => {
    if (!mapId || busy || debugSession.current) return
    restoreRequest.current?.abort()
    const controller = new AbortController()
    restoreRequest.current = controller
    const originalInput = current.current
    try {
      const response = await fetch(`/api/maps/${encodeURIComponent(mapId)}/algorithms/experiments/${encodeURIComponent(id)}`, { signal: controller.signal })
      if (!response.ok) throw new Error('无法恢复实验')
      const saved = await response.json() as Experiment
      if (controller.signal.aborted) return
      if (current.current !== originalInput) { setHistoryMessage('输入已修改，已取消恢复历史，保留当前草稿。'); return }
      stop()
      const request = saved.request
      const start: Point = [request.fromLon, request.fromLat], end: Point = [request.toLon, request.toLat]
      const closures = (request.closedEdges ?? []).join(',')
      setSource(request.source); setFrom(start); setTo(end); setClosedEdges(closures); setSteps(request.steps); setPicking(false)
      setResult(saved.result); setResultInput(JSON.stringify([mapId, start, end, request.source, closures, request.steps]))
      setMessage('已恢复保存的代码、实验条件与历史结果；点击运行可以重新验证。')
    } catch (error) { if (!controller.signal.aborted) setHistoryMessage(error instanceof Error ? error.message : '无法恢复实验') }
  }
  const deleteExperiment = async (id: string) => {
    if (!mapId) return
    try {
      const response = await fetch(`/api/maps/${encodeURIComponent(mapId)}/algorithms/experiments/${encodeURIComponent(id)}`, { method: 'DELETE' })
      if (!response.ok) throw new Error('删除实验失败')
      if (mapId === activeMap.current) void refreshHistory()
    } catch (error) { setHistoryMessage(error instanceof Error ? error.message : '删除实验失败') }
  }
  return {
    opened, capabilities, source, setSource, from, to, picking, closedEdges, setClosedEdges,
    steps, setSteps, busy, message, result, stale, trace, playback,
    experiments, historyMessage, refreshHistory, restoreExperiment, deleteExperiment,
    debug, breakpoints, setBreakpoints, debugAction,
    routeData: !stale && result?.ok ? result.geojson ?? null : null,
    run, stop: () => { stop(); setMessage('已停止运行。') },
    open: (start: Point | null, end: Point | null) => {
      setOpened(true)
      if (!from && !to) { setFrom(start); setTo(end); setPicking(!start || !end) }
    },
    close: () => { stop(); restoreRequest.current?.abort(); playback.stop(); setOpened(false) },
    setPoints: (start: Point, end: Point) => {
      stop(); setFrom(start); setTo(end); setPicking(false); setMessage('')
    },
    pick: (lon: number, lat: number) => {
      if (!from || to) { setFrom([lon, lat]); setTo(null) }
      else { setTo([lon, lat]); setPicking(false) }
    },
    repick: () => { stop(); setFrom(null); setTo(null); setPicking(true); setResult(null) },
  }
}
export type AlgorithmLabAPI = ReturnType<typeof useAlgorithmLab>
