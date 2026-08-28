# Zeus 路由内核设计与当前实现

> 状态：四种基准算法 + edge-to-edge 转向约束已实现  
> 最后更新：2026-08-28  
> 上层方案：[overall-architecture.md](overall-architecture.md)

## 1. 当前目标

路由内核第一阶段建立以下闭环：

```text
起终点经纬度
→ R-tree 吸附到有向边（含正反 twin 展开）
→ 最优先搜索（dijkstra / astar / bidijkstra / biastar）
→ 首末边按 offset 裁剪的完整路线
→ WGS84 GeoJSON + 距离 / 时长 / 扩展节点 / 计算耗时统计
```

路由只消费只读 `MapRuntime`（出边 CSR + 入边 CSR + R-tree），不修改地图数据。K 条路径、动态权重和重规划属于后续阶段。

## 2. 已实现能力

### 2.1 代价模型

```text
edge_cost = length_m / max(speed_limit_mps, 0.1)
route_cost = 首边剩余段 + 途经边 + 末边前段（全部按时间累加）
```

- 优化目标是最短旅行时间；距离作为统计同时输出。
- 零或非法限速边 clamp 到 0.1 m/s（质检 error 级、未阻断发布的数据仍可路由，代价大而有限）。
- 全图最大限速 `vmax` 取 clamp 后的值，供启发式使用。

### 2.2 起终点吸附与 twin 展开

- `matchPoint` 候选中按 `score → lateral_distance → edge index` **确定性选优**：`matchPoint` 内部排序非稳定，双向路正反 twin 同分时方向才能可复现。
- 选中边 E（offset s，长度 L）后展开两个虚拟端点：
  - 正向：沿 E 前进到 `E.to`，prefix 代价 `(L - s) / v`；
  - 反向：twin `T`（同 `road_id` 且 from/to 互换），同一物理点在 T 上偏移 `L - s`，前进到 `T.to = E.from`。
- 终点对称：goal 进入边起点节点，suffix 代价为进入后行驶到 offset 的时间。
- **直达组合**：枚举 (start, goal)，同边且 `s_o ≤ s_d` 时得到单边部分路径（覆盖同边正向、零距离），同时作为搜索剪枝下界——「同一条双向路终点在身后」会直接解析为沿反向 twin 的直达段，无需绕行。

### 2.3 统一搜索框架

```text
多源多目标最优先搜索，代价 = 已行时间
Dijkstra：h ≡ 0
A*：h(n) = min(目标进入节点) ||n - g|| / vmax
```

- 启发式**可采纳且一致**（任一边 `len/v ≥ len/vmax ≥ 欧氏/vmax`），首次 settle 即最优，无需重开节点。
- `priority_queue<pair<double, NodeIndex>, greater>`：f 同分按节点索引出队，结果**确定性可复现**，为算法对比实验服务。
- 懒删除过期堆条目；`dist / pred_edge / closed` 按节点一次分配。
- 剪枝：弹出 `f ≥ 已知最优`（直达组合或已找到的完整路径）即终止。
- `expanded_nodes` 口径为被 settle 的节点数。

### 2.4 双向搜索（bidijkstra / biastar）

独立实现 `runBidirectionalSearch`，与单向版共用 `SearchQuery/SearchOutput` 契约：

- **入边 CSR**：`RoutePlanner` 构造时经 `buildIncomingAdjacency` 一次构建（按 `edge.to` 计数排序，桶内 EdgeIndex 升序），跳过 `to` 非法的边。
- **对称势（Ikeda）**：`p(v) = (h_t(v) − h_s(v)) / 2`，h_t / h_s 分别为到 goal 进入节点 / start 出发节点的欧氏距离除以 vmax；势函数在节点首次被搜索触及时计算并缓存，不再为每次查询线性扫描全图。约减代价 `w_red = max(0, w + p(v) − p(u))`。bidijkstra 即 `p ≡ 0`。
- **初始标签**：`dist_f[s] = prefix + p(s)`、`dist_b[g] = suffix − p(g)`。该约定下势函数在任意相遇点**精确相消**，追踪的 `mu` 直接就是真实秒数——无需还原，`mu` 也因此可以合法地用直达组合时间做种子。
- **终止判据**：每轮取双侧过滤 stale 后的队顶 `top_f + top_b ≥ mu` 即停止；`top_f ≤ top_b` 时先扩展正向（确定性）。一侧耗尽（top=∞）即停止是安全的：若存在任何 start→goal 路径，goal 进入节点必被正向 settle 并触发连接检测。
- **三处连接检测**（与标签改善解耦，严格 `<` 保先发现）：正向/反向松弛时对侧标签有限；settle 时对侧标签有限（节点相遇场景，不可省略）。
- **禁止用 mu 剪枝松弛**（约减标签 = real + p(v)，p 可正），松弛只做 `candidate < dist[to]` 判断。
- `total_time_s` 在结束时用最终标签重算；`expanded_nodes` 为两侧 settle 之和。

