export type Severity = 'info' | 'warning' | 'error' | 'fatal'

export interface InspectResult {
  uploadId: string
  sourceFile: string
  shapefile?: string
  turnRestrictionsFile?: string
  files: string[]
  driver: string
  layer: string
  featureCount: number
  geometry: string
  crs: string
  fields: string[]
  navigationCompatible: boolean
  suggestedUsage: 'road-network' | 'reference-layer' | 'unsupported'
  osmRoadData: boolean
  raw: string
}

export interface OSMPreprocessOptions {
  enabled: boolean
  includeService: boolean
  includeTrack: boolean
  includePrivate: boolean
  minLengthMeters: number
}

export interface OSMCleaningNormalization {
  geometryCollectionsConverted: number
  defaultSpeedApplied: number
  mphSpeedConverted: number
  impliedOnewayApplied: number
  reverseOnewayNormalized: number
  duplicateGeometriesRemoved: number
}

export interface OSMCleaningSummary {
  profile: string
  options: OSMPreprocessOptions
  inputFeatures: number
  outputFeatures: number
  filteredFeatures: number
  normalization: OSMCleaningNormalization
  excludedByReason: Record<string, number>
  outputByClass: Record<string, number>
}

export interface Mapping {
  idField: string
  onewayField: string
  speedField: string
  lanesField: string
  roadClassField: string
  zLevelField: string
  bridgeField: string
  tunnelField: string
  targetCrs: string
  defaultSpeedKph: number
  snapToleranceMeters: number
  defaultBidirectional: boolean
}

export interface ValidationSummary {
  nodes: number
  directedEdges: number
  components: number
  largestComponentNodes: number
  turnTransitions: number
  fatal: number
  errors: number
  warnings: number
  info: number
}

export interface ValidationIssue {
  severity: Severity
  code: string
  source: string
  location: [number, number]
  message: string
}

export interface MapRecord {
  id: string
  name: string
  createdAt: string
  source: string
  summary: ValidationSummary
  issues: ValidationIssue[]
  cleaning?: OSMCleaningSummary
}

export interface ReferenceLayerStyle {
  color: string
  opacity: number
}

export interface ReferenceLayerRecord {
  id: string
  name: string
  createdAt: string
  source: string
  geometry: string
  featureCount: number
  crs: string
  style: ReferenceLayerStyle
}

export type ReferenceGeoJSON = GeoJSON.FeatureCollection<
  GeoJSON.Geometry,
  GeoJSON.GeoJsonProperties
>

export interface ReferenceLayerView {
  record: ReferenceLayerRecord
  data: ReferenceGeoJSON
  visible: boolean
}

export interface ReferenceFeatureSelection {
  layerId: string
  layerName: string
  featureIndex: number
  geometryType: GeoJSON.Geometry['type']
  properties: Record<string, unknown>
}

export type JobStatus = 'queued' | 'running' | 'succeeded' | 'failed' | 'cancelled'

export interface ImportJob {
  id: string
  status: JobStatus
  phase: string
  progress: number
  message: string
  createdAt: string
  updatedAt: string
  map?: MapRecord
  error?: string
}

export interface MatchCandidate {
  edge: number
  roadId: string
  source: string
  offsetS: number
  distance: number
  headingDelta: number
  confidence: number
  projectedRuntime: [number, number]
}

export interface QueryResponse {
  runtimePoint: [number, number]
  matches: MatchCandidate[]
}

export interface QueryRequest {
  lon: number
  lat: number
  heading?: number
  maxDistance: number
  limit: number
}

export type RouteAlgorithm = 'dijkstra' | 'astar' | 'bidijkstra' | 'biastar'

export interface RouteMatch {
  edge: number
  roadId: string
  source: string
  offsetS: number
  distance: number
  confidence: number
}

export interface RouteRequest {
  fromLon: number
  fromLat: number
  toLon: number
  toLat: number
  algorithm: RouteAlgorithm
  maxDistance: number
}

export interface RouteResponse {
  ok: boolean
  algorithm: string
  effectiveAlgorithm?: string
  reason?: string
  message?: string
  origin: RouteMatch
  destination: RouteMatch
  edges: number
  lengthM: number
  timeS: number
  expandedNodes: number
  computeMs: number
  geojson?: RouteGeoJSON
}

export interface RouteProperties {
  ROAD_ID: string
  SOURCE_ID: string
  CLASS: string
  LENGTH_M: number
  EDGE_INDEX: number
}

export type RouteGeoJSON = GeoJSON.FeatureCollection<GeoJSON.LineString, RouteProperties>

