# Zeus 地图引擎设计与当前实现

> 状态：MVP 已实现  
> 最后更新：2026-08-28  
> 上层方案：[overall-architecture.md](overall-architecture.md)

## 1. 当前目标

地图引擎第一阶段建立以下闭环：

```text
道路 Shapefile / GeoJSON
→ 字段映射和坐标标准化
→ 几何清洗
→ 交叉切分与端点吸附
→ 有向道路拓扑
→ 拓扑验证
→ .zmap 运行时地图
→ 点到道路匹配
→ edge + offset_s 转世界坐标
```

当前实现是道路级导航地图 MVP，不是完整车道级高精地图。若源数据只有道路中心线，系统不会伪造真实车道连接关系。

## 2. 已实现能力

### 2.1 矢量道路导入

- 通过 GDAL/OGR 读取 LineString 和 MultiLineString。
- 支持单文件 `.geojson` / `.json` FeatureCollection。
- 检查 `.shp`、`.shx`、`.dbf`、`.prj` 文件是否完整。
- 缺少 `.cpg` 时生成编码检查警告。
- 读取 ID、单双向、限速、每方向车道数、道路等级、z-level、bridge 和 tunnel 字段。
- 使用 `key=value` 映射文件适配不同来源的属性字段。
- 删除连续重复点、非法坐标和零长度折线。
- 保留源 Feature ID，支持问题追溯。

### 2.2 坐标标准化

- 源数据为米制投影坐标时默认保留源 CRS。
- 源数据为地理坐标时，根据数据中心自动选择本地 UTM CRS。
- 可通过 `target_crs` 或 `--target-crs` 显式指定运行时 CRS。
- 运行时地图同时记录源 CRS 和运行时 CRS 的 WKT。
- CLI 查询支持运行时 XY，也支持 WGS84 经纬度输入。

### 2.3 拓扑构建

- 使用线段 R-tree 缩小交叉检测候选范围。
- 对相同 z-level 的真实几何交点切分道路。
- 相邻线段的公共形状点不会被重复处理。
- 不同 z-level 的二维相交不会建立连接。
- 按米制吸附容差合并相邻道路端点。
- 双向道路生成两条 DirectedEdge，单向道路只生成允许方向。
- 拓扑节点、导航边和折线几何分开存储。
- 使用稳定哈希 ID 和 32 位密集运行时索引。

### 2.4 拓扑验证

- 缺失运行时 CRS。
- 空节点集和空边集。
- 非法节点引用。
- 自环边。
- 非法边长度和限速。
- 非法几何范围。
- 几何端点和拓扑节点不一致。
- 同一道路的重复有向边。
- 孤立节点和悬挂端点。
- 没有入边或没有出边的节点。
- 弱连通分量数量和最大连通分量大小。

问题分为 `info`、`warning`、`error` 和 `fatal`。存在 fatal 时 CLI 不发布 `.zmap`。

### 2.5 运行时地图

`.zmap` v2 保存：

- 地图元数据和坐标系。
- 拓扑节点。
- 有向道路边。
- 共享折线点池。
- 源要素 ID 和基础道路属性。
- 每条有向边的可用车道数。
- `(from_edge,to_edge)` 禁止转换和转向惩罚。

加载器继续兼容 v1 地图，旧边按 1 车道且无转向转换读取。

加载后构建：

- CSR 风格出边数组。
- 边级包围盒 R-tree、每边对应的连续 segment 范围。
- edge-to-edge 转换哈希表。
- 只读 `MapRuntime`。

运行时不保留源矢量文件、OGRFeature 或 OGRGeometry 对象。

### 2.6 位置查询

外部点定位：

```text
XY 或 WGS84
→ 转换为运行时坐标
→ 以 max_distance 构造范围框，查询相交的边包围盒
→ 对每条候选边的全部 segment 做精确投影
→ 距离和航向评分
→ edge + offset_s + 置信度
```

内部仿真车辆直接保存：

```cpp
struct VehicleMapPosition {
    EdgeIndex edge;
    double offset_s;
    std::int16_t lane_index;
    float lateral_offset_m;
};
```

因此车辆每 Tick 的位置推进不执行全地图匹配。`MapRuntime::worldPose` 在渲染或输出时将逻辑位置转换为 XY 和航向。

### 2.7 Web 几何导出

`GeoJsonExporter` 将运行时米制坐标转换为 WGS84，并按逻辑道路片段导出 GeoJSON。双向道路共享一条显示几何，同时通过 `EDGE_IDS` 保留对应有向边索引，通过 `DIRECTION=both|forward|reverse` 保留通行方向。

拓扑节点可独立导出为点图层，包含稳定 `NODE_ID`、密集 `NODE_INDEX`、`IN_DEGREE` 和 `OUT_DEGREE`。道路、节点和质检事件三个产物共同用于 Web MVP；大地图后续改用矢量瓦片。

