import {
  Bot,
  ChevronRight,
  GitBranch,
  LoaderCircle,
  MapPin,
  Pause,
  Play,
  Save,
  Sparkles,
  Square,
  TimerReset,
  Trash2,
  X,
  Zap,
} from 'lucide-react'
import type { MapRecord } from '../types'
import type { AgentSessionApi } from './useAgentSession'
import { algorithmLabels, formatCoordinate, formatTime } from './agentGeo'

interface AgentMissionPanelProps {
  agent: AgentSessionApi
  maps: MapRecord[]
  activeMap: MapRecord | null
  onMapChange: (map: MapRecord) => void
}

/** Left column: mission setup before the session, environment control during. */
export function AgentMissionPanel({
  agent, maps, activeMap, onMapChange,
}: AgentMissionPanelProps) {
  const { session, state, observation, decisionId, busy } = agent
  return (
    <aside className="agent-mission">
      <section className="agent-panel-heading">
        <div><span>MISSION</span><h1>导航任务</h1></div>
        <Bot size={20} />
      </section>

      <label className="agent-field">
        <span>运行地图</span>
        <select
          value={activeMap?.id ?? ''}
          disabled={Boolean(session)}
          onChange={(event) => {
            const selected = maps.find((item) => item.id === event.target.value)
            if (selected) onMapChange(selected)
          }}
        >
          <option value="" disabled>选择地图</option>
          {maps.map((map) => <option value={map.id} key={map.id}>{map.name}</option>)}
        </select>
      </label>

      <div className="agent-od-card">
        <div className="agent-od-line" />
        <div><i className="is-origin">A</i><span>起点</span><strong>{formatCoordinate(agent.origin)}</strong></div>
        <div><i className="is-destination">B</i><span>终点</span><strong>{formatCoordinate(agent.destination)}</strong></div>
        {!session && (agent.origin || agent.destination) && (
          <button type="button" onClick={agent.resetOD}><X size={12} /> 重设</button>
        )}
      </div>

      {!session ? (
        <>
          <p className="agent-map-hint"><MapPin size={13} /> 在地图上依次点击起点与终点</p>
          <label className="agent-field">
            <span>初始规划工具</span>
            <select value={agent.algorithm} onChange={(event) => agent.setAlgorithm(event.target.value as typeof agent.algorithm)}>
              {agent.selectableAlgorithms.map((item) => <option value={item} key={item}>{algorithmLabels[item]}</option>)}
            </select>
          </label>
          <div className="agent-config-grid">
            <label><span>任务时域<small>SIM SEC</small></span><input type="number" min="60" max="28800" value={agent.durationSeconds} onChange={(event) => agent.setDurationSeconds(Number(event.target.value))} /></label>
            <label><span>决策周期<small>SIM SEC</small></span><input type="number" min="1" max="3600" value={agent.decisionIntervalSeconds} onChange={(event) => agent.setDecisionIntervalSeconds(Number(event.target.value))} /></label>
          </div>
          <button
            className="agent-primary"
            type="button"
            disabled={!activeMap || !agent.origin || !agent.destination || Boolean(busy)}
            onClick={() => void agent.startSession()}
          >
            {busy === 'create' ? <LoaderCircle className="spin" size={16} /> : <Sparkles size={16} />}
            <span>{busy === 'create' ? '正在创建环境' : '启动 Agent Environment'}</span>
            <ChevronRight size={15} />
          </button>
        </>
      ) : (
        <>
          <section className="agent-state-card">
            <div className="agent-state-card__head">
              <span><i /> ENVIRONMENT</span>
              <strong>{state?.finished ? 'FINISHED' : decisionId ? 'AWAITING ACTION' : 'PAUSED'}</strong>
            </div>
            <div className="agent-state-metrics">
              <div><span>仿真时间</span><strong>{formatTime(state?.simulationTimeS ?? 0)}</strong></div>
              <div><span>状态版本</span><strong>v{state?.stateVersion ?? 0}</strong></div>
              <div><span>当前边</span><strong>{observation?.position.edgeId ?? '—'}</strong></div>
              <div><span>剩余 ETA</span><strong>{observation ? formatTime(observation.remainingEtaS) : '—'}</strong></div>
            </div>
            {decisionId && <div className="agent-barrier"><Pause size={12} /> 仿真已冻结，等待版本 v{state?.stateVersion} 的动作</div>}
          </section>

          <div className="agent-run-controls">
            <button type="button" disabled={Boolean(busy) || Boolean(decisionId) || state?.finished} onClick={() => void agent.stepSession(true)}>
              {busy === 'event' ? <LoaderCircle className="spin" size={14} /> : <Zap size={14} />}
              <span>推进至事件<small>EVENT DRIVEN</small></span>
            </button>
            <button type="button" disabled={Boolean(busy) || Boolean(decisionId) || state?.finished} onClick={() => void agent.stepSession(false)}>
              {busy === 'step' ? <LoaderCircle className="spin" size={14} /> : <Play size={14} />}
              <span>单步推进<small>+1 TICK</small></span>
            </button>
          </div>

          <section className="agent-snapshot-section">
            <div className="agent-section-label"><GitBranch size={13} /><span>实验快照</span><small>{agent.snapshots.length}</small></div>
            <button className="agent-snapshot-create" type="button" disabled={Boolean(busy) || Boolean(decisionId)} onClick={() => void agent.createSnapshot()}>
              <Save size={13} /> 保存当前边界
            </button>
            {agent.snapshots.map((snapshot) => (
              <div className="agent-snapshot-row" key={snapshot.snapshotId}>
                <span><strong>T{snapshot.tick}</strong><small>{snapshot.snapshotId.slice(-8)}</small></span>
                <button type="button" aria-label="恢复快照" onClick={() => void agent.restoreSnapshot(snapshot)}><TimerReset size={12} /></button>
                <button type="button" aria-label="删除快照" onClick={() => void agent.deleteSnapshot(snapshot)}><Trash2 size={12} /></button>
              </div>
            ))}
          </section>

          <button className="agent-close" type="button" disabled={Boolean(busy)} onClick={() => void agent.closeSession()}>
            <Square size={11} fill="currentColor" /> 关闭当前会话
          </button>
        </>
      )}

      {agent.error && (
        <div className="agent-error">
          <X size={13} /><span>{agent.error}</span>
          <button type="button" onClick={agent.dismissError}>关闭</button>
        </div>
      )}
    </aside>
  )
}
