import { Pause, Play } from 'lucide-react'
import type { VehicleFrameGeoJSON } from '../../types'
import type { PlaybackApi } from '../usePlayback'
import type { RouteSimApi } from '../useRouteSimulation'
import { formatDuration } from '../format'

interface PlaybackStageProps {
  playback: PlaybackApi
  routeSim: RouteSimApi
  vehicleFrame: VehicleFrameGeoJSON
}

export function PlaybackStage({ playback, routeSim, vehicleFrame }: PlaybackStageProps) {
  const playbackData = routeSim.simResult?.playback
  if (!playbackData) return null
  return (
    <div className="wb-playback">
      <div className="wb-playback__time">
        <span>PLAYBACK</span>
        <strong>{formatDuration(playback.time)}</strong>
        <small>/ {formatDuration(playbackData.duration_s)}</small>
      </div>
      <input
        type="range"
        min="0"
        max={playbackData.duration_s}
        step="0.1"
        value={playback.time}
        onChange={(event) => playback.seek(Number(event.target.value))}
      />
      <div className="wb-playback__controls">
        <button
          type="button"
          aria-label={playback.playing ? '暂停回放' : '播放回放'}
          onClick={playback.togglePlay}
        >
          {playback.playing ? <Pause size={13} fill="currentColor" /> : <Play size={13} fill="currentColor" />}
        </button>
        {([1, 10, 30] as const).map((speed) => (
          <button
            type="button"
            className={playback.speed === speed ? 'is-active' : ''}
            key={speed}
            onClick={() => playback.setSpeed(speed)}
          >{speed}×</button>
        ))}
        <span title="当前时刻已发生的重规划次数">
          {(playbackData.reroutes ?? []).filter((reroute) => reroute.time_s <= playback.time).length} REROUTE · {vehicleFrame.features.length} ACTIVE
        </span>
      </div>
    </div>
  )
}
