# Zeus 中观交通仿真内核设计

> 状态：MVP 已实现，含预编排控制、动态权重重规划和出口流率
> 最后更新：2026-08-30
> 目标：在不依赖 SUMO 的前提下，让带 OD 的车辆通过 Zeus 路由内核规划路线，并以确定性的中观模型在真实路网上推进、导出和回放。

## 1. 本阶段边界

本阶段实现“第一辆 Zeus 车上路”的完整闭环：

```text
车辆 OD
→ RoutePlanner 路线池
→ C++ 中观推进
→ 车辆轨迹 GeoJSON + Playback JSON + 汇总统计
→ Go 同步控制端点
→ Web 控制时间线 + 时间滑块与倍速回放
```

当前模型保留每辆车的独立 ID、路线、路段位置、发车和到达时间，但不模拟车道级跟车、换道和车辆动力学。实现完全独立，不链接或调用 SUMO。

## 2. 模块结构

| 模块 | 职责 |
| --- | --- |
| `simulation_types.h` | 配置、需求、车辆记录、采样、统计和运行结果 |
| `vehicle_store.h` | 仿真热路径的 SoA 车辆存储 |
| `simulation_engine.h/.cc` | 路线池、容量、固定步长推进、死锁检测和统计 |
| `playback_exporter.h/.cc` | WGS84 轨迹 GeoJSON 和轻量回放 JSON |
| `zeus-map simulate` | 批量坐标转换、参数解析、仿真执行和文件导出 |
| Go `POST .../simulate` | 请求预算校验、C++ 进程并发限制和产物内联 |
| Web `playback.ts` | 二分查找与线性插值，构建某时刻的车辆帧 |
| Web `MapCanvas.tsx` | MapLibre 轨迹线和车辆 circle layer |

依赖方向为：

```text
zeus_simulation_core
  ├── zeus_routing_core
  └── zeus_map_engine + GDAL（传递依赖）
```

## 3. 配置和输入

`SimulationConfig` 的 MVP 字段：

| 字段 | 默认值 | 语义 |
| --- | ---: | --- |
| `step_seconds` | 1 s | 固定 tick 步长 |
| `duration_seconds` | 900 s | 仿真时域 |
| `sample_interval_seconds` | 15 s | 周期采样目标间隔 |
| `jam_spacing_m` | 7 m | 单车占用的等效路段长度 |
| `min_speed_ratio` | 0.15 | 拥堵时自由流速度下限比例 |
| `exit_headway_ff_s` | 0 s | 自由流状态的路段最小驶出间隔，0 表示关闭 |
| `exit_headway_jam_s` | 0 s | 拥堵状态的路段最小驶出间隔，0 表示沿用自由流值 |
| `reroute_interval_seconds` | 0 s | 周期拥堵权重扫描间隔，0 表示关闭 |
| `reroute_cost_ratio` | 1.25 | 相对上次发布权重的显著变化倍率 |
| `deadlock_probe_ticks` | 300 | 连续无移动 tick 的死锁阈值 |

`VehicleDemand` 在进入内核前已转换到地图运行时坐标系，包含起点、终点、请求发车时间和四种路由算法之一。MVP 刻意没有随机行为，因此也不设置 seed；相同输入应逐采样完全一致。

## 4. 路线池

每条需求先通过 `RoutePlanner` 匹配并规划。缓存键由以下字段组成：

```text
origin.x/y IEEE-754 位模式
+ destination.x/y IEEE-754 位模式
+ algorithm
```

完全相同的 OD 和算法只规划一次，车辆仅保存 `route_id`。例如 100 辆同 OD 车辆的 `route_plans=1`，避免把仿真初始化退化为 N 次相同搜索。

规划失败只将对应车辆标记为 `unroutable`；只要至少一辆可规划，运行结果仍为 `ok=true`。全部不可规划时 CLI 返回退出码 3，并输出 `reason=unroutable`。

## 5. 路段容量与速度

有向边容量定义为：

```text
capacity[e] = max(1, floor(length_m[e] / jam_spacing_m) × lane_count[e])
```

同一个 tick 的移动阶段使用只读 `occupancy[]` 快照。为保证单车不因自身占用而减速，车辆看到的密度为同边其他车辆数：

