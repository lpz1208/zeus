# ALT 地标 A* 路由

`alt` 已贯通 CLI、HTTP 路由、仿真车辆、Agent plan/commit、Python reactive Benchmark 与工作台选择器。它返回当前路况下的最短时间路线，保留禁转、转向罚时、部分道路起终点、twin 候选和当前道路驶出语义。选择 `alt` 即可使用，不需要额外配置。

## 预处理与正确性

在忽略转向规则的基础道路图上，为每个地标 L 分别执行正向和反向 Dijkstra，保存 d(L,v) 与 d(v,L)。有向下界为：

```
h(v,t) = max(0, max_L[d(L,t)-d(L,v)], max_L[d(v,L)-d(t,L)])
```

无穷距离不相减；可达性矛盾证明目标不可达。浮点距离差减去相对保护量，搜索允许重新打开状态。多个目标入口取下界与目标部分道路费用之和的最小值，实际搜索仍使用入边状态并检查最后一次转向。当前 overlay 只能关闭道路或把基础费用乘以不小于 1 的因子，所以预处理下界在封路、恢复及费用变化后仍有效，无须重建。

算法依据：[Goldberg 与 Harrelson，Computing the Shortest Path: A* Search Meets Graph Theory](https://www.microsoft.com/en-us/research/publication/computing-the-shortest-path-a-search-meets-graph-theory/)。实现采用确定性的图距离最远点选择，默认最多 8 个地标，优先覆盖尚未覆盖的连通部分，跳过孤立点。

## 生命周期与费用

每个 `RoutePlanner` 首次 ALT 查询通过 `call_once` 构建只读索引，之后所有车辆、起终点及算法切换均可复用。并发首次查询只发布一份索引。Worker 重启后重新构建，尚无磁盘地标文件或跨进程共享。Session Worker 的仿真引擎与 Agent 候选共用同一个规划器；ALT 车辆初始化就会触发预处理，因此后续首次 plan 通常已显示复用。普通路由 Worker 与 Session Worker 属于不同进程，各自保留索引，不能把单份预算视为服务总内存上限。

距离表预算为每个规划器 256 MiB，按节点数自动降低地标数量；连一个地标也放不下时采用零下界。预算只包含双向 `double` 距离表，不包含优先队列、入边索引、搜索标签和其他临时内存。每次查询仍需要 O(E+V) 标签空间；复用的是地标表，不是上次搜索结果。预处理运行约 2L 次 Dijkstra，首次查询可能明显更慢，收益需由多次查询摊销。

成功响应包含 `landmarkCount`、`landmarkBytes`、`landmarkPreprocessMs`、`landmarkReused`；CLI 使用相应 snake_case 字段。`computeMs` 包含吸附、首次预处理（如有）和搜索，HTTP 传输及 GeoJSON 输出不计入。扩展数按边状态统计，不是不同路口数；不能仅凭扩展数宣称端到端加速。

## 模型边界与恢复

ALT 只使用当前静态费用，不预测已知未来变速；携带出发时间或速度计划的请求应选择 `tddijkstra`。它不是 LPA*/D* Lite 增量修复，也不是 CH。

Agent 提交保存精确道路序列，快照继续使用格式 3 / `zeus-session-replay-v3`，无需保存地标表；重启恢复后精确路径不依赖预处理状态。与现有增量候选相同，超过 9999 条道路的 ALT 候选拒绝提交。

## 验证

原生路由测试包括 600 组与 Dijkstra 的费用和合法性对拍、有向不连通图的独立全源最短路下界检查、零预算退化、部分边、自环、32 次封路/费用/起终点变化以及 8 个并发首次查询。规则网格验证预处理复用与扩展数下降，仿真测试覆盖恢复重路由。端到端脚本验证 HTTP 指标、Agent 封路/开放、移动车辆、多车复用、算法切换、精确提交、跨进程恢复、到达及 Python Benchmark。

2026-09-26：临时路网及武汉地图完整端到端均通过。武汉 OD `(114.4911555,30.9567005) → (114.4565038,30.9259153)` 的距离表为 14,742,784 字节（8 个地标）；单次观测的首次预处理为 20.705 ms，复用查询 computeMs 为 0.084 ms，扩展数 ALT 26 / Dijkstra 79，路线费用一致。Agent 移动与封路场景同样通过。该记录用于验证功能与观测指标，不代表大规模性能基准。
