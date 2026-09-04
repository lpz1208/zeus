import { useEffect, useState } from 'react'
import type { SimulateResponse } from '../types'

export interface PlaybackApi {
  time: number
  playing: boolean
  speed: 1 | 10 | 30
  duration: number
  setSpeed(speed: 1 | 10 | 30): void
  /** Restarts from zero when the console sits at the end. */
  togglePlay(): void
  seek(time: number): void
  stop(): void
}

/**
 * Playback clock for a simulation result. Nulling or rotating simResult
 * resets the clock, which is what every route/scenario change relies on.
 */
export function usePlayback(simResult: SimulateResponse | null): PlaybackApi {
  const [time, setTime] = useState(0)
  const [playing, setPlaying] = useState(false)
  const [speed, setSpeedState] = useState<1 | 10 | 30>(1)
  const duration = simResult?.playback?.duration_s ?? 0

  useEffect(() => {
    setTime(0)
    setPlaying(false)
  }, [simResult])

  useEffect(() => {
    if (!playing || !simResult?.playback) return
    const total = simResult.playback.duration_s
    const timer = window.setInterval(() => {
      setTime((current) => Math.min(total, current + speed * 0.1))
    }, 100)
    return () => window.clearInterval(timer)
  }, [playing, speed, simResult])

  useEffect(() => {
    if (simResult?.playback && time >= simResult.playback.duration_s) {
      setPlaying(false)
    }
  }, [time, simResult])

  const togglePlay = () => {
    if (!playing && duration > 0 && time >= duration) setTime(0)
    setPlaying((current) => !current)
  }

  const seek = (next: number) => {
    setTime(next)
    setPlaying(false)
  }

  return {
    time, playing, speed, duration,
    setSpeed: setSpeedState,
    togglePlay, seek, stop: () => setPlaying(false),
  }
}
