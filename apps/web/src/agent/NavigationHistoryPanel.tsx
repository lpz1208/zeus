import { useEffect, useRef, useState } from 'react'
import { Download, History, RefreshCw, RotateCcw, Trash2 } from 'lucide-react'
import type { AgentSessionApi } from './useAgentSession'
import { navigationRecordLabels } from './algorithmNavigation'
import { exportNavigationHistory, navigationHistory, type NavigationHistory, type NavigationHistorySummary } from './navigationHistory'

export function NavigationHistoryPanel({ agent }: { agent: AgentSessionApi }) {
  const [items, setItems] = useState<NavigationHistorySummary[]>([])
  const [selected, setSelected] = useState<NavigationHistory | null>(null)
  const [error, setError] = useState('')
  const [busy, setBusy] = useState(false)
  const [confirmDelete, setConfirmDelete] = useState(false)
  const [refresh, setRefresh] = useState(0)
  const generation = useRef(0)
  const map = agent.historyMapId
  useEffect(() => {
    const current = ++generation.current
    setSelected(null); setConfirmDelete(false); setError(''); setItems([])
    if (!map) return
    setBusy(true)
    navigationHistory.list(map).then(value => {
      if (current === generation.current) setItems(value)
    }).catch(reason => {
      if (current === generation.current) setError(String(reason))
    }).finally(() => { if (current === generation.current) setBusy(false) })
    return () => { generation.current++ }
  }, [map, agent.historyVersion, refresh])

  const load = async (id: string) => {
    if (!map || busy) return
    const current = generation.current
    setBusy(true); setError(''); setConfirmDelete(false)
    try {
      const value = await navigationHistory.get(map, id)
      if (current === generation.current) setSelected(value)
    } catch (reason) { if (current === generation.current) setError(String(reason)) }
    finally { if (current === generation.current) setBusy(false) }
  }
  const remove = async () => {
    if (!map || !selected || busy || agent.busy) return
    if (!confirmDelete) { setConfirmDelete(true); return }
    const current = generation.current
    setBusy(true); setError('')
    try {
      await navigationHistory.remove(map, selected.id)
      if (current === generation.current) setRefresh(value => value + 1)
    } catch (reason) { if (current === generation.current) setError(String(reason)) }
    finally { if (current === generation.current) setBusy(false) }
  }
  const disabled = busy || Boolean(agent.busy)
  return <section className="agent-navigation-records agent-navigation-history" aria-label="导航历史">
    <header><History size={13} /><strong>导航历史 <small>{items.length}/100</small></strong>
      <button type="button" disabled={disabled || !map} onClick={() => setRefresh(value => value + 1)} aria-label="刷新导航历史"><RefreshCw size={12} /></button>
    </header>
    <p>源码、决策记录和车辆边界保存在服务器。恢复会创建暂停的会话，之后可手动继续。</p>
    {error && <p role="alert" className="agent-navigation-error">{error}</p>}
    {busy && <p role="status">正在读取导航历史…</p>}
    {!busy && !items.length && <p>还没有保存的导航。开启自动导航或执行单次决策后会自动保存。</p>}
    <div className="agent-navigation-history-list">
      {items.map(item => <button type="button" key={item.id} disabled={disabled} aria-pressed={selected?.id === item.id} onClick={() => void load(item.id)}>
        <span><strong>T{item.tick}</strong><small>{new Date(item.updatedAt).toLocaleString()}</small></span>
        <span>{item.records} 次决策 · {item.finished ? '已结束' : '可恢复'}</span>
      </button>)}
    </div>
    {selected && <div className="agent-navigation-history-detail">
      <p>保存边界 T{selected.snapshot.tick} · v{selected.snapshot.stateVersion} · {selected.records.length} 条记录</p>
      <div className="agent-navigation-history-actions">
        <button type="button" disabled={disabled} onClick={() => void agent.restoreNavigation(selected)}><RotateCcw size={12} /> 恢复代码与车辆</button>
        <button type="button" disabled={disabled} onClick={() => agent.updateSource(selected.source)}>使用源码</button>
        <button type="button" onClick={() => exportNavigationHistory(selected, `${selected.id}.json`)}><Download size={12} /> 导出</button>
        <button type="button" disabled={disabled} onClick={() => void remove()}><Trash2 size={12} /> {confirmDelete ? '确认删除历史' : '删除'}</button>
        {confirmDelete && <button type="button" onClick={() => setConfirmDelete(false)}>取消</button>}
      </div>
      <details><summary>查看保存的源码</summary><pre>{selected.source}</pre></details>
      <details><summary>最近 {Math.min(10, selected.records.length)} 条决策 · 完整记录可导出</summary>
        {selected.records.slice(-10).reverse().map(record => <div className="agent-history-decision" key={record.id}>
          <strong>v{record.stateVersion} · {record.reason} · {navigationRecordLabels[record.status]}</strong>
          <p>{record.error || `${record.edges?.length ?? 0} 条道路 · ${record.computeMs?.toFixed(1) ?? '—'} ms`}</p>
          {record.logs?.map((log, index) => <pre key={index}>{log}</pre>)}
        </div>)}
      </details>
      <p>只恢复已保存边界。关闭页面时尚未保存的请求可能已执行；恢复的新会话不沿用原会话的后续状态。</p>
    </div>}
  </section>
}