```text
density_ratio = (occupancy[e] - 1) / capacity[e]
v_eff = free_speed[e] × clamp(1 - density_ratio, 0.15, 1.0)
```

入口和跨边准入要求：

```text
occupancy[next] + delta_in[next] < capacity[next]
```

不满足时车辆停在网络入口或当前边端点，本 tick 的剩余时间预算清零。`delta_in` 立即保留本 tick 的名额，按车辆 ID 的稳定顺序解决最后一个容量名额；`delta_out` 到 tick 末才释放，因而瓶颈不会在同一 tick 内被无限串行穿透。

可选出口放行门控按当前占用率在自由流与拥堵间隔之间线性插值：

```text
ratio = min(1, occupancy[e] / effective_capacity[e])
exit_headway[e] = ff + (jam - ff) × ratio
event_time - last_exit_time[e] >= exit_headway[e]
```

两个值均为 0 时关闭门控，保持原容量模型。`jam` 小于 `ff` 时内核按 `ff` 归一化。该门控限制整条有向边的汇总驶出率，不代表逐车道信号相位。

## 6. 两阶段确定性更新

每个 tick 分为以下阶段：

1. 按 ID 顺序尝试插入到期的等待车辆。
2. 将成功插入数量提交到 `occupancy`，形成移动阶段只读快照。
3. 按 ID 推进所有驾驶中车辆；跨边只写 `delta_in/delta_out`。
4. 在到达目标采样时刻后的第一个 tick 边界记录周期样本。
5. 一次性提交所有跨边迁移和离网释放。
6. 更新连续无移动计数并判断死锁。

插入车辆在本 tick 移动前已计入密度，因此同刻释放的一组车不会获得一个虚假的自由流 tick。未来仍有计划发车的等待车辆不会触发死锁计数。

## 7. tick 内事件推进

单车在一个 tick 内维护 `remaining` 时间预算并循环：

```text
distance_to_end = edge_end - offset
time_to_end = distance_to_end / v_eff

if time_to_end > remaining:
    offset += v_eff × remaining
    remaining = 0
else:
    到达边界并记录精确事件时刻
    若为末边：到达并离网
    若下一边有容量：route_index++，继续消耗 remaining
    否则：停在边界，remaining = 0
```

因此一个 tick 可以自然跨越多条短边。距离接近零时走专门的边界分支，避免除零或在零长区间死循环。

## 8. 与 RoutePlanner 的区间语义

仿真不假设路线总是从首边起点走到末边终点：

- 首边从 `RoutePath.start_offset_m` 起步。
- 末边只走到 `RoutePath.end_offset_m`。
- 单边路径走 `[start_offset_m, end_offset_m]`。
- 环线中同一 edge 可在不同 `route_index` 出现多次，推进依据索引而非 edge ID。

这保证道路匹配、路线长度和仿真行驶距离使用同一组语义。

## 9. 车辆、道路和路口控制

`SimulationEngine::run` 接收按仿真时间编排的 `SimulationControlEvent`。事件在第一个满足 `tick_time >= requested_time` 的 tick 边界生效，并同时记录请求时刻和实际生效时刻。同一时刻的事件保持输入顺序，因而重复运行结果确定。

| 对象 | 目标 ID | 命令 | tick 语义 |
| --- | --- | --- | --- |
| 车辆 | `vehicle_id` | `hold` / `release` | 暂停等待车辆的插入，或将行驶车辆停在当前位置并继续占用当前边；恢复后继续原路线 |
| 车辆 | `vehicle_id` | `speed_factor` | 将该车自由流速度乘以 0.05–3 的系数 |
| 道路 | 有向 `edge_id` | `close` / `open` | 阻止新车进入；已经在该边上的车辆可以继续驶离 |
| 道路 | 有向 `edge_id` | `speed_factor` | 将该边上所有车辆速度乘以 0.05–3 的系数 |
| 道路 | 有向 `edge_id` | `capacity_factor` | 将入口容量乘以 0.05–10；降低容量不强制驱逐已在边上的车辆 |
| 路口 | `node_id` | `close` / `open` | 阻止车辆从上游边穿越该节点，也阻止以该节点为首边起点的新车插入 |

封闭控制只改变准入，不会把在途车辆瞬移或删除。车辆暂停仍按采样周期输出静止位置。未来尚未执行的控制事件会抑制“连续无移动”死锁误报，否则在计划开放道路前可能过早结束。

