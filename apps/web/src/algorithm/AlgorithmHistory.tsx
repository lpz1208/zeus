import { useState } from 'react'
import type { AlgorithmLabAPI } from './useAlgorithmLab'

export function AlgorithmHistory({ lab }: { lab: AlgorithmLabAPI }) {
  const [selection, setSelection] = useState<string[]>([])
  const selected = lab.experiments.filter(item => selection.includes(item.id))
  const comparable = selected.length === 2 && selected.every(item => item.phase === 'verified') && selected[0].snapshotId === selected[1].snapshotId
  return <div className="algorithm-history">
    <p className="algorithm-intro">每次完成的实验自动保存代码、条件和结果。每张地图最多 100 次，重启后可恢复；勾选两次实验进行比较。</p>
    {lab.historyMessage && <p role="status">{lab.historyMessage}</p>}
    <button onClick={() => void lab.refreshHistory()}>刷新历史</button>
    {selected.length === 2 && (comparable ? <div className="algorithm-comparison">
      <table><caption>相同路网快照 · 旅行时间对比</caption><thead><tr><th>指标</th>{selected.map(item => <th key={item.id}>{item.codeRevision.slice(0, 7)}</th>)}</tr></thead>
        <tbody>{[
          ['执行方式', ...selected.map(item => item.executionMode === 'debug' ? '逐步调试' : '运行验证')],
          ['旅行时间 / s', ...selected.map(item => item.timeS.toFixed(1))],
          ['路线长度 / km', ...selected.map(item => (item.lengthM / 1000).toFixed(2))],
          ['后继查询', ...selected.map(item => item.expandedNodes.toLocaleString())],
          ['解释执行 / ms', ...selected.map(item => item.computeMs.toFixed(0))],
        ].map(row => <tr key={row[0]}>{row.map((value, index) => <td key={index}>{value}</td>)}</tr>)}</tbody>
      </table><small>执行时间受机器负载影响；快照一致不代表代码或步数预算一致。</small>
    </div> : <p className="algorithm-stale">只有通过校验且路网快照一致的实验才能比较。请保持地图、起终点和封路条件相同。</p>)}
    {!lab.experiments.length && <p className="algorithm-note">还没有保存的实验。运行一次算法即可开始记录。</p>}
    {lab.experiments.map(item => <article key={item.id} className="algorithm-history-item">
      <label><input type="checkbox" aria-label={`对比实验 ${item.id}`} checked={selection.includes(item.id)} disabled={!selection.includes(item.id) && selected.length === 2}
        onChange={event => setSelection(previous => event.target.checked ? [...previous.filter(id => lab.experiments.some(record => record.id === id)), item.id] : previous.filter(id => id !== item.id))} />
        <strong>{item.codeRevision.slice(0, 10)}</strong><span>{item.ok ? item.phase === 'verified' ? `${item.timeS.toFixed(1)} s` : '不可达' : '验证失败'}</span></label>
      <small>{new Date(item.createdAt).toLocaleString()} · 快照 {item.snapshotId.slice(0, 8)}</small>
      <div><button disabled={lab.busy || Boolean(lab.debug)} onClick={() => void lab.restoreExperiment(item.id)}>恢复代码与结果</button><button onClick={() => void lab.deleteExperiment(item.id)}>删除记录</button></div>
    </article>)}
  </div>
}