export interface SimulateRequest {
  fromLon: number
  fromLat: number
  toLon: number
  toLat: number
  count: number
  spreadSeconds: number
  durationSeconds: number
  stepSeconds: number
  sampleIntervalSeconds: number
  exitHeadwayFfSeconds: number
  exitHeadwayJamSeconds: number
  rerouteIntervalSeconds: number
  rerouteCostRatio: number
  algorithm: RouteAlgorithm
  vehicleControls: VehicleSimulationControl[]
  roadControls: RoadSimulationControl[]
  junctionControls: JunctionSimulationControl[]
  signalPlans: JunctionSignalPlan[]
}

export type VehicleControlAction = 'hold' | 'release' | 'speedFactor'
export type RoadControlAction = 'close' | 'open' | 'speedFactor' | 'capacityFactor'
export type JunctionControlAction = 'close' | 'open'

export interface VehicleSimulationControl {
  timeSeconds: number
  vehicleId: number
  action: VehicleControlAction
  value?: number
}

export interface RoadSimulationControl {
  timeSeconds: number
  edgeIds: number[]
  action: RoadControlAction
  value?: number
}

export interface JunctionSimulationControl {
  timeSeconds: number
  nodeId: number
  action: JunctionControlAction
}

export interface SignalMovement {
  fromEdgeId: number
  toEdgeId: number
}

export interface SignalPhase {
  greenSeconds: number
  saturationFlowVph: number
  movements: SignalMovement[]
}

export interface JunctionSignalPlan {
  nodeId: number
  offsetSeconds: number
  yellowSeconds: number
  allRedSeconds: number
  phases: SignalPhase[]
}

export interface SimulationControls {
  vehicleControls: VehicleSimulationControl[]
  roadControls: RoadSimulationControl[]
  junctionControls: JunctionSimulationControl[]
  signalPlans: JunctionSignalPlan[]
}

export interface SimulateResponse {
  ok: boolean
  reason?: string
  message?: string
  vehicles: number
  arrived: number
  unroutable: number
  waitingAtEnd: number
  drivingAtEnd: number
  ticks: number
  routePlans: number
  samples: number
  avgTravelS: number
  minTravelS: number
  maxTravelS: number
  totalDistanceM: number
  deadlock: boolean
  computeMs: number
  controlEvents: number
  vehicleControls: number
  roadControls: number
  junctionControls: number
  rerouteAttempts: number
  rerouteSucceeded: number
  rerouteFailed: number
  signalPlans: number
  signalPhases: number
  signalWaitEvents: number
  signalRedWaitEvents: number
  signalSaturationWaitEvents: number
  signalMovementsPassed: number
  geojson?: TrajectoryGeoJSON
  playback?: PlaybackData
}

export interface TrajectoryProperties {
  VEHICLE_ID: number
  DEPART_S: number | null
  ARRIVE_S: number | null
  TRAVEL_S: number | null
  DISTANCE_M: number
}

export type TrajectoryGeoJSON = GeoJSON.FeatureCollection<
  GeoJSON.LineString,
  TrajectoryProperties
>

export interface PlaybackVehicle {
  id: number
  depart_s: number | null
  arrive_s: number | null
  samples: [number, number, number][]
}

export interface PlaybackData {
  duration_s: number
  step_s: number
  sample_interval_s: number
  reroute_interval_s?: number
  reroute_cost_ratio?: number
  controls: PlaybackControl[]
  reroutes?: PlaybackReroute[]
  signal_plans?: PlaybackSignalPlan[]
  vehicles: PlaybackVehicle[]
}

export type BenchmarkStrategyKind = 'fixed' | 'reactive' | 'rule_agent' | 'model_agent'
export type BenchmarkJobStatus = 'queued' | 'running' | 'completed' | 'failed' | 'cancelled'

export interface BenchmarkRoadControl {
  timeSeconds: number
  edgeIds: number[]
  action: RoadControlAction
  value: number
}

export interface BenchmarkVehicleControl {
  timeSeconds: number
  vehicleId: number
  action: VehicleControlAction
  value: number
}

export interface BenchmarkScenario {
  id: string
  mapId: string
  origin: [number, number]
  destination: [number, number]
  durationSeconds: number
  stepSeconds: number
  rerouteIntervalSeconds: number
  rerouteCostRatio: number
  sampleIntervalSeconds: number
  maxDecisions: number
  seed: number
  roadControls: BenchmarkRoadControl[]
  vehicleControls: BenchmarkVehicleControl[]
}

export interface BenchmarkStrategy {
  id: string
  kind: BenchmarkStrategyKind
  algorithm: RouteAlgorithm
}

export interface BenchmarkManifest {
  name: string
  repetitions: number
  congestionSpeedThresholdMps: number
  modelInputUsdPerMillionTokens: number
  modelOutputUsdPerMillionTokens: number
  scenarios: BenchmarkScenario[]
  strategies: BenchmarkStrategy[]
}