内核通过只读 `RoutingOverlay` 同时表达 edge 可用性和动态代价。动态代价由道路控制与当前占用快照确定：

```text
congestion_speed_ratio = clamp(1 - max(0, occupancy - 1) / effective_capacity,
                               min_speed_ratio, 1)
routing_factor = max(1,
  (1 / edge_speed_factor)
  × (1 / min(1, edge_capacity_factor))
  × (1 / congestion_speed_ratio))
```

因子下限为 1，保证现有 A* 静态最大限速启发式仍可采纳；道路提速或扩容不会生成低于静态自由流的代价。封路/关闭路口立即屏蔽 edge，限速或降容事件立即重建目标 edge 权重。启用 `reroute_interval_seconds` 后，内核还会周期扫描全图占用率；只有权重相对上次发布值变化达到 `reroute_cost_ratio` 的 edge 才形成事件，避免每 tick 重规划。

每次事件只检查未来路线包含变化 edge 的等待或在途车辆。在途车辆以当前 edge + offset 作为精确起点，可以驶完已经进入但刚被封闭的当前边；终点固定为原路线的精确末 edge + offset，禁止重新吸附后悄悄改变目的地。动态代价规划只有在路线不同且预计时间更短时才切换；没有合法绕行或没有更优替代都记录为失败并保留原路线。每次尝试记录车辆、时刻、新旧 route ID 和结果，并汇总 `reroute_attempts/succeeded/failed`。

当前只从“现有路线包含的变化 edge”向外寻找更优路线；重新开放、提速、扩容或拥堵消散的替代道路不会主动把已经绕行的车辆吸回，避免全车扫描和路线振荡。

### 9.1 转向级信号相位

`JunctionSignalPlan` 绑定一个 `node_id`，包含周期偏移、统一的黄灯/全红清空时间和有序相位。每个 `SignalPhase` 定义绿灯秒数、一组 `(from_edge, to_edge)` 允许转向，以及 60–7200 veh/h 的转向饱和流率（默认 1800）。内核启动时验证：同一路口最多一套方案、入边终点和出边起点必须是方案节点、转向不能被地图规则禁止。

周期按以下顺序循环：

```text
phase 0 green → yellow → all red
phase 1 green → yellow → all red
...
cycle_position = (event_time + offset_seconds) mod cycle_seconds
```

车辆到达边界时，仅当当前绿灯相位显式包含其转向才可进入下一边。每项转向独立维护上次放行时刻，并满足：

```text
event_time - last_pass[from_edge,to_edge] >= 3600 / saturation_flow_vph
```

因此同一相位内的不同转向互不争用放行时钟；1800 veh/h 对应 2 秒最小车头时距。未配置方案的路口保持常绿；已配置方案但没有列入任何相位的转向保持红灯。当前中观模型没有“车辆已进入路口内部”的状态，因此黄灯与全红都停止新的边界转移，而不模拟黄灯抢行。信号等待不计为死锁；统计分别记录 `signal_red_wait_events`、`signal_saturation_wait_events` 和 `signal_movements_passed`，`signal_wait_events` 为两类等待之和。

该模型已经表达方向相位、清空时间和显式转向饱和流率，但尚未从转向车道数自动推导流率，也未自动生成冲突组、优先级或配时方案。

这是离线确定性场景控制：运行按钮提交完整控制时间线，C++ 一次计算完成。它不是运行中通过 HTTP 逐 tick 下发的实时遥控；后续异步常驻 Worker 可以复用同一事件模型实现暂停、单步和在线命令。

## 10. 采样和产物

每车记录：

- 实际插入时刻。
- 每次跨边的 tick 内精确时刻。
- 周期采样对应的 tick 边界。
- 到达时刻。

`last_sample_t` 防止同一事件时刻重复。周期不是 tick 的整数倍时，采样落在达到目标时刻后的第一个 tick 边界，例如 step=2 s、interval=3 s 时为 4、6、10……秒。

### 10.1 轨迹 GeoJSON

每辆有至少两个空间样本的车输出一个 WGS84 `LineString`，字段为：

- `VEHICLE_ID`
- `DEPART_S`
- `ARRIVE_S`
- `TRAVEL_S`
- `DISTANCE_M`

