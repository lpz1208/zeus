import {
  BrainCircuit,
  Check,
  ChevronRight,
  Clock3,
  Gauge,
  MapPin,
  Route,
  ShieldCheck,
  Waypoints,
  Wrench,
  Zap,
} from 'lucide-react'
import type { AgentSessionApi } from './useAgentSession'
import { algorithmLabels, formatTime } from './agentGeo'

/** Right column: 观察 / 工具 / 轨迹 three-tab inspector. */
export function AgentInspector({ agent }: { agent: AgentSessionApi }) {
  const { observation, registry, candidates, timeline, tab, setTab } = agent
  return (
    <aside className="agent-inspector">
      <div className="agent-inspector-head">
        <div><span>AGENT STATE</span><h2>决策控制台</h2></div>
        <ShieldCheck size={20} />
      </div>
      <div className="agent-tabs" role="tablist">
        <button type="button" className={tab === 'observation' ? 'is-active' : ''} onClick={() => setTab('observation')}>观察</button>
        <button type="button" className={tab === 'tools' ? 'is-active' : ''} onClick={() => setTab('tools')}>工具 <span>{candidates.length}</span></button>
        <button type="button" className={tab === 'trace' ? 'is-active' : ''} onClick={() => setTab('trace')}>轨迹 <span>{timeline.length}</span></button>
      </div>

      <div className="agent-inspector-scroll">
        {tab === 'observation' && (observation ? (
          <>
            <section className="agent-observation-hero">
              <div><span>VEHICLE {observation.vehicleId}</span><strong>{observation.state.toUpperCase()}</strong></div>
              <i className={observation.routeInvalidated ? 'is-alert' : ''}>{observation.routeInvalidated ? 'ROUTE INVALID' : 'ROUTE VALID'}</i>
            </section>
            <section className="agent-observation-grid">
              <div><MapPin size={13} /><span>位置</span><strong>EDGE {observation.position.edgeId}</strong><small>OFFSET {observation.position.offsetM.toFixed(1)} M</small></div>
              <div><Clock3 size={13} /><span>剩余时间</span><strong>{formatTime(observation.remainingEtaS)}</strong><small>ESTIMATED ETA</small></div>
              <div><Route size={13} /><span>剩余路线</span><strong>{observation.remainingEdgeIds.length}</strong><small>DIRECTED EDGES</small></div>
              <div><Gauge size={13} /><span>附近道路</span><strong>{observation.nearbyRoads.length}</strong><small>WITHIN 2 KM</small></div>
            </section>
            <section className="agent-observation-section">
              <div className="agent-section-label"><Zap size={13} /><span>环境事件</span><small>{observation.activeEvents.length}</small></div>
              {observation.activeEvents.length === 0
                ? <div className="agent-empty-row"><Check size={13} /> 当前感知范围无动态事件</div>
                : observation.activeEvents.map((event) => (
                    <div className="agent-event-row" key={event.eventId}>
                      <i /><span><strong>{event.type}</strong><small>{event.affectedEdgeIds.map((edge) => `E${edge}`).join(' · ')}</small></span>
                    </div>
                  ))}
            </section>
            <section className="agent-observation-section">
              <div className="agent-section-label"><Waypoints size={13} /><span>道路态势</span><small>TOP {Math.min(8, observation.nearbyRoads.length)}</small></div>
              <div className="agent-road-list">
                {observation.nearbyRoads.slice(0, 8).map((road) => (
                  <div key={road.edgeId} className={road.closed ? 'is-closed' : ''}>
                    <span>E{road.edgeId}</span>
                    <i><em style={{ width: `${Math.min(100, road.occupancyRatio * 100)}%` }} /></i>
                    <strong>{road.closed ? 'CLOSED' : `${Math.round(road.speedMps * 3.6)} km/h`}</strong>
                  </div>
                ))}
              </div>
            </section>
          </>
        ) : (
          <div className="agent-empty"><BrainCircuit size={24} /><strong>等待 Observation</strong><span>创建任务并推进到决策事件后，环境状态会出现在这里。</span></div>
        ))}

        {tab === 'tools' && (
          <>
            <section className="agent-tool-registry">
              <div className="agent-section-label"><Wrench size={13} /><span>Tool Registry</span><small>{registry?.registryVersion ?? 'OFFLINE'}</small></div>
              {(registry?.algorithms ?? []).map((tool) => (
                <button
                  type="button"
                  className={`agent-tool ${agent.algorithm === tool.algorithmId ? 'is-selected' : ''}`}
                  key={tool.algorithmId}
                  disabled={!agent.session || !agent.decisionId || Boolean(agent.busy)}
                  onClick={() => { agent.setAlgorithm(tool.algorithmId); void agent.planWithAlgorithms([tool.algorithmId]) }}
                >
                  <span><strong>{algorithmLabels[tool.algorithmId]}</strong><small>v{tool.algorithmVersion} · {tool.searchDirection}</small></span>
                  <div><i>DYNAMIC</i>{tool.usesHeuristic && <i>HEURISTIC</i>}{tool.exact && <i>EXACT</i>}</div>
                  <ChevronRight size={13} />
                </button>
              ))}
            </section>

            {candidates.length > 0 && (
              <section className="agent-candidates">
                <div className="agent-section-label"><Route size={13} /><span>候选路线</span><small>{candidates.filter((item) => item.ok).length} VALID</small></div>
                {candidates.map((candidate) => (
                  <button
                    type="button"
                    className={`agent-candidate ${agent.selectedCandidate === candidate.candidateId ? 'is-selected' : ''}`}
                    key={candidate.candidateId}
                    disabled={!candidate.ok}
                    onClick={() => agent.selectCandidate(candidate.candidateId)}
                  >
                    <span className="agent-candidate__check">{agent.selectedCandidate === candidate.candidateId ? <Check size={11} /> : null}</span>
                    <span><strong>{algorithmLabels[candidate.algorithm]}</strong><small>{candidate.candidateId}</small></span>
                    {candidate.ok
                      ? <><b>{formatTime(candidate.timeS ?? 0)}</b><em>{((candidate.lengthM ?? 0) / 1000).toFixed(2)} km</em></>
                      : <em>{candidate.reason}</em>}
                  </button>
                ))}
                <p className="agent-candidates__hint">通过地图上的决策横幅提交所选候选。</p>
              </section>
            )}
          </>
        )}

        {tab === 'trace' && (timeline.length > 0 ? (
          <section className="agent-timeline">
            {timeline.map((event) => (
              <article key={event.id} className={`is-${event.kind}`}>
                <div><i /><span>{formatTime(event.simulationTime)}</span></div>
                <section><strong>{event.title}</strong><p>{event.detail}</p>{event.stateVersion !== undefined && <small>STATE VERSION {event.stateVersion}</small>}</section>
              </article>
            ))}
          </section>
        ) : (
          <div className="agent-empty"><Clock3 size={24} /><strong>尚无决策轨迹</strong><span>Observation、工具调用、Guard 和动作会按仿真时间记录。</span></div>
        ))}
      </div>
    </aside>
  )
}