export interface BenchmarkJob {
  jobId: string
  status: BenchmarkJobStatus
  createdAt: string
  startedAt: string | null
  completedAt: string | null
  totalRuns: number
  completedRuns: number
  successfulRuns: number
  cancelRequested: boolean
  manifest: BenchmarkManifest
  error: string | null
}

export interface BenchmarkMetricSummary {
  mean: number
  p50: number
  p95: number
  minimum: number
  maximum: number
}

export interface BenchmarkAggregate {
  scenario_id: string
  strategy_id: string
  runs: number
  successes: number
  success_rate: number
  travel_time_s: BenchmarkMetricSummary | null
  route_length_m: BenchmarkMetricSummary | null
  replanning_count: BenchmarkMetricSummary | null
  congestion_exposure_s: BenchmarkMetricSummary | null
  route_tool_calls: BenchmarkMetricSummary | null
  decision_wall_ms: BenchmarkMetricSummary | null
  model_latency_ms: BenchmarkMetricSummary | null
  model_cost_usd: BenchmarkMetricSummary | null
  real_time_factor: BenchmarkMetricSummary | null
}

export interface BenchmarkRun {
  run_id: string
  scenario_id: string
  strategy_id: string
  strategy_kind: BenchmarkStrategyKind
  algorithm: RouteAlgorithm
  execution_mode: string
  model_name: string
  repetition: number
  seed: number
  success: boolean
  arrived: boolean
  finished: boolean
  travel_time_s: number | null
  route_length_m: number | null
  replanning_count: number
  environment_reroute_attempts: number
  congestion_exposure_s: number
  decisions: number
  route_tool_calls: number
  decision_wall_ms: number
  model_latency_ms: number
  model_calls: number
  model_failures: number
  input_tokens: number
  output_tokens: number
  model_cost_usd: number
  simulation_time_s: number
  wall_seconds: number
  real_time_factor: number
  compute_ms: number | null
  error: string | null
}

export interface BenchmarkReport {
  format_version: number
  name: string
  manifest: BenchmarkManifest
  started_at: string
  completed_at: string
  wall_seconds: number
  cancelled: boolean
  runs: BenchmarkRun[]
  aggregates: BenchmarkAggregate[]
}

export interface PlaybackSignalPlan {
  node_id: number
  offset_s: number
  yellow_s: number
  all_red_s: number
  phases: Array<{
    green_s: number
    saturation_flow_vph: number
    movements: [number, number][]
  }>
}

export interface PlaybackReroute {
  time_s: number
  vehicle_id: number
  old_route_id: number
  new_route_id: number
  success: boolean
}

export interface PlaybackControl {
  requested_s: number
  effective_s: number
  scope: 'vehicle' | 'edge' | 'junction'
  target_id: number
  action: 'hold' | 'release' | 'close' | 'open' | 'speed_factor' | 'capacity_factor'
  value: number
}

export type VehicleFrameGeoJSON = GeoJSON.FeatureCollection<
  GeoJSON.Point,
  { VEHICLE_ID: number; HELD: boolean; SPEED_FACTOR: number }
>

export const emptyRouteGeoJSON: RouteGeoJSON = { type: 'FeatureCollection', features: [] }

export const emptyVehicleFrame: VehicleFrameGeoJSON = { type: 'FeatureCollection', features: [] }

export interface RoadProperties {
  ROAD_ID: string
  SOURCE_ID: string
  CLASS: string
  EDGE_IDS: string
  DIRECTION: 'both' | 'forward' | 'reverse'
  LENGTH_M: number
  SPEED_KPH: number
  Z_LEVEL: number
}

export type RoadGeoJSON = GeoJSON.FeatureCollection<GeoJSON.LineString, RoadProperties>

export interface NodeProperties {
  NODE_ID: string
  NODE_INDEX: number
  IN_DEGREE: number
  OUT_DEGREE: number
}

export type NodeGeoJSON = GeoJSON.FeatureCollection<GeoJSON.Point, NodeProperties>

export interface IssueProperties {
  ISSUE_INDEX: number
  SEVERITY: Severity
  CODE: string
  SOURCE: string
  MESSAGE: string
  RUNTIME_X: number
  RUNTIME_Y: number
}

export type IssueGeoJSON = GeoJSON.FeatureCollection<GeoJSON.Point, IssueProperties>

export interface AgentAlgorithmCapability {
  algorithmId: RouteAlgorithm
  algorithmVersion: string
  supportedObjectives: string[]
  searchDirection: string
  supportsDynamicWeights: boolean
  supportsIncrementalRepair: boolean
  supportsKCandidates: boolean
  supportsTimeDependency: boolean
  deterministic: boolean
  exact: boolean
  usesHeuristic: boolean
}

