import { useEffect, useImperativeHandle, useRef, type Ref } from 'react'
import { EditorState, StateEffect, StateField } from '@codemirror/state'
import { EditorView, keymap, lineNumbers, highlightActiveLine, drawSelection, Decoration, type DecorationSet } from '@codemirror/view'
import { defaultKeymap, history, historyKeymap, indentWithTab } from '@codemirror/commands'
import { python } from '@codemirror/lang-python'
import { bracketMatching, indentOnInput, syntaxHighlighting, HighlightStyle } from '@codemirror/language'
import { tags } from '@lezer/highlight'
import { autocompletion, completionKeymap, type CompletionContext } from '@codemirror/autocomplete'
import { linter, lintGutter, type Diagnostic } from '@codemirror/lint'

export interface AlgorithmEditorHandle { focusLine: (line: number) => void }
const debugLine = StateEffect.define<number | null>()
const debugDecoration = StateField.define<DecorationSet>({
  create: () => Decoration.none,
  update(value, transaction) {
    value = value.map(transaction.changes)
    for (const effect of transaction.effects) if (effect.is(debugLine)) {
      value = effect.value && effect.value <= transaction.state.doc.lines
        ? Decoration.set([Decoration.line({ class: 'cm-debug-line' }).range(transaction.state.doc.line(effect.value).from)]) : Decoration.none
    }
    return value
  },
  provide: field => EditorView.decorations.from(field),
})
interface Props {
  ref?: Ref<AlgorithmEditorHandle>
  source: string
  onChange: (source: string) => void
  onRun: () => void
  methods: { signature: string; description: string }[]
  activeLine?: number | null
}

export default function AlgorithmEditor({ ref, source, onChange, onRun, methods, activeLine }: Props) {
  const container = useRef<HTMLDivElement>(null)
  const view = useRef<EditorView | null>(null)
  const latest = useRef({ onChange, onRun, methods })
  latest.current = { onChange, onRun, methods }
  useImperativeHandle(ref, () => ({ focusLine(number) {
    const editor = view.current
    if (!editor) return
    const line = editor.state.doc.line(Math.max(1, Math.min(number, editor.state.doc.lines)))
    editor.dispatch({ selection: { anchor: line.from, head: line.to }, effects: EditorView.scrollIntoView(line.from, { y: 'center' }) })
    editor.focus()
  } }), [])
  useEffect(() => {
    if (!container.current) return
    let pending: AbortController | null = null
    const complete = (context: CompletionContext) => {
      const method = context.matchBefore(/ctx\.\w*/)
      if (method) return { from: method.from + 4, options: latest.current.methods.flatMap(item =>
        Array.from(item.signature.matchAll(/ctx\.(\w+)\(/g), match => ({ label: match[1], type: 'function', info: item.description }))) }
      const word = context.matchBefore(/\w+/)
      if (!context.explicit && !word) return null
      return { from: word?.from ?? context.pos, options: ['def', 'return', 'if', 'else', 'elif', 'for', 'while', 'in', 'not', 'and', 'or', 'break', 'continue', 'True', 'False', 'None', 'len', 'range', 'min', 'max', 'abs', 'ctx'].map(label => ({ label, type: 'keyword' })) }
    }
    const editor = new EditorView({ parent: container.current, state: EditorState.create({ doc: source, extensions: [
      lineNumbers(), history(), drawSelection(), highlightActiveLine(), python(), indentOnInput(), bracketMatching(), debugDecoration,
      syntaxHighlighting(HighlightStyle.define([
        { tag: tags.keyword, color: '#d4b5f1' },
        { tag: tags.function(tags.variableName), color: '#d9edb3' },
        { tag: [tags.number, tags.bool, tags.null], color: '#efc28f' },
        { tag: tags.string, color: '#a8d5bd' },
        { tag: tags.comment, color: '#94a4ae', fontStyle: 'italic' },
        { tag: tags.operator, color: '#a8cee6' },
      ])), lintGutter(),
      autocompletion({ override: [complete] }),
      keymap.of([{ key: 'Mod-Enter', run: () => { latest.current.onRun(); return true } }, ...completionKeymap, indentWithTab, ...defaultKeymap, ...historyKeymap]),
      EditorState.tabSize.of(4), EditorView.contentAttributes.of({ 'aria-label': '导航算法代码', spellcheck: 'false' }),
      EditorView.updateListener.of(update => { if (update.docChanged) latest.current.onChange(update.state.doc.toString()) }),
      linter(async current => {
        pending?.abort()
        const controller = new AbortController()
        pending = controller
        const code = current.state.doc.toString()
        try {
          const response = await fetch('/api/algorithms/check', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ source: code }), signal: controller.signal })
          if (!response.ok) return [{ from: 0, to: 0, severity: 'warning', message: '服务端语法检查暂不可用；运行时仍会校验。' }] as Diagnostic[]
          const result = await response.json() as { ok: boolean; error?: string; line?: number }
          if (controller.signal.aborted || code !== current.state.doc.toString() || result.ok) return []
          const line = current.state.doc.line(Math.max(1, Math.min(result.line ?? 1, current.state.doc.lines)))
          return [{ from: line.from, to: line.to, severity: 'error', message: result.error ?? '语法错误' }] as Diagnostic[]
        } catch { return [] }
      }, { delay: 700 }),
      EditorView.theme({
        '&': { height: '100%', color: '#dce7ec', backgroundColor: '#171c20', fontSize: '12px' },
        '.cm-scroller': { overflow: 'auto', fontFamily: 'var(--mono)', lineHeight: '20px' },
        '.cm-content': { padding: '10px 0', caretColor: '#d9edb3' },
        '.cm-line': { padding: '0 10px' },
        '.cm-gutters': { backgroundColor: '#171c20', color: '#72818c', borderRight: '1px solid #2c343b' },
        '.cm-activeLine, .cm-activeLineGutter': { backgroundColor: '#242e36' },
        '.cm-debug-line': { backgroundColor: '#514925 !important', boxShadow: 'inset 3px 0 #e8c765' },
        '.cm-cursor': { borderLeftColor: '#d9edb3' },
        '&.cm-focused .cm-selectionBackground, .cm-selectionBackground': { backgroundColor: '#35485b' },
        '.cm-tooltip': { backgroundColor: '#252f37', border: '1px solid #51616d', color: '#edf2f5' },
        '.cm-tooltip-autocomplete > ul > li[aria-selected]': { backgroundColor: '#405563', color: '#fff' },
      }, { dark: true }),
    ] }) })
    view.current = editor
    return () => { pending?.abort(); editor.destroy(); view.current = null }
    // The editor owns its document; external changes are applied below.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])
  useEffect(() => {
    const editor = view.current
    if (editor && editor.state.doc.toString() !== source) editor.dispatch({ changes: { from: 0, to: editor.state.doc.length, insert: source } })
  }, [source])
  useEffect(() => {
    const editor = view.current
    if (!editor) return
    const line = activeLine && activeLine <= editor.state.doc.lines ? activeLine : null
    editor.dispatch({ effects: line ? [debugLine.of(line), EditorView.scrollIntoView(editor.state.doc.line(line).from, { y: 'center' })] : [debugLine.of(null)] })
  }, [activeLine])
  return <div className="algorithm-codemirror" ref={container} />
}
