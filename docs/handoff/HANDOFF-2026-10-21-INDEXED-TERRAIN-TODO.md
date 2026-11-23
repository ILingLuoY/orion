# 索引化地形（留待后续）— 2026-10-21

## 结论

「4 顶点/quad + 全局共享索引」的 native 侧实现**已完成并实测**，但因**着色器按非索引布局取顶点**导致画面崩坏，已用常量关闭：

```cpp
// cpp/VulkanCore.cpp
static constexpr bool SECTION_INDEXED_TERRAIN_ENABLED = false;
```

当前线上行为 = 改动前（每 quad 6 顶点三角形、非索引 BLAS）。

## 实测记录

| 项 | 数值 |
|---|---|
| 开启后症状 | 每 quad 的**第二个**三角形错位 ⇒ 彩色噪点、黑区、乱闪（用户截图确认） |
| 根因 | `rt_test.rchit` 等按 `primitiveIndex * 3u` 直取顶点（非索引假设） |
| 关闭态 `meshSec / meshMB` | 147 / 22.5MB（同局）；400 / 136.7MB（18:06 Broville） |
| 每 section 网格 | 0.153MB/section vs 0.342MB/section |
| 该局显存 | `vramUsedMB=7035.6 / budget=6943.8`（大头是贴图/帧资源，不是网格） |
| 预期收益 | Broville 满载约 **-45MB**（SOLID 部分 -33%） |

## native 侧（已完成，仅被常量关闭）

- `uploadChunk`：`indexedSolidQuads` 分支，SOLID 层（layerId 0）存 4 顶点/quad。
- `sectionQuadIndexAddress()`：全局共享索引缓冲，16384 quad / 196608 B，host-visible，
  图案 `0,1,2, 0,2,3`；所有段共用（索引相对各自 vertex buffer）。
- `createSectionBLAS` / `recordSectionBLASBuild`：`indexType=UINT16` + `indexData` +
  `maxVertex=顶点上界`，`triangleCount = quads*2`。
- `RTSectionAS::indexedGeometry`；refit（`ORION_AS_REFIT` / `run\as_refit.on`）与此路径兼容。
- 回退判据：quads > 16384 的段仍按 6 顶点/quad 存（两处判据严格互补）。
- OMM（cutout）段**不**走索引（非 opaque），micromap 逻辑未改动。

## 待做（要做就得一次做完）

1. **shader 改顶点取值**：`rt_test.rchit`、`rt_test_translucent.rchit`、`rt_test.rahit`
   （以及 deferred 变体）—— 从 `primitiveIndex*3 + k` 改为经索引缓冲取 3 个顶点。
2. **描述符**：新增 index buffer device address（或塞进现有 UBO push），并与
   `VulkanCore` 侧写入保持一致。
3. **打开开关**：`SECTION_INDEXED_TERRAIN_ENABLED = true`，重编 SPV + 重编 DLL。
4. **验收**：
   - 视觉：无噪点/黑区，草/玻璃/水/实体不受影响；
   - `[RT MEM]`：`meshMB` 降约 1/3（SOLID 部分），`asTrueMB` 同步降；
   - `[RT GEOMETRY] counters`：`asRefitHits` 仍正常。

## 代价

改动涉及 4~5 个 shader 变体 + 描述符布局，需要**一次 SPV 重编（冷编约 10 分钟）**；
收益上限约 45MB 显存。当前显存是被贴图/帧资源顶满的，故优先级不高。
