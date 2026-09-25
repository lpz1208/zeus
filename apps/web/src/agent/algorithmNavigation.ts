import type { AgentRouteCandidate, AgentSessionObservation, AgentStepResponse } from '../types'
import type { CodeObservation, LabResult } from '../algorithm/useAlgorithmLab'

export interface NavigationRecord {
  id: string
  decisionId: string
  sessionId?: string
  mapId?: string
  source: string
  stateVersion: number
  simulationTimeS: number
  reason: string
  observation?: CodeObservation
  codeRevision?: string
  snapshotId?: string
  action?: 'keep_route' | 'commit_route'
  status: 'planning' | 'planned' | 'cancelled' | 'queued' | 'applied' | 'kept' | 'error'
  edges?: number[]
  timeS?: number
  computeMs?: number
  logs?: string[] | null
  error?: string
  line?: number
  afterVersion?: number
}
export interface VehiclePlan { result: LabResult; candidate?: AgentRouteCandidate }
export const navigationRecordLabels: Record<NavigationRecord['status'], string> = {
  planning: '运行中', planned: '已计算', cancelled: '已取消提交', queued: '已入队，尚未推进',
  applied: '车辆已换路', kept: '已保持路线', error: '失败，已暂停',
}
export interface NavigationOptions {
  runId: string
  source: string
  vehicleId: number
  once: boolean
  maxDecisions: number
  active: () => boolean
  request: <T>(path: string, body?: unknown) => Promise<T>
  onObservation: (value: AgentSessionObservation) => Promise<void>
  onPlan: (value: VehiclePlan) => void
  onRecord: (value: NavigationRecord) => void
  onPhase: (value: string) => void
  yieldBetween: () => Promise<void>
  checkpoint?: () => Promise<void>
}

// Cooperative cancellation: never abort a resident C++ worker request.
// Every awaited boundary is checked before the next mutation is submitted.
export async function runAlgorithmNavigation(options: NavigationOptions): Promise<'paused' | 'complete' | 'once' | 'limit'> {
  const { request, active, vehicleId, source } = options
  let observation = await request<AgentSessionObservation>('')
  await options.onObservation(observation)
  if (!active()) return 'paused'
  if (!observation.paused && !observation.finished) throw new Error('请先暂停车辆会话')
  const vehicle = () => observation.agents.find(item => item.vehicleId === vehicleId)
  const completed = () => observation.finished || vehicle()?.state === 'arrived'
  if (completed()) return 'complete'
  if (!vehicle() || vehicle()?.state === 'unroutable') throw new Error('车辆未获得可行的初始路线')
  if (options.maxDecisions <= 0) return 'limit'
  let checkpointFailed = false
  const checkpoint = async () => {
    try { await options.checkpoint?.() } catch (error) { checkpointFailed = true; throw error }
  }
  // Persist a recoverable boundary before the first action. A failed save
  // must never be followed by another vehicle mutation.
  await checkpoint()
  if (!active()) return 'paused'
  // First decision runs before departure, or immediately at the resumed
  // paused boundary. Later calls are driven solely by simulation events.
  if (!observation.decisionId) {
    const opened = await request<AgentStepResponse>('/decisions', { vehicleId, basedOnStateVersion: observation.stateVersion })
    observation = { ...observation, decisionId: opened.decisionId }
    await options.onObservation(observation)
  }
  let count = 0
  let record: NavigationRecord | null = null
  const emit = () => { if (record) options.onRecord({ ...record }) }
  const observe = async () => {
    observation = await request<AgentSessionObservation>('')
    await options.onObservation(observation)
  }
  const step = async (ticks: number, untilEvent: boolean) => {
    const result = await request<AgentStepResponse>('/step', {
      ...(untilEvent ? { untilEvent: true, maxTicks: ticks } : { ticks }),
      basedOnStateVersion: observation.stateVersion,
    })
    await observe()
    if (result.state.stateVersion !== observation.stateVersion) throw new Error('车辆被其他操作推进，自动导航已暂停')
  }
  try {
    while (active()) {
      if (completed()) return 'complete'
      if (!observation.paused) throw new Error('车辆会话已不在暂停边界')
      if (observation.decisionId) {
        if (count >= options.maxDecisions) return 'limit'
        record = {
          id: `${options.runId}:${count}`, decisionId: observation.decisionId, source, stateVersion: observation.stateVersion,
          simulationTimeS: observation.simulationTimeS,
          reason: observation.decisionReason || (observation.tick === 0 ? 'initial' : 'manual'), status: 'planning',
        }
        emit()
        options.onPhase('运行代码')
        const planned = await request<VehiclePlan>('/algorithms/plan', { vehicleId, basedOnStateVersion: observation.stateVersion, source, steps: 5000000 })
        const result = planned.result
        if (!result) throw new Error('缺少算法执行结果')
        record = { ...record, observation: result.observation, codeRevision: result.codeRevision, snapshotId: result.snapshotId,
          logs: result.logs, line: result.line, computeMs: result.computeMs, edges: planned.candidate?.edges, timeS: planned.candidate?.timeS, status: 'planned' }
        options.onPlan(planned)
        emit()
        if (!active()) { record.status = 'cancelled'; emit(); return 'paused' }
        if (!result.ok || (result.phase !== 'verified' && result.phase !== 'kept')) {
          throw new Error(result.error || (result.phase === 'unreachable' ? '当前无路可达，自动导航已暂停' : '算法验证未通过'))
        }
        if (result.observation?.stateVersion !== observation.stateVersion || result.observation.vehicle?.vehicleId !== vehicleId) {
          throw new Error('算法观察与当前车辆版本不一致')
        }
        if (result.phase === 'verified' && (!planned.candidate?.ok || planned.candidate.basedOnStateVersion !== observation.stateVersion)) {
          throw new Error('缺少当前版本的合法候选')
        }
        record.action = result.phase === 'kept' ? 'keep_route' : 'commit_route'
        options.onPhase(record.action === 'keep_route' ? '保持路线' : '提交路径')
        const acknowledged = await request<{ accepted: boolean; reason: string }>('/actions', {
          decisionId: observation.decisionId, agentId: 'default', vehicleId, kind: record.action,
          candidateId: planned.candidate?.candidateId, basedOnStateVersion: observation.stateVersion, reasonCode: 'user_code_navigation',
        })
        if (!acknowledged.accepted) throw new Error(acknowledged.reason || '车辆拒绝执行动作')
        record.status = 'queued'
        emit()
        observation = { ...observation, decisionId: '' }
        await options.onObservation(observation)
        if (!active()) return 'paused'
        // Apply one tick before declaring success; a newly scheduled closure
        // may invalidate a path after the commit was accepted.
        const oldRoute = vehicle()?.routeId
        await step(1, false)
        if (!vehicle()) throw new Error('无法读取车辆执行后的状态')
        if (record.action === 'commit_route' && vehicle()?.routeId === oldRoute) {
          throw new Error('提交后路径未被采用，可能遇到新的道路控制；已保留原路线并暂停')
        }
        record.status = record.action === 'keep_route' ? 'kept' : 'applied'
        record.afterVersion = observation.stateVersion
        emit()
        record = null
        ++count
        await checkpoint()
        if (options.once) return 'once'
        if (count >= options.maxDecisions) return 'limit'
      } else {
        options.onPhase('推进至事件')
        await step(16, true)
      }
      if (active()) await options.yieldBetween()
    }
    return 'paused'
  } catch (error) {
    if (record) {
      record.status = 'error'
      record.error = error instanceof Error ? error.message : String(error)
      emit()
    }
    throw error
  } finally {
    if (!checkpointFailed) await checkpoint()
  }
}
