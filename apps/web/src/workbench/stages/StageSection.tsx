import { ChevronDown } from 'lucide-react'

export type StageStatus = 'idle' | 'active' | 'done' | 'failed'

interface StageSectionProps {
  index: 1 | 2 | 3 | 4
  title: string
  subtitle: string
  status: StageStatus
  open: boolean
  onToggle(): void
}

/** Collapsible stage header with a status dot, shared by all four stages. */
export function StageSection({
  index, title, subtitle, status, open, onToggle,
}: StageSectionProps) {
  return (
    <button
      type="button"
      className={`wb-stage__head is-${status} ${open ? 'is-open' : ''}`}
      aria-expanded={open}
      onClick={onToggle}
    >
      <span className="wb-stage__num">{String(index).padStart(2, '0')}</span>
      <span className="wb-stage__titles">
        <strong>{title}</strong>
        <small>{subtitle}</small>
      </span>
      <i className="wb-stage__dot" aria-label={status} />
      <ChevronDown size={14} className="wb-stage__chevron" />
    </button>
  )
}
