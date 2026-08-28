# Zeus

Zeus 是一个独立开发的网页端导航算法验证平台。当前仓库已完成地图引擎、四算法路由内核和确定性中观交通仿真 MVP：道路 Shapefile 或 GeoJSON 可以编译为只读 `.zmap`，OSM 道路可自动执行机动车画像清洗；用户可在 Web 点选 OD、规划路线，按车辆、道路和路口编排控制事件，运行多车仿真并通过时间滑块回放车辆轨迹。

## 快速启动

环境需要 C++20、CMake、GDAL/OGR、Boost、Go 和 Node.js。

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
- `cpp/simulation-core`：C++ 确定性中观车辆推进、路线池、入口容量、回溢、车辆/道路/路口控制、采样和轨迹导出。
- `tools/zeus-map`：地图检查、导入、验证、GeoJSON 导出、位置查询、路径规划和仿真 CLI。
- `apps/control-server`：Go 地图与仿真控制 API、按地图常驻的 C++ 路由 Worker、仿真进程并发门禁和静态 Web 托管；`cmd/zeus-osm-turns` 从 OSM PBF 提取机动车 via-node 转向限制。
- `apps/web`：React + MapLibre 地图工作台、路线规划、控制时间线和车辆回放。
- `docs`：整体架构、地图引擎、路由内核和 Web 工作台设计。

详细说明：

- [整体架构](docs/overall-architecture.md)
- [地图引擎](docs/map-engine-design.md)
- [路由内核](docs/routing-core-design.md)
- [中观仿真内核](docs/simulation-core-design.md)
- [Web 地图工作台](docs/web-map-workbench.md)

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
