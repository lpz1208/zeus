# Zeus

[![CI](https://github.com/lpz1208/zeus/actions/workflows/ci.yml/badge.svg)](https://github.com/lpz1208/zeus/actions/workflows/ci.yml)

Zeus 是一个独立开发的地理空间导航智能体仿真与评测平台。当前仓库已完成作为 Agent Environment 基础的地图引擎、十算法路由内核和确定性中观交通仿真 MVP：道路 Shapefile 或 GeoJSON 可以编译为只读 `.zmap`，OSM 道路可自动执行机动车画像清洗；用户可在 Web 点选 OD、规划路线（Yen K 最短路一次产出多条可对比候选，搜索扩展过程可动画回放），按车辆、道路和路口编排控制事件，配置转向级信号相位与独立饱和放行率，运行多车仿真并通过时间滑块回放车辆轨迹。封路、限速、降容和可选的周期拥堵扫描会更新动态路由权重并重规划受影响车辆，路段还可配置密度插值的出口放行间隔。

平台已经把同步仿真演进为有状态 Environment：Navigation Agent 通过结构化 Observation 感知道路世界，把 Dijkstra、A*、双向搜索、Yen K 最短路、LPA*、D* Lite 和时间依赖 Dijkstra 作为 Tools 动态选择，并通过带状态版本的 Action 提交路线；任意算法的搜索 settle 序列可记录并在 Web 以波前动画回放。LLM 不替代路径算法，也不进入逐 tick 热路径。时间依赖路由使用已配置的道路变速事件预测通行时间，模型边界见 [实现说明](docs/time-dependent-routing.md)。

## 快速启动

环境需要 C++20、CMake、GDAL/OGR、Boost、Go、Node.js、Python 3.9+、curl 和 Protobuf 编译器。

```bash
make run
```

该命令构建并监管 Go 控制面与 Python Benchmark Job Service；任一进程异常退出会停止另一进程，`Ctrl-C` 会统一优雅关闭。

然后访问：

```text
http://127.0.0.1:8080
```

启动脚本使用独立的 `/api/live` 检查控制服务存活，`/api/health` 继续返回包含 Benchmark 的整体状态。修改 `ZEUS_ADDR` 时检查地址会同步推导；本地请求自动绕过终端代理，curl 不读取个人配置。检查失败会输出实际 URL、curl 退出码、HTTP 状态及错误原因，可用 `ZEUS_STARTUP_TIMEOUT` 调整默认 30 秒的启动期限。Vite 的大文件警告不代表启动失败。

## 分步构建

```bash
make build-map
make build-server
make build-web
make test
make algorithm-e2e
```

`make test` 包含 C++、Go、Python、前端类型检查与自动导航测试。`make algorithm-e2e` 在临时目录创建小型路网，启动真实控制服务，验证算法运行、封路、单步调试、实验持久化、精确路径跨重启恢复和前端自动导航到达；无需模型服务。CI 同时运行这两项和前端生产构建。使用已有真实路网的验收方式见[算法实验台文档](docs/algorithm-lab.md#端到端验收)。

## 当前组件

- `cpp/map-engine`：C++ 地图导入、OSM 可行车清洗、拓扑、运行时索引和地图匹配。
- `cpp/routing-core`：C++ Dijkstra、A*、双向 Dijkstra、双向 A*，含起终点吸附与路线导出。
- `cpp/simulation-core`：C++ 确定性中观车辆推进、路线池、入口容量、出口流率（默认 1.4/2.0 s 且到达免闸）、队列序放行、per-edge KPI、回溢、转向信号相位、动态权重重规划、agent 车辆决策事件与路线注入、车辆/道路/路口控制、采样和轨迹导出；提供 tick 边界控制的 `SimulationSession`（reset/step/stepUntilEvent/observe/snapshot/commit/keep/resume/run-to-end/pause/close）。
- `tools/zeus-map`：地图检查、导入、验证、GeoJSON 导出、位置查询、路径规划、仿真和常驻 session-worker CLI。
- `proto/agent/v1`：Agent 环境目标协议（Observation/Action/DecisionTrace、三种决策模式）；当前由 session-worker 帧协议承载同一语义。
- `apps/control-server`：Go 地图与仿真控制 API、按地图常驻的 C++ 路由 Worker、仿真进程并发门禁、Agent 决策屏障协调器和静态 Web 托管；`cmd/zeus-osm-turns` 从 OSM PBF 提取机动车 via-node/via-way 转向限制。
- `apps/web`：React + MapLibre 地图工作台、路线规划、控制时间线和车辆回放。
- `docs`：整体架构、Agent Environment、地图引擎、路由内核和 Web 工作台设计。

详细说明：

- [整体架构](docs/overall-architecture.md)
- [地理空间导航智能体环境与实施计划](docs/geospatial-agent-environment.md)
- [地图引擎](docs/map-engine-design.md)
- [路由内核](docs/routing-core-design.md)
- [中观仿真内核](docs/simulation-core-design.md)
- [Web 地图工作台](docs/web-map-workbench.md)
- [自定义导航算法实验台：控制边界与方法契约](docs/algorithm-lab.md)

## 自定义导航算法

点击工作台顶部 **CODE**，选择或精确输入起终点，编写 Python 子集的 `route(ctx)` 并运行验证。编辑器提供高亮、方法补全、语法检查、单步与断点调试；C++ 独立校验路线，与同条件 Dijkstra 比较，并在地图回放搜索。支持临时封路、步数预算和停止运行；代码修改后旧结果自动标记过期。完成的静态实验保存到服务器，可恢复源码、条件与结果，并比较相同快照下的两次实验。

在 **AGENT → 决策控制台 → 代码** 中，同样的代码可以基于车辆当前位置、封路与拥堵代价生成候选；通过决策横幅提交后，车辆执行该精确路径。提交和执行分别检查版本与道路合法性，快照恢复保留完整路径和偏移。完整接口与限制见[算法实验台文档](docs/algorithm-lab.md)。

支持 **开启自动导航 / 单次决策 / 暂停 / 停止**：从出发前开始，根据事件串行执行用户代码和车辆动作。`ctx.observation()` 读取当前车辆及决策原因，`ctx.keep()` 明确保持原路线。代码失败或路径执行失败时自动暂停；每次决策保留源码、观察、路径和日志，可导出 JSON。源码、决策记录和车辆边界自动保存到服务器，刷新后可在“导航历史”查看、导出并恢复到独立暂停会话。保存失败会停止后续导航。选择“交给后台运行”后可关闭页面；后台任务支持暂停、继续、停止、导出和删除，服务重启后可手动恢复最近检查点。后台任务占用会话期间，手动写操作被阻止。

点击 **调试** 可在语句执行前暂停，单步进入函数、继续到指定行断点，并查看局部变量和调用栈。暂停保留同一进程现场，等待时间不计入计算预算；空闲 60 秒会自动回收，重复调试命令由暂停序号校验拦截。

## 转向代价

建图期自动生成转向罚时（U-turn 5 s、≥100° 急左转 2 s、支路进干路 3 s），与转向限制 sidecar max-merge，让路口延误进入路由代价。

## Agent 会话

常驻 worker 承载有状态仿真会话，支持观察、事件驱动决策和动作注入（详见 [智能体环境设计](docs/geospatial-agent-environment.md)）：

```bash
printf 'reset\ts1\t900\t1\t30\t1.4\t2.0\t0\t1.25\t0\tod.csv\t\t\nstep_event\ts1\t600\nshutdown\n' \
  | ./build/zeus-map session-worker city.zmap
```

HTTP 侧由 `/api/maps/{id}/agent/sessions` 系列端点驱动：创建（OD 第 7 列 `agent` 标记）、step(untilEvent) 返回 decisionId、plan 产候选（可带 `kPaths` 与 `recordTrace`，K 最短路一次返回多条各自可提交的候选）、actions 提交 commit_route/keep_route（state version + 仿真时间 TTL 校验）、result 内联导出；`GET /api/maps/{id}/agent/tools` 返回 `routing-tools-v2` 十算法能力注册表。动作只有在 C++ Worker 接受后才关闭决策；墙上超时会实际提交 keep fallback，活动决策未解决前不能继续 step；run 使用非阻塞 resume，之后可以 pause/observe。暂停边界可创建带版本的持久化快照，并通过确定性动作重放恢复成独立 Session；快照落在地图数据目录中，控制服务或 Worker 重启后仍可恢复。

`apps/agent-runtime` 提供 A2 单导航智能体闭环（Python，uv 管理）：`EnvironmentClient` HTTP 传输抽象、`RulePolicy` 确定性基线、LangGraph 八节点主决策图（纯循环仅作故障兜底）、Action Guard、Gymnasium 风格适配器，以及严格 JSON 输出的 Chat Completions 兼容 `ModelProvider`。模型只能选择环境签发的 `candidateId`，失败时确定性降级为规则策略。成功和失败尝试中供应商已返回的 token 用量都会累计，失败耗时也进入模型延迟统计；未返回用量的超时请求无法推算实际计费。运行时支持 SQLite Checkpointer、稳定 `thread_id` 中断/恢复，以及可查询的 Observation→Tools→Decision→Guard→Action DecisionTrace。`make agent-runtime-test` 跑单测；起服务后 `make agent-runtime-e2e` 在真实地图上验证封路→失效→重规划→到达全链路。

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
# 分步部署时先在另一个终端只运行 Go 控制面
make run-control

# 当前终端运行独立 Benchmark Job Service
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

服务正常停机或异常退出后，未完成且未经用户取消的任务会在重启时从头重新排队，以保证每次策略对照使用完整一致的 Episode；用户取消会持久化，重启后不会执行。统一启动会等待控制 API 就绪后恢复评测，关闭时先停止评测并释放 Session，再关闭控制服务。运行中取消会中断模型请求或重试退避，并在当前安全决策边界生效。`--model-timeout` 是单次模型决策的总时间预算，包含所有请求、重试和退避（默认 60 秒）。可用 `--workers` 和 `--max-pending` 控制同时运行数与队列容量。

Web 顶栏的 `BENCH` 工作区使用同源 `/api/benchmarks`，可编辑多场景与四类策略，查看场景 × 策略进度、取消任务、浏览历史和聚合指标，并下载 JSON/CSV 报告。Go 服务可用 `--benchmark-url` 覆盖上游地址；只有需要绕过控制面调试时，才使用前端环境变量 `VITE_BENCHMARK_BASE_URL` 直连任务服务。

报告 v5 包含 Guard 阻止、动作提交尝试、服务端拒绝/错误、保持路线降级、道路序列提交计数，以及 C++ 实际路线应用成功/失败、改道/同路应用、剩余路线重叠率和 A→B→A 回切次数。结果页可对比均值并展开每次运行；JSON/CSV 同步包含指标，旧报告缺失值显示为 `—`。获准提交与实际执行分别统计；口径见 [Benchmark 指标](docs/benchmark-metrics.md)。

`make run` 的本地监管参数均可通过环境变量覆盖：`ZEUS_ADDR`、`ZEUS_CONTROL_BASE_URL`、`ZEUS_DATA_DIR`、`ZEUS_BENCHMARK_HOST`、`ZEUS_BENCHMARK_PORT`、`ZEUS_BENCHMARK_DB`、`ZEUS_BENCHMARK_WORKERS`、`ZEUS_BENCHMARK_MAX_PENDING` 和 `ZEUS_BENCHMARK_MODEL_TIMEOUT`。模型密钥仍只从服务进程环境读取。

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

Zeus 不依赖 SUMO。对于已有的 OSM PBF，可先生成可审计、可 diff 的转向 sidecar，再随道路数据编译进 `.zmap` v3：

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

提取器支持机动车 `no_*`、`only_*` 和 `restriction:motorcar`，会跳过 `except=motorcar`。已支持拓扑唯一的 via-way 道路链，搜索和车辆重规划保留进入历史；conditional 与复杂车型例外仍不展开，跳过原因会计数。格式、回退和边界见 [via-way 实现说明](docs/via-way-routing.md)。

### 自定义代码与随机事件评测

Benchmark 支持 `custom_code` 策略，在初始边界及后续决策事件调用 `route(ctx)`。报告 v5 保存源码 SHA-256、场景 SHA-256、实际道路事件、逐次代码决策和原生 A→B→A 路线回切次数；源码本身保存在报告 manifest。代码错误、无路或动作未采用会终止该次运行，不自动改用其他算法。

场景可配置 `randomEvents`：候选道路、事件数、发生窗口、持续时间以及封路/降速类型。同一重复轮次的所有策略共享 `seed + repetition - 1` 生成的事件；事件对齐仿真 tick，结束时恢复道路。JSON/CSV 均包含实际生成的事件。详见 [Benchmark 指标与可重复性](docs/benchmark-metrics.md)。

2026-09-25：新增格式 3 快照内容/地图校验、道路恢复收益扫描与换路稳定性参数，并实现含转向限制的双向 Dijkstra/A*。各项完成状态、验收与后续缺项见 [实现进度](docs/implementation-roadmap.md)。恢复扫描默认关闭，旧格式快照恢复时标记为未校验。

已接入 `lpa`（LPA*）与 `dstar`（D* Lite）增量路由：Agent 会话复用搜索状态，支持动态封路/恢复和费用变化；普通路由执行单次搜索。使用方法、性能边界与快照契约 v3 兼容性见 [增量路由说明](docs/incremental-routing.md)。

已接入 `alt`（地标 A*）：按需预处理有向地标距离并复用，界面显示地标数量和预处理状态；实际搜索保留禁转、封路、动态费用与部分道路起终点语义。内存预算、冷启动成本和使用方式见 [ALT 说明](docs/alt-routing.md)。

已接入 `ch`（收缩层级）：静态路况复用包含转向语义的捷径索引，预处理达到预算时保留精确可搜索的核心图；封路或费用变化时明确回退至双向 Dijkstra，界面显示实际算法和原因。使用方式及边界见 [CH 说明](docs/ch-routing.md)。
