import { useMemo, useState } from 'react'
import { ArrowDownToLine, Award, BarChart3, DatabaseZap } from 'lucide-react'
import type { BenchmarkAggregate, BenchmarkReport } from '../types'
import type { BenchmarkJobsApi } from './useBenchmarkJobs'

type MetricKey = 'travel_time_s' | 'congestion_exposure_s' | 'decision_wall_ms' | 'route_tool_calls'

const METRICS: Array<{ key: MetricKey; label: string; unit: string }> = [
  { key: 'travel_time_s', label: '旅行时间', unit: 's' },
  { key: 'congestion_exposure_s', label: '拥堵暴露', unit: 'veh·s' },
  { key: 'decision_wall_ms', label: '决策延迟', unit: 'ms' },
  { key: 'route_tool_calls', label: '工具调用', unit: 'calls' },
]

function download(name: string, body: string, type: string) {
  const url = URL.createObjectURL(new Blob([body], { type }))
  const anchor = document.createElement('a')
  anchor.href = url
  anchor.download = name
  anchor.click()
  URL.revokeObjectURL(url)
}

function downloadJson(report: BenchmarkReport) {
  download(`${report.name}.json`, JSON.stringify(report, null, 2), 'application/json')
}

function downloadCsv(report: BenchmarkReport) {
  if (!report.runs.length) return
  const keys = Object.keys(report.runs[0]) as Array<keyof typeof report.runs[0]>
  const escape = (value: unknown) => `"${String(value ?? '').replaceAll('"', '""')}"`
  const rows = [keys.join(','), ...report.runs.map((run) => keys.map((key) => escape(run[key])).join(','))]
  download(`${report.name}-runs.csv`, rows.join('\n'), 'text/csv;charset=utf-8')
}

function bestAggregate(aggregates: BenchmarkAggregate[]) {
  return [...aggregates].sort((a, b) => (
    b.success_rate - a.success_rate
    || (a.travel_time_s?.mean ?? Infinity) - (b.travel_time_s?.mean ?? Infinity)
    || (a.decision_wall_ms?.mean ?? Infinity) - (b.decision_wall_ms?.mean ?? Infinity)
  ))[0]
}

export function BenchmarkReportPanel({ jobs }: { jobs: BenchmarkJobsApi }) {
  const [metric, setMetric] = useState<MetricKey>('travel_time_s')
  const report = jobs.report
  const active = jobs.activeJob
  const definition = METRICS.find((item) => item.key === metric)!
  const maximum = useMemo(() => Math.max(
    1,
    ...(report?.aggregates.map((item) => item[metric]?.mean ?? 0) ?? []),
  ), [metric, report])
  const winner = report ? bestAggregate(report.aggregates) : null

  return (
    <aside className="bench-report">
      <div className="bench-heading">
        <div><span className="eyebrow">COMPARATIVE EVIDENCE</span><h2>结果对照</h2></div>
        <BarChart3 size={19} />
      </div>

      {!report && <div className="bench-report-empty">
        <DatabaseZap size={24} />
        <strong>{active ? '等待实验结果' : '选择历史任务'}</strong>
        <p>{active?.status === 'failed' ? active.error : '任务完成后，这里会显示跨策略聚合指标与逐次运行证据。'}</p>
      </div>}

      {report && <>
        <section className="bench-winner">
          <div><Award size={16} /><span>LEADING STRATEGY</span></div>
          <strong>{winner?.strategy_id ?? '—'}</strong>
          <p>{winner ? `${winner.scenario_id} · 成功率 ${(winner.success_rate * 100).toFixed(0)}% · ${winner.successes}/${winner.runs} runs` : '没有可比较结果'}</p>
        </section>

        <div className="bench-report-actions">
          <button type="button" onClick={() => downloadJson(report)}><ArrowDownToLine size={12} /> JSON</button>
          <button type="button" disabled={!report.runs.length} onClick={() => downloadCsv(report)}><ArrowDownToLine size={12} /> CSV</button>
          <span>FORMAT V{report.format_version}</span>
        </div>

        <section className="bench-chart-section">
          <div className="bench-metric-tabs">
            {METRICS.map((item) => <button className={metric === item.key ? 'is-active' : ''} type="button" onClick={() => setMetric(item.key)} key={item.key}>{item.label}</button>)}
          </div>
          <div className="bench-bars">
            {report.aggregates.map((aggregate) => {
              const value = aggregate[metric]?.mean
              return <article key={`${aggregate.scenario_id}-${aggregate.strategy_id}`}>
                <div><strong>{aggregate.strategy_id}</strong><small>{aggregate.scenario_id}</small></div>
                <span><i style={{ width: `${value == null ? 0 : Math.max(2, value / maximum * 100)}%` }} /></span>
                <b>{value == null ? '—' : `${value.toFixed(value < 10 ? 2 : 1)} ${definition.unit}`}</b>
              </article>
            })}
          </div>
        </section>

        <section className="bench-score-table">
          <div className="bench-section-title"><span>AGGREGATE SCORECARD</span><small>MEAN / P95</small></div>
          <div className="bench-score-table__head"><span>STRATEGY</span><span>SUCCESS</span><span>TRAVEL</span><span>LATENCY</span></div>
          {report.aggregates.map((aggregate) => <div className="bench-score-table__row" key={`${aggregate.scenario_id}-${aggregate.strategy_id}`}>
            <span><strong>{aggregate.strategy_id}</strong><small>{aggregate.scenario_id}</small></span>
            <b>{(aggregate.success_rate * 100).toFixed(0)}%</b>
            <b>{aggregate.travel_time_s ? `${aggregate.travel_time_s.mean.toFixed(0)}s` : '—'}<small>{aggregate.travel_time_s ? `P95 ${aggregate.travel_time_s.p95.toFixed(0)}` : ''}</small></b>
            <b>{aggregate.decision_wall_ms ? `${aggregate.decision_wall_ms.mean.toFixed(1)}ms` : '—'}<small>{aggregate.decision_wall_ms ? `P95 ${aggregate.decision_wall_ms.p95.toFixed(1)}` : ''}</small></b>
          </div>)}
        </section>

        <section className="bench-report-meta">
          <span><small>WALL TIME</small><strong>{report.wall_seconds.toFixed(2)} s</strong></span>
          <span><small>EPISODES</small><strong>{report.runs.length}</strong></span>
          <span><small>STATUS</small><strong>{report.cancelled ? 'PARTIAL' : 'COMPLETE'}</strong></span>
        </section>
      </>}
    </aside>
  )
}
