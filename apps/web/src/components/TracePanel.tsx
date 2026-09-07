import { Pause, Play } from 'lucide-react'
import type { SearchTrace } from '../types'
import type { TracePlaybackApi } from '../hooks/useTracePlayback'

interface TracePanelProps {
  trace: SearchTrace | null
  playback: TracePlaybackApi
}

/**
 * Replay console for a recorded search settle sequence: play/pause, a
 * progress scrubber over the settle ordinals, and the reveal counter. Shared
 * by the route workbench and the agent workbench.
 */
export function TracePanel({ trace, playback }: TracePanelProps) {
  if (!trace || trace.steps.length === 0) return null
  const revealed = Math.floor(playback.progress * trace.steps.length)
  const lastOrder = trace.steps[Math.max(0, revealed - 1)]?.order ?? 0

  return (
    <div className="wb-trace" data-testid="search-trace-panel">
      <div className="wb-trace-head">
        <span className="wb-trace-title">搜索过程</span>
        {trace.sampled && <span className="wb-trace-sampled">已采样</span>}
      </div>
      <div className="wb-trace-controls">
        <button
          type="button"
          className="wb-trace-toggle"
          aria-label={playback.playing ? '暂停搜索回放' : '播放搜索回放'}
          onClick={playback.togglePlay}
        >
          {playback.playing ? <Pause size={16} /> : <Play size={16} />}
        </button>
        <input
          className="wb-trace-range"
          type="range"
          min={0}
          max={1}
          step={1 / trace.steps.length}
          value={playback.progress}
          aria-label="搜索回放进度"
          onChange={(event) => playback.seek(Number(event.target.value))}
        />
        <div className="wb-trace-speeds">
          {([1, 4, 16] as const).map((option) => (
            <button
              key={option}
              type="button"
              className={`wb-trace-speed ${playback.speed === option ? 'is-active' : ''}`}
              onClick={() => playback.setSpeed(option)}
            >
              {option}×
            </button>
          ))}
        </div>
      </div>
      <div className="wb-trace-meta">
        <span>step {lastOrder}/{trace.stepCount}</span>
      </div>
    </div>
  )
}
