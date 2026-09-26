// Runs the real Web navigation loop against Go, Python and C++ in an isolated
// data directory. No model provider, browser, or external map download required.
import assert from 'node:assert/strict'
import { spawn, execFile, execFileSync } from 'node:child_process'
import { once } from 'node:events'
import { mkdtemp, mkdir, writeFile, readFile, copyFile, rm } from 'node:fs/promises'
import { createServer } from 'node:net'
import { tmpdir } from 'node:os'
import { dirname, resolve, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { parseArgs, promisify } from 'node:util'
import { setTimeout as delay } from 'node:timers/promises'
import { runAlgorithmNavigation } from '../apps/web/src/agent/algorithmNavigation.ts'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const execFileAsync = promisify(execFile)
const { values } = parseArgs({ options: {
  map: { type: 'string' }, origin: { type: 'string' }, destination: { type: 'string' },
  'closed-edge': { type: 'string' },
} })
if (values.map && (!values.origin || !values.destination)) {
  throw new Error('--map requires --origin lon,lat and --destination lon,lat')
}
function point(value, fallback) {
  const coords = value ? value.split(',').map(Number) : fallback
  assert.ok(coords.length === 2 && coords.every(Number.isFinite), 'expected lon,lat')
  return coords
}
const origin = point(values.origin, [114.0002, 30])
const destination = point(values.destination, [114.0038, 30])
const dir = await mkdtemp(join(tmpdir(), 'zeus-algorithm-e2e-'))
const mapDir = join(dir, 'maps', 'map_e2e')
const mapAPI = '/api/maps/map_e2e'
let server, serverDone, base, logs = '', serverError

async function request(path, body, method = body === undefined ? 'GET' : 'POST', expected = 200) {
  const response = await fetch(base + path, {
    method, headers: { 'Content-Type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body), signal: AbortSignal.timeout(45000),
  })
  const text = await response.text()
  assert.equal(response.status, expected, `${method} ${path}: ${text.slice(0, 800)}`)
  return text ? JSON.parse(text) : null
}

async function startServer() {
  const portProbe = createServer()
  portProbe.listen(0, '127.0.0.1')
  await once(portProbe, 'listening')
  const port = portProbe.address().port
  await new Promise((resolve, reject) => portProbe.close(error => error ? reject(error) : resolve()))
  base = `http://127.0.0.1:${port}`
  logs = ''; serverError = undefined
  server = spawn(join(root, 'build/zeus-server'), [
    '--addr', `127.0.0.1:${port}`, '--data-dir', dir,
    '--zeus-map', join(root, 'build/zeus-map'), '--web-dir', join(root, 'apps/web/dist'),
  ], { cwd: root, stdio: ['ignore', 'pipe', 'pipe'] })
  serverDone = new Promise(resolve => server.once('close', resolve))
  server.on('error', error => { serverError = error })
  for (const stream of [server.stdout, server.stderr]) {
    stream.on('data', chunk => { logs = (logs + chunk).slice(-12000) })
  }
  for (let i = 0; i < 100; i++) {
    if (serverError) throw serverError
    assert.equal(server.exitCode, null, logs)
    try {
      const response = await fetch(base + '/api/live', { signal: AbortSignal.timeout(500) })
      if (response.ok) return
    } catch { /* wait for the listener */ }
    await delay(100)
  }
  throw new Error(`control server did not start\n${logs}`)
}

async function stopServer() {
  if (!server) return
  if (server.exitCode === null && server.signalCode === null) server.kill('SIGTERM')
  const killTimer = setTimeout(() => server.kill('SIGKILL'), 12000)
  try { await serverDone } finally { clearTimeout(killTimer); server = undefined }
}

async function prepareMap() {
  await mkdir(mapDir, { recursive: true })
  let summary
  if (values.map) {
    const source = resolve(values.map)
    // Copy only the runtime and metadata; never change the user's experiments.
    await copyFile(join(source, 'map.zmap'), join(mapDir, 'map.zmap'))
    summary = JSON.parse(await readFile(join(source, 'record.json'), 'utf8')).summary
  } else {
    const a = [114, 30], b = [114.001, 30], c = [114.003, 30], d = [114.004, 30], e = [114.002, 30.001]
    const roads = [[a, b], [b, c], [c, d], [b, e], [e, c]]
    const source = join(dir, 'roads.geojson')
    await writeFile(source, JSON.stringify({ type: 'FeatureCollection', features: roads.map((coordinates, id) => ({
      type: 'Feature', properties: { id: String(id), oneway: 'yes', speed: 36 },
      geometry: { type: 'LineString', coordinates },
    })) }))
    execFileSync(join(root, 'build/zeus-map'), ['import', source, '--output', join(mapDir, 'map.zmap'),
      '--id-field', 'id', '--oneway-field', 'oneway', '--speed-field', 'speed'], { stdio: 'pipe', timeout: 30000 })
    summary = { nodes: 5, directedEdges: 5 }
  }
  await writeFile(join(mapDir, 'record.json'), JSON.stringify({
    id: 'map_e2e', name: 'Isolated algorithm E2E', createdAt: new Date().toISOString(), summary, issues: [],
  }))
}

async function prepareViaMap() {
  const directory = join(dir, 'maps', 'map_via')
  await mkdir(directory, { recursive: true })
  const a = [114, 30], b = [114.001, 30], c = [114.003, 30]
  const d = [114.004, 30], e = [114.005, 30], f = [114.003, 30.001]
  const source = join(dir, 'via-roads.geojson'), turns = join(dir, 'via-turns.csv')
  await writeFile(source, JSON.stringify({ type: 'FeatureCollection',
    features: [[a,b], [b,c], [c,d], [d,e], [c,f], [f,d]].map((coordinates, id) => ({
      type: 'Feature', properties: { id: String(id), oneway: 'yes', speed: 36 },
      geometry: { type: 'LineString', coordinates },
    })) }))
  await writeFile(turns, '0,114.001,30,2,no_via,1\n')
  execFileSync(join(root, 'build/zeus-map'), ['import', source, '--output', join(directory, 'map.zmap'),
    '--id-field', 'id', '--oneway-field', 'oneway', '--speed-field', 'speed',
    '--turn-restrictions', turns], { stdio: 'pipe', timeout: 30000 })
  await writeFile(join(directory, 'record.json'), JSON.stringify({ id: 'map_via',
    name: 'Via-way history E2E', createdAt: new Date().toISOString(),
    summary: { nodes: 6, directedEdges: 6 }, issues: [] }))
}

async function viaWayScenario(template) {
  const api = '/api/maps/map_via'
  const points = { fromLon: 114.0005, fromLat: 30, toLon: 114.0045, toLat: 30 }
  const condition = { ...points, source: template, steps: 5000000 }
  const full = await request(`${api}/algorithms/run`, condition)
  verified(full)
  assert.equal(full.edges, 5, 'incoming road must force the detour after via-way')
  const fresh = await request(`${api}/algorithms/run`, { ...condition, fromLon: 114.0015 })
  verified(fresh)
  assert.equal(fresh.edges, 3, 'starting inside via-way must not inherit an absent incoming road')
  const forbidden = edgeIDs(fresh)[1]
  assert.ok(!edgeIDs(full).includes(forbidden))
  for (const algorithm of ['dijkstra', 'astar', 'bidijkstra', 'biastar', 'kshortest', 'lpa', 'dstar', 'alt', 'ch', 'tddijkstra']) {
    const route = await request(`${api}/route`, { ...points, algorithm, kPaths: 3 })
    assert.equal(route.ok, true)
    assert.deepEqual(edgeIDs(route), edgeIDs(full))
    assert.equal(route.effectiveAlgorithm, algorithm === 'tddijkstra' ? algorithm : 'dijkstra')
    if (!['dijkstra', 'tddijkstra'].includes(algorithm)) assert.equal(route.fallbackReason, 'via_way_history')
  }
  const created = await request(`${api}/agent/sessions`, {
    vehicles: [{ ...points, algorithm: 'ch', agent: true }], durationSeconds: 120,
    stepSeconds: 1, sampleIntervalSeconds: 5, rerouteIntervalSeconds: 0,
    exitHeadwayFfSeconds: 1.4, exitHeadwayJamSeconds: 2, rerouteCostRatio: 1.25,
  })
  let path = `${api}/agent/sessions/${created.sessionId}`
  await request(`${path}/step`, { ticks: 8 })
  const before = await request(path)
  assert.equal(before.agents[0].edgeId, edgeIDs(fresh)[0], 'vehicle should be inside via-way')
  const saved = await request(`${path}/snapshots`, {})
  await request(path, undefined, 'DELETE')
  await stopServer()
  await startServer()
  const restored = await request(`${api}/agent/snapshots/${saved.snapshotId}/restore`, {})
  path = `${api}/agent/sessions/${restored.state.sessionId}`
  assert.deepEqual((await request(path)).agents, before.agents)
  const planned = await request(`${path}/plan`, { algorithm: 'ch', vehicleId: 0 })
  assert.equal(planned.fallbackReason, 'via_way_history')
  assert.ok(!planned.edges.includes(forbidden), 'restored native query lost entry history')
  const records = []
  const outcome = await runAlgorithmNavigation({
    runId: 'via-way', source: template, vehicleId: 0, once: false, maxDecisions: 20,
    active: () => true, request: (suffix, body) => request(path + suffix, body),
    onObservation: async () => {}, onPlan: value => {
      if (value.result?.phase === 'verified') assert.ok(!edgeIDs(value.result).includes(forbidden))
    }, onRecord: value => records.push(value), onPhase: () => {},
    yieldBetween: async () => {}, checkpoint: async () => {},
  })
  assert.equal(outcome, 'complete')
  assert.ok(records.some(record => record.status === 'applied'))
  assert.equal((await request(path)).agents[0].state, 'arrived')
  await request(path, undefined, 'DELETE')
  console.log('via-way: import -> 10 algorithms -> custom states -> moving origin -> restart -> exact commit -> arrival')
}

const edgeIDs = result => result.geojson.features.map(feature => feature.properties.EDGE_INDEX)
function verified(result) {
  assert.equal(result.ok, true, result.error)
  assert.equal(result.phase, 'verified', result.error)
  assert.ok(result.edges > 0 && result.timeS > 0)
  assert.ok(Math.abs(result.timeS - result.baseline.timeS) < 0.001, 'Dijkstra costs disagree')
}

async function reusableRoutingScenario(condition, closed, duration, selectedAlgorithm) {
  const alt = selectedAlgorithm === 'alt'
  const ch = selectedAlgorithm === 'ch'
  const preprocessed = alt || ch
  const reused = candidate => ch ? candidate.chReused : alt ? candidate.landmarkReused : candidate.incrementalReused
  const vehicle = { fromLon: condition.fromLon, fromLat: condition.fromLat,
    toLon: condition.toLon, toLat: condition.toLat, algorithm: selectedAlgorithm, agent: true }
  const settings = { durationSeconds: duration, stepSeconds: 1, sampleIntervalSeconds: 5,
    exitHeadwayFfSeconds: 1.4, exitHeadwayJamSeconds: 2, rerouteIntervalSeconds: 0, rerouteCostRatio: 1.25 }
  const created = await request(`${mapAPI}/agent/sessions`, { ...settings, vehicles: [vehicle],
    rerouteRecoveryIntervalSeconds: 1,
    roadControls: [{ timeSeconds: 1, edgeIds: [closed], action: 'close' },
      { timeSeconds: 3, edgeIds: [closed], action: 'open' }],
    vehicleControls: [{ timeSeconds: 0, vehicleId: 0, action: 'hold' },
      { timeSeconds: 8, vehicleId: 0, action: 'release' }],
  })
  let path = `${mapAPI}/agent/sessions/${created.sessionId}`
  const plan = (algorithm = selectedAlgorithm, vehicleId = 0) => request(`${path}/plan`, { algorithm, vehicleId, recordTrace: true })
  const apply = async (boundary, candidate) => {
    assert.ok(boundary.decisionId)
    const action = await request(`${path}/actions`, { kind: 'commit_route', vehicleId: 0,
      candidateId: candidate.candidateId, decisionId: boundary.decisionId,
      basedOnStateVersion: boundary.state.stateVersion })
    assert.equal(action.accepted, true)
    return request(`${path}/step`, { ticks: 1 })
  }
  const first = await plan()
  const repeat = await plan()
  assert.equal(reused(first), preprocessed, 'vehicle initialization shares preprocessed indexes')
  assert.equal(reused(repeat), true)
  if (!preprocessed) assert.equal(repeat.expandedNodes, 0)
  if (alt) {
    assert.ok(first.landmarkCount > 0 && first.landmarkCount <= 8)
    assert.ok(first.landmarkBytes > 0 && first.landmarkBytes <= 256 * 1024 * 1024)
    assert.equal(repeat.landmarkPreprocessMs, 0)
    assert.equal(repeat.landmarkBytes, first.landmarkBytes)
  }
  if (ch) {
    assert.equal(first.effectiveAlgorithm, 'ch')
    assert.ok(first.chBytes > 0)
    assert.equal(repeat.chPreprocessMs, 0)
  }
  assert.deepEqual(repeat.edges, first.edges)
  const closedBoundary = await request(`${path}/step`, { ticks: 2 })
  const repaired = await plan()
  const baseline = await plan('dijkstra')
  assert.equal(reused(repaired), !ch)
  if (ch) {
    assert.equal(repaired.effectiveAlgorithm, 'bidijkstra')
    assert.equal(repaired.fallbackReason, 'ch_dynamic_weights')
  }
  if (!preprocessed) assert.ok(repaired.updatedEdges >= 1)
  assert.ok(!repaired.edges.includes(closed))
  assert.ok(Math.abs(repaired.timeS - baseline.timeS) < .001)
  await apply(closedBoundary, repaired)
  const openBoundary = await request(`${path}/step`, { ticks: 1 })
  assert.equal(openBoundary.state.decisionReason, 'route_improved')
  const recovered = await plan()
  assert.equal(reused(recovered), true)
  if (ch) assert.equal(recovered.effectiveAlgorithm, 'ch')
  assert.deepEqual(recovered.edges, first.edges)
  await apply(openBoundary, recovered)
  const before = await request(path)
  const saved = await request(`${path}/snapshots`, {})
  await request(path, undefined, 'DELETE')
  await stopServer()
  await startServer()
  const restored = await request(`${mapAPI}/agent/snapshots/${saved.snapshotId}/restore`, {})
  path = `${mapAPI}/agent/sessions/${restored.state.sessionId}`
  const after = await request(path)
  assert.deepEqual(after.agents, before.agents, `${selectedAlgorithm} exact path changed after restart`)
  assert.equal(reused(await plan()), preprocessed, 'restoring vehicles rebuilds indexes during initialization')
  assert.equal(reused(await plan()), true)
  const moving = await request(`${path}/step`, { ticks: 12 })
  assert.equal(moving.state.finished, false, 'vehicle should still be travelling after release')
  const moved = await plan()
  const movedBaseline = await plan('dijkstra')
  if (ch && moved.effectiveAlgorithm !== 'ch') assert.equal(moved.fallbackReason, 'ch_dynamic_weights')
  else assert.equal(reused(moved), true)
  assert.ok(Math.abs(moved.timeS - movedBaseline.timeS) < .001, 'moving-origin costs disagree')
  if (!preprocessed) assert.equal((await plan()).expandedNodes, 0)
  const complete = await request(`${path}/step`, { untilEvent: true, maxTicks: 20000 })
  assert.equal(complete.state.finished, true)
  const result = await request(`${path}/result`)
  assert.equal(result.summary.arrived, 1)
  await request(path, undefined, 'DELETE')
  // Landmark tables belong to the planner and are shared across vehicle queries.
  // Incremental labels instead retain only four vehicle contexts.
  const fleet = await request(`${mapAPI}/agent/sessions`, { ...settings,
    vehicles: Array.from({ length: 5 }, () => ({ ...vehicle, departSeconds: 100 })) })
  path = `${mapAPI}/agent/sessions/${fleet.sessionId}`
  for (let vehicleId = 0; vehicleId < 5; ++vehicleId)
    assert.equal(reused(await plan(selectedAlgorithm, vehicleId)), preprocessed)
  assert.equal(reused(await plan(selectedAlgorithm, 4)), true)
  assert.equal(reused(await plan(selectedAlgorithm, 0)), preprocessed)
  const alternate = selectedAlgorithm === 'dstar' ? 'lpa' : 'dstar'
  assert.equal((await plan(alternate, 0)).incrementalReused, false, 'algorithm switch resets labels')
  assert.equal((await plan(alternate, 0)).incrementalReused, true)
  assert.equal(reused(await plan(selectedAlgorithm, 0)), preprocessed)
  await request(path, undefined, 'DELETE')
  console.log(`${selectedAlgorithm}: warm reuse -> closure repair -> reopening -> restart -> movement -> arrival; initial plan expansions ${first.expandedNodes}, unchanged ${repeat.expandedNodes}, repair ${repaired.expandedNodes}; bounded cache and algorithm switch verified`)
}

async function timeDependentScenario(condition, closed, baseline) {
  const routePoints = { fromLon: condition.fromLon, fromLat: condition.fromLat, toLon: condition.toLon, toLat: condition.toLat }
  const route = (departureTimeSeconds, speedChanges = []) => request(`${mapAPI}/route`, {
    ...routePoints, algorithm: 'tddijkstra', departureTimeSeconds, speedChanges, recordTrace: true,
  })
  const speedChanges = [{ edgeId: closed, timeSeconds: 0, speedFactor: .05 },
    { edgeId: closed, timeSeconds: 1000, speedFactor: 1 }]
  const early = await route(0, speedChanges)
  const late = await route(2000, speedChanges)
  assert.equal(early.ok, true)
  assert.equal(late.ok, true)
  assert.equal(late.effectiveAlgorithm, 'tddijkstra')
  assert.equal(late.departureTimeSeconds, 2000)
  assert.ok(early.timeS > late.timeS)
  assert.ok(Math.abs(late.timeS - baseline.timeS) < .001)
  assert.ok(Math.abs(late.arrivalTimeSeconds - 2000 - late.timeS) < .002)
  for (const invalid of [{ algorithm: 'astar', speedChanges }, { departureTimeSeconds: -1 },
    { speedChanges: [{ edgeId: closed, timeSeconds: 0, speedFactor: 0 }] },
    { speedChanges: [{ edgeId: 4294967295, timeSeconds: 0, speedFactor: 1 }] }]) {
    await request(`${mapAPI}/route`, { ...routePoints, algorithm: 'tddijkstra', ...invalid }, 'POST', 400)
  }
  const created = await request(`${mapAPI}/agent/sessions`, {
    durationSeconds: Math.max(900, baseline.timeS * 4 + 400), stepSeconds: 1,
    sampleIntervalSeconds: 5, rerouteIntervalSeconds: 0,
    exitHeadwayFfSeconds: 1.4, exitHeadwayJamSeconds: 2, rerouteCostRatio: 1.25,
    vehicles: [{ fromLon: condition.fromLon, fromLat: condition.fromLat,
      toLon: condition.toLon, toLat: condition.toLat, algorithm: 'tddijkstra', agent: true, departSeconds: 100 }],
    roadControls: [{ timeSeconds: 0, edgeIds: [closed], action: 'speedFactor', value: .05 },
      { timeSeconds: 10, edgeIds: [closed], action: 'speedFactor', value: 1 }],
  })
  let path = `${mapAPI}/agent/sessions/${created.sessionId}`
  const plan = () => request(`${path}/plan`, { algorithm: 'tddijkstra', vehicleId: 0, recordTrace: true })
  const first = await plan()
  assert.equal(first.departureTimeSeconds, 100)
  assert.ok(Math.abs(first.timeS - late.timeS) < .001)
  let boundary = await request(`${path}/step`, { ticks: 10 })
  assert.equal(boundary.state.simulationTimeS, 10)
  const beforeRecoveryTick = await plan()
  assert.ok(Math.abs(beforeRecoveryTick.timeS - first.timeS) < .001,
    'pending recovery must replace the still-published speed penalty exactly once')
  if (!boundary.decisionId) boundary = await request(`${path}/decisions`, { vehicleId: 0, basedOnStateVersion: boundary.state.stateVersion })
  assert.ok(boundary.decisionId)
  const action = await request(`${path}/actions`, { kind: 'commit_route', vehicleId: 0,
    candidateId: beforeRecoveryTick.candidateId, decisionId: boundary.decisionId,
    basedOnStateVersion: boundary.state.stateVersion })
  assert.equal(action.accepted, true)
  await request(`${path}/step`, { ticks: 1 })
  const saved = await request(`${path}/snapshots`, {})
  const before = await request(path)
  await request(path, undefined, 'DELETE')
  await stopServer()
  await startServer()
  const restored = await request(`${mapAPI}/agent/snapshots/${saved.snapshotId}/restore`, {})
  path = `${mapAPI}/agent/sessions/${restored.state.sessionId}`
  const resumed = await request(path)
  assert.deepEqual(resumed.agents, before.agents)
  const after = await plan()
  assert.deepEqual(after.edges, first.edges)
  assert.ok(Math.abs(after.timeS - first.timeS) < .001)
  if (resumed.decisionId) {
    const kept = await request(`${path}/actions`, { kind: 'keep_route', vehicleId: 0,
      decisionId: resumed.decisionId, basedOnStateVersion: resumed.stateVersion })
    assert.equal(kept.accepted, true)
  }
  const complete = await request(`${path}/step`, { untilEvent: true, maxTicks: 20000 })
  assert.equal(complete.state.finished, true)
  assert.equal((await request(`${path}/result`)).summary.arrived, 1)
  await request(path, undefined, 'DELETE')
  console.log(`tddijkstra: departure forecasts ${early.timeS}s / ${late.timeS}s -> tick-boundary recovery -> exact commit -> restart -> arrival`)
}

async function main() {
  await prepareMap()
  await prepareViaMap()
  await startServer()
  const { template } = await request('/api/algorithms/capabilities')
  assert.equal((await request('/api/algorithms/check', { source: template })).ok, true)
  await viaWayScenario(template)
  const condition = { fromLon: origin[0], fromLat: origin[1], toLon: destination[0], toLat: destination[1], source: template, steps: 5000000 }
  const initial = await request(`${mapAPI}/algorithms/run`, condition)
  verified(initial)
  assert.ok(initial.runId && !initial.storageError, initial.storageError)
  let staticBaseline
  for (const algorithm of ['dijkstra', 'bidijkstra', 'biastar', 'lpa', 'dstar', 'tddijkstra', 'alt', 'ch']) {
    const route = await request(`${mapAPI}/route`, {
      fromLon: origin[0], fromLat: origin[1], toLon: destination[0], toLat: destination[1], algorithm,
    })
    assert.equal(route.ok, true)
    assert.equal(route.effectiveAlgorithm, algorithm)
    if (algorithm === 'dijkstra') staticBaseline = route
    if (algorithm === 'ch') {
      assert.equal(route.chReused, false)
      assert.ok(route.chBytes > 0)
      const warm = await request(`${mapAPI}/route`, {
        fromLon: origin[0], fromLat: origin[1], toLon: destination[0], toLat: destination[1], algorithm,
      })
      assert.equal(warm.chReused, true)
      assert.equal(warm.chPreprocessMs ?? 0, 0)
      assert.deepEqual(edgeIDs(warm), edgeIDs(route))
      console.log(`ch static: ${route.chShortcuts ?? 0} shortcuts, ${route.chCoreStates ?? 0} core states, ${route.chBytes} bytes; preprocessing ${route.chPreprocessMs}ms; warm ${warm.computeMs}ms, ${warm.expandedNodes} expansions vs Dijkstra ${staticBaseline.expandedNodes}`)
    }
    if (algorithm === 'alt') {
      assert.equal(route.landmarkReused, false)
      assert.ok(route.landmarkCount > 0 && route.landmarkBytes > 0)
      const warm = await request(`${mapAPI}/route`, {
        fromLon: origin[0], fromLat: origin[1], toLon: destination[0], toLat: destination[1], algorithm,
      })
      assert.equal(warm.landmarkReused, true)
      assert.equal(warm.landmarkPreprocessMs ?? 0, 0)
      assert.deepEqual(edgeIDs(warm), edgeIDs(route))
      console.log(`alt static: ${route.landmarkCount} landmarks, ${route.landmarkBytes} bytes; preprocessing ${route.landmarkPreprocessMs}ms; warm ${warm.computeMs}ms, ${warm.expandedNodes} expansions vs Dijkstra ${staticBaseline.expandedNodes}`)
    }
    assert.ok(Math.abs(route.timeS - initial.baseline.timeS) < .001, `${algorithm} disagrees with Dijkstra`)
  }
  const edges = edgeIDs(initial)
  assert.ok(edges.length >= 3, 'choose an OD with at least three roads')
  const closed = values['closed-edge'] === undefined ? edges[Math.floor(edges.length / 2)] : Number(values['closed-edge'])
  assert.ok(Number.isSafeInteger(closed) && edges.slice(1, -1).includes(closed),
    '--closed-edge must be an interior edge of the initial route')
  const detour = await request(`${mapAPI}/algorithms/run`, { ...condition, closedEdges: [closed] })
  assert.notEqual(detour.phase, 'unreachable',
    `closing edge ${closed} disconnects this OD; choose another --closed-edge or OD`)
  verified(detour)
  assert.ok(!edgeIDs(detour).includes(closed), 'closed road appears in detour')
  assert.notEqual(initial.snapshotId, detour.snapshotId, 'closure must change snapshot')
  console.log(`static: ${initial.edges} edges; closure ${closed} -> ${detour.edges} edges; C++ verified`)

  await timeDependentScenario(condition, closed, initial)
  for (const algorithm of ['lpa', 'dstar', 'alt', 'ch'])
    await reusableRoutingScenario(condition, closed, Math.max(600, detour.timeS * 3 + 300), algorithm)

  let debug = await request(`${mapAPI}/algorithms/debug`, condition)
  assert.equal(debug.kind, 'paused')
  const debugPath = `${mapAPI}/algorithms/debug/${debug.sessionId}`
  const oldSequence = debug.sequence
  debug = await request(debugPath, { action: 'step', sequence: oldSequence })
  assert.equal(debug.kind, 'paused')
  assert.ok(debug.sequence > oldSequence)
  await request(debugPath, { action: 'step', sequence: oldSequence }, 'POST', 409)
  const breakpoint = template.split('\n').findIndex(line => line.includes('ctx.log(')) + 1
  assert.ok(breakpoint > 0, 'template must log the completed path')
  debug = await request(debugPath, { action: 'continue', sequence: debug.sequence, breakpoints: [breakpoint] })
  assert.equal(debug.kind, 'paused')
  assert.equal(debug.line, breakpoint)
  assert.ok(debug.variables.path, 'breakpoint must expose the computed path')
  debug = await request(debugPath, { action: 'continue', sequence: debug.sequence, breakpoints: [] })
  assert.equal(debug.kind, 'finished')
  verified(debug.result)
  assert.equal(debug.result.snapshotId, initial.snapshotId)
  console.log('debug: pause -> step -> reject duplicate -> breakpoint -> continue -> native validation')

  const created = await request(`${mapAPI}/agent/sessions`, {
    vehicles: [{ ...condition, source: undefined, steps: undefined, agent: true }],
    durationSeconds: Math.max(600, detour.timeS * 3 + 300), stepSeconds: 1, sampleIntervalSeconds: 5,
    exitHeadwayFfSeconds: 1.4, exitHeadwayJamSeconds: 2, rerouteCostRatio: 1.25,
    rerouteRecoveryIntervalSeconds: 5, rerouteMinGainSeconds: 12, rerouteCooldownSeconds: 30,
    rerouteIntervalSeconds: 0, roadControls: [{ timeSeconds: 5, edgeIds: [closed], action: 'close' }],
  })
  let sessionPath = `${mapAPI}/agent/sessions/${created.sessionId}`
  const records = new Map()
  let observation
  let lastHistory
  const checkpointFor = source => {
    let saved
    return async () => {
      const current = await request(sessionPath)
      saved = await request(`${sessionPath}/navigation-history`, {
        id: saved?.id, revision: saved?.revision ?? 0, basedOnStateVersion: current.stateVersion, source,
        records: [...records.values()].filter(record => record.sessionId === sessionPath.split('/').at(-1)),
      })
      lastHistory = saved
    }
  }
  const options = {
    runId: 'before-restart', source: template, vehicleId: 0, once: true, maxDecisions: 100,
    active: () => true, request: (path, body) => request(sessionPath + path, body),
    onObservation: async value => { observation = value }, onPlan: () => {},
    onRecord: value => records.set(value.id, { ...value, sessionId: sessionPath.split('/').at(-1), mapId: 'map_e2e' }),
    onPhase: () => {}, yieldBetween: async () => {}, checkpoint: checkpointFor(template),
  }
  assert.equal(await runAlgorithmNavigation(options), 'once')
  assert.equal(observation.tick, 1)
  assert.equal([...records.values()][0].status, 'applied')
  const beforeRestart = observation.agents[0]
  const snapshot = await request(`${sessionPath}/snapshots`, {})
  assert.equal(snapshot.formatVersion, 3)
  assert.equal(snapshot.integrity, 'verified')
  assert.match(snapshot.mapRevision, /^[a-f0-9]{64}$/)
  assert.match(snapshot.checksum, /^[a-f0-9]{64}$/)
  // Durable files are authoritative even when the process has a warm cache.
  const snapshotFile = join(dir, 'agent-snapshots', `${snapshot.snapshotId}.json`)
  const pristineSnapshot = await readFile(snapshotFile, 'utf8')
  const modifiedSnapshot = JSON.parse(pristineSnapshot)
  modifiedSnapshot.tick += 1
  await writeFile(snapshotFile, JSON.stringify(modifiedSnapshot))
  await request(`${mapAPI}/agent/snapshots/${snapshot.snapshotId}/restore`, {}, 'POST', 409)
  await writeFile(snapshotFile, pristineSnapshot)
  const historyBeforeRestart = lastHistory
  assert.equal(historyBeforeRestart.tick, 1)
  await request(`${sessionPath}/navigation-history`, {
    id: historyBeforeRestart.id, revision: historyBeforeRestart.revision - 1,
    basedOnStateVersion: observation.stateVersion, source: template, records: [],
  }, 'POST', 409)
  await stopServer()
  await startServer()
  const stored = await request(`${mapAPI}/algorithms/experiments/${initial.runId}`)
  assert.equal(stored.request.source, template)
  assert.deepEqual(stored.result.geojson, initial.geojson)
  const historyPath = `${mapAPI}/agent/navigation-history/${historyBeforeRestart.id}`
  const archived = await request(historyPath)
  assert.equal(archived.source, template)
  assert.equal(archived.records[0].status, 'applied')
  assert.equal(archived.snapshot.tick, 1)
  assert.equal(archived.snapshot.request.rerouteRecoveryIntervalSeconds, 5)
  assert.equal(archived.snapshot.request.rerouteMinGainSeconds, 12)
  assert.equal(archived.snapshot.request.rerouteCooldownSeconds, 30)
  assert.equal(archived.snapshot.mapRevision, snapshot.mapRevision)
  assert.equal(archived.snapshot.formatVersion, 3)
  assert.ok((await request(`${mapAPI}/agent/navigation-history`)).some(item => item.id === archived.id))
  const restored = await request(`${historyPath}/restore`, { revision: archived.revision })
  assert.ok(restored.state.sessionId, 'restored session ID missing')
  assert.equal(restored.integrity, 'verified')
  sessionPath = `${mapAPI}/agent/sessions/${restored.state.sessionId}`
  const restoredObservation = await request(sessionPath)
  assert.equal(restoredObservation.tick, 1)
  assert.deepEqual(restoredObservation.agents[0], beforeRestart, 'restart changed exact vehicle path/position')
  console.log('restart: experiment and navigation history survive; exact vehicle path/offset restored')

  const keepSource = 'def route(ctx):\n    ctx.log(ctx.observation()["vehicle"]["remainingEtaS"])\n    return ctx.keep()'
  assert.equal(await runAlgorithmNavigation({ ...options, runId: 'keep-after-restart',
    source: keepSource, checkpoint: checkpointFor(keepSource),
  }), 'once')
  assert.equal(observation.tick, 2)
  assert.equal(observation.agents[0].routeId, beforeRestart.routeId, 'keep changed the route')
  assert.equal(records.get('keep-after-restart:0').status, 'kept')

  assert.equal(await runAlgorithmNavigation({ ...options, runId: 'after-restart', once: false, checkpoint: checkpointFor(template) }), 'complete')
  assert.equal(observation.agents[0].state, 'arrived', 'simulation ended before arrival')
  const invalidated = [...records.values()].filter(record => record.observation?.vehicle?.routeInvalidated)
  assert.ok(invalidated.length > 0, 'closure never triggered a code decision')
  assert.ok(invalidated.every(record => record.status === 'applied' && !record.edges.includes(closed)))
  assert.ok(invalidated.every(record => Number.isFinite(record.observation.vehicle.remainingEtaS)),
    'blocked-route ETA must remain valid JSON across the worker/Python boundary')
  const result = await request(`${sessionPath}/result`)
  assert.ok(result.playback, 'missing trajectory replay')
  assert.equal(result.playback.route_comparison_version, 1)
  const applied = result.playback.reroutes.filter(item => item.success)
  assert.ok(applied.some(item => item.route_changed === true && item.remaining_overlap_ratio < 1),
    'closure reroute must carry native evidence of changed coverage')
  assert.ok(applied.some(item => item.route_changed === false && item.remaining_overlap_ratio === 1),
    'same-path application must preserve full overlap after replay')
  assert.ok(applied.every(item => Number.isFinite(item.remaining_overlap_ratio)
    && item.remaining_overlap_ratio >= 0 && item.remaining_overlap_ratio <= 1))
  await request(sessionPath, undefined, 'DELETE')
  const completedHistory = await request(`${mapAPI}/agent/navigation-history/${lastHistory.id}`)
  assert.equal(completedHistory.snapshot.tick, observation.tick)
  await request(historyPath, undefined, 'DELETE')
  await request(historyPath, undefined, 'GET', 404)
  console.log(`navigation: initial commit -> restart -> closure -> custom reroute -> arrived at tick ${observation.tick}`)

  const backgroundSession = await request(`${mapAPI}/agent/sessions`, {
    vehicles: [{ ...condition, source: undefined, steps: undefined, agent: true }],
    durationSeconds: Math.max(600, detour.timeS * 3 + 300), stepSeconds: 1,
    sampleIntervalSeconds: 5, rerouteIntervalSeconds: 30,
    exitHeadwayFfSeconds: 1.4, exitHeadwayJamSeconds: 2, rerouteCostRatio: 1.25,
    roadControls: [{ timeSeconds: 5, edgeIds: [closed], action: 'close' }],
  })
  const background = await request(`${mapAPI}/agent/sessions/${backgroundSession.sessionId}/navigation-jobs`,
    { source: template, vehicleId: 0 }, 'POST', 202)
  const jobPath = `${mapAPI}/agent/navigation-jobs/${background.id}`
  await request(jobPath, { action: 'pause' })
  const waitJob = async statuses => {
    for (let attempt = 0; attempt < 300; attempt++) {
      const job = await request(jobPath)
      assert.notEqual(job.status, 'error', job.error)
      if (statuses.includes(job.status)) return job
      await delay(50)
    }
    throw new Error('background navigation did not reach expected boundary')
  }
  const paused = await waitJob(['paused'])
  assert.ok(paused.snapshot, 'pause must retain a recoverable checkpoint')
  await request(`${mapAPI}/agent/sessions/${paused.sessionId}/step`, { ticks: 1 }, 'POST', 409)
  await request(`${mapAPI}/agent/sessions/${paused.sessionId}`, undefined, 'DELETE', 409)
  await stopServer()
  await startServer()
  const recovered = await request(jobPath)
  assert.equal(recovered.status, 'paused')
  assert.equal(recovered.tick, paused.tick)
  assert.equal(recovered.source, template)
  const resumed = await request(jobPath, { action: 'resume' })
  assert.notEqual(resumed.sessionId, paused.sessionId, 'restart must create a fresh session')
  const completed = await waitJob(['completed'])
  assert.ok(completed.records.some(record => record.status === 'applied'))
  assert.equal((await request(`${mapAPI}/agent/sessions/${completed.sessionId}`)).agents[0].state, 'arrived')
  await request(`${mapAPI}/agent/sessions/${completed.sessionId}`, undefined, 'DELETE')
  await request(jobPath, undefined, 'DELETE')
  console.log(`background: owned session -> pause -> restart -> restore -> arrived at tick ${completed.tick}`)

  // Exercise the actual Python client against native decision/observation shapes.
  const benchmarkManifest = {
    name: 'custom-code-e2e', repetitions: 1,
    scenarios: [{ id: 'closure', mapId: 'map_e2e', origin, destination,
      durationSeconds: Math.max(600, detour.timeS * 3 + 300), rerouteIntervalSeconds: 30,
      roadControls: [{ timeSeconds: 5, edgeIds: [closed], action: 'close' }] }],
    strategies: [{ id: 'custom', kind: 'custom_code', source: template }, { id: 'lpa', kind: 'reactive', algorithm: 'lpa' }, { id: 'dstar', kind: 'reactive', algorithm: 'dstar' }, { id: 'td', kind: 'reactive', algorithm: 'tddijkstra' }, { id: 'alt', kind: 'reactive', algorithm: 'alt' }, { id: 'ch', kind: 'reactive', algorithm: 'ch' }],
  }
  const manifestFile = join(dir, 'benchmark.json')
  await writeFile(manifestFile, JSON.stringify(benchmarkManifest))
  // Keep draining the server's piped logs while Python drives HTTP requests.
  // A synchronous child wait can fill the pipe and block the control server.
  const { stdout: benchmarkOutput } = await execFileAsync('uv', ['run', '--project', 'apps/agent-runtime', 'python', '-c', `
import json, sys
from zeus_agent.benchmark import BenchmarkManifest, run_benchmark
from zeus_agent.client import HttpEnvironmentClient
manifest = BenchmarkManifest.model_validate_json(open(sys.argv[2]).read())
report = run_benchmark(manifest, lambda map_id: HttpEnvironmentClient(map_id=map_id, base_url=sys.argv[1]))
run = report.runs[0]
assert run.success, run.error
assert run.source_revision and run.scenario_revision
assert run.custom_decisions and all(record['status'] == 'applied' for record in run.custom_decisions)
assert run.applied_route_changes >= 1
assert run.route_reversals == 0
for incremental in report.runs[1:]:
    assert incremental.success, incremental.error
    assert incremental.applied_route_changes >= 1
print(json.dumps({'success': run.success, 'decisions': run.decisions, 'overlap': run.route_overlap_ratio}))
`, base, manifestFile], { cwd: root, encoding: 'utf8', timeout: 120000,
    env: { ...process.env, UV_CACHE_DIR: join(root, '.cache/uv') } })
  console.log('custom benchmark:', benchmarkOutput.trim())

  // Hold the vehicle while deliberately selecting B then A. Restoring a native
  // snapshot between the two applications must preserve reversal evidence.
  const reversalSession = await request(`${mapAPI}/agent/sessions`, {
    vehicles: [{ ...condition, source: undefined, steps: undefined, algorithm: 'dijkstra', agent: true }],
    durationSeconds: Math.max(600, detour.timeS * 3 + 300), stepSeconds: 1,
    sampleIntervalSeconds: 5, rerouteIntervalSeconds: 0,
    exitHeadwayFfSeconds: 1.4, exitHeadwayJamSeconds: 2, rerouteCostRatio: 1.25,
    vehicleControls: [
      { timeSeconds: 0, vehicleId: 0, action: 'hold' },
      { timeSeconds: 3, vehicleId: 0, action: 'release' },
    ],
  })
  let reversalSessionPath = `${mapAPI}/agent/sessions/${reversalSession.sessionId}`
  const avoiding = template.replace('            next_state = step["state"]',
    `            if step["edge"] == ${closed}:\n                continue\n            next_state = step["state"]`)
  assert.notEqual(avoiding, template)
  const reversalOptions = {
    runId: 'reversal', source: avoiding, vehicleId: 0, once: true, maxDecisions: 10,
    active: () => true, request: (path, body) => request(reversalSessionPath + path, body),
    onObservation: async () => {}, onPlan: () => {}, onRecord: () => {},
    onPhase: () => {}, yieldBetween: async () => {}, checkpoint: async () => {},
  }
  assert.equal(await runAlgorithmNavigation(reversalOptions), 'once')
  const reversalSnapshot = await request(`${reversalSessionPath}/snapshots`, {})
  await request(reversalSessionPath, undefined, 'DELETE')
  const reversalRestored = await request(`${mapAPI}/agent/snapshots/${reversalSnapshot.snapshotId}/restore`, {})
  reversalSessionPath = `${mapAPI}/agent/sessions/${reversalRestored.state.sessionId}`
  assert.equal(await runAlgorithmNavigation({ ...reversalOptions, source: template, once: false }), 'complete')
  const reversalResult = await request(`${reversalSessionPath}/result`)
  assert.equal(reversalResult.playback.route_reversal_version, 1)
  const reversals = reversalResult.playback.reroutes.filter(item => item.route_reversed === true)
  assert.equal(reversals.length, 1, 'A -> B -> snapshot restore -> A must count one reversal')
  assert.equal(reversalResult.playback.reroutes[0].route_reversed, false)
  await request(reversalSessionPath, undefined, 'DELETE')
  console.log('reversal: A -> B -> snapshot restore -> A = one native reversal')


}

try {
  await main()
  console.log('Algorithm E2E passed')
} catch (error) {
  console.error(logs)
  throw error
} finally {
  await stopServer()
  await rm(dir, { recursive: true, force: true })
}
