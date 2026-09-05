# Zeus Web 地图工作台

> 状态：地图、Agent 与 Benchmark 工作台已实现
> 最后更新：2026-09-05

## 1. 定位

Web 地图工作台是 C++ 地图引擎的可视化控制面，负责数据上传、字段映射、地图编译、拓扑质检、路网展示和点选定位。它不在浏览器中重写拓扑或地图匹配算法。

```text
React Web
├→ Go Control Server → zeus-map CLI → C++ Map Engine
└→ Python Benchmark Job Service → LangGraph Agent Runtime
```

## 2. 已实现功能

- 拖拽或选择单个 GeoJSON 文件，或完整 Shapefile 数据包。
- 调用 C++ `inspect` 检查图层、CRS、要素数量和字段。
- 在检查阶段区分道路网络、参考图层和不支持的几何；分类依据 C++ `inspect` 的图层层级几何类型，图层为混合几何（`Unknown (any)`）时依据逐要素几何类型统计，混合 LineString / MultiLineString 识别为道路数据。点/面数据不会进入异步编译队列。
- 点、面数据可发布为工作区级参考图层，统一转换为 WGS84 并保留源属性。
- 参考图层支持导入时设置颜色和不透明度，并可在图层列表中独立显隐。
- 参考图层支持重命名、颜色和不透明度二次编辑，以及带确认的整层删除。
- 参考图层列表提供显式定位操作；即使当前道路地图位于其他城市，也可一键将视口缩放到目标边界。
- 没有已发布道路地图时，页面完成地图与参考图层清单加载后会自动聚焦首个可见参考图层，避免演示路网覆盖真实边界的视口。
- 点击参考点或面要素可查看源属性，并以图层 ID 和要素序号高亮当前选择。
- 根据常见字段名自动猜测道路 ID、单双向、限速、等级和高程字段。
- 检查结果包含精确 `highway` 字段时自动识别为 OSM 道路，默认启用汽车画像清洗；可配置服务道路、土路、受限道路和最短道路长度。
- OSM 清洗在 Go 异步任务中调用 C++ `preprocess-osm`，生成标准字段 GeoJSON 后直接进入拓扑编译，不在浏览器中处理或回传中间大文件。
- 地图版本持久化清洗摘要，质检页展示输入、保留、过滤、过滤原因和方向/限速规范化数量。
- 用户可调整字段映射、吸附容差和默认限速。
- 调用 C++ 构建拓扑并发布 `.zmap`。
- 地图编译作为后台任务执行，通过 SSE 推送排队、拓扑、验证、导出和发布进度，并支持取消。
- 展示节点、边、连通分量、错误和警告。
- 将运行时地图导出为 WGS84 GeoJSON 并通过 MapLibre 渲染。
- 在高缩放级别渲染拓扑节点和单行道路方向箭头。
- 点击道路可查看来源 ID、道路类型、方向、限速、长度、z-level 和对应有向边。
- 将带坐标的质检事件导出为 WGS84 点图层；点击列表或地图事件可联动定位。
- 显示地图版本列表。
- 点击地图，以经纬度调用 C++ R-tree 道路匹配。
- 路由模式：右侧「路由 / 仿真」标签页激活后，地图依次点击起点和终点，调用四种基准算法之一计算路径；路线以紫色渲染并自动缩放，面板展示距离、预计时长、扩展节点、计算耗时、途经边数与起终吸附信息；算法切换立即重算；吸附失败与不可达给出中文原因。
- 交通仿真：复用当前 OD 和算法，可配置车辆数、线性发车分布、时域、采样间隔、自由流/拥堵出口间隔，以及周期拥堵扫描间隔和动态权重阈值；转向信号编辑器可为地图拾取的节点编排绿灯相位、黄灯、全红、周期偏移和每转向独立饱和流率，转向使用 `fromEdge > toEdge` 表达；Go 端点同步触发 C++ 中观内核，返回统计、轨迹 GeoJSON 和回放数据。
- 仿真回放：琥珀色轨迹叠加在路网上，车辆使用 MapLibre circle layer；时间滑块支持播放/暂停和 1×/10×/30×，每帧通过二分插值更新 source，不创建 DOM Marker。
- 封路、限速、降容及达到阈值的实时拥堵权重会触发受影响车辆重规划；结果面板展示尝试/成功/失败数，Playback 保存逐车重规划事件并在时间控制台累计显示。
- Agent 工作台（顶栏 AGENT 按钮进入）：地图点选 OD 创建有状态 Agent 会话，事件驱动推进到决策边界；仿真在决策屏障处冻结，决策横幅展示触发原因与状态版本，可一键比较 Tool Registry 全部算法并提交所选候选或保持路线；候选路线、当前剩余路线与起点预览在地图上联动渲染。
- Agent 车辆实时位置：观察返回的 `edgeId + offsetM`（米）按道路要素 `EDGE_IDS`/`DIRECTION` 还原有向边几何并插值为经纬度，复用 MapCanvas 车辆圆点层渲染；观察/工具/轨迹三标签控制台展示位置、ETA、附近道路态势、环境事件、算法能力注册表和带状态版本徽章的决策轨迹；支持会话快照保存、恢复与删除。
- Benchmark 工作台（顶栏 BENCH 按钮进入）：用场景清单组合地图、OD、仿真参数、道路/车辆控制事件和 fixed、reactive、rule-agent、model-agent 四类策略；支持多场景复制、重复次数与模型计费参数，异步提交后轮询任务进度、取消运行、浏览历史，并以场景 × 策略矩阵和旅行时间、拥堵暴露、决策延迟、工具调用图表对照结果。完成或取消的报告可下载 JSON 与逐次运行 CSV。
- Benchmark 工作台采用研究账本式三栏布局，与地图/Agent 工作台共享设计令牌；在 980 px 收敛为两栏、700 px 收敛为单栏，顶栏工作区入口在窄屏仍可访问。
- 高亮最佳道路，展示 edge、road ID、offset、横向距离和置信度。
- 没有控制服务或地图时展示合成演示路网。
- 工作台采用白色技术地图主题：左侧集中数据与图层管理，中央保留最大地图视野，右侧将要素详情和拓扑质检收拢为两个标签页。
- MapLibre Worker 通过 Vite 的 `?worker&url` 管线构建为自包含静态资源，生产服务无需 CDN；空白样式不声明未配置的 `glyphs`，交互查询只访问已完成创建的图层。