`saveReference` 提供一条与导航编译隔离的矢量转换管线。它读取第一个点或面图层，将任意有效源 CRS 转换为 WGS84，复制属性字段并输出 GeoJSON。该过程不构建 `MapData`，也不会生成节点、边或 R-tree。

## 3. 代码结构

```text
cpp/map-engine/
├── include/zeus/map/
│   ├── types.h
│   ├── shapefile_importer.h
│   ├── map_builder.h
│   ├── map_validator.h
│   ├── map_serializer.h
│   └── map_runtime.h
└── src/
    ├── types.cc
    ├── shapefile_importer.cc
    ├── map_builder.cc
    ├── map_validator.cc
    ├── map_serializer.cc
    ├── map_runtime.cc
    └── geojson_exporter.cc

tools/zeus-map/main.cc
tests/map_engine_tests.cc
testdata/maps/example.mapping
```

## 4. 构建

依赖：

- 支持 C++20 的编译器。
- CMake 3.24 或更高版本。
- GDAL/OGR。
- Boost.Geometry。

构建命令：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

## 5. CLI 使用

### 5.1 检查数据集

```bash
./build/zeus-map inspect roads.shp
```

GeoJSON 使用相同命令：

```bash
./build/zeus-map inspect roads.geojson
```

输出驱动、图层、要素数量、几何类型、CRS 和属性字段。当图层几何类型为 `Unknown (any)`（例如混合 LineString 与 MultiLineString 的 GeoJSON）时，额外输出逐要素几何类型统计行 `layer[i].geometry_counts`，控制服务和字段映射据此识别道路数据。

### 5.2 清洗 OSM 可行车路网

```bash
./build/zeus-map preprocess-osm raw-osm-roads.geojson \
  --output drivable-roads.geojson \
  --report cleaning-report.json \
  --profile car \
  --include-service false \
  --include-track false \
  --include-private false \
  --min-length 2
```

汽车画像默认保留高速、主干、次干、支路、居住区道路等机动车道路，排除步行、
骑行、施工、服务、林间土路和受限道路。`service`、`track` 与私有道路均可显式
放开，便于停车场、园区或乡村实验。

输出统一使用 `road_id / road_class / oneway / speed_kph / lanes / z_level / bridge /
tunnel` 等 Zeus 字段。处理过程会：

- 将 `oneway=-1` 规范为反向，识别显式单/双向，并为高速和环岛补充隐式单行。
- 解析普通 km/h 限速和 mph 限速；缺失或不可解析时按道路等级补充默认值。
- 将 OSM 总车道数保守换算为每方向车道数；缺失时高速、主干默认 2，其余默认 1。
- 由 `layer / bridge / tunnel` 生成标准高程层。
- 从 GeometryCollection 提取线状部分，过滤空几何、短边和完全重复的线几何。
- 生成 JSON 报告，记录各过滤原因、输出道路等级分布和属性修复次数。

### 5.3 导入并编译

```bash
./build/zeus-map import roads.shp \
  --mapping testdata/maps/example.mapping \
  --output city.zmap
```

GeoJSON 直接进入相同编译管线：

```bash
./build/zeus-map import roads.geojson \
  --mapping testdata/maps/example.mapping \
  --turn-restrictions turns.csv \
  --output city.zmap
```

`turns.csv` 为可读中间格式：`from_source_id,via_x,via_y,to_source_id,no|only|penalty[,penalty_s]`。编译器按 via 节点解析道路切分后的 incoming/outgoing edge；无法解析的规则产生 `TURN_RESTRICTION_UNRESOLVED` warning。

Web 地图编译台也允许道路数据包附带至多一个 `.csv`。Go 服务校验 sidecar 属于本次上传后，将其复制到地图版本目录并通过 `--turn-restrictions` 交给 C++；`record.json` 保存原始文件名，拓扑摘要展示实际生成的转换数量。

也可以通过命令行覆盖映射：

```bash
./build/zeus-map import roads.shp \
  --output city.zmap \
  --id-field LINK_ID \
  --oneway-field DIR \
  --speed-field SPEED \
  --class-field FCLASS \
  --level-field Z_LEVEL \
  --snap 0.5
```

### 5.4 验证运行时地图

```bash
./build/zeus-map validate city.zmap
```

### 5.5 导出 Web 路网

```bash
./build/zeus-map geojson city.zmap --output roads.geojson
```

导出质检事件点图层：

```bash
./build/zeus-map issues-geojson city.zmap --output issues.geojson
```

导出拓扑节点点图层：

```bash
./build/zeus-map nodes-geojson city.zmap --output nodes.geojson
```

将点面矢量数据转换为 Web 参考图层：

```bash
./build/zeus-map reference-geojson boundary.shp --output boundary.geojson
```

源数据必须包含有效 CRS；Shapefile 需要 `.prj`，标准 GeoJSON 默认使用 WGS84。空几何会被跳过，完全没有有效几何时命令失败。