坐标转换对象只构造一次，所有采样点批量转换。

### 10.2 Playback JSON

```json
{
  "duration_s": 900,
  "step_s": 1,
  "sample_interval_s": 15,
  "reroute_interval_s": 30,
  "reroute_cost_ratio": 1.25,
  "controls": [
    {"requested_s": 30, "effective_s": 30, "scope": "vehicle", "target_id": 7, "action": "hold", "value": 1}
  ],
  "reroutes": [
    {"time_s": 45, "vehicle_id": 7, "old_route_id": 0, "new_route_id": 1, "success": true}
  ],
  "signal_plans": [
    {"node_id": 42, "offset_s": 0, "yellow_s": 3, "all_red_s": 1,
     "phases": [{"green_s": 30, "saturation_flow_vph": 1800,
                  "movements": [[12, 18], [13, 18]]}]}
  ],
  "vehicles": [
    {
      "id": 1,
      "depart_s": 0,
      "arrive_s": 812.4,
      "samples": [[0, 114.26, 30.47], [15, 114.261, 30.471]]
    }
  ]
}
```

未到达车辆省略 `arrive_s`。Web 使用二分查找定位相邻样本并线性插值；显示区间为 `depart_s <= t <= arrive_s`，未到达车辆显示到仿真时域末端。

## 11. CLI

单 OD 批量车辆：

```bash
./build/zeus-map simulate data/maps/MAP_ID/map.zmap \
  --lon 114.2661 --lat 30.4796 \
  --dest-lon 114.3163 --dest-lat 30.5278 \
  --count 100 --spread 600 \
  --algorithm biastar --duration 900 --step 1 --sample-interval 15 \
  --exit-headway-ff 0.5 --exit-headway-jam 1.5 \
  --reroute-interval 30 --reroute-cost-ratio 1.25 \
  --controls tests/fixtures/simulation_controls.csv \
  --signals tests/fixtures/simulation_signals.csv \
  --output /tmp/trajectories.geojson \
  --playback /tmp/playback.json
```

`--spread S` 在 `count > 1` 时将首车放在 0 秒、末车放在 S 秒并线性分布。也可用 `--od-file`，每行格式为：

```text
lon,lat,dest_lon,dest_lat,depart_s[,algorithm]
```

`--signals` 每行表示一个相位内的一项允许转向；同一节点和相位可以重复多行，时间参数必须一致：

```text
node_id,phase_index,green_s,yellow_s,all_red_s,offset_s,from_edge,to_edge,saturation_flow_vph
42,0,30,3,1,0,12,18,1800
42,0,30,3,1,0,13,18,1800
42,1,25,3,1,0,17,11,1200
```

以 `#` 开头的行忽略。所有 OD 坐标收集后通过同一个 GDAL 转换批量转入运行时 CRS。

控制文件每行格式为：

```text
time_s,vehicle|edge|junction,target_id,hold|release|close|open|speed_factor|capacity_factor[,value]
```

CLI 输出 `control_events`、`vehicle_controls`、`edge_controls` 和 `junction_controls`，用于校验实际应用数量。

## 12. Go 控制端点

```text
POST /api/maps/{id}/simulate
```

端点是当前 MVP 的同步低频控制接口，不承担逐 tick 状态传输。它启动一个受控 C++ CLI 进程，读取两个临时产物后在单次响应中返回统计、GeoJSON 和 playback。

请求限制：

- `count`：1–10,000。
- `durationSeconds`：60–28,800。
- `stepSeconds`：0.1–10。
- `spreadSeconds`：0–duration。
- `sampleIntervalSeconds >= max(stepSeconds, 1)`。
- `exitHeadwayFfSeconds`、`exitHeadwayJamSeconds`：0–60 s；拥堵值非零时不得小于自由流值。
- `rerouteIntervalSeconds`：0–3,600 s，非零时不得小于 tick；0 关闭周期扫描。
- `rerouteCostRatio`：省略时为 1.25，显式值范围 1.01–10。
- `count × duration / sampleInterval <= 400,000`。
- 算法仅允许 `dijkstra`、`astar`、`bidijkstra`、`biastar`。
- 展开道路多 edge 控制后，总目标事件不超过 10,000 条。
- 车辆、edge、node 目标必须落在当前需求和地图版本的有效 ID 范围内。