## 3. 技术栈

### Web

- React。
- TypeScript。
- Vite。
- MapLibre GL JS。
- Lucide 图标。
- IBM Plex Sans Variable 和 JetBrains Mono Variable 本地字体包。

### 控制服务

- Go 标准库 `net/http`。
- Multipart 文件上传。
- JSON REST API。
- 进度事件使用 SSE；任务状态可通过普通 HTTP 查询和取消。
- 受控进程调用 C++ `zeus-map`。
- 本地文件存储，后续替换为 PostgreSQL 和对象存储。

## 4. API

| 方法 | 路径 | 功能 |
| --- | --- | --- |
| GET | `/api/health` | 服务健康状态 |
| GET | `/api/maps` | 地图版本列表 |
| POST | `/api/maps/inspect` | 上传并检查 GeoJSON 或 Shapefile |
| POST | `/api/maps/import` | 创建异步地图编译任务，返回 `202` 和任务快照 |
| GET | `/api/reference-layers` | 获取工作区参考图层列表 |
| POST | `/api/reference-layers` | 将已检查的点/面数据转换并发布为参考图层 |
| PATCH | `/api/reference-layers/{id}` | 更新参考图层名称或显示样式 |
| DELETE | `/api/reference-layers/{id}` | 删除参考图层元数据和 GeoJSON 文件 |
| GET | `/api/reference-layers/{id}/geojson` | 获取参考图层的 WGS84 GeoJSON |
| GET | `/api/jobs/{id}` | 查询任务状态和当前进度 |
| GET | `/api/jobs/{id}/events` | 订阅任务 SSE 进度事件 |
| POST | `/api/jobs/{id}/cancel` | 取消排队中或执行中的任务 |
| GET | `/api/maps/{id}` | 地图版本和质检信息 |
| GET | `/api/maps/{id}/geojson` | 获取 WGS84 路网几何 |
| GET | `/api/maps/{id}/nodes.geojson` | 获取 WGS84 拓扑节点及入度、出度 |
| GET | `/api/maps/{id}/issues.geojson` | 获取 WGS84 质检事件点图层 |
| POST | `/api/maps/{id}/query` | XY 或经纬度道路匹配 |
| POST | `/api/maps/{id}/route` | 四种基准算法路径规划，返回统计与路线 GeoJSON；吸附失败与不可达返回 200 + `ok:false` |
| POST | `/api/maps/{id}/simulate` | 运行确定性中观仿真，返回统计、轨迹 GeoJSON 和 Playback JSON；全部不可规划返回 200 + `ok:false` |
| GET | `/api/maps/{id}/agent/tools` | 路由算法 Tool Registry（能力元数据） |
| POST | `/api/maps/{id}/agent/sessions` | 创建有状态 Agent 会话（OD 含 `agent` 标记、控制事件与信号） |
| GET | `/api/maps/{id}/agent/sessions/{session}` | 会话热边观察（占用/封闭/代价因子与 agent 车辆状态） |
| GET | `/api/maps/{id}/agent/sessions/{session}/agent/{vehicle}` | 单 agent 车辆观察（位置、ETA、剩余路线、附近道路、事件） |
| POST | `/api/maps/{id}/agent/sessions/{session}/plan` | 用指定算法从实时位置规划候选路线 |
| POST | `/api/maps/{id}/agent/sessions/{session}/step` | 单步或 `untilEvent` 推进；决策边界返回 `decisionId` |
| POST | `/api/maps/{id}/agent/sessions/{session}/actions` | 提交 `commit_route`/`keep_route`（状态版本 + 仿真时间 TTL 校验） |
| POST | `/api/maps/{id}/agent/sessions/{session}/run` | 非阻塞恢复自由运行 |
| POST | `/api/maps/{id}/agent/sessions/{session}/pause` | 在下一边界暂停 |
| GET | `/api/maps/{id}/agent/sessions/{session}/result` | 完成后内联轨迹与回放导出 |
| POST | `/api/maps/{id}/agent/sessions/{session}/snapshots` | 创建会话快照 |
| POST | `/api/maps/{id}/agent/snapshots/{snapshot}/restore` | 从快照恢复为新会话（回放已提交动作） |
| DELETE | `/api/maps/{id}/agent/snapshots/{snapshot}` | 删除快照 |
| DELETE | `/api/maps/{id}/agent/sessions/{session}` | 关闭会话 |

