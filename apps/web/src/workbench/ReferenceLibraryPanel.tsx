import {
  AlertTriangle,
  Eye,
  EyeOff,
  Layers3,
  LocateFixed,
  Save,
  Settings2,
  Trash2,
  X,
} from 'lucide-react'
import { useState } from 'react'
import type { ReferenceLayerStyle } from '../types'
import type { ReferenceLibraryApi } from '../hooks/useReferenceLayers'
import { formatNumber } from './format'

interface ReferenceLibraryPanelProps {
  references: ReferenceLibraryApi
  /** Caller clears map selections when a layer hides or disappears. */
  onVisibilityToggle(id: string): void
  onLayerDeleted(id: string): void
}

interface EditorDraft {
  name: string
  style: ReferenceLayerStyle
}

export function ReferenceLibraryPanel({
  references, onVisibilityToggle, onLayerDeleted,
}: ReferenceLibraryPanelProps) {
  const [editingId, setEditingId] = useState<string | null>(null)
  const [draft, setDraft] = useState<EditorDraft | null>(null)
  const { layers, views } = references

  const openEditor = (id: string | null) => {
    if (id === null || editingId === id) {
      setEditingId(null)
      setDraft(null)
      return
    }
    const record = layers.find((item) => item.id === id)
    if (!record) return
    setEditingId(id)
    setDraft({ name: record.name, style: { ...record.style } })
  }

  const save = async () => {
    if (!editingId || !draft) return
    const updated = await references.saveLayer(editingId, draft)
    if (updated) {
      setEditingId(null)
      setDraft(null)
    }
  }

  return (
    <section className="wb-lib">
      <div className="section-label">
        <Layers3 size={14} />
        <span>参考图层</span>
        <small>{layers.length}</small>
      </div>
      {references.error && (
        <div className="wb-error is-compact" role="alert">
          <AlertTriangle size={13} />
          <span>{references.error}</span>
          <button type="button" aria-label="关闭错误" onClick={references.dismissError}><X size={12} /></button>
        </div>
      )}
      {layers.length === 0 ? (
        <div className="wb-lib-empty is-slim">
          <Layers3 size={18} />
          <span>点或面数据将在这里作为地图上下文叠加</span>
        </div>
      ) : views.map(({ record, visible }) => (
        <div className={`wb-ref-row ${visible ? 'is-visible' : ''}`} key={record.id}>
          <div className="wb-ref-row__main">
            <button
              type="button"
              className="wb-ref-row__toggle"
              aria-pressed={visible}
              onClick={() => {
                references.toggleVisibility(record.id)
                onVisibilityToggle(record.id)
              }}
            >
              <i style={{ '--layer-color': record.style.color } as React.CSSProperties} />
              <span>
                <strong>{record.name}</strong>
                <small>{record.geometry} · {formatNumber(record.featureCount)} FEATURES</small>
              </span>
              {visible ? <Eye size={14} /> : <EyeOff size={14} />}
            </button>
            <button
              type="button"
              className="wb-ref-row__focus"
              aria-label={`定位到${record.name}`}
              title="定位到图层"
              onClick={() => references.focusLayer(record.id)}
            >
              <LocateFixed size={14} />
            </button>
            <button
              type="button"
              className="wb-ref-row__edit"
              aria-label={`编辑${record.name}`}
              aria-expanded={editingId === record.id}
              onClick={() => openEditor(record.id)}
            >
              <Settings2 size={14} />
            </button>
          </div>
          {editingId === record.id && draft && (
            <div className="wb-ref-editor">
              <label className="field-control">
                <span>图层名称</span>
                <input
                  value={draft.name}
                  onChange={(event) => setDraft({ ...draft, name: event.target.value })}
                />
              </label>
              <div className="wb-ref-editor__style">
                <input
                  type="color"
                  aria-label="图层颜色"
                  value={draft.style.color}
                  onChange={(event) => setDraft({
                    ...draft,
                    style: { ...draft.style, color: event.target.value },
                  })}
                />
                <label>
                  <span>OPACITY {Math.round(draft.style.opacity * 100)}%</span>
                  <input
                    type="range"
                    min="0.05"
                    max="1"
                    step="0.05"
                    value={draft.style.opacity}
                    onChange={(event) => setDraft({
                      ...draft,
                      style: { ...draft.style, opacity: Number(event.target.value) },
                    })}
                  />
                </label>
              </div>
              <div className="wb-ref-editor__actions">
                <button
                  type="button"
                  className="wb-ref-editor__save"
                  disabled={references.mutating || !draft.name.trim()}
                  onClick={() => void save()}
                ><Save size={12} /> 保存</button>
                <button
                  type="button"
                  className="wb-ref-editor__delete"
                  disabled={references.mutating}
                  onClick={() => void references.removeLayer(record).then((deleted) => {
                    if (deleted) onLayerDeleted(record.id)
                  })}
                ><Trash2 size={12} /> 删除</button>
              </div>
            </div>
          )}
        </div>
      ))}
    </section>
  )
}