请求中的 `vehicleControls`、`roadControls` 和 `junctionControls` 由 Go 白名单校验后写入私有临时控制文件，不把任意用户文本拼接为 shell 命令。道路 UI 使用 `EDGE_IDS`，一次道路控制可展开到双向或多段有向边。

服务端 `simSlots` 默认容量为 2，限制同时加载完整 `.zmap` 的 C++ 进程数；等待期间客户端取消会返回 503。全部不可规划是合法计算结果，返回 HTTP 200 和 `ok:false`；无效请求返回 400；原生命令异常返回 422。

该 JSON 内联端点只用于 MVP 和万级以内受控请求。十万级实时仿真不能每 tick 经 HTTP 传完整车态，后续应使用 Worker 内存态 + 降采样 Protobuf/WebSocket 帧 + 分块对象存储回放。

## 13. Web 回放

右侧“路由 / 仿真”面板复用地图点击得到的 OD 和算法。用户可设置车辆数、发车分布、时长、采样间隔、自由流/拥堵出口间隔，以及拥堵扫描间隔和权重阈值，运行后看到：

- 到达率、原生计算耗时。
- 到达、在途、等待、不可规划、路线池命中和样本数。
- 总里程、平均旅行时间和死锁状态。
- 时间滑块、播放/暂停、1×/10×/30×。
- 车辆 ID 级暂停、恢复和速度控制。
- 当前选中道路的封闭、开放、速度和容量控制。
- 地图节点拾取后的路口封闭和开放控制。
- 按时间排序的控制列表、删除操作和实际应用数量。
- 动态重规划尝试、成功、失败总数，以及回放时刻前累计发生的重规划数。

轨迹使用半透明琥珀色 line layer，车辆使用 MapLibre canvas circle layer，每帧只对 GeoJSON source 调用 `setData`，不创建大量 DOM Marker。正常车辆为琥珀色，受速度系数限制的车辆为橙色，处于 `hold` 状态的车辆为红色。

## 14. 测试矩阵

| 用例 | 已验证结果 |
| --- | --- |
| 单车 100 m 道路，offset 10→90，20 m/s | 精确到达 4.0 s，距离 80.0 m |
| 3×50 m，50 m/s | 到达 3.0 s，边界样本 1.0/2.0 s |
| capacity=14 的 14 车同刻进入 | 速度触底 3 m/s，旅行约 26.7 s |
| 同边 15 车 | 第 15 车在 27 s 获准进入，全部到达 |
| 70 m→7 m(cap=1)→70 m 的 5 车 | 全部到达且产生明显回溢延误 |
| 同输入运行两次 | 到达、距离、样本逐元素一致 |
| duration=2 s | `arrived=0`、`driving_at_end=1` |
| 1 可达 + 1 跨分量 | `ok=true`、`arrived=1`、`unroutable=1` |
| 延迟发车 + 低死锁阈值 | 不误报死锁，按请求时刻插入 |
| step=2、sample=3 | 周期样本为 4/6/10 s |
| WGS84 导出 | GeoJSON 可由 GDAL 回读，字段和要素数正确 |
| Go API | 200/400/422 和信号量取消路径通过 |
| 车辆控制 | 0 s 暂停、3 s 恢复后准确延迟发车；速度系数改变到达时间 |
| 道路控制 | 下游边关闭至 15 s 后开放，车辆在边界排队并于 25 s 到达 |
| 路口控制 | 中间节点关闭至 12 s 后开放，车辆于 22 s 到达 |
| 控制确定性 | 请求/生效时刻和三类统计写入 playback，真实武汉图 CLI 应用 6/6 条事件 |
| 出口放行间隔 | 同刻到达边端的车辆按配置的最小 headway 依次离网 |
| 封路重规划 | 在途车辆保持当前 edge/offset，绕开未来封闭边；无合法绕行时保留原路线并记录失败 |
| 限速/降容重规划 | 动态代价上升后选择预计时间更短的绕行路线；无收益时保留原路线 |
| 周期拥堵重规划 | 11 车双分支瓶颈中，排队车辆绕开实时拥堵的短分支并全部到达 |

## 15. 武汉路网实测