Benchmark Job Service 默认独立监听 `127.0.0.1:8090`，Go 控制面将下列同源路径反向代理到任务服务；可用 `--benchmark-url` 覆盖上游地址。前端默认使用同源路径，仅在绕过控制面调试时通过 `VITE_BENCHMARK_BASE_URL` 指定直连地址：

| 方法 | 路径 | 功能 |
| --- | --- | --- |
| GET | `/health` | Benchmark 服务健康状态 |
| POST | `/api/benchmarks` | 校验清单并创建异步评测任务 |
| GET | `/api/benchmarks?limit=50` | 获取最近任务及进度 |
| GET | `/api/benchmarks/{id}` | 获取单个任务状态 |
| GET | `/api/benchmarks/{id}/result` | 获取完成或已取消任务的版本化报告 |
| POST | `/api/benchmarks/{id}/cancel` | 请求在安全决策边界取消 |

上传限制为 512 MiB。每次上传必须是一个 `.geojson` / `.json` 文件，或一套同名的 `.shp`、`.shx`、`.dbf`、`.prj`（`.cpg` 可选）。接口使用 `sourceFile` 标识主数据文件；旧版 `shapefile` 请求字段仍可兼容。`POST /api/maps/import` 可携带 `osmPreprocess`，字段为 `enabled`、`includeService`、`includeTrack`、`includePrivate` 和 `minLengthMeters`。只有检查结果含精确 `highway` 字段时才允许启用。C++ 命令不经过 shell 拼接，并有执行超时。

