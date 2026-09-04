import { useCallback, useEffect, useRef, useState } from 'react'
import { api } from '../api'
import type {
  MapRecord,
  RouteAlgorithm,
  RouteResponse,
  SimulateResponse,
  SimulationControls,
} from '../types'

const emptySimulationControls: SimulationControls = {
  vehicleControls: [],
  roadControls: [],
  junctionControls: [],
  signalPlans: [],
}

const defaultSimConfig = {
  count: 100,
  spreadSeconds: 600,
  durationSeconds: 900,
  stepSeconds: 1,
  sampleIntervalSeconds: 15,
  exitHeadwayFfSeconds: 0,
  exitHeadwayJamSeconds: 0,
  rerouteIntervalSeconds: 0,
  rerouteCostRatio: 1.25,
}

export type SimConfig = typeof defaultSimConfig

const SIM_CONFIG_KEY = 'zeus.simConfig'
const SIM_CONTROLS_KEY = 'zeus.simControls'

function loadSession<T>(key: string, fallback: T): T {
  try {
    const raw = sessionStorage.getItem(key)
    return raw ? { ...fallback, ...JSON.parse(raw) } : fallback
  } catch {
    return fallback
  }
}

export interface RouteSimApi {
  routeMode: boolean
  routeAlgorithm: RouteAlgorithm
  routeStart: [number, number] | null
  routeEnd: [number, number] | null
  routeResult: RouteResponse | null
  routeBusy: boolean
  simConfig: SimConfig
  patchSimConfig(patch: Partial<SimConfig>): void
  simResult: SimulateResponse | null
  simBusy: boolean
  simControls: SimulationControls
  /** Any scenario edit after a run flips this true (stage 2 becomes active). */
  scenarioDirty: boolean
  applyControls(controls: SimulationControls): void
  junctionPickMode: boolean
  setJunctionPickMode(active: boolean): void
  selectedControlNodeId: number | null
  selectJunction(nodeId: number): void
  toggleRouteMode(): void
  handleRoutePoint(longitude: number, latitude: number): void
  setRouteAlgorithm(algorithm: RouteAlgorithm): void
  clearRoute(): void
  runSimulation(): Promise<void>
  resetForMapChange(): void
  error: string
  dismissError(): void
}

/**
 * Route planning and mesoscopic simulation state, including the scenario
 * editors' shared junction-pick source. Any OD/scenario/algorithm change
 * nulls the simulation result (playback follows via usePlayback's
 * [simResult] effect). simConfig/simControls survive workbench unmounts
 * via sessionStorage.
 */
