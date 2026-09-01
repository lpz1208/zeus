# Zeus

Zeus 是一个独立开发的地理空间导航智能体仿真与评测平台。当前仓库已完成作为 Agent Environment 基础的地图引擎、四算法路由内核和确定性中观交通仿真 MVP：道路 Shapefile 或 GeoJSON 可以编译为只读 `.zmap`，OSM 道路可自动执行机动车画像清洗；用户可在 Web 点选 OD、规划路线，按车辆、道路和路口编排控制事件，配置转向级信号相位与独立饱和放行率，运行多车仿真并通过时间滑块回放车辆轨迹。封路、限速、降容和可选的周期拥堵扫描会更新动态路由权重并重规划受影响车辆，路段还可配置密度插值的出口放行间隔。

下一阶段将把同步仿真演进为有状态 Environment，让 Navigation Agent 通过结构化 Observation 感知道路世界，把 Dijkstra、A*、双向搜索以及后续 D* Lite、K 最短路和时间依赖路由作为 Tools 动态选择，并通过可校验的 Action 提交路线。LLM 不替代路径算法，也不进入逐 tick 热路径。

## 快速启动

环境需要 C++20、CMake、GDAL/OGR、Boost、Go、Node.js 和 Protobuf 编译器。

```bash
make run
```

然后访问：

```text
http://127.0.0.1:8080
```

## 分步构建

```bash
make build-map
make build-server
make build-web
make test
```

## 当前组件

- `cpp/map-engine`：C++ 地图导入、OSM 可行车清洗、拓扑、运行时索引和地图匹配。
- `cpp/routing-core`：C++ Dijkstra、A*、双向 Dijkstra、双向 A*，含起终点吸附与路线导出。
- `cpp/simulation-core`：C++ 确定性中观车辆推进、路线池、入口容量、出口流率（默认 1.4/2.0 s 且到达免闸）、队列序放行、per-edge KPI、回溢、转向信号相位、动态权重重规划、agent 车辆决策事件与路线注入、车辆/道路/路口控制、采样和轨迹导出；提供 tick 边界控制的 `SimulationSession`（reset/step/stepUntilEvent/observe/snapshot/commit/keep/resume/run-to-end/pause/close）。
- `tools/zeus-map`：地图检查、导入、验证、GeoJSON 导出、位置查询、路径规划、仿真和常驻 session-worker CLI。
- `proto/agent/v1`：Agent 环境目标协议（Observation/Action/DecisionTrace、三种决策模式）；当前由 session-worker 帧协议承载同一语义。
- `apps/control-server`：Go 地图与仿真控制 API、按地图常驻的 C++ 路由 Worker、仿真进程并发门禁、Agent 决策屏障协调器和静态 Web 托管；`cmd/zeus-osm-turns` 从 OSM PBF 提取机动车 via-node 转向限制。
- `apps/web`：React + MapLibre 地图工作台、路线规划、控制时间线和车辆回放。
- `docs`：整体架构、Agent Environment、地图引擎、路由内核和 Web 工作台设计。

详细说明：

- [整体架构](docs/overall-architecture.md)
- [地理空间导航智能体环境与实施计划](docs/geospatial-agent-environment.md)
- [地图引擎](docs/map-engine-design.md)
- [路由内核](docs/routing-core-design.md)
- [中观仿真内核](docs/simulation-core-design.md)
- [Web 地图工作台](docs/web-map-workbench.md)

## 转向代价

建图期自动生成转向罚时（U-turn 5 s、≥100° 急左转 2 s、支路进干路 3 s），与转向限制 sidecar max-merge，让路口延误进入路由代价。

## Agent 会话

常驻 worker 承载有状态仿真会话，支持观察、事件驱动决策和动作注入（详见 [智能体环境设计](docs/geospatial-agent-environment.md)）：

```bash
printf 'reset\ts1\t900\t1\t30\t1.4\t2.0\t0\t1.25\t0\tod.csv\t\t\nstep_event\ts1\t600\nshutdown\n' \
  | ./build/zeus-map session-worker city.zmap
```

HTTP 侧由 `/api/maps/{id}/agent/sessions` 系列端点驱动：创建（OD 第 7 列 `agent` 标记）、step(untilEvent) 返回 decisionId、plan 产候选、actions 提交 commit_route/keep_route（state version + 仿真时间 TTL 校验）、result 内联导出；`GET /api/maps/{id}/agent/tools` 返回 `routing-tools-v1` 四算法能力注册表。动作只有在 C++ Worker 接受后才关闭决策；墙上超时会实际提交 keep fallback，活动决策未解决前不能继续 step；run 使用非阻塞 resume，之后可以 pause/observe。暂停边界还可创建进程内快照，并通过确定性动作重放恢复成独立 Session，用于实验分叉；跨 Worker 重启的持久化快照仍在后续计划中。

## OSM 转向限制

Zeus 不依赖 SUMO。对于已有的 OSM PBF，可先生成可审计、可 diff 的转向 sidecar，再随道路数据编译进 `.zmap` v2：

```bash
./build/zeus-osm-turns \
  --input data/wuhan/hubei-latest.osm.pbf \
  --bbox 113.696653,29.972873,115.076933,31.362241 \
  --output data/wuhan/wuhan-turn-restrictions.csv

./build/zeus-map import roads.geojson \
  --mapping roads.mapping \
  --turn-restrictions data/wuhan/wuhan-turn-restrictions.csv \
  --output city.zmap
```

提取器支持机动车 `no_*`、`only_*` 和 `restriction:motorcar`，会跳过 `except=motorcar`。当前明确不展开 via-way、conditional 与复杂车型例外，并在命令输出中按原因计数。