GeoJSON 输入要求根对象可被 GDAL 识别为矢量数据集，道路几何为 `LineString` 或 `MultiLineString`。标准 GeoJSON 默认按 WGS84 读取，属性字段与 Shapefile 一样进入字段映射步骤。`Point`、`MultiPoint`、`Polygon`、`MultiPolygon` 会被识别为参考数据，可发布为独立图层，但不会被误当成道路编译。

参考图层与地图版本是多对多的显示关系：图层不写入 `.zmap`，不参与交叉切分、端点吸附、拓扑质检、R-tree 道路匹配或车辆仿真。浏览器可以将同一个行政区、建筑物或 POI 图层叠加到任意道路地图版本。

## 5. 启动

在仓库根目录：

```bash
make run
```

页面地址：

```text
http://127.0.0.1:8080
```

开发前端时可以分别启动：

```bash
./build/zeus-server \
  --addr 127.0.0.1:8080 \
  --data-dir data \
  --zeus-map ./build/zeus-map \
  --web-dir ./apps/web/dist
```

```bash
npm --prefix apps/web run dev
```

Vite 会将 `/api` 代理到 `127.0.0.1:8080`，其中 `/api/benchmarks` 再由 Go 控制面转发到 Benchmark Job Service。

运行 BENCH 工作台前另开终端启动持久化评测任务服务：

```bash
make agent-benchmark-service
```

## 6. 数据目录

```text
data/
├── uploads/
│   └── upl_xxx/
│       ├── roads.geojson
│       ├── inspect.json
│       └── 或 roads.shp + roads.shx + roads.dbf + roads.prj
├── maps/
│   └── map_xxx/
│       ├── mapping.conf
│       ├── osm-drivable.geojson       # 启用 OSM 清洗时生成
│       ├── osm-cleaning-report.json   # 启用 OSM 清洗时生成
│       ├── map.zmap
│       ├── roads.geojson
│       ├── nodes.geojson
│       ├── issues.geojson
│       └── record.json
└── reference-layers/
    └── ref_xxx/
        ├── layer.geojson
        └── record.json
```

当前 `data/` 已加入 `.gitignore`。

## 7. 验证状态

- C++ 单元和端到端测试通过。
- Go API、异步任务状态转换、取消、并发上限和竞态测试通过。
- React TypeScript 和 Vite 生产构建通过。
- C++ 中观仿真数值矩阵、Go 仿真端点 200/400/422 与并发信号量取消路径通过。
- 使用真实 Shapefile 和 GeoJSON 通过 HTTP 完成上传、检查、地图编译、道路/节点 GeoJSON 获取和经纬度道路匹配。
- 验证 `MultiPolygon` 行政区数据在编译前返回 `422`，不会创建无效后台任务。
- 使用武汉市 `MultiPolygon` 边界完成检查、WGS84 转换、参考图层发布、列表和 GeoJSON 获取闭环；地图版本列表保持不变。
- 验证武汉边界持久化数据为 1 个有效 WGS84 `MultiPolygon`；空路网自动定位和图层行手动定位均复用通用 GeoJSON 包围盒计算，支持嵌套的 Polygon / MultiPolygon 坐标。
- 验证生产构建会输出独立 MapLibre Worker，控制服务以 JavaScript MIME 类型返回该资源，避免静态回退页被浏览器误当作模块 Worker。
- 使用临时 `Point` POI 完成创建、属性读取、重命名、样式更新和删除闭环；删除后接口返回 `404`，武汉边界不受影响。
- 通过 HTTP 完成混合 LineString / MultiLineString 武汉 OSM 路网（68,559 条道路）的检查与编译发布：115,178 节点、269,060 有向边、无 fatal 错误；导出 GeoJSON 157,769 要素，经纬度道路匹配返回正确候选。
- 使用 `preprocess-osm` 将武汉原始路网清洗为 41,687 条可行车道路，生成按过滤原因和道路等级统计的 JSON 报告；清洗版地图编译及双向 A* 路由回归通过。
- 通过 Web API 上传武汉 68,559 条 OSM 道路，检查结果自动返回 `osmRoadData:true`；异步任务在约 3.6 秒内完成清洗、148,787 条有向边编译和报告持久化，任务结果包含完整 `cleaning` 摘要。
- 在武汉 269,060 条有向边地图上完成 100 车、900 tick、双向 A* 仿真：相同 OD 仅规划 1 次，输出 100 条轨迹和 10,142 个样本，C++ 推进约 61 ms；Web 类型检查和生产构建通过。
- 动态路由 overlay、在途精确位置重规划、原终点保持、限速/降容/周期拥堵绕行、成功/失败统计、出口 headway 和转向级信号相位的 C++/Go/Web 类型回归通过。
- Agent 工作台 TypeScript 类型检查与生产构建通过；组件拆分为 shell + 任务面板 + 检查器 + 决策横幅，会话状态收敛到 `useAgentSession` hook。
- 车辆位置插值（edgeId+offsetM 按有向边几何还原）在武汉路网上冒烟验证：决策边界冻结、四算法候选比较、候选提交后车辆标记沿剩余路线移动。
- Benchmark 工作台通过真实服务完成任务列表、历史报告、前端清单提交、进度轮询和运行中取消闭环；同源代理通过 Go `8082` 提交武汉固定 A* 实验并读取完成状态与报告，未由客户端直连 Python `8092`；桌面 1440 px、平板 760 px、手机 390 px 均无横向溢出和控制台错误。

