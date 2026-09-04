import {
  AlertTriangle,
  CarFront,
  Check,
  ChevronRight,
  FileCode2,
  FileStack,
  Layers3,
  LoaderCircle,
  Palette,
  SlidersHorizontal,
  UploadCloud,
  X,
  Zap,
} from 'lucide-react'
import { ChangeEvent, DragEvent, useRef } from 'react'
import type { MapIntakeApi } from './useMapIntake'
import { FieldSelect } from './FieldSelect'
import { formatNumber } from './format'

/** Upload → inspect → (reference publish | mapping + OSM profile) → compile. */
export function IntakePanel({ intake }: { intake: MapIntakeApi }) {
  const fileInputRef = useRef<HTMLInputElement>(null)
  const {
    stage, files, inspect, mapping, osmPreprocess, mapName, importJob,
    referenceDraftStyle, referenceImporting, error,
  } = intake

  const onFileChange = (event: ChangeEvent<HTMLInputElement>) => {
    if (event.target.files) void intake.handleFiles(Array.from(event.target.files))
  }

  const onDrop = (event: DragEvent<HTMLButtonElement>) => {
    event.preventDefault()
    void intake.handleFiles(Array.from(event.dataTransfer.files))
  }

  return (
    <>
      <button
        className={`wb-dropzone ${stage === 'uploading' ? 'is-loading' : ''}`}
        type="button"
        onClick={() => fileInputRef.current?.click()}
        onDragOver={(event) => event.preventDefault()}
        onDrop={onDrop}
        data-testid="upload-zone"
      >
        <input
          ref={fileInputRef}
          type="file"
          accept=".shp,.shx,.dbf,.prj,.cpg,.geojson,.json,.csv,application/geo+json,application/json,text/csv"
          multiple
          onChange={onFileChange}
          hidden
        />
        {stage === 'uploading'
          ? <LoaderCircle className="spin" size={27} />
          : <UploadCloud size={27} strokeWidth={1.4} />}
        <strong>{stage === 'uploading' ? '正在解析数据结构' : '拖入 SHP 数据包或 GEOJSON'}</strong>
        <span>道路文件 · 可附加一个 TURN RESTRICTIONS .CSV</span>
        <i>SELECT FILES</i>
      </button>

      {files.length > 0 && (
        <div className="wb-files">
          {files.map((file) => (
            <span key={`${file.name}-${file.size}`}><FileCode2 size={12} /> {file.name}</span>
          ))}
        </div>
      )}

      {inspect && (
        <>
          <div className="wb-source-card">
            <div className="wb-source-card__icon"><FileStack size={19} /></div>
            <div>
              <span>{inspect.driver}</span>
              <strong>{formatNumber(inspect.featureCount)} FEATURES</strong>
              <small>{inspect.geometry} · {inspect.crs || 'CRS UNKNOWN'}</small>
            </div>
            <Check size={15} className="wb-source-card__check" />
          </div>

          {inspect.suggestedUsage === 'reference-layer' ? (
            <section className="wb-ref-intake" aria-label="发布参考图层">
              <div className="wb-ref-intake__head">
                <Layers3 size={17} />
                <div>
                  <strong>识别为参考图层</strong>
                  <span>将转换为 WGS84 后叠加显示，不参与导航拓扑与道路匹配。</span>
                </div>
                <small>NON-ROUTABLE</small>
              </div>
              <label className="field-control field-control--full">
                <span>图层名称</span>
                <input value={mapName} onChange={(event) => intake.setMapName(event.target.value)} />
              </label>
              <div className="wb-style-grid">
                <label className="wb-style-color">
                  <span>图层颜色</span>
                  <div>
                    <input
                      type="color"
                      value={referenceDraftStyle.color}
                      onChange={(event) => intake.setReferenceDraftStyle({
                        ...referenceDraftStyle, color: event.target.value,
                      })}
                    />
                    <strong>{referenceDraftStyle.color.toUpperCase()}</strong>
                  </div>
                </label>
                <label className="wb-style-opacity">
                  <span>不透明度 <strong>{Math.round(referenceDraftStyle.opacity * 100)}%</strong></span>
                  <input
                    type="range"
                    min="0.05"
                    max="1"
                    step="0.05"
                    value={referenceDraftStyle.opacity}
                    onChange={(event) => intake.setReferenceDraftStyle({
                      ...referenceDraftStyle, opacity: Number(event.target.value),
                    })}
                  />
                </label>
              </div>
              <button
                className="wb-primary"
                type="button"
                disabled={referenceImporting}
                onClick={() => void intake.importReferenceLayer()}
              >
                {referenceImporting ? <LoaderCircle className="spin" size={16} /> : <Palette size={16} />}
                <span>{referenceImporting ? '正在转换并发布' : '发布为参考图层'}</span>
                <ChevronRight size={16} />
              </button>
            </section>
          ) : !inspect.navigationCompatible ? (
            <div className="wb-gate" role="alert">
              <AlertTriangle size={18} />
              <div>
                <strong>{inspect.geometry} 不能构建导航拓扑</strong>
                <span>没有识别到可用的线状道路或点面参考几何，请检查 GeoJSON 数据结构。</span>
              </div>
              <small>NON-ROUTABLE</small>
            </div>
          ) : (
            <>
              {inspect.osmRoadData && (
                <section className={`wb-osm ${osmPreprocess.enabled ? 'is-enabled' : ''}`} aria-label="OSM 机动车清洗">
                  <div className="wb-osm__head">
                    <span className="wb-osm__mark"><CarFront size={16} /></span>
                    <div>
                      <strong>OSM 汽车画像</strong>
                      <small>HIGHWAY SCHEMA DETECTED</small>
                    </div>
                    <label className="wb-switch">
                      <input
                        type="checkbox"
                        checked={osmPreprocess.enabled}
                        onChange={(event) => intake.setOSMPreprocess({ ...osmPreprocess, enabled: event.target.checked })}
                      />
                      <span aria-hidden="true" />
                      <em>{osmPreprocess.enabled ? '已启用' : '未启用'}</em>
                    </label>
                  </div>
                  <p>在服务器端过滤非机动车道路，并统一方向、限速、层级与访问权限。</p>
                  {osmPreprocess.enabled && (
                    <div className="wb-osm__options">
                      <label>
                        <input type="checkbox" checked={osmPreprocess.includeService} onChange={(event) => intake.setOSMPreprocess({ ...osmPreprocess, includeService: event.target.checked })} />
                        <span>服务道路<small>停车场 / 园区</small></span>
                      </label>
                      <label>
                        <input type="checkbox" checked={osmPreprocess.includeTrack} onChange={(event) => intake.setOSMPreprocess({ ...osmPreprocess, includeTrack: event.target.checked })} />
                        <span>土路<small>乡村 / 林间</small></span>
                      </label>
                      <label>
                        <input type="checkbox" checked={osmPreprocess.includePrivate} onChange={(event) => intake.setOSMPreprocess({ ...osmPreprocess, includePrivate: event.target.checked })} />
                        <span>受限道路<small>PRIVATE ACCESS</small></span>
                      </label>
                      <label className="wb-osm__min-length">
                        <span>最短道路<small>METERS</small></span>
                        <input type="number" min="0.1" max="1000" step="0.5" value={osmPreprocess.minLengthMeters} onChange={(event) => intake.setOSMPreprocess({ ...osmPreprocess, minLengthMeters: Number(event.target.value) })} />
                      </label>
                    </div>
                  )}
                </section>
              )}
              <section className="wb-mapping">
                <div className="section-label">
                  <SlidersHorizontal size={14} />
                  <span>{osmPreprocess.enabled ? '拓扑参数' : '字段映射'}</span>
                  <small>{osmPreprocess.enabled ? 'CANONICAL' : `${inspect.fields.length} FIELDS`}</small>
                </div>
                <label className="field-control field-control--full">
                  <span>地图名称</span>
                  <input value={mapName} onChange={(event) => intake.setMapName(event.target.value)} />
                </label>
                {osmPreprocess.enabled ? (
                  <div className="wb-canonical"><Check size={13} /><span>清洗结果自动使用 Zeus 标准字段映射</span><small>ROAD_ID · ONEWAY · SPEED · CLASS · Z</small></div>
                ) : (
                  <div className="wb-field-grid">
                    <FieldSelect label="道路 ID" value={mapping.idField} fields={inspect.fields} optional={false} onChange={(idField) => intake.setMappingField('idField', idField)} />
                    <FieldSelect label="单双向" value={mapping.onewayField} fields={inspect.fields} onChange={(onewayField) => intake.setMappingField('onewayField', onewayField)} />
                    <FieldSelect label="限速" value={mapping.speedField} fields={inspect.fields} onChange={(speedField) => intake.setMappingField('speedField', speedField)} />
                    <FieldSelect label="车道数" value={mapping.lanesField} fields={inspect.fields} onChange={(lanesField) => intake.setMappingField('lanesField', lanesField)} />
                    <FieldSelect label="道路等级" value={mapping.roadClassField} fields={inspect.fields} onChange={(roadClassField) => intake.setMappingField('roadClassField', roadClassField)} />
                    <FieldSelect label="高程层" value={mapping.zLevelField} fields={inspect.fields} onChange={(zLevelField) => intake.setMappingField('zLevelField', zLevelField)} />
                    <FieldSelect label="桥梁" value={mapping.bridgeField} fields={inspect.fields} onChange={(bridgeField) => intake.setMappingField('bridgeField', bridgeField)} />
                  </div>
                )}
                <div className="wb-field-grid wb-field-grid--numeric">
                  <label className="field-control">
                    <span>吸附容差 / m</span>
                    <input type="number" min="0.01" step="0.1" value={mapping.snapToleranceMeters} onChange={(event) => intake.setMappingField('snapToleranceMeters', Number(event.target.value))} />
                  </label>
                  {!osmPreprocess.enabled && (
                    <label className="field-control">
                      <span>默认限速 / km·h⁻¹</span>
                      <input type="number" min="1" value={mapping.defaultSpeedKph} onChange={(event) => intake.setMappingField('defaultSpeedKph', Number(event.target.value))} />
                    </label>
                  )}
                </div>
              </section>

              <button className="wb-primary" type="button" onClick={() => void intake.importMap()} disabled={stage === 'importing'} data-testid="compile-map">
                {stage === 'importing' ? <LoaderCircle className="spin" size={17} /> : <Zap size={17} fill="currentColor" />}
                <span>{stage === 'importing' ? (importJob?.phase === 'preprocessing' ? '正在清洗 OSM 路网' : '正在构建拓扑') : (osmPreprocess.enabled ? '清洗并编译地图' : '编译运行时地图')}</span>
                <ChevronRight size={17} />
              </button>
              {stage === 'importing' && importJob && (
                <div className="wb-progress" role="status" aria-live="polite">
                  <div className="wb-progress__meta">
                    <span>{importJob.phase.toUpperCase()}</span>
                    <strong>{importJob.progress}%</strong>
                  </div>
                  <div className="wb-progress__track"><i style={{ width: `${importJob.progress}%` }} /></div>
                  <div className="wb-progress__message">
                    <span>{importJob.message}</span>
                    <button type="button" onClick={() => void intake.cancelImport()}>取消</button>
                  </div>
                </div>
              )}
            </>
          )}
        </>
      )}

      {error && (
        <div className="wb-error" role="alert">
          <AlertTriangle size={15} />
          <span>{error}</span>
          <button type="button" aria-label="关闭错误" onClick={intake.dismissError}><X size={13} /></button>
        </div>
      )}
    </>
  )
}
