import { lazy, Suspense, useEffect, useRef, useState } from 'react'
import { BookOpen, Bug, Code2, LoaderCircle, Play, Square, X, Crosshair, ArrowRight, CheckCircle2, AlertTriangle } from 'lucide-react'
import { TracePanel } from '../components/TracePanel'
import type { AlgorithmLabAPI } from './useAlgorithmLab'
import type { AlgorithmEditorHandle } from './AlgorithmEditor'
import { AlgorithmHistory } from './AlgorithmHistory'
import './algorithm.css'
const AlgorithmEditor = lazy(() => import('./AlgorithmEditor'))

export function AlgorithmConsole({ lab, mapName }: { lab: AlgorithmLabAPI; mapName: string }) {
  const [tab, setTab] = useState<'result' | 'methods' | 'history' | 'debug'>('methods')
  const editor = useRef<AlgorithmEditorHandle>(null)
  const [coordinates, setCoordinates] = useState(['', ''])
  const [coordinateError, setCoordinateError] = useState('')
  useEffect(() => {
    setCoordinates([lab.from?.join(', ') ?? '', lab.to?.join(', ') ?? ''])
    setCoordinateError('')
  }, [lab.from, lab.to])
  const applyCoordinates = () => {
    const points = coordinates.map(value => value.split(/[,，]/).map(part => part.trim() ? Number(part.trim()) : NaN))
    if (points.some(point => point.length !== 2 || !point.every(Number.isFinite) || Math.abs(point[0]) > 180 || Math.abs(point[1]) > 90)) {
      setCoordinateError('请按“经度, 纬度”输入有效 WGS84 坐标。'); return
    }
    lab.setPoints(points[0] as [number, number], points[1] as [number, number])
    setCoordinateError('')
  }
  const { capabilities: api, result } = lab
  useEffect(() => { if (tab === 'debug' && result && !lab.debug && !lab.busy) setTab('result') }, [tab, result, lab.debug, lab.busy])
  const execute = () => { setTab('result'); void lab.run() }
  const jumpToError = () => {
    if (!result?.line || !editor.current) return
    editor.current.focusLine(result.line)
  }
  const difference = result?.baseline.ok && result.baseline.timeS > 0
    ? (result.timeS / result.baseline.timeS - 1) * 100 : null
  return (
    <aside className="algorithm-console" aria-label="自定义导航算法实验台">
      <header className="algorithm-title">
        <div><Code2 size={17} /><strong>算法实验台</strong><span>PYTHON SUBSET</span></div>
        <button onClick={lab.close} title="关闭算法实验台" aria-label="关闭算法实验台"><X size={16} /></button>
      </header>
      <div className="algorithm-context">
        <strong title={mapName}>{mapName}</strong><span>只读路网 · 静态旅行时间</span>
        <div className="algorithm-od"><span>{lab.from ? lab.from.map(n => n.toFixed(6)).join(', ') : '在地图点击起点'}</span><ArrowRight size={12} /><span>{lab.to ? lab.to.map(n => n.toFixed(6)).join(', ') : '点击终点'}</span></div>
        <button onClick={lab.repick}><Crosshair size={12} />{lab.picking ? '正在选点' : '重新选取起终点'}</button>
        <details className="algorithm-coordinates"><summary>精确输入起终点</summary>
          <label>起点<input aria-label="算法起点经纬度" value={coordinates[0]} placeholder="经度, 纬度" onChange={e => setCoordinates([e.target.value, coordinates[1]])} /></label>
          <label>终点<input aria-label="算法终点经纬度" value={coordinates[1]} placeholder="经度, 纬度" onChange={e => setCoordinates([coordinates[0], e.target.value])} /></label>
          <small>WGS84 · 最多吸附至 100 米内可通行道路</small>
          <button onClick={applyCoordinates}>应用坐标</button>
          {coordinateError && <p role="alert">{coordinateError}</p>}
        </details>
      </div>
      <div className="algorithm-toolbar">
        <span>route.py <small>{lab.stale ? '有未验证修改' : '草稿'}</small></span>
        {lab.busy || lab.debug ? <button className="algorithm-stop" onClick={lab.stop}><Square size={12} />停止</button>
          : <div className="algorithm-actions"><button onClick={() => { setTab('debug'); void lab.debugAction('start') }} disabled={!lab.from || !lab.to || !api || !lab.source.trim()}><Bug size={12} />调试</button>
            <button className="algorithm-run" onClick={execute} disabled={!lab.from || !lab.to || !api || !lab.source.trim()}><Play size={12} />运行验证 <kbd>⌘ ↵</kbd></button></div>}
      </div>
      {lab.debug && <div className="algorithm-debug-controls">
        <span>{lab.busy ? '正在执行…' : `暂停在第 ${lab.debug.line} 行 · 尚未执行`}</span>
        <button disabled={lab.busy} onClick={() => { setTab('debug'); void lab.debugAction('step') }}>单步</button>
        <button disabled={lab.busy} onClick={() => { setTab('debug'); void lab.debugAction('continue') }}>继续到断点</button>
      </div>}
      <div className="algorithm-editor">
        <Suspense fallback={<span className="algorithm-editor-loading">正在载入编辑器…</span>}>
          <AlgorithmEditor ref={editor} source={lab.source} onChange={lab.setSource} onRun={() => { if (!lab.busy && !lab.debug) execute() }} methods={api?.methods ?? []} activeLine={!lab.busy ? lab.debug?.line : null} />
        </Suspense>
      </div>
      <div className="algorithm-budget"><span>{new TextEncoder().encode(lab.source).length.toLocaleString()} / 32,768 B</span><span>5 秒 · 1 GiB · {lab.steps.toLocaleString()} 步</span></div>
      <details className="algorithm-options"><summary>实验条件与执行限制</summary>
        <label>调试断点行号<input value={lab.breakpoints} onChange={e => lab.setBreakpoints(e.target.value)} placeholder="例如 8, 23" /></label>
        <small>暂停时可修改；继续执行将在这些可执行语句前暂停。无断点则执行到结束。</small>
        <label>临时封闭道路索引<input value={lab.closedEdges} onChange={e => lab.setClosedEdges(e.target.value)} placeholder="例如 203, 418" /></label>
        <small>只影响本次实验；两个方向需分别指定有向道路索引。相同封路条件同时用于基准。</small>
        <label>执行步数预算<select value={lab.steps} onChange={e => lab.setSteps(Number(e.target.value))}><option value={200000}>200,000 · 快速调试</option><option value={2000000}>2,000,000 · 标准</option><option value={5000000}>5,000,000 · 扩展</option></select></label>
        <small>每次语句、表达式、循环和后继查询计数。达到预算会终止；不改变平台内存与时间上限。</small>
      </details>
      <nav className="algorithm-tabs" aria-label="算法辅助面板">
        <button onClick={() => setTab('result')} aria-pressed={tab === 'result'}>验证结果</button>
        <button onClick={() => setTab('methods')} aria-pressed={tab === 'methods'}><BookOpen size={12} />方法与语法</button>
        <button onClick={() => setTab('history')} aria-pressed={tab === 'history'}>实验历史 {lab.experiments.length || ''}</button>
        {lab.debug && <button onClick={() => setTab('debug')} aria-pressed={tab === 'debug'}>调试变量</button>}
      </nav>
      <div className="algorithm-output">
        {lab.message && <p role="status" className="algorithm-message">{lab.busy && <LoaderCircle size={13} className="spin" />}{lab.message}</p>}
        {tab === 'debug' ? <div className="algorithm-debug-output">
          {lab.debug ? <>
            <strong>调用栈：{lab.debug.frames?.join(' → ')}</strong>
            <p className="algorithm-note">已执行 {lab.debug.steps?.toLocaleString() ?? 0} 步 · 后继查询 {lab.debug.expandedNodes ?? 0} 次</p>
            <dl>{Object.entries(lab.debug.variables ?? {}).map(([name, value]) => <div key={name}><dt>{name}</dt><dd>{value}</dd></div>)}</dl>
            {(lab.debug.logs?.length ?? 0) > 0 && <pre className="algorithm-logs">{lab.debug.logs?.join('\n')}</pre>}
            <p className="algorithm-note">显示当前函数的前 24 个变量；容器只预览前 6 项、最多两层，不能修改现场变量。黄色行是下一条将执行的语句。</p>
          </> : <p className="algorithm-note">调试启动后停在第一条语句前，可查看局部变量并单步执行。暂停不计入计算时间，60 秒无操作自动结束。</p>}
        </div> : tab === 'history' ? <AlgorithmHistory lab={lab} /> : tab === 'methods' ? <>
          <p className="algorithm-intro">你控制搜索逻辑；平台提供合法转移、资源边界和独立校验。修改代码后点击运行。</p>
          {api?.methods.map(method => <article className="algorithm-method" key={method.signature}><code>{method.signature}</code><p>{method.description}</p></article>)}
          <p className="algorithm-syntax">{api?.syntax}</p>
          <button className="algorithm-template" onClick={() => api && lab.setSource(api.template)} disabled={!api}>载入 Dijkstra 模板</button>
        </> : result ? <>
          {lab.stale && <p className="algorithm-stale">输入已改变：以下为上一版本结果，地图预览已隐藏。</p>}
          <div className={`algorithm-verdict ${result.ok ? 'is-ok' : 'is-error'}`}>
            {result.ok ? <CheckCircle2 size={17} /> : <AlertTriangle size={17} />}
            <strong>{result.phase === 'verified' ? '路线已通过 C++ 校验' : result.phase === 'unreachable' ? '无路可达 · 与基准一致' : '验证未通过'}</strong>
          </div>
          {result.error && <p className="algorithm-error">{result.error}{Boolean(result.line) && <button onClick={jumpToError}>定位第 {result.line} 行</button>}</p>}
          {result.phase === 'verified' && <div className="algorithm-metrics">
            <div><span>预计旅行时间</span><strong>{result.timeS.toFixed(1)} <small>s</small></strong></div>
            <div><span>路线长度</span><strong>{(result.lengthM / 1000).toFixed(2)} <small>km</small></strong></div>
            <div><span>相对 Dijkstra</span><strong>{difference === null ? '—' : `${difference > 0 ? '+' : ''}${difference.toFixed(2)}%`}</strong></div>
            <div><span>后继查询 / 执行耗时</span><strong>{result.expandedNodes.toLocaleString()} <small>/ {result.computeMs.toFixed(0)} ms</small></strong></div>
          </div>}
          <TracePanel trace={lab.trace} playback={lab.playback} />
          {(result.logs?.length ?? 0) > 0 && <pre className="algorithm-logs">{result.logs?.join('\n')}</pre>}
          <div className="algorithm-provenance"><span title={result.codeRevision}>代码 {result.codeRevision.slice(0, 10)}</span><span title={result.snapshotId}>快照 {result.snapshotId.slice(0, 10)}</span></div>
          <p className="algorithm-note">仅验证当前只读路网和临时封路；结果不提交给车辆，不包含实时拥堵或信号等待。搜索事件用于回放，g/f 为算法报告值。</p>
        </> : <div className="algorithm-empty"><Code2 size={28} /><strong>从一次真实路网实验开始</strong><p>选定起终点，修改模板，点击运行。平台会比较相同条件下的 Dijkstra 结果。</p></div>}
      </div>
    </aside>
  )
}