## 8. 当前限制

1. 地图记录暂存在本地文件，不支持多实例并发写入。
2. 任务队列当前位于单个 Go 进程内，服务重启后不恢复进行中的任务；生产版应接入持久化队列。
3. GeoJSON 适合 MVP，中大型地图需要切换为矢量瓦片和分级加载。
4. 参考图层目前按工作区全局加载；大型行政区、建筑物和 POI 数据应生成 MVT，并按视口请求。
5. 不带具体坐标的全局问题（例如整个路网不连通）只显示在列表中，不生成地图点。
6. 参考要素属性面板当前最多展示前 12 个属性，尚未提供属性搜索和复杂对象展开。
7. 删除参考图层当前是不可恢复的文件删除，生产版应增加软删除、审计日志或对象存储版本。
8. 尚未提供浏览器端问题修复和重新编译操作。
9. 尚未实现用户、项目和权限系统。
10. 前端 MapLibre 目前打入主包，后续可按路由或模块进行代码分割。
11. 仿真端点当前同步内联完整回放，受 40 万周期样本预算保护；十万级实时运行需要异步 Worker、分块回放和 WebSocket 二进制帧。
12. Benchmark Job Service 已通过 Go 控制面提供同源反向代理，但仍是独立 Python 进程；尚未实现统一鉴权、用户级配额、自动进程监管和跨进程分布式调度。
13. Benchmark 清单会记录随机种子，但种子驱动的随机事故/拥堵生成器与路线抖动、无效动作等二阶段指标尚未实现。

## 9. 下一步

1. 会话级暂停、单步、事件推进与恢复运行已随 Agent 会话交付；剩余部分是传统批量仿真的异步 run、分块回放和实时二进制帧。
2. Agent 调试时间线已随 Agent 工作台交付（观察/工具/Guard/动作按仿真时间记录并带状态版本徽章）；剩余部分是 LLM Reasoning Summary 展示与决策回放。
3. 为 Benchmark Job Service 增加统一鉴权、用户级配额和进程监管，并为长实验增加分布式调度、实时事件流和报告路由级代码分割。
4. 为已实现的转向级信号相位增加冲突组、从转向车道数自动推导流率、自动配时，以及替代道路恢复后的重规划冷却与收益扫描。
5. 增加问题筛选、批量确认、修复操作及版本差异对比。
6. 将任务状态和地图元数据迁移到 PostgreSQL，并接入持久化工作队列。
