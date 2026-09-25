import { useMemo, useState } from 'react'
import { ArrowDownToLine, Award, BarChart3, DatabaseZap } from 'lucide-react'
import type { BenchmarkAggregate, BenchmarkReport } from '../types'
import type { BenchmarkJobsApi } from './useBenchmarkJobs'

type MetricKey = 'travel_time_s' | 'congestion_exposure_s' | 'decision_wall_ms' | 'route_tool_calls'
  | 'guard_rejections' | 'action_rejections' | 'fallbacks' | 'route_change_requests'
  | 'applied_route_changes' | 'native_route_failures' | 'route_overlap_ratio' | 'route_reversals'

const METRICS: Array<{ key: MetricKey; label: string; unit: string }> = [
  { key: 'travel_time_s', label: '旅行时间', unit: 's' },
  { key: 'congestion_exposure_s', label: '拥堵暴露', unit: 'veh·s' },
  { key: 'decision_wall_ms', label: '决策延迟', unit: 'ms' },
  { key: 'route_tool_calls', label: '工具调用', unit: 'calls' },
  { key: 'guard_rejections', label: 'Guard 阻止', unit: '次' },
  { key: 'action_rejections', label: '提交拒绝', unit: '次' },
  { key: 'fallbacks', label: '保持路线降级', unit: '次' },
  { key: 'route_change_requests', label: '变更路线提交', unit: '次' },
  { key: 'applied_route_changes', label: '实际改道', unit: '次' },
  { key: 'native_route_failures', label: '路线应用失败', unit: '次' },
  { key: 'route_overlap_ratio', label: '剩余路线重叠', unit: '%' },
  { key: 'route_reversals', label: '路线回切', unit: '次' },
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
  const escape = (value: unknown) => `"${(typeof value === 'object' && value !== null ? JSON.stringify(value) : String(value ?? '')).replaceAll('"', '""')}"`
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
            {METRICS.map((item) => <button className={metric === item.key ? 'is-active' : ''} type="button" aria-pressed={metric === item.key} onClick={() => setMetric(item.key)} key={item.key}>{item.label}</button>)}
          </div>
          <div className="bench-bars">
            {report.aggregates.map((aggregate) => {
              const value = aggregate[metric]?.mean
              return <article key={`${aggregate.scenario_id}-${aggregate.strategy_id}`}>
                <div><strong>{aggregate.strategy_id}</strong><small>{aggregate.scenario_id}</small></div>
                <span><i style={{ width: `${value == null ? 0 : Math.max(2, value / maximum * 100)}%` }} /></span>
                <b>{value == null ? '—' : metric === 'route_overlap_ratio'
                  ? `${(value * 100).toFixed(1)}%`
                  : `${value.toFixed(value < 10 ? 2 : 1)} ${definition.unit}`}</b>
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

        <section className="bench-quality" aria-label="决策可靠性">
          <div className="bench-section-title"><span>DECISION RELIABILITY</span><small>PER EPISODE</small></div>
          <p>统计含失败的 Episode；缺失数据以 — 显示，不参与均值。Guard 阻止包含收益不足与冷却限制，提交拒绝包含 HTTP 4xx 或明确拒绝的回执。</p>
          <p>变更与同路提交仅比较有完整道路序列的获准请求；实际应用发生在下一 tick，可能因封路失败。这两项不代表已执行次数或路线抖动。</p>
          <p>实际改道取自仿真执行记录。重叠率按有向道路覆盖长度计算，排除已行驶部分；越低表示路线变化越大，无成功应用时显示 —。</p>
          <p>路线回切统计实际采用的 A→B→A，忽略同路重提交和失败动作，并核对已行驶前缀。无时间窗口；因道路恢复而合理回切也计入，不能仅凭次数判断策略优劣。</p>
          {report.runs.map(run => <details key={run.run_id}>
            <summary><span><strong>{run.strategy_id}</strong><small>{run.scenario_id} · #{run.repetition}</small></span><b>{run.success ? '成功' : '未成功'}</b></summary>
            <dl>
              <div><dt>随机种子</dt><dd>{run.seed}</dd></div>
              <div><dt>源码版本</dt><dd title={run.source_revision ?? ''}>{run.source_revision?.slice(0, 12) ?? '—'}</dd></div>
              <div><dt>场景版本</dt><dd title={run.scenario_revision ?? ''}>{run.scenario_revision?.slice(0, 12) ?? '—'}</dd></div>
              <div><dt>代码决策记录</dt><dd>{run.custom_decisions?.length ?? '—'}（完整记录见 JSON）</dd></div>
              <div><dt>Guard 阻止</dt><dd>{run.guard_rejections ?? '—'}</dd></div>
              <div><dt>提交尝试（含降级）</dt><dd>{run.action_attempts ?? '—'}</dd></div>
              <div><dt>提交拒绝</dt><dd>{run.action_rejections ?? '—'}</dd></div>
              <div><dt>服务错误</dt><dd>{run.action_failures ?? '—'}</dd></div>
              <div><dt>保持路线降级获准</dt><dd>{run.fallbacks ?? '—'}</dd></div>
              <div><dt>模型调用失败</dt><dd>{run.model_failures}</dd></div>
              <div><dt>变更路线提交</dt><dd>{run.route_change_requests ?? '—'}</dd></div>
              <div><dt>相同路线提交</dt><dd>{run.unchanged_route_requests ?? '—'}</dd></div>
              <div><dt>路线应用成功</dt><dd>{run.native_route_applications ?? '—'}</dd></div>
              <div><dt>路线应用失败</dt><dd>{run.native_route_failures ?? '—'}</dd></div>
              <div><dt>实际改道</dt><dd>{run.applied_route_changes ?? '—'}</dd></div>
              <div><dt>同路应用</dt><dd>{run.applied_unchanged_routes ?? '—'}</dd></div>
              <div><dt>平均剩余路线重叠</dt><dd>{run.route_overlap_ratio == null ? '—' : `${(run.route_overlap_ratio * 100).toFixed(1)}%`}</dd></div>
              <div><dt>路线回切（A→B→A）</dt><dd>{run.route_reversals ?? '—'}</dd></div>
            </dl>
            {run.error && <p className="bench-quality-error">{run.error}</p>}
          </details>)}
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
