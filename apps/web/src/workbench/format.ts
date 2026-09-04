import type { Severity } from '../types'

export function formatNumber(value: number): string {
  return new Intl.NumberFormat('zh-CN').format(value)
}

export function severityLabel(severity: Severity): string {
  return { fatal: '致命', error: '错误', warning: '警告', info: '信息' }[severity]
}

export function formatDuration(seconds: number): string {
  const total = Math.round(seconds)
  const hours = Math.floor(total / 3600)
  const minutes = Math.floor((total % 3600) / 60)
  const rest = total % 60
  const mm = String(minutes).padStart(2, '0')
  const ss = String(rest).padStart(2, '0')
  return hours > 0 ? `${hours}:${mm}:${ss}` : `${mm}:${ss}`
}

export function routeFailureLabel(reason?: string): string {
  return {
    unmatched_origin: '起点未能吸附到道路',
    unmatched_destination: '终点未能吸附到道路',
    unreachable: '起终点之间不可达',
    empty_map: '地图为空',
  }[reason ?? ''] ?? '路径规划失败'
}

export function cleaningReasonLabel(reason: string): string {
  return {
    access_restricted: '访问受限',
    non_drivable_class: '非机动车道路',
    service_disabled: '服务道路',
    track_disabled: '土路',
    too_short: '短小几何',
    duplicate_geometry: '重复几何',
    empty_geometry: '空几何',
    unsupported_geometry: '不支持的几何',
    missing_highway: '缺少 highway',
  }[reason] ?? reason.replaceAll('_', ' ')
}

export function formatReferenceValue(value: unknown): string {
  if (value === null || value === undefined) return 'NULL'
  const text = typeof value === 'object' ? JSON.stringify(value) : String(value)
  return text.length > 160 ? `${text.slice(0, 157)}…` : text
}
