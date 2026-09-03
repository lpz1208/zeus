import { useMemo, useState } from 'react'
import { ArrowLeft, BrainCircuit, ChevronRight, MapPin } from 'lucide-react'
import { MapCanvas } from '../MapCanvas'
import type {
  IssueGeoJSON,
  MapRecord,
  NodeGeoJSON,
  ReferenceLayerView,
  RoadGeoJSON,
  RouteGeoJSON,
  VehicleFrameGeoJSON,
} from '../types'
import { emptyVehicleFrame } from '../types'
import { buildRoadEdgeIndex, positionOnRoad, routeForEdges } from './agentGeo'
import { useAgentSession } from './useAgentSession'
import { AgentMissionPanel } from './AgentMissionPanel'
import { AgentInspector } from './AgentInspector'
import { AgentDecisionBanner } from './AgentDecisionBanner'
import './agent.css'

interface AgentWorkbenchProps {
  maps: MapRecord[]
  activeMap: MapRecord | null
  onMapChange: (map: MapRecord) => void
  data: RoadGeoJSON
  nodeData: NodeGeoJSON
  issueData: IssueGeoJSON
  referenceLayers: ReferenceLayerView[]
  onExit: () => void
}

export function AgentWorkbench({
  maps, activeMap, onMapChange, data, nodeData, issueData, referenceLayers, onExit,
}: AgentWorkbenchProps) {
  const agent = useAgentSession(activeMap)
  const [pointer, setPointer] = useState<[number, number]>([116.391, 39.907])

  // Directed-edge geometry index is built once per map; the live agent marker
  // interpolates (edgeId, offsetM) onto the road polyline each observation.
  const roadIndex = useMemo(() => buildRoadEdgeIndex(data), [data])
  const agentFrame = useMemo<VehicleFrameGeoJSON>(() => {
    const o = agent.observation
    if (!o || o.state !== 'driving') return emptyVehicleFrame
    const point = positionOnRoad(roadIndex, o.position.edgeId, o.position.offsetM)
    if (!point) return emptyVehicleFrame
    return {
      type: 'FeatureCollection',
      features: [{
        type: 'Feature',
        properties: { VEHICLE_ID: o.vehicleId, HELD: false, SPEED_FACTOR: 1 },
        geometry: { type: 'Point', coordinates: point },
      }],
    }
  }, [agent.observation, roadIndex])

  const selectedRoute = useMemo<RouteGeoJSON | null>(() => {
    const candidate = agent.candidates.find((item) => item.candidateId === agent.selectedCandidate)
    if (candidate?.edges) return routeForEdges(data, candidate.edges)
    if (agent.session) return routeForEdges(data, agent.observation?.remainingEdgeIds)
    return agent.previewRoute
  }, [agent.candidates, agent.selectedCandidate, agent.session, agent.observation, agent.previewRoute, data])

  return (
    <main className="agent-shell">
      <header className="agent-topbar">
        <button className="agent-back" type="button" onClick={onExit}>
          <ArrowLeft size={15} /> 地图工作台
        </button>
        <div className="agent-brand">
          <span><BrainCircuit size={18} /></span>
          <div><strong>ZEUS</strong><small>NAVIGATION AGENT</small></div>
        </div>
        <div className="agent-loop" aria-label="Agent 决策闭环">
          {['OBSERVE', 'PLAN', 'GUARD', 'ACT'].map((item, index) => (
            <span key={item} className={
              (item === 'OBSERVE' && agent.decisionId && agent.candidates.length === 0)
              || (item === 'PLAN' && agent.candidates.length > 0)
              || (item === 'GUARD' && agent.busy === 'action')
                ? 'is-active' : ''
            }>{item}{index < 3 && <ChevronRight size={11} />}</span>
          ))}
        </div>
        <div className="agent-runtime-status">
          <i className={agent.session ? 'is-live' : ''} />
          <span>{agent.session ? 'SESSION ACTIVE' : 'ENVIRONMENT READY'}</span>
          <strong>{agent.state ? `T${agent.state.tick}` : 'T—'}</strong>
        </div>
      </header>

      <AgentMissionPanel agent={agent} maps={maps} activeMap={activeMap} onMapChange={onMapChange} />

      <div className="agent-map-wrap">
        <MapCanvas
          data={data}
          nodeData={nodeData}
          issueData={issueData}
          referenceLayers={referenceLayers}
          referenceFocus={null}
          mapName={activeMap?.name ?? 'NO MAP SELECTED'}
          isDemo={!activeMap}
          selectedMatch={null}
          selectedRoad={null}
          selectedReferenceFeature={null}
          focusedIssueIndex={null}
          routeMode={!agent.session}
          routeStart={agent.origin}
          routeEnd={agent.destination}
          routeData={selectedRoute}
          trajectoryData={null}
          vehicleFrame={agentFrame}
          junctionPickMode={false}
          selectedControlNodeId={null}
          onQuery={async () => null}
          onIssueSelect={() => undefined}
          onRoadSelect={() => undefined}
          onJunctionSelect={() => undefined}
          onReferenceFeatureSelect={() => undefined}
          onPointerMove={(longitude, latitude) => setPointer([longitude, latitude])}
          onRoutePoint={agent.handleRoutePoint}
        />
        <div className="agent-map-readout">
          <span><MapPin size={11} /> {pointer[0].toFixed(5)}, {pointer[1].toFixed(5)}</span>
          <strong>{agent.session ? 'LIVE ENVIRONMENT' : 'MISSION SETUP'}</strong>
        </div>
        <AgentDecisionBanner agent={agent} />
      </div>

      <AgentInspector agent={agent} />

      <footer className="agent-statusbar">
        <span><i /> C++ ENVIRONMENT</span>
        <span>MODEL <strong>OPERATOR / RULE BASELINE</strong></span>
        <span>MODE <strong>BARRIER</strong></span>
        <span className="agent-statusbar__right">STATE VERSION GUARD ENABLED</span>
      </footer>
    </main>
  )
}
