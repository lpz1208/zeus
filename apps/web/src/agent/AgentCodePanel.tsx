import { lazy, Suspense, useEffect, useRef, useState } from 'react'
import { Play, Square, Pause, Download, SkipForward } from 'lucide-react'
import type { Capabilities } from '../algorithm/useAlgorithmLab'
import type { AlgorithmEditorHandle } from '../algorithm/AlgorithmEditor'
import type { AgentSessionApi } from './useAgentSession'
import { navigationRecordLabels } from './algorithmNavigation'
import { BackgroundNavigationPanel } from './BackgroundNavigationPanel'
import { NavigationHistoryPanel } from './NavigationHistoryPanel'
import '../algorithm/algorithm.css'

const Editor = lazy(() => import('../algorithm/AlgorithmEditor'))

export function AgentCodePanel({ agent }: { agent: AgentSessionApi }) {
  const [capabilities, setCapabilities] = useState<Capabilities | null>(null)
  const [error, setError] = useState('')
  const editor = useRef<AlgorithmEditorHandle>(null)
  const latest = useRef(agent)
  const vehicleDone = agent.state?.finished || agent.observation?.state === 'arrived' || agent.observation?.state === 'unroutable'
  latest.current = agent
  useEffect(() => {
    const controller = new AbortController()
    fetch('/api/algorithms/capabilities', { signal: controller.signal }).then(async response => {
      if (!response.ok) throw new Error('无法加载算法方法')
      return response.json() as Promise<Capabilities>
    }).then(value => {
      if (controller.signal.aborted) return
      setCapabilities(value)
      if (!latest.current.source) latest.current.updateSource(value.template)
    }).catch(reason => { if (!controller.signal.aborted) setError(String(reason)) })
    return () => controller.abort()
  }, [])
  useEffect(() => {
    if (agent.codeResult?.line) editor.current?.focusLine(agent.codeResult.line)
  }, [agent.codeResult])
  return <section className="agent-code-panel">
    <p>代码从车辆当前位置规划，读取当前封路与拥堵代价。可手动生成候选，或开启按事件触发的自动导航。</p>
    <div className="agent-navigation-controls">
      <button type="button" disabled={!agent.session || Boolean(agent.busy) || !agent.source.trim() || vehicleDone} onClick={() => void agent.startNavigation()}><Play size={12} /> {agent.navigation.status === 'paused' ? '继续自动导航' : '开启自动导航'}</button>
      <button type="button" disabled={!agent.session || Boolean(agent.busy) || !agent.source.trim() || vehicleDone} onClick={() => void agent.startNavigation(true)}><SkipForward size={12} /> 单次决策</button>
      <button type="button" disabled={agent.navigation.status !== 'running'} onClick={() => agent.pauseNavigation()}><Pause size={12} /> 暂停</button>
      <button type="button" disabled={!['running', 'pausing', 'paused'].includes(agent.navigation.status)} onClick={() => agent.pauseNavigation(true)}><Square size={12} /> 停止</button>
    </div>
    {agent.navigation.phase && <p className="agent-navigation-status" role="status">{agent.navigation.phase}</p>}
    {agent.lastCheckpoint && <p role="status">已保存至 T{agent.lastCheckpoint.tick} · {agent.lastCheckpoint.records} 条决策记录</p>}
    <div className="algorithm-toolbar">
      <span>route(ctx) <small>v{agent.state?.stateVersion ?? '—'}</small></span>
      {agent.codeRunning
        ? <button type="button" onClick={agent.stopCode} disabled={!agent.source}><Square size={12} /> 停止代码</button>
        : <button type="button" disabled={!agent.decisionId || Boolean(agent.busy) || !agent.source.trim()} onClick={() => void agent.planWithSource()}><Play size={12} /> 生成候选</button>}
    </div>
    <div className="algorithm-editor">
      <Suspense fallback={<div className="algorithm-editor-loading">加载编辑器…</div>}>
        <Editor ref={editor} source={agent.source} onChange={agent.updateSource} onRun={() => void agent.planWithSource()} methods={capabilities?.methods ?? []} />
      </Suspense>
    </div>
    <div className="algorithm-budget"><span>5 秒 · 500 万步</span><span>Cmd/Ctrl + Enter</span></div>
    {!agent.decisionId && !vehicleDone && <p>自动导航和单次决策可直接开始；手动生成候选需先“推进至事件”。</p>}
    {error && <p role="alert">{error}</p>}
    {agent.codeResult && <div className="agent-code-result" role="status">
      <strong>{agent.codeResult.phase === 'verified' ? '路径已验证' : agent.codeResult.phase === 'kept' ? '代码选择保持当前路线' : agent.codeResult.phase === 'unreachable' ? '当前无路可达' : '执行未通过'}</strong>
      {agent.codeResult.error && <p>{agent.codeResult.error}</p>}
      <small>{agent.codeResult.expandedNodes} 次查询 · {agent.codeResult.computeMs.toFixed(1)} ms</small>
      {agent.codeResult.logs?.map((line, i) => <pre key={i}>{line}</pre>)}
      {agent.codeResult.phase === 'kept' && agent.decisionId && !agent.busy && <button type="button" onClick={() => void agent.submitAction('keep_route')}>提交保持路线</button>}
    </div>}
    <div className="agent-navigation-records">
      <header><strong>决策记录 · {agent.navigationRecords.length}/200</strong>
        <button type="button" disabled={!agent.navigationRecords.length} onClick={agent.exportNavigationRecords}><Download size={12} /> 导出</button>
        <button type="button" disabled={Boolean(agent.busy) || !agent.navigationRecords.length} onClick={agent.clearNavigationRecords}>清空</button>
      </header>
      {agent.navigationRecords.length === 0 && <p>本次导航的观察、代码、路径和执行结果将显示在这里。每次开始创建独立历史，保存失败时自动暂停。</p>}
      {agent.navigationRecords.length > 10 && <p>显示最近 10 条，导出包含全部记录。</p>}
      {agent.navigationRecords.slice(-10).reverse().map(record => <details key={record.id}>
        <summary><span>v{record.stateVersion} · {record.reason}</span><strong>{navigationRecordLabels[record.status]}</strong></summary>
        <p>{record.simulationTimeS.toFixed(1)} s · {record.computeMs?.toFixed(1) ?? '—'} ms · {record.edges?.length ?? 0} 条道路</p>
        {record.error && <p className="agent-navigation-error">{record.line ? `第 ${record.line} 行：` : ''}{record.error}</p>}
        {record.observation?.vehicle && <p>车辆 {record.observation.vehicle.vehicleId} · {record.observation.vehicle.state} · {record.observation.vehicle.position ? `E${record.observation.vehicle.position.edgeId} / ${record.observation.vehicle.position.offsetM.toFixed(2)} m` : '尚未出发'} · {record.observation.vehicle.routeInvalidated ? '原路线失效' : '原路线有效'}</p>}
        {record.edges && <pre>路径 {record.edges.slice(0, 40).join(' → ')}{record.edges.length > 40 ? ' …（完整路径见导出）' : ''}</pre>}
        {record.logs?.map((line, index) => <pre key={index}>{line}</pre>)}
        <small>CODE {record.codeRevision?.slice(0, 12) ?? '—'} · SNAPSHOT {record.snapshotId?.slice(0, 12) ?? '—'}</small>
      </details>)}
    </div>
    <BackgroundNavigationPanel agent={agent} />
    <NavigationHistoryPanel agent={agent} />
    <details><summary>可用方法与提交规则</summary>
      <p>页面内自动导航从当前边界开始，出发前也会执行代码。关闭页面、修改代码或暂停后不会再发起下一步；已发出的请求会完成。每次最多推进 16 帧再检查暂停，已入队的动作会在下一次推进时生效。代码失败或无路时停止自动循环，不代选路线。</p>
      {capabilities?.methods.map(method => <div className="algorithm-method" key={method.signature}><code>{method.signature}</code><p>{method.description}</p></div>)}
    </details>
  </section>
}
