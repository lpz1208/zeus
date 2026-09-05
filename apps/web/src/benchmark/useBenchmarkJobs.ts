import { useCallback, useEffect, useMemo, useState } from 'react'
import { api } from '../api'
import type { BenchmarkJob, BenchmarkManifest, BenchmarkReport } from '../types'

const ACTIVE_STATUSES = new Set(['queued', 'running'])

export function useBenchmarkJobs() {
  const [jobs, setJobs] = useState<BenchmarkJob[]>([])
  const [activeJobId, setActiveJobId] = useState<string | null>(null)
  const [report, setReport] = useState<BenchmarkReport | null>(null)
  const [loading, setLoading] = useState(true)
  const [submitting, setSubmitting] = useState(false)
  const [serviceOnline, setServiceOnline] = useState(false)
  const [error, setError] = useState<string | null>(null)

  const activeJob = useMemo(
    () => jobs.find((job) => job.jobId === activeJobId) ?? null,
    [activeJobId, jobs],
  )

  const refresh = useCallback(async (quiet = false) => {
    if (!quiet) setLoading(true)
    try {
      const next = await api.listBenchmarkJobs()
      setJobs(next)
      setServiceOnline(true)
      setError(null)
      setActiveJobId((current) => (
        current && next.some((job) => job.jobId === current)
          ? current
          : next[0]?.jobId ?? null
      ))
    } catch (cause) {
      setServiceOnline(false)
      if (!quiet) setError(cause instanceof Error ? cause.message : '评测任务服务不可用。')
    } finally {
      if (!quiet) setLoading(false)
    }
  }, [])

  useEffect(() => {
    void refresh()
  }, [refresh])

  useEffect(() => {
    if (!jobs.some((job) => ACTIVE_STATUSES.has(job.status))) return
    const timer = window.setInterval(() => void refresh(true), 1200)
    return () => window.clearInterval(timer)
  }, [jobs, refresh])

  useEffect(() => {
    setReport(null)
    if (!activeJob || !['completed', 'cancelled'].includes(activeJob.status)) return
    let cancelled = false
    void api.getBenchmarkReport(activeJob.jobId).then((next) => {
      if (!cancelled) setReport(next)
    }).catch((cause) => {
      if (!cancelled && activeJob.status === 'completed') {
        setError(cause instanceof Error ? cause.message : '评测报告读取失败。')
      }
    })
    return () => { cancelled = true }
  }, [activeJob?.jobId, activeJob?.status])

  const submit = useCallback(async (manifest: BenchmarkManifest) => {
    setSubmitting(true)
    setError(null)
    try {
      const job = await api.createBenchmarkJob(manifest)
      setJobs((current) => [job, ...current.filter((item) => item.jobId !== job.jobId)])
      setActiveJobId(job.jobId)
      setServiceOnline(true)
      return job
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : '无法创建评测任务。')
      throw cause
    } finally {
      setSubmitting(false)
    }
  }, [])

  const cancel = useCallback(async (jobId: string) => {
    setError(null)
    try {
      const job = await api.cancelBenchmarkJob(jobId)
      setJobs((current) => current.map((item) => item.jobId === jobId ? job : item))
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : '取消任务失败。')
    }
  }, [])

  return {
    jobs,
    activeJob,
    activeJobId,
    report,
    loading,
    submitting,
    serviceOnline,
    error,
    setActiveJobId,
    dismissError: () => setError(null),
    refresh,
    submit,
    cancel,
  }
}

export type BenchmarkJobsApi = ReturnType<typeof useBenchmarkJobs>