在已发布的武汉地图 `map_71ad2c3c46bb59ae`（269,060 条有向边）上，以双向 A*、同 OD 100 车、0–600 s 线性发车、900 s 时域、1 s tick、15 s 采样运行：

| 指标 | 数值 |
| --- | ---: |
| 路线规划次数 | 1 |
| 到达 / 在途 / 不可规划 | 15 / 85 / 0 |
| tick | 900 |
| 样本 | 10,142 |
| 总行驶距离 | 614,877.576 m |
| 平均已到达旅行时间 | 798.777 s |
| 最大已到达旅行时间 | 811.085 s |
| 死锁 | 0 |
| C++ 内核计算时间 | 61.175 ms |
| 轨迹要素 | 100 |

这组数字是功能回归基线，不是十万车性能结论；耗时会随硬件、编译模式和 OD 改变。

### 15.1 Stateful SimulationSession

`SimulationEngine::run()` 现已接受可选的 `SimulationRunControl`，并在每个 tick 修改状态前进入控制点。`SimulationSession` 用一个专属工作线程承载原确定性内核，外部只在已提交的 tick 边界执行：

```text
reset
→ paused at tick 0 / state_version N
→ step(k)
→ paused at tick k / state_version N+k
→ observe / Agent decision
→ step / runToEnd
→ result / close
```

核心边界：

- 仿真热状态仍只由原 C++ 工作线程读写，不允许控制线程在 tick 中途修改车辆或路段数组。
- `step(k)` 阻塞到第 k 个新边界或提前终止；`runToEnd()` 可由另一线程请求在下一边界暂停。
- 每次 reset 使用比旧状态更大的 version base，每个已提交 tick 再加一，因此旧 Observation 不能在新一轮仿真中碰巧重新有效。
- close 在边界取消并 join 工作线程，保留带 `stats.cancelled=true` 的部分 `SimulationResult` 供审计。
- `barrier_wait_ms` 单独记录墙上等待，`compute_ms` 只保留初始化与真实仿真计算时间。

当前 Session 仍是进程内 C++ API，尚未暴露常驻 CLI/gRPC；Observation 暂时只有 tick、仿真时间、版本和生命周期状态，车辆/道路聚合及动态动作提交属于下一切片。

## 16. 已知简化

1. 已支持显式转向级信号相位、独立饱和流率和节点整体开/闭；尚未自动推导冲突区、从转向车道数计算流率或生成信号配时。
2. 路段内所有车辆在同 tick 使用同一密度快照；出口间隔是边级汇总门控，不做逐车道车头时距和排队位置细分。
3. 封闭、限速、降容和显著拥堵会触发受影响车辆重规划；替代道路恢复或代价下降目前不会主动扫描所有车辆并吸回原路。
4. 单 OD 在闭环或容量组合下仍可能死锁，由 300 tick 探测器提前结束并显式标记。
5. 当前控制是运行前提交的确定性时间线，Go API 同步运行并内联完整回放；不适合十万车交互式长时域任务或运行中人工接管。
6. Web 使用采样点间经纬度线性插值；低采样率下短弯道的视觉路径可能切角，但轨迹采样本身来自道路几何。

## 17. 后续优先级

1. 为已实现的 C++ `SimulationSession` 增加常驻 session-worker/gRPC 边界，并补齐 until-event、snapshot、restore 和车辆/道路聚合 Observation。
2. 将已定义的 Observation、Action、Tool 与 DecisionTrace Protobuf 接入 Session，并把 Go `DecisionCoordinator` 的 state version/TTL 校验连接到 C++ 当前状态。
3. 增加路线稳定性窗口、重规划冷却时间，并支持替代道路恢复后的受控全局收益扫描；规则策略作为 Navigation Agent 的确定性回归基线。
4. 仿真任务异步化，增加暂停、单步、取消和持久化 run。
5. Protobuf/WebSocket 降采样实时帧与分块回放文件。
6. 在转向级信号相位上增加冲突组、车道数驱动的流率推导、自动配时和更可靠的车道方向解析。
7. 1 万、5 万、10 万车辆的 Release 基准、内存剖析和分区并行，并增加事件聚合与 Agent 唤醒预算。
8. 多 OD/OD 矩阵、需求曲线和场景版本。

Agent 层设计与分阶段计划见 [geospatial-agent-environment.md](geospatial-agent-environment.md)。
