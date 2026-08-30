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