导入时可通过 `--issues-output issues.geojson` 使用同一份完整质检报告直接生成问题图层，确保 `ISSUE_INDEX` 与 API 问题列表一致。带位置的问题会从运行时米制 CRS 转换到 WGS84；全局性问题不伪造坐标。

### 5.6 运行时 XY 匹配

```bash
./build/zeus-map query city.zmap \
  --x 500000 \
  --y 3450000 \
  --heading 90 \
  --max-distance 50 \
  --limit 5
```

### 5.7 经纬度匹配

```bash
./build/zeus-map query city.zmap \
  --lon 116.391 \
  --lat 39.907 \
  --heading 90
```

### 5.8 逻辑车辆位置转世界坐标

```bash
./build/zeus-map pose city.zmap \
  --edge 42 \
  --offset 125.5 \
  --lateral 0
```

## 6. 字段映射

映射文件采用简单 `key=value`，当前支持：

```text
id_field
oneway_field
speed_field
road_class_field
z_level_field
bridge_field
tunnel_field
target_crs
default_speed_kph
snap_tolerance_m
default_bidirectional
```

单双向字段识别以下常见值：

- 正向：`1`、`yes`、`true`、`forward`、`ft`、`f`。
- 反向：`-1`、`reverse`、`backward`、`tf`、`r`。
- 双向：`0`、`no`、`false`、`both`、`b`。

真实数据接入前必须通过 `inspect` 查看字段，并建立数据源专属映射文件。

## 7. 当前限制

1. `.zmap` v2 是 MVP 二进制格式，尚未实现跨字节序和内存映射；加载器兼容 v1。
2. 当前只处理第一个矢量图层。
3. 已有 edge-to-edge 转换和 OSM PBF via-node restriction 提取器，但尚未支持 via-way、conditional、完整车型和时间段例外。
4. 重叠共线道路只会被基础验证发现一部分，尚未完整处理。
5. 接近道路中部但没有真正相交的悬挂端点不会自动吸附到道路中部。
6. 没有 z-level、bridge、tunnel 信息时，二维相交默认视为同层连接；真实立交数据必须提供高程语义或人工修复。
7. 当前点匹配使用距离和航向，没有实现连续 GPS 的 HMM/Viterbi 匹配。
8. OSM 预处理已支持基础机动车访问权限过滤；编译地图仍没有车型 AccessMask、时间段规则和动态交通。
9. 当前车道数只用于容量，`lane_index` 仍是位置接口预留，没有车道连接拓扑。
10. R-tree 在加载时重建，后续应编译为可持久化 Packed R-tree。

## 8. 下一步

按优先级建议：

1. 为大规模道路和参考数据构建 MVT 分块导出与 LOD。
2. 增加近失配端点、重叠共线、疑似立交的专项检查。
3. 扩展 OSM restriction 到 via-way、conditional、车型 AccessMask 和时间段规则。
4. 将 `.zmap` 升级为扁平数组和 mmap 加载格式。
5. 增加地图版本 manifest、内容哈希和 JSON 质检报告。
6. 增加 OSM 清洗画像模板和按项目保存的参数预设。
7. 增加连续 GPS 轨迹匹配。
8. 对目标城市地图执行导入、查询和内存基准。

## 9. 验证状态

自动化端到端测试会动态创建 Shapefile，并将编译结果导出后重新作为 GeoJSON 输入，覆盖以下情况：

- 双向道路。
- 单向道路。
- 同层十字交叉。
- 小距离端点吸附。

测试覆盖 SHP/GeoJSON 导入、道路切分、方向边、验证、`.zmap` v2 保存加载、v1 真实武汉地图兼容读取、边级 R-tree 匹配和 `edge + offset_s` 世界坐标计算。长曲线含 33 个重叠 segment 的 Golden Map 会同时返回另一条邻近边，防止候选挤出。转向 sidecar、PBF relation 解析、车道数、转换序列化和吸附折叠后孤儿节点清理也有回归覆盖。

OSM 清洗测试覆盖道路等级筛选、访问权限、服务道路开关、短边和重复几何过滤、
GeometryCollection 线提取、mph 转换、道路等级默认限速、高速/环岛隐式单行和
反向单行标准化。

武汉真实数据从 68,559 条 OSM 线要素清洗为 41,687 条可行车道路；编译结果从
115,178 节点 / 269,060 有向边降到 68,191 节点 / 148,787 有向边，连通分量从
495 降到 277，无 error/fatal。PBF 市域范围提取出 105 条受支持的 via-node 关系，
其中在机动车清洗图上形成 62 个可执行 edge-to-edge 转换；其余缺失道路或拓扑不吻合项保留为
`TURN_RESTRICTION_UNRESOLVED` 警告。同一条汉口—武昌验证路线可由四种算法正常求解。
