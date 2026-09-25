import test from 'node:test'
import assert from 'node:assert/strict'
import { runAlgorithmNavigation } from '../src/agent/algorithmNavigation.ts'

function fixture({ keep = false, failure = false, adopt = true, pauseOn = '' } = {}) {
  let active = true
  let version = 1
  let tick = 0
  let decision = ''
  let routeId = 0
  let queued = false
  const calls = []
  const records = new Map()
  const observation = () => ({ stateVersion: version, tick, simulationTimeS: tick, paused: true, finished: tick >= 4, decisionId: decision,
    agents: [{ vehicleId: 0, state: tick >= 4 ? 'arrived' : tick ? 'driving' : 'waiting', routeId }] })
  const options = {
    runId: 'test', source: 'def route(ctx):\n    return ctx.keep()', vehicleId: 0, once: false, maxDecisions: 20,
    active: () => active,
    request: async (path, body) => {
      calls.push({ path, body })
      if (pauseOn && pauseOn === path) active = false
      if (!path) return observation()
      if (path === '/decisions') {
        assert.equal(body.basedOnStateVersion, version)
        decision = `d${version}`
        return { state: observation(), decisionId: decision }
      }
      if (path === '/algorithms/plan') {
        assert.equal(body.basedOnStateVersion, version)
        return { result: { ok: !failure, phase: failure ? 'execution' : keep ? 'kept' : 'verified', error: failure ? '预算耗尽' : undefined,
          observation: { mode: 'vehicle', stateVersion: version, vehicle: { vehicleId: 0 } }, logs: ['选择原因'], codeRevision: 'code', snapshotId: 'snapshot' },
          candidate: keep || failure ? undefined : { ok: true, candidateId: 'candidate', basedOnStateVersion: version, edges: [1, 2] } }
      }
      if (path === '/actions') {
        assert.equal(body.decisionId, decision)
        assert.equal(body.basedOnStateVersion, version)
        queued = body.kind === 'commit_route'
        decision = ''
        return { accepted: true }
      }
      if (path === '/step') {
        assert.equal(body.basedOnStateVersion, version)
        assert.equal(decision, '', 'cannot advance through an unresolved barrier')
        assert.ok(!body.untilEvent || body.maxTicks === 16)
        if (queued && adopt) routeId++
        queued = false
        tick++
        version++
        if (body.untilEvent) decision = `d${version}`
        return { state: observation(), decisionId: decision }
      }
      throw new Error(path)
    },
    onObservation: async () => {}, onPlan: () => {}, onRecord: value => records.set(value.id, value), onPhase: () => {}, yieldBetween: async () => {},
  }
  return { options, calls, records }
}

test('continuous mode plans before departure and acts at later events', async () => {
  const f = fixture()
  assert.equal(await runAlgorithmNavigation(f.options), 'complete')
  const mutations = f.calls.filter(call => call.path).map(call => call.path)
  assert.deepEqual(mutations.slice(0, 4), ['/decisions', '/algorithms/plan', '/actions', '/step'])
  assert.ok(f.calls.filter(call => call.path === '/actions').length >= 2)
  assert.ok([...f.records.values()].every(record => record.status === 'applied' && record.logs[0] === '选择原因'))
})

test('single decision keeps the route and pauses after exactly one tick', async () => {
  const f = fixture({ keep: true })
  f.options.once = true
  assert.equal(await runAlgorithmNavigation(f.options), 'once')
  assert.equal(f.calls.filter(call => call.path === '/step').length, 1)
  assert.equal(f.calls.find(call => call.path === '/actions').body.kind, 'keep_route')
  assert.equal([...f.records.values()][0].status, 'kept')
})

test('pause while Python runs prevents committing its result', async () => {
  const f = fixture({ pauseOn: '/algorithms/plan' })
  assert.equal(await runAlgorithmNavigation(f.options), 'paused')
  assert.ok(!f.calls.some(call => call.path === '/actions' || call.path === '/step'))
  assert.equal([...f.records.values()][0].status, 'cancelled')
})

test('pause during commit leaves a truthful queued record without advancing', async () => {
  const f = fixture({ pauseOn: '/actions' })
  assert.equal(await runAlgorithmNavigation(f.options), 'paused')
  assert.ok(!f.calls.some(call => call.path === '/step'))
  assert.equal([...f.records.values()][0].status, 'queued')
})

test('code failure records its observation and never submits or advances', async () => {
  const f = fixture({ failure: true })
  await assert.rejects(runAlgorithmNavigation(f.options), /预算耗尽/)
  assert.ok(!f.calls.some(call => call.path === '/actions' || call.path === '/step'))
  assert.equal([...f.records.values()][0].observation.stateVersion, 1)
  assert.equal([...f.records.values()][0].status, 'error')
})

test('a closure preventing route adoption stops the loop after its application tick', async () => {
  const f = fixture({ adopt: false })
  await assert.rejects(runAlgorithmNavigation(f.options), /路径未被采用/)
  assert.equal(f.calls.filter(call => call.path === '/step').length, 1)
  assert.equal([...f.records.values()][0].status, 'error')
})

test('record limit stops immediately without deleting old decisions or extra stepping', async () => {
  const f = fixture()
  f.options.maxDecisions = 1
  assert.equal(await runAlgorithmNavigation(f.options), 'limit')
  assert.equal(f.calls.filter(call => call.path === '/step').length, 1)
})

test('a failed initial checkpoint prevents all vehicle mutations', async () => {
  const f = fixture()
  f.options.checkpoint = async () => { throw new Error('storage full') }
  await assert.rejects(runAlgorithmNavigation(f.options), /storage full/)
  assert.ok(f.calls.every(call => call.path === ''))
})

test('a failed checkpoint after adoption preserves the applied record and stops advancing', async () => {
  const f = fixture()
  let saves = 0
  f.options.checkpoint = async () => { if (++saves === 2) throw new Error('storage offline') }
  await assert.rejects(runAlgorithmNavigation(f.options), /storage offline/)
  assert.equal(saves, 2, 'do not retry an ambiguously acknowledged write')
  assert.equal(f.calls.filter(call => call.path === '/step').length, 1)
  assert.equal([...f.records.values()][0].status, 'applied')
})

test('pause during commit checkpoints queued state without applying another tick', async () => {
  const f = fixture({ pauseOn: '/actions' })
  const checkpoints = []
  f.options.checkpoint = async () => checkpoints.push([...f.records.values()].map(record => record.status))
  assert.equal(await runAlgorithmNavigation(f.options), 'paused')
  assert.deepEqual(checkpoints, [[], ['queued']])
  assert.ok(!f.calls.some(call => call.path === '/step'))
})

test('code failures are included in the final checkpoint', async () => {
  const f = fixture({ failure: true })
  const checkpoints = []
  f.options.checkpoint = async () => checkpoints.push([...f.records.values()].map(record => record.status))
  await assert.rejects(runAlgorithmNavigation(f.options), /预算耗尽/)
  assert.deepEqual(checkpoints, [[], ['error']])
})
