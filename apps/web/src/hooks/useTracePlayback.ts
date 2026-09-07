import { useEffect, useState } from 'react'
import type { SearchTrace } from '../types'

export interface TracePlaybackApi {
  /** Fraction of steps revealed, in [0, 1]. */
  progress: number
  playing: boolean
  speed: 1 | 4 | 16
  stepCount: number
  setSpeed(speed: 1 | 4 | 16): void
  /** Restarts from zero when the animation sits at the end. */
  togglePlay(): void
  seek(progress: number): void
  stop(): void
}

/**
 * Playback clock for a recorded search settle trace. The timeline is the
 * settle ordinal (not wall time): progress p reveals steps[:floor(p * n)].
 * Nulling or rotating the trace resets the clock.
 */
export function useTracePlayback(trace: SearchTrace | null): TracePlaybackApi {
  const [progress, setProgress] = useState(0)
  const [playing, setPlaying] = useState(false)
  const [speed, setSpeedState] = useState<1 | 4 | 16>(1)
  const stepCount = trace?.steps.length ?? 0

  useEffect(() => {
    setProgress(0)
    setPlaying(false)
  }, [trace])

  useEffect(() => {
    if (!playing || !trace || trace.steps.length === 0) return
    const timer = window.setInterval(() => {
      // Reveal ~3% of the sequence per tick at 1x; the interval keeps the
      // animation smooth regardless of trace length.
      const increment = (0.03 * speed) / 1
      setProgress((current) => Math.min(1, current + increment))
    }, 100)
    return () => window.clearInterval(timer)
  }, [playing, speed, trace])

  useEffect(() => {
    if (trace && trace.steps.length > 0 && progress >= 1) {
      setPlaying(false)
    }
  }, [progress, trace])

  const togglePlay = () => {
    if (!playing && stepCount > 0 && progress >= 1) setProgress(0)
    setPlaying((current) => !current)
  }

  const seek = (next: number) => {
    setProgress(Math.max(0, Math.min(1, next)))
    setPlaying(false)
  }

  return {
    progress, playing, speed, stepCount,
    setSpeed: setSpeedState,
    togglePlay, seek, stop: () => setPlaying(false),
  }
}
