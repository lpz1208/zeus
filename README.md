# Zeus

Zeus 是一个独立开发的地理空间导航智能体仿真与评测平台。当前仓库已完成作为 Agent Environment 基础的地图引擎、四算法路由内核和确定性中观交通仿真 MVP：道路 Shapefile 或 GeoJSON 可以编译为只读 `.zmap`，OSM 道路可自动执行机动车画像清洗；用户可在 Web 点选 OD、规划路线，按车辆、道路和路口编排控制事件，配置转向级信号相位与独立饱和放行率，运行多车仿真并通过时间滑块回放车辆轨迹。封路、限速、降容和可选的周期拥堵扫描会更新动态路由权重并重规划受影响车辆，路段还可配置密度插值的出口放行间隔。

平台已经把同步仿真演进为有状态 Environment：Navigation Agent 通过结构化 Observation 感知道路世界，把 Dijkstra、A* 和双向搜索作为 Tools 动态选择，并通过带状态版本的 Action 提交路线。LLM 不替代路径算法，也不进入逐 tick 热路径；D* Lite、K 最短路和时间依赖路由仍在后续计划中。

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

HTTP 侧由 `/api/maps/{id}/agent/sessions` 系列端点驱动：创建（OD 第 7 列 `agent` 标记）、step(untilEvent) 返回 decisionId、plan 产候选、actions 提交 commit_route/keep_route（state version + 仿真时间 TTL 校验）、result 内联导出；`GET /api/maps/{id}/agent/tools` 返回 `routing-tools-v1` 四算法能力注册表。动作只有在 C++ Worker 接受后才关闭决策；墙上超时会实际提交 keep fallback，活动决策未解决前不能继续 step；run 使用非阻塞 resume，之后可以 pause/observe。暂停边界可创建带版本的持久化快照，并通过确定性动作重放恢复成独立 Session；快照落在地图数据目录中，控制服务或 Worker 重启后仍可恢复。

`apps/agent-runtime` 提供 A2 单导航智能体闭环（Python，uv 管理）：`EnvironmentClient` HTTP 传输抽象、`RulePolicy` 确定性基线、LangGraph 八节点主决策图（纯循环仅作故障兜底）、Action Guard、Gymnasium 风格适配器，以及严格 JSON 输出的 Chat Completions 兼容 `ModelProvider`。模型只能选择环境签发的 `candidateId`，失败时确定性降级为规则策略。运行时支持 SQLite Checkpointer、稳定 `thread_id` 中断/恢复，以及可查询的 Observation→Tools→Decision→Guard→Action DecisionTrace。`make agent-runtime-test` 跑单测；起服务后 `make agent-runtime-e2e` 在真实地图上验证封路→失效→重规划→到达全链路。

批量评测入口按清单运行“场景 × 策略 × 重复次数”，首批策略包含固定算法、事件触发的单算法动态重规划、规则 Agent 和模型 Agent；版本化报告内嵌原始清单，记录成功率、旅行时间、路线长度、重规划、路线工具调用、拥堵暴露、节点级决策延迟、实时倍率、token 与可配置模型费用，并导出 JSON 和逐次运行 CSV：

```bash
cd apps/agent-runtime
cp examples/benchmark.example.json /tmp/zeus-benchmark.json
# 编辑 mapId、OD、控制事件；若保留 model-agent，还需配置下方三个模型变量。
uv run python -m zeus_agent.benchmark_cli \
  --manifest /tmp/zeus-benchmark.json \
  --output /tmp/zeus-benchmark-report.json \
  --csv /tmp/zeus-benchmark-runs.csv
```

前端或其他客户端应通过持久化任务服务运行长实验，而不是直接启动 CLI。任务服务默认监听 `127.0.0.1:8090`，使用 SQLite 保存清单、进度、取消状态和报告，并通过受限线程池控制并发；Go 控制面默认把同源 `/api/benchmarks` 代理到该服务：

```bash
# 先在另一个终端运行 make run
make agent-benchmark-service

curl -X POST http://127.0.0.1:8080/api/benchmarks \
  -H 'Content-Type: application/json' \
  --data-binary @apps/agent-runtime/examples/benchmark.example.json
```

任务 API：

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `POST` | `/api/benchmarks` | 校验清单并创建异步任务 |
| `GET` | `/api/benchmarks?limit=50` | 查询最近任务 |
| `GET` | `/api/benchmarks/{id}` | 查询状态与运行进度 |
| `GET` | `/api/benchmarks/{id}/result` | 获取完成或已取消任务的报告 |
| `POST` | `/api/benchmarks/{id}/cancel` | 请求安全边界取消 |
| `GET` | `/health` | 服务健康检查 |

服务重启时，未完成任务会从头重新排队，以保证每次策略对照使用完整一致的 Episode；运行中取消会在当前安全决策边界生效。若正等待模型响应，最长等待时间由 `--model-timeout` 限制。可用 `--workers` 和 `--max-pending` 控制同时运行数与队列容量。

Web 顶栏的 `BENCH` 工作区使用同源 `/api/benchmarks`，可编辑多场景与四类策略，查看场景 × 策略进度、取消任务、浏览历史和聚合指标，并下载 JSON/CSV 报告。Go 服务可用 `--benchmark-url` 覆盖上游地址；只有需要绕过控制面调试时，才使用前端环境变量 `VITE_BENCHMARK_BASE_URL` 直连任务服务。

默认 CLI 使用 LangGraph + 规则基线；接兼容模型服务时只从环境变量读取密钥：

```bash
export ZEUS_MODEL_API_KEY='...'
export ZEUS_MODEL='your-model-id'
export ZEUS_MODEL_BASE_URL='https://provider.example/v1'
cd apps/agent-runtime
uv run python -m zeus_agent.run --map-id <map-id> --provider openai-compatible
```

需要把快速仿真与慢速推理解耦时，可在确定性节点边界持久化并稍后恢复；恢复不会重新创建环境 Session，也不会重放已执行动作：

```bash
uv run python -m zeus_agent.run --map-id <id> \
  --checkpoint-db .runs/checkpoints.sqlite \
  --trace-db .runs/traces.sqlite \
  --thread-id experiment-01 --interrupt-after observe

uv run python -m zeus_agent.run --map-id <id> \
  --checkpoint-db .runs/checkpoints.sqlite \
  --trace-db .runs/traces.sqlite \
  --thread-id experiment-01 --resume

uv run python -m zeus_agent.trace \
  --db .runs/traces.sqlite --thread-id experiment-01 --node decide
```

Agent 图状态和环境快照分开持久化：LangGraph SQLite 保存决策节点，控制面保存带地图标识、请求、动作日志和目标 tick 的环境快照；恢复时若原 Session 已丢失，会从环境快照确定性重建。

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
