# Zeus 中观交通仿真内核设计

> 状态：MVP 已实现，含车辆/道路/路口预编排控制  
> 最后更新：2026-08-28  
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
  "controls": [
    {"requested_s": 30, "effective_s": 30, "scope": "vehicle", "target_id": 7, "action": "hold", "value": 1}
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
  --controls tests/fixtures/simulation_controls.csv \
  --output /tmp/trajectories.geojson \
  --playback /tmp/playback.json
```

`--spread S` 在 `count > 1` 时将首车放在 0 秒、末车放在 S 秒并线性分布。也可用 `--od-file`，每行格式为：

```text
lon,lat,dest_lon,dest_lat,depart_s[,algorithm]
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
- `count × duration / sampleInterval <= 400,000`。
- 算法仅允许 `dijkstra`、`astar`、`bidijkstra`、`biastar`。
- 展开道路多 edge 控制后，总目标事件不超过 10,000 条。
- 车辆、edge、node 目标必须落在当前需求和地图版本的有效 ID 范围内。

请求中的 `vehicleControls`、`roadControls` 和 `junctionControls` 由 Go 白名单校验后写入私有临时控制文件，不把任意用户文本拼接为 shell 命令。道路 UI 使用 `EDGE_IDS`，一次道路控制可展开到双向或多段有向边。

服务端 `simSlots` 默认容量为 2，限制同时加载完整 `.zmap` 的 C++ 进程数；等待期间客户端取消会返回 503。全部不可规划是合法计算结果，返回 HTTP 200 和 `ok:false`；无效请求返回 400；原生命令异常返回 422。

该 JSON 内联端点只用于 MVP 和万级以内受控请求。十万级实时仿真不能每 tick 经 HTTP 传完整车态，后续应使用 Worker 内存态 + 降采样 Protobuf/WebSocket 帧 + 分块对象存储回放。

## 13. Web 回放

右侧“路由 / 仿真”面板复用地图点击得到的 OD 和算法。用户可设置车辆数、发车分布、时长和采样间隔，运行后看到：

- 到达率、原生计算耗时。
- 到达、在途、等待、不可规划、路线池命中和样本数。
- 总里程、平均旅行时间和死锁状态。
- 时间滑块、播放/暂停、1×/10×/30×。
- 车辆 ID 级暂停、恢复和速度控制。
- 当前选中道路的封闭、开放、速度和容量控制。
- 地图节点拾取后的路口封闭和开放控制。
- 按时间排序的控制列表、删除操作和实际应用数量。

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

## 16. 已知简化

1. 容量已乘以每方向车道数，但仍只有下游入口容量；路口控制目前是整个节点开/闭，没有转向级相位、冲突区、饱和流率和自动信号配时。
2. 路段内所有车辆在同 tick 使用同一密度快照，不做车头时距和排队位置细分。
3. 路线在仿真开始前固定，不根据拥堵动态重规划。
4. 单 OD 在闭环或容量组合下仍可能死锁，由 300 tick 探测器提前结束并显式标记。
5. 当前控制是运行前提交的确定性时间线，Go API 同步运行并内联完整回放；不适合十万车交互式长时域任务或运行中人工接管。
6. Web 使用采样点间经纬度线性插值；低采样率下短弯道的视觉路径可能切角，但轨迹采样本身来自道路几何。

## 17. 后续优先级

1. 转向级信号相位、路口出口流率、冲突组和更可靠的车道方向解析。
2. 将动态路段旅行时间反馈给 RoutePlanner，并按事件触发批量重规划。
3. 仿真任务异步化，增加暂停、单步、取消和持久化 run。
4. Protobuf/WebSocket 降采样实时帧与分块回放文件。
5. 1 万、5 万、10 万车辆的 Release 基准、内存剖析和分区并行。
6. 多 OD/OD 矩阵、需求曲线和场景版本。