### 2.5 转向约束搜索

`.zmap` v2 可保存 `(from_edge,to_edge)` 转换，转换可为禁止或非负秒数惩罚。存在转换数据时，搜索状态从 node 升级为“到达当前节点的 incoming edge”：

```text
state = incoming_edge
relax(incoming_edge → outgoing_edge)
cost += edge_time(outgoing_edge) + turn_penalty(incoming_edge,outgoing_edge)
```

禁止转换返回无穷代价，不参与松弛；缺省转换允许且代价为零。目标 suffix 前也会检查最后一条边到目标边的转换，避免在终点入口绕过禁转。当前限制是：带转向规则的 `bidijkstra/biastar` 选择会复用正确的单向 edge-state 搜索，尚未实现 restriction-safe 的反向 line graph；无转向规则地图仍使用原双向实现。

地图编译可用可读 sidecar：

```text
from_source_id,via_x,via_y,to_source_id,no|only|penalty[,penalty_s]
```

via 坐标使用道路源 CRS。`only` 会展开为该 incoming edge 到其他所有 outgoing edge 的禁止转换。该文本格式可手写、diff 和制作 Golden Map。`build/zeus-osm-turns` 会双遍扫描 OSM PBF：第一遍收集机动车 restriction relation，第二遍只读取所需 via node 坐标，并按 relation ID 确定性输出同一格式：

```bash
./build/zeus-osm-turns --input region.osm.pbf \
  --bbox min_lon,min_lat,max_lon,max_lat --output turns.csv
```

### 2.6 失败语义

吸附失败（起点/终点超距）、不可达（连通分量不同）、空图都是**一等计算结果**：CLI 退出码 3，HTTP 返回 200 + `{ok:false, reason}`；只有参数错误或进程异常才返回 4xx。

## 3. 代码结构

```text
cpp/routing-core/
├── include/zeus/routing/
│   ├── route_types.h     # Algorithm / RouteRequest / RouteResult / 失败枚举
│   ├── search.h          # SearchEndpoint / SearchQuery / runShortestPathSearch
│   ├── route_planner.h   # RoutePlanner：吸附 + twin + 直达组合 + 组装
│   └── route_exporter.h  # RouteGeoJsonExporter
└── src/
    ├── route_types.cc
    ├── search.cc         # Dijkstra 与 A* 共用同一实现
    ├── route_planner.cc
    └── route_exporter.cc # 每途经边一个 WGS84 Feature，首末边裁剪

tests/routing_tests.cc    # zeus-routing-tests 测试目标
```

依赖关系：`zeus_routing_core → zeus_map_engine`（GDAL/Boost 传递链接）。C++ 内核只接收运行时米制坐标，经纬度转换在 CLI 层通过 GDAL 完成。

## 4. CLI

```bash
./build/zeus-map route city.zmap \
  --lon 114.2661 --lat 30.4796 \
  --dest-lon 114.3163 --dest-lat 30.5278 \
  --algorithm astar \
  --max-distance 100 \
  --output route.geojson
```

成功输出 `route=ok / algorithm / origin.edge… / dest.edge… / edges / length_m / time_s / expanded_nodes / compute_ms [/ features / output]`；失败输出 `route=failed / reason / message` 并以退出码 3 结束。`--output` 写 WGS84 GeoJSON，每途经有向边一个 Feature，属性为 `ROAD_ID / SOURCE_ID / CLASS / LENGTH_M / EDGE_INDEX`，首末 Feature 按 offset 裁剪；失败时不写文件。

### 4.1 常驻路由 Worker

控制服务不再为每个 HTTP 路由请求执行一次完整 CLI。`zeus-map route-worker MAP.zmap` 启动时只加载一次 `.zmap`，并只构造一次 `MapRuntime`、边级 R-tree、入边 CSR 与 `RoutePlanner`。Go 通过 stdin/stdout 的长度前缀帧协议发送 OD、算法、吸附距离和临时 GeoJSON 输出路径；业务结果仍使用原 `route=ok/failed` 文本，因此 HTTP 的 200 计算失败语义保持不变。

`RouteWorkerManager` 按不可变地图版本缓存 Worker，默认最多驻留 4 张地图；超限时只淘汰无在途请求的 LRU Worker。单 Worker 内请求串行，避免 stdout 帧交叉。请求取消或超时会终止卡住的子进程，下一次请求自动重启；服务收到 SIGINT/SIGTERM 时回收全部 Worker。可用 `--route-worker-maps` 调整驻留上限。

## 5. HTTP API

