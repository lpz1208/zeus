import { LoaderCircle, Pause, RefreshCw, ShieldCheck, Wrench } from 'lucide-react'
import type { AgentSessionApi } from './useAgentSession'
import { algorithmLabels, formatTime } from './agentGeo'

/**
 * The single decision surface of the workbench: floats over the map while a
 * decision barrier is open and the simulation is frozen.
 */
export function AgentDecisionBanner({ agent }: { agent: AgentSessionApi }) {
  if (!agent.decisionId) return null
  const selected = agent.candidates.find((item) => item.candidateId === agent.selectedCandidate)
  const reason = agent.state?.decisionReason || 'periodic'
  return (
    <div className={`agent-decision-banner ${reason === 'route_invalidated' ? 'is-alert' : ''}`}>
      <div className="agent-decision-banner__head">
        <span className="agent-decision-banner__id">
          <Pause size={12} fill="currentColor" /> DECISION {agent.decisionId.slice(-8)}
        </span>
        <span className="agent-decision-banner__reason">{reason}</span>
        <span className="agent-decision-banner__version">v{agent.state?.stateVersion ?? 0}</span>
      </div>
      <p>
        {selected
          ? `已选 ${algorithmLabels[selected.algorithm]} 候选 · ${formatTime(selected.timeS ?? 0)} · ${((selected.lengthM ?? 0) / 1000).toFixed(2)} km`
          : '仿真已冻结 · 比较工具后提交候选，或保持当前路线'}
      </p>
      <div className="agent-decision-banner__actions">
        <button type="button" disabled={Boolean(agent.busy)} onClick={() => void agent.planWithAlgorithms(agent.selectableAlgorithms)}>
          {agent.busy === 'plan' ? <LoaderCircle className="spin" size={13} /> : <Wrench size={13} />}
          比较全部工具
        </button>
        <button type="button" disabled={Boolean(agent.busy)} onClick={() => void agent.submitAction('keep_route')}>
          <RefreshCw size={13} /> 保持路线
        </button>
        <button
          type="button"
          className="is-primary"
          disabled={!agent.selectedCandidate || Boolean(agent.busy)}
          onClick={() => void agent.submitAction('commit_route')}
        >
          {agent.busy === 'action' ? <LoaderCircle className="spin" size={13} /> : <ShieldCheck size={13} />}
          提交所选候选
        </button>
      </div>
    </div>
  )
}
