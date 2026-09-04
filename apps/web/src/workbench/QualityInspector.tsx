import { CarFront, Check, CircleDot, ShieldCheck } from 'lucide-react'
import { useMemo } from 'react'
import type { MapLibraryApi } from '../hooks/useMapLibrary'
import type { MapInteractionApi } from './useMapInteraction'
import { cleaningReasonLabel, formatNumber, severityLabel } from './format'

interface QualityInspectorProps {
  library: MapLibraryApi
  interaction: MapInteractionApi
}

export function QualityInspector({ library, interaction }: QualityInspectorProps) {
  const { activeMap, issueData } = library
  const summary = activeMap?.summary
  const qualityScore = useMemo(() => {
    if (!summary) return 98
    const penalty = summary.fatal * 30 + summary.errors * 8 + summary.warnings * 0.8
    return Math.max(0, Math.round(100 - penalty))
  }, [summary])
  const locatedIssueIndices = useMemo(
    () => new Set(issueData.features.map((feature) => feature.properties.ISSUE_INDEX)),
    [issueData],
  )

  return (
    <>
      <section className="wb-quality-hero">
        <div><span>拓扑健康度</span><strong>{qualityScore}<small>/100</small></strong></div>
        <i className={qualityScore >= 90 ? 'is-good' : qualityScore >= 70 ? 'is-warning' : 'is-danger'}>
          {qualityScore >= 90 ? '结构稳定' : qualityScore >= 70 ? '需要复核' : '高风险'}
        </i>
      </section>
      <div className="metric-matrix">
        <article><span>拓扑节点</span><strong>{formatNumber(summary?.nodes ?? 28)}</strong></article>
        <article><span>有向边</span><strong>{formatNumber(summary?.directedEdges ?? 42)}</strong></article>
        <article><span>转向规则</span><strong>{formatNumber(summary?.turnTransitions ?? 0)}</strong></article>
        <article><span>致命错误</span><strong>{summary?.fatal ?? 0}</strong></article>
      </div>
      {activeMap?.cleaning && (
        <section className="wb-section wb-cleaning">
          <div className="section-label"><CarFront size={14} /><span>OSM 清洗报告</span><small>{activeMap.cleaning.profile.toUpperCase()}</small></div>
          <div className="wb-cleaning__hero">
            <div>
              <span>可行车保留率</span>
              <strong>{Math.round(activeMap.cleaning.outputFeatures / Math.max(1, activeMap.cleaning.inputFeatures) * 100)}<small>%</small></strong>
            </div>
            <div className="wb-cleaning__counts">
              <span><small>INPUT</small>{formatNumber(activeMap.cleaning.inputFeatures)}</span>
              <span><small>OUTPUT</small>{formatNumber(activeMap.cleaning.outputFeatures)}</span>
              <span><small>FILTERED</small>{formatNumber(activeMap.cleaning.filteredFeatures)}</span>
            </div>
          </div>
          <div className="wb-cleaning__bar"><i style={{ width: `${activeMap.cleaning.outputFeatures / Math.max(1, activeMap.cleaning.inputFeatures) * 100}%` }} /></div>
          <div className="wb-cleaning__reasons">
            {Object.entries(activeMap.cleaning.excludedByReason)
              .sort(([, left], [, right]) => right - left)
              .map(([reason, count]) => (
                <div key={reason}><span>{cleaningReasonLabel(reason)}</span><strong>{formatNumber(count)}</strong></div>
              ))}
          </div>
          <div className="wb-cleaning__normalization">
            <span>默认限速 <strong>{formatNumber(activeMap.cleaning.normalization.defaultSpeedApplied)}</strong></span>
            <span>方向修正 <strong>{formatNumber(activeMap.cleaning.normalization.impliedOnewayApplied + activeMap.cleaning.normalization.reverseOnewayNormalized)}</strong></span>
            <span>最短边 <strong>{activeMap.cleaning.options.minLengthMeters} m</strong></span>
          </div>
        </section>
      )}
      <section className="wb-section">
        <div className="section-label"><ShieldCheck size={14} /><span>质检事件</span><small>{activeMap?.issues.length ?? 0}</small></div>
        <div className="wb-issue-list">
          {!activeMap || activeMap.issues.length === 0 ? (
            <div className="wb-allclear"><Check size={16} /><span>没有需要处理的拓扑事件</span></div>
          ) : activeMap.issues.slice(0, 12).map((issue, index) => (
            <button
              type="button"
              className={`wb-issue wb-issue--${issue.severity} ${interaction.focusedIssueIndex === index ? 'is-focused' : ''}`}
              key={`${issue.code}-${index}`}
              disabled={!locatedIssueIndices.has(index)}
              title={locatedIssueIndices.has(index) ? '在地图中定位' : '这是全局问题，没有单一位置'}
              onClick={() => interaction.selectIssue(index)}
            >
              <CircleDot size={12} />
              <div><strong>{issue.code}</strong><span>{issue.message}</span></div>
              <small>{severityLabel(issue.severity)}</small>
            </button>
          ))}
        </div>
      </section>
    </>
  )
}
