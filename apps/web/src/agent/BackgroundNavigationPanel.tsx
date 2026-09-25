import { useEffect, useRef, useState } from 'react'
import type { AgentSessionApi } from './useAgentSession'

interface Job {
  id: string
  sessionId: string
  status: string
  tick: number
  records: number
  error?: string
}
const labels: Record<string, string> = { running: '运行中', pausing: '正在暂停', paused: '已暂停', stopped: '已停止', completed: '已完成', error: '失败' }

export function BackgroundNavigationPanel({ agent }: { agent: AgentSessionApi }) {
  const [jobs, setJobs] = useState<Job[]>([])
  const [error, setError] = useState('')
  const [pending, setPending] = useState(false)
  const latest = useRef(agent)
  latest.current = agent
  const mapId = agent.historyMapId
  const base = `/api/maps/${encodeURIComponent(mapId ?? '')}/agent`
  const request = async (path: string, method = 'GET', body?: unknown) => {
    const response = await fetch(`${base}${path}`, { method, ...(body === undefined ? {} : { headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) }) })
    const value = await response.json()
    if (!response.ok) throw new Error(value.error || '后台导航请求失败')
    return value
  }
  useEffect(() => {
    setJobs([]); setError('')
    if (!mapId) { setJobs([]); return }
    let active = true
    let timer: ReturnType<typeof setTimeout>
    const refresh = async () => {
      try {
        const response = await fetch(`/api/maps/${encodeURIComponent(mapId)}/agent/navigation-jobs`)
        const values = await response.json()
        if (!response.ok) throw new Error(values.error || '无法加载后台任务')
        if (active) {
          setJobs(values)
          const current = (values as Job[]).find(job => job.sessionId === latest.current.session)
          if (current && (!latest.current.busy || latest.current.busy === 'background')) await latest.current.attachBackgroundSession(current.sessionId)
        }
      } catch (reason) { if (active) setError(String(reason)) }
      finally { if (active) timer = setTimeout(refresh, 2000) }
    }
    void refresh()
    return () => { active = false; clearTimeout(timer) }
  }, [mapId])
  const act = async (work: () => Promise<unknown>) => {
    setPending(true); setError('')
    try { await work(); setJobs(await request('/navigation-jobs')) }
    catch (reason) { setError(String(reason)) }
    finally { setPending(false) }
  }
  return <section className="agent-background-navigation">
    <header><strong>后台导航</strong><button type="button" disabled={!agent.session || Boolean(agent.busy) || pending || !agent.source.trim() || jobs.some(job => job.sessionId === agent.session && ['running', 'pausing', 'paused'].includes(job.status))} onClick={() => void act(async () => { const job = await request(`/sessions/${agent.session}/navigation-jobs`, 'POST', { source: agent.source, vehicleId: agent.agentVehicleId }); await agent.attachBackgroundSession(job.sessionId) })}>交给后台运行</button></header>
    <p>关闭页面后继续运行。暂停保留会话，停止后释放手动操作；控制在当前决策与保存完成后生效。服务重启后需手动恢复。</p>
    {error && <p role="alert">{error}</p>}
    {jobs.map(job => <article key={job.id}>
      <strong>{labels[job.status] ?? job.status} · T{job.tick} · {job.records} 次决策</strong>
      <small>{job.id}</small>
      {job.error && <p role="alert">{job.error}</p>}
      <div className="agent-navigation-controls">
        <button type="button" disabled={pending || Boolean(agent.busy && agent.busy !== 'background')} onClick={() => void act(async () => { const detail = await request(`/navigation-jobs/${job.id}`); await agent.attachBackgroundSession(detail.sessionId, detail.source) })}>连接车辆</button>
        {job.status === 'running' && <button type="button" disabled={pending} onClick={() => void act(() => request(`/navigation-jobs/${job.id}`, 'POST', { action: 'pause' }))}>暂停</button>}
        {job.status === 'paused' && <button type="button" disabled={pending} onClick={() => void act(async () => { const detail = await request(`/navigation-jobs/${job.id}`, 'POST', { action: 'resume' }); await agent.attachBackgroundSession(detail.sessionId, detail.source) })}>继续 / 恢复</button>}
        {['running', 'pausing', 'paused'].includes(job.status) && <button type="button" disabled={pending} onClick={() => void act(() => request(`/navigation-jobs/${job.id}`, 'POST', { action: 'stop' }))}>停止</button>}
        <button type="button" disabled={pending} onClick={() => void act(async () => { const detail = await request(`/navigation-jobs/${job.id}`); const url = URL.createObjectURL(new Blob([JSON.stringify(detail, null, 2)], { type: 'application/json' })); const link = document.createElement('a'); link.href = url; link.download = `${job.id}.json`; link.click(); URL.revokeObjectURL(url) })}>导出</button>
        {['completed', 'stopped', 'error'].includes(job.status) && <button type="button" disabled={pending} onClick={() => void act(() => request(`/navigation-jobs/${job.id}`, 'DELETE'))}>删除记录</button>}
      </div>
    </article>)}
  </section>
}
