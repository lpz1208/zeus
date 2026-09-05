import { ArrowLeft, Beaker, CircleDot, Layers3 } from 'lucide-react'
import type { MapRecord } from '../types'
import { BenchmarkComposer } from './BenchmarkComposer'
import { BenchmarkJobMatrix } from './BenchmarkJobMatrix'
import { BenchmarkReportPanel } from './BenchmarkReportPanel'
import { useBenchmarkJobs } from './useBenchmarkJobs'
import './benchmark.css'

interface BenchmarkWorkbenchProps {
  maps: MapRecord[]
  activeMap: MapRecord | null
  onExit(): void
}

export function BenchmarkWorkbench({ maps, activeMap, onExit }: BenchmarkWorkbenchProps) {
  const jobs = useBenchmarkJobs()
  const running = jobs.jobs.filter((job) => ['queued', 'running'].includes(job.status)).length

  return (
    <main className="bench-shell">
      <header className="bench-topbar">
        <button className="bench-back" type="button" onClick={onExit}>
          <ArrowLeft size={14} /> 地图工作台
        </button>
        <div className="bench-brand">
          <span><Beaker size={17} /></span>
          <div><strong>ZEUS</strong><small>BENCHMARK LAB</small></div>
        </div>
        <div className="bench-protocol">
          {['DESIGN', 'QUEUE', 'EXECUTE', 'COMPARE'].map((item, index) => <span className={index === 0 && !jobs.activeJob ? 'is-active' : jobs.activeJob && index <= (jobs.activeJob.status === 'completed' ? 3 : jobs.activeJob.status === 'running' ? 2 : 1) ? 'is-active' : ''} key={item}>
            <i>{String(index + 1).padStart(2, '0')}</i>{item}
          </span>)}
        </div>
        <div className="bench-runtime">
          <i className={jobs.serviceOnline ? 'is-online' : ''} />
          <span><small>JOB SERVICE</small><strong>{jobs.serviceOnline ? 'ONLINE' : 'OFFLINE'}</strong></span>
          <b>{running} ACTIVE</b>
        </div>
      </header>

      <BenchmarkComposer
        maps={maps}
        activeMap={activeMap}
        submitting={jobs.submitting}
        serviceOnline={jobs.serviceOnline}
        onSubmit={jobs.submit}
      />
      <BenchmarkJobMatrix jobs={jobs} />
      <BenchmarkReportPanel jobs={jobs} />

      <footer className="bench-statusbar">
        <span><CircleDot size={9} /> REPRODUCIBLE RUN MATRIX</span>
        <span>STORE <strong>SQLITE / WAL</strong></span>
        <span>EXECUTION <strong>BOUNDED WORKERS</strong></span>
        <span className="bench-statusbar__right"><Layers3 size={10} /> ENVIRONMENT-AUTHORITATIVE METRICS</span>
      </footer>
    </main>
  )
}