export interface AgentToolRegistry {
  registryVersion: string
  algorithms: AgentAlgorithmCapability[]
}

export interface AgentVehicleSpec {
  fromLon: number
  fromLat: number
  toLon: number
  toLat: number
  departSeconds: number
  algorithm: RouteAlgorithm
  agent: boolean
}

export interface AgentSessionRequest {
  vehicles: AgentVehicleSpec[]
  durationSeconds: number
  stepSeconds: number
  sampleIntervalSeconds: number
  exitHeadwayFfSeconds: number
  exitHeadwayJamSeconds: number
  rerouteIntervalSeconds: number
  rerouteCostRatio: number
  minSpeedRatio: number
  vehicleControls?: VehicleSimulationControl[]
  roadControls?: RoadSimulationControl[]
  junctionControls?: JunctionSimulationControl[]
  signalPlans?: JunctionSignalPlan[]
}

// Agent session wire shapes. Authoritative sources (keep in sync):
// - create/reset   -> tools/zeus-map/session_worker.cc commandReset
// - step/pause hdr -> session_worker.cc writeStateHeader
// - observe body   -> session_worker.cc writeSnapshot
// - agent-observe  -> session_worker.cc commandAgentObserve
// - Go mirror      -> apps/control-server/agent_sessions.go sessionStateFields

/** State header shared by every session response. */
export interface AgentSessionState {
  tick: number
  simulationTimeS: number
  stateVersion: number
  finished: boolean
  cancelled?: boolean
  decisionDue?: boolean
  decisionReason?: string
  agentVehicleIds?: number[]
}

/** Create-session (worker `reset`) response; superset of the state header. */
export interface AgentSessionCreated extends AgentSessionState {
  sessionId: string
  ready?: boolean
  paused?: boolean
  vehicles?: number
  /** agent-controlled vehicle indices (NOT AgentVehicleState objects) */
  agents?: number[]
}

export interface AgentEdgeState {
  edgeId: number
  occupancy: number
  capacity: number
  closed: boolean
  speedFactor: number
  costFactor: number
  meanSpeedMps: number
}

export interface AgentVehicleState {
  vehicleId: number
  state: string
  edgeId: number
  offsetM: number
  routeId: number
  destinationEdgeId: number
  remainingEtaS: number
  routeInvalidated: boolean
  held: boolean
  remainingEdgeIds: number[]
}

export interface AgentSessionObservation extends AgentSessionState {
  counts: {
    arrived: number
    driving: number
    waiting: number
    unroutable: number
  }
  edges: AgentEdgeState[]
  /** full agent vehicle states — present on observe responses only */
  agents: AgentVehicleState[]
}

export interface AgentNearbyRoad {
  edgeId: number
  speedMps: number
  freeFlowSpeedMps: number
  occupancyRatio: number
  estimatedTravelTimeS: number
  closed: boolean
}

export interface AgentActiveEvent {
  eventId: string
  type: string
  affectedEdgeIds: number[]
}

export interface AgentVehicleObservation extends AgentSessionState {
  vehicleId: number
  state: string
  position: { edgeId: number; offsetM: number }
  destinationEdgeId: number
  remainingEtaS: number
  routeInvalidated: boolean
  remainingEdgeIds: number[]
  nearbyRoads: AgentNearbyRoad[]
  activeEvents: AgentActiveEvent[]
  availableAlgorithms: AgentAlgorithmCapability[]
}

export interface AgentStepResponse {
  state: AgentSessionState
  decisionId?: string
}

export interface AgentRouteCandidate {
  candidateId: string
  vehicleId: number
  algorithm: RouteAlgorithm
  effectiveAlgorithm?: RouteAlgorithm
  basedOnStateVersion?: number
  ok: boolean
  reason?: string
  message?: string
  timeS?: number
  lengthM?: number
  expandedNodes?: number
  edges?: number[]
}

export interface AgentActionRequest {
  decisionId: string
  agentId: string
  vehicleId: number
  kind: 'keep_route' | 'commit_route'
  candidateId?: string
  basedOnStateVersion: number
  validUntilSimulationTime?: number
  reasonCode: string
}

export interface AgentActionResult {
  accepted: boolean
  reason: string
  appliesAtNextTick: boolean
}

export interface AgentSnapshot {
  snapshotId: string
  sourceSessionId: string
  tick: number
  simulationTimeS: number
  stateVersion: number
  actionCount: number
  storage: string
}

export interface AgentSnapshotRestore {
  snapshotId: string
  /** the restored session carries its new sessionId (worker restore payload) */
  state: AgentSessionCreated
  decisionId?: string
}