```text
POST /api/maps/{id}/route
{
  "fromLon": 114.2661, "fromLat": 30.4796,
  "toLon": 114.3163, "toLat": 30.5278,
  "algorithm": "astar",        # 可选，默认 dijkstra
  "maxDistance": 100           # 可选，(0, 1000]，默认 100
}
```

响应包含统计、起终吸附候选和 `geojson`（FeatureCollection）。吸附失败与不可达返回 200 + `ok:false, reason, message`。

## 6. Web 工作台

右侧「路由」标签页：激活路由模式 → 地图第一次点击设起点、第二次点击设终点并计算（第三次点击重新开始）；算法下拉切换并立即重算；路线以紫色线渲染并自动缩放；结果面板展示距离、预计时长、扩展节点、计算耗时、途经边数与起终吸附信息；吸附失败 / 不可达给出中文原因。

## 7. 测试与基准

### 7.1 单元测试（zeus-routing-tests）

短直路直达、时间 vs 距离、跨分量不可达、单行环路、A\* 与 Dijkstra 对拍、吸附失败、twin、GeoJSON 导出；双向算法专项覆盖四算法交叉验证和确定性。新增 Golden Map 覆盖：四种算法均绕开禁止转换、转向惩罚改变最优路线。

### 7.2 武汉真实图基准

地图：`data/maps/map_71ad2c3c46bb59ae`（115,178 节点 / 269,060 有向边 / 495 连通分量）。

| 路线 | 算法 | 距离 | 时长 | 扩展节点 | 搜索耗时 |
| --- | --- | --- | --- | --- | --- |
| 汉口站 → 武昌站（8.47 km） | Dijkstra | 8,466.264 m | 761.964 s | 8,098 | 1.01 ms |
| 汉口站 → 武昌站 | A* | 8,466.264 m | 761.964 s | 5,308 | 0.76 ms |
| 汉口站 → 武昌站 | Bidirectional Dijkstra | 8,466.264 m | 761.964 s | 6,268 | 0.90 ms |
| 汉口站 → 武昌站 | Bidirectional A* | 8,466.264 m | 761.964 s | 4,269 | 1.06 ms |
| 光谷 → 武昌站（11.68 km） | Dijkstra | 11,684.316 m | 748.646 s | 14,943 | — |
| 光谷 → 武昌站 | A* | 11,684.316 m | 748.646 s | 6,851 | — |
| 光谷 → 武昌站 | Bidirectional Dijkstra | 11,684.316 m | 748.646 s | 7,865 | — |
| 光谷 → 武昌站 | Bidirectional A* | 11,684.316 m | 748.646 s | 3,725 | — |

四种算法代价完全一致（最优性交叉验证）。相对 Dijkstra 的扩展节点削减：A* 34%–54%，双向 Dijkstra 23%–47%，**双向 A* 47%–75%（最优）**。`compute_ms` 只含吸附与搜索。武汉 35 MB v1 地图的真实 HTTP 请求中，Worker 冷加载在文件缓存冷热不同的两次测量为约 299–516 ms；随后连续 10 次相同请求的端到端 P50 为 20.6 ms、P95 为 21.8 ms（包含 87 Feature GeoJSON 生成和 HTTP JSON 返回）。以同轮 299 ms 冷请求计，热请求 P50 加速 14.5 倍。

导出验证：87 条途经边生成 87 个 Feature，相邻 Feature 端点全部衔接，`LENGTH_M` 合计与 `length_m` 一致。

## 8. 当前限制

1. 起终点各取吸附最优的一条边（含 twin），不从边中段向其他方向离开；需要掉头的场景通过路口绕行完成，多候选多源搜索留待后续。
2. 并列最优路径下 Dijkstra 与 A* 可能返回不同但等价的边序列。
3. OSM PBF 已能自动生成转向 sidecar，但当前只支持单 via-node 的机动车 `no_*` / `only_*`；via-way、conditional 和完整车型例外尚未进入运行时模型。
4. 带转向规则时双向算法暂退化为单向 edge-state 搜索，保证正确性优先。
5. 欧氏距离/全图最大限速启发式仍偏弱；已消除双向 A* 的全图势函数预扫描，ALT/CH 仍未实现。
6. 当前每张地图默认只有一个串行路由 Worker；高并发阶段需要按地图分片多个只读 Worker，或将线程安全搜索上下文下沉到同一进程线程池。

## 9. 下一步

1. 支持 via-way、conditional restriction 和车型 AccessMask。
2. restriction-safe 双向 edge-state 搜索，以及 ALT landmark 预处理。
3. 路由 Worker 分片、空闲 TTL 和无需临时文件的 GeoJSON 帧输出。
4. 固定 OD 矩阵的 P50/P95 基准与 SUMO duarouter 离线对拍。