export function useRouteSimulation(
  activeMap: MapRecord | null,
  options: { onRouteReady(): void },
): RouteSimApi {
  const onRouteReadyRef = useRef(options.onRouteReady)
  onRouteReadyRef.current = options.onRouteReady

  const [routeMode, setRouteMode] = useState(false)
  const [routeAlgorithm, setRouteAlgorithmState] = useState<RouteAlgorithm>('dijkstra')
  const [routeStart, setRouteStart] = useState<[number, number] | null>(null)
  const [routeEnd, setRouteEnd] = useState<[number, number] | null>(null)
  const [routeResult, setRouteResult] = useState<RouteResponse | null>(null)
  const [routeBusy, setRouteBusy] = useState(false)
  const [simConfig, setSimConfig] = useState<SimConfig>(() =>
    loadSession(SIM_CONFIG_KEY, defaultSimConfig))
  const [simResult, setSimResult] = useState<SimulateResponse | null>(null)
  const [simBusy, setSimBusy] = useState(false)
  const [simControls, setSimControls] = useState<SimulationControls>(() =>
    loadSession(SIM_CONTROLS_KEY, emptySimulationControls))
  const [scenarioDirty, setScenarioDirty] = useState(false)
  const [junctionPickMode, setJunctionPickModeState] = useState(false)
  const [selectedControlNodeId, setSelectedControlNodeId] = useState<number | null>(null)
  const [error, setError] = useState('')

  useEffect(() => {
    try {
      sessionStorage.setItem(SIM_CONFIG_KEY, JSON.stringify(simConfig))
    } catch { /* storage full or blocked */ }
  }, [simConfig])

  useEffect(() => {
    try {
      sessionStorage.setItem(SIM_CONTROLS_KEY, JSON.stringify(simControls))
    } catch { /* storage full or blocked */ }
  }, [simControls])

  const computeRoute = useCallback(async (
    from: [number, number],
    to: [number, number],
    algorithm: RouteAlgorithm,
  ) => {
    if (!activeMap) return
    setRouteBusy(true)
    setError('')
    try {
      const result = await api.routeMap(activeMap.id, {
        fromLon: from[0],
        fromLat: from[1],
        toLon: to[0],
        toLat: to[1],
        algorithm,
        maxDistance: 100,
      })
      setRouteResult(result)
      onRouteReadyRef.current()
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : '路径规划失败。')
    } finally {
      setRouteBusy(false)
    }
  }, [activeMap])

  // Latest-value refs keep handleRoutePoint/setRouteAlgorithm stable while
  // preserving the original pick choreography (start → end → recompute;
  // any new OD drops the route and the simulation result).
  const algorithmRef = useRef(routeAlgorithm)
  algorithmRef.current = routeAlgorithm
  const startRef = useRef(routeStart)
  startRef.current = routeStart
  const endRef = useRef(routeEnd)
  endRef.current = routeEnd

  const handleRoutePoint = useCallback((longitude: number, latitude: number) => {
    if (!startRef.current || endRef.current) {
      setRouteStart([longitude, latitude])
      setRouteEnd(null)
      setRouteResult(null)
      setSimResult(null)
      return
    }
    setRouteEnd([longitude, latitude])
    void computeRoute(startRef.current, [longitude, latitude], algorithmRef.current)
  }, [computeRoute])

  const setRouteAlgorithm = (algorithm: RouteAlgorithm) => {
    setRouteAlgorithmState(algorithm)
    setSimResult(null)
    if (startRef.current && endRef.current) {
      void computeRoute(startRef.current, endRef.current, algorithm)
    }
  }

  const runSimulation = async () => {
    if (!activeMap || !routeStart || !routeEnd) return
    setSimBusy(true)
    setError('')
    try {
      const result = await api.simulateMap(activeMap.id, {
        fromLon: routeStart[0],
        fromLat: routeStart[1],
        toLon: routeEnd[0],
        toLat: routeEnd[1],
        algorithm: routeAlgorithm,
        ...simControls,
        ...simConfig,
      })
      setSimResult(result)
      setScenarioDirty(false)
      if (!result.ok) setError(result.message || '交通仿真无法规划给定 OD。')
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : '交通仿真失败。')
    } finally {
      setSimBusy(false)
    }
  }

  const clearRoute = () => {
    setRouteStart(null)
    setRouteEnd(null)
    setRouteResult(null)
    setSimResult(null)
    setSimControls(emptySimulationControls)
    setJunctionPickModeState(false)
    setSelectedControlNodeId(null)
  }

  const toggleRouteMode = () => {
    setRouteMode((current) => !current)
    onRouteReadyRef.current()
  }

  /** Single source of route-mode exclusion for both scenario editors. */
  const setJunctionPickMode = (active: boolean) => {
    setJunctionPickModeState(active)
    if (active) setRouteMode(false)
  }

  const selectJunction = (nodeId: number) => {
    setSelectedControlNodeId(nodeId)
    setJunctionPickModeState(false)
  }

  const patchSimConfig = (patch: Partial<SimConfig>) => {
    setSimConfig((current) => ({ ...current, ...patch }))
    setSimResult(null)
    setScenarioDirty(true)
  }

  const applyControls = (controls: SimulationControls) => {
    setSimControls(controls)
    setSimResult(null)
    setScenarioDirty(true)
  }

  const resetForMapChange = () => {
    setRouteStart(null)
    setRouteEnd(null)
    setRouteResult(null)
    setSimResult(null)
    setJunctionPickModeState(false)
  }

  return {
    routeMode, routeAlgorithm, routeStart, routeEnd, routeResult, routeBusy,
    simConfig, patchSimConfig, simResult, simBusy, simControls, scenarioDirty,
    applyControls, junctionPickMode, setJunctionPickMode, selectedControlNodeId,
    selectJunction, toggleRouteMode, handleRoutePoint, setRouteAlgorithm,
    clearRoute, runSimulation, resetForMapChange,
    error, dismissError: () => setError(''),
  }
}
