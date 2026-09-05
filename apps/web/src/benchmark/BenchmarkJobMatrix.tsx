import {
  AlertTriangle, Check, Clock3, History, LoaderCircle, RefreshCw, Square,
} from 'lucide-react'
import type { BenchmarkJob } from '../types'
import type { BenchmarkJobsApi } from './useBenchmarkJobs'

function dateLabel(value: string) {
  return new Intl.DateTimeFormat('zh-CN', {
    month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit',
  }).format(new Date(value))
}

function StatusMark({ status }: { status: BenchmarkJob['status'] }) {
  if (status === 'completed') return <Check size={13} />
  if (status === 'failed') return <AlertTriangle size={13} />
  if (status === 'running') return <LoaderCircle className="spin" size={13} />
  return <Clock3 size={13} />
}

export function BenchmarkJobMatrix({ jobs }: { jobs: BenchmarkJobsApi }) {
  const active = jobs.activeJob
  const progress = active?.totalRuns
    ? Math.round(active.completedRuns / active.totalRuns * 100)
    : 0

  return (
    <section className="bench-center">
      <header className="bench-center-head">
        <div>
          <span className="eyebrow">EXECUTION LEDGER</span>
          <h2>运行矩阵</h2>
        </div>
        <button type="button" onClick={() => void jobs.refresh()} disabled={jobs.loading}>
          <RefreshCw className={jobs.loading ? 'spin' : ''} size={13} /> REFRESH
        </button>
      </header>

      {jobs.error && <div className="bench-error">
        <AlertTriangle size={14} /><span>{jobs.error}</span>
        <button type="button" onClick={jobs.dismissError}>关闭</button>
      </div>}

      {!active && <div className="bench-zero">
        <div><span>00</span><i /></div>
        <strong>尚无评测任务</strong>
        <p>在左侧组合场景与策略。每个单元格代表同一环境中的一组可复现实验。</p>
      </div>}

      {active && <>
        <article className={`bench-active-job is-${active.status}`}>
          <div className="bench-active-job__top">
            <span><StatusMark status={active.status} /> {active.status.toUpperCase()}</span>
            <small>{active.jobId.slice(-10)}</small>
            {['queued', 'running'].includes(active.status) && <button type="button" onClick={() => void jobs.cancel(active.jobId)}>
              <Square size={10} fill="currentColor" /> CANCEL
            </button>}
          </div>
          <div className="bench-active-job__title">
            <div><span>ACTIVE EXPERIMENT</span><h3>{active.manifest.name}</h3></div>
            <strong>{String(progress).padStart(2, '0')}<small>%</small></strong>
          </div>
          <div className="bench-progress"><i style={{ width: `${progress}%` }} /></div>
          <div className="bench-active-job__stats">
            <span><small>COMPLETE</small><strong>{active.completedRuns} / {active.totalRuns}</strong></span>
            <span><small>SUCCESS</small><strong>{active.successfulRuns}</strong></span>
            <span><small>SCENARIOS</small><strong>{active.manifest.scenarios.length}</strong></span>
            <span><small>STRATEGIES</small><strong>{active.manifest.strategies.length}</strong></span>
          </div>
        </article>

        <div className="bench-matrix-wrap">
          <div className="bench-section-title"><span>MATRIX CELLS</span><small>{active.manifest.repetitions}× REPEAT</small></div>
          <div className="bench-matrix" style={{ '--strategy-count': active.manifest.strategies.length } as React.CSSProperties}>
            <div className="bench-matrix__corner">SCENARIO</div>
            {active.manifest.strategies.map((strategy) => <div className="bench-matrix__strategy" key={strategy.id}>
              <strong>{strategy.id}</strong><small>{strategy.kind.replace('_', ' ')}</small>
            </div>)}
            {active.manifest.scenarios.flatMap((scenario, scenarioIndex) => [
              <div className="bench-matrix__scenario" key={`${scenario.id}-label`}><strong>{scenario.id}</strong><small>SEED {scenario.seed}</small></div>,
              ...active.manifest.strategies.map((strategy, strategyIndex) => {
                const offset = (scenarioIndex * active.manifest.strategies.length + strategyIndex) * active.manifest.repetitions
                const completed = Math.max(0, Math.min(active.manifest.repetitions, active.completedRuns - offset))
                const done = completed === active.manifest.repetitions
                const running = !done && completed >= 0 && active.status === 'running' && active.completedRuns >= offset
                return <div className={`bench-matrix__cell ${done ? 'is-done' : running ? 'is-running' : ''}`} key={`${scenario.id}-${strategy.id}`}>
                  <span>{completed}/{active.manifest.repetitions}</span>
                  <i>{done ? <Check size={12} /> : running ? <LoaderCircle className="spin" size={12} /> : null}</i>
                </div>
              }),
            ])}
          </div>
        </div>
      </>}

      <div className="bench-history">
        <div className="bench-section-title"><span><History size={12} /> HISTORY</span><small>{jobs.jobs.length} JOBS</small></div>
        <div className="bench-history-list">
          {jobs.jobs.map((job) => <button className={job.jobId === jobs.activeJobId ? 'is-active' : ''} type="button" onClick={() => jobs.setActiveJobId(job.jobId)} key={job.jobId}>
            <i className={`is-${job.status}`}><StatusMark status={job.status} /></i>
            <span><strong>{job.manifest.name}</strong><small>{dateLabel(job.createdAt)} · {job.totalRuns} RUNS</small></span>
            <b>{job.status === 'running' ? `${Math.round(job.completedRuns / job.totalRuns * 100)}%` : job.status.toUpperCase()}</b>
          </button>)}
        </div>
      </div>
    </section>
  )
}
