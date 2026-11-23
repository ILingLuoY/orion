# 地形 32B 顶点压缩 — 实现与交接（2026-10-21 落地，待实机验收）

> 档位：**生产档**（材质在命中着色器里求值；延迟档已作废，见 `deliverables/延迟材质-2026-10-20.md` §16）。
> 状态：**四批改动全部完成**，DLL 与 8 个 SPV 已同批部署到六处 / 四处；**尚未实机跑图验收**（见 §3）。
> 一句话：地形四个桶的 GPU RT 顶点从 48B 压到 32B，`asTrueMB` 预计 −127MB，CPU 侧与 Java 侧零改动。

## 0. 产物指纹（回退与复核的基准）

| 项 | 值 |
|---|---|
| DLL | `5774848` B，md5 `82CC4B03`，`2026-10-21 22:14:47`，六处一致（源改完后重编过一次，以此值为准） |
| 回退包 | 六处 DLL 与四处 SPV 都有 `.bak_pre_vertex32b_20261021` |
| 开关 | `SECTION_VERTEX_32B_ENABLED = true`（`VulkanCore.cpp:14161`） |

| SPV | 字节 | md5 前 8 | 与改动前 |
|---|---|---|---|
| `rt_test.rgen.spv` | 554696 | `0A7DB203` | −16B（生产档 rgen 不取顶点，32B 代码被 DCE，只剩解包器声明） |
| `rt_test_ser.rgen.spv` | 567388 | `4D9918F5` | 同上 |
| `rt_test_sharc_update.rgen.spv` | 570132 | `AC4C2020` | 同上 |
| `rt_test.rchit.spv` | 146656 | `3399723C` | +3.1KB（解包器） |
| `rt_test_translucent.rchit.spv` | 107056 | `D85BFE29` | +3.2KB |
| `rt_test.rahit.spv` | 41504 | `0A86AD15` | +3.2KB |
| `rt_test_translucent.rahit.spv` | 35648 | `34833C69` | +3.3KB |
| `rt_test.rmiss.spv` | 138876 | `05905FD6` | 未变 |

四处部署目录（已验证 md5 逐一相同）：`cpp/shaders`（基准）、`src/main/resources/shaders`（打包）、`build/resources/main/shaders`（dev 启动时解压源）、`run/shaders`（实际加载）。
编译口径：`-DRT_BUILD_PRODUCTION_MODE -DSECTION_VERTEX_32B=1`，`--target-env vulkan1.2`。

## 1. 改动清单（锚点）

### native（`cpp/VulkanCore.cpp`）

| 位置 | 内容 |
|---|---|
| `:14104-14161` | `struct SectionVertex`（32B，`static_assert` 钉死）、`SECTION_VERTEX_PAD1_BIAS`、`packSectionVertexColor()`、`makeSectionVertex()`、总开关 |
| `:5045` / `:20212` | `GPUSectionVertexAddress` 的第 4 个 lane 由 `motionFloats`（调试）改为 **`vertexStride`**；写入点写 `section.vertexStride`（32/48） |
| `:19066` | **新增** `sectionUsesPackedVertices(sectionKey, indexedGeometry, vertexCount)` —— 判据在函数内算，不依赖调用点传参 |
| `:19095` / `:19157-19164` | `createSectionBLAS` 删掉 `packSectionVertex` 形参，改用上面的 helper 定 `sectionVertexStride`，据此置 `packedSectionVertices` |
| `:64976` | refit 分支的 `newVertexBytes` 用同一判据算步长（否则 `vertexBytes` 永不相等 ⇒ refit 静默失效） |
| `:65452` | staging 逐顶点 `makeSectionVertex()` 打包（CPU 源仍是 48B `Vertex`） |

### shader

| 文件 | 内容 |
|---|---|
| `material_common.glsl:89-135` | `SectionVertex32` + `loadSectionVertex(addr, stride, index)`（未定义宏时回落 48B） |
| `rt_test.rchit:124`、`rt_test_translucent.rchit:125`、`rt_test.rahit:82`、`rt_test_translucent.rahit:80` | **各持一份同样的解包器**（这四个 TU 自带 `Vertex`/`Vertices`，且不 include `material_common.glsl`；内容必须与 `material_common.glsl` 逐字同步） |
| `material_resolve.glsl:113-115`、`material_resolve_glass.glsl:70-72` | `loadSectionVertex(rawAddr, sectionAddressEntry.w, firstVertex + N)` |
| `rt_test.rahit:374-376`、`rt_test_translucent.rahit:514-516` | 同上（这两处先把表项取成 `sectionAddressEntry`，原来只内联 `.xy`） |

### 工具

`_deploy_stale_spv.py` 与 `tools/audit_spv_freshness.py` 都新增 `BASE_FLAGS = ['-DRT_BUILD_PRODUCTION_MODE', '-DSECTION_VERTEX_32B=1']`。
**不加这两项，前者会在下次按 mtime 重编时把生产档静默覆盖成延迟档、后者会把全部产物误判成漂移。**

## 2. 32B 布局与语义（C++ 与 GLSL 必须逐字节一致）

```cpp
struct SectionVertex {          // 32B
    float x, y, z;              // 12
    float u, v;                 //  8
    uint32_t color;             //  4  RGBA8: r | g<<8 | b<<16 | a<<24
    float materialId;           //  4  语义不变
    float pad2;                 //  4  pad2' = pad2 + (pad1 > 0 ? 64 : 0)
};
```

解码：`pad1 = (pad2' >= 64) ? 0.25 : 0`，`pad2 = (pad2' >= 64) ? pad2' - 64 : pad2'`。

- 近场地形 `pad1` 只有 `0 / 0.25`（雨面候选，`:53602`），`pad2` 是玻璃连通掩码（`:53606`，0..31）⇒ 编码后 64..95，与 `-1` 不相撞；
- 颜色无损的前提是近场 RGBA 就是 raw 字节 `/255.0f`（`:53496` 起四个通道），`byte → f32 → byte` 恒等。

**哪些段会被打包**（`sectionUsesPackedVertices`）：
- 只有近场地形四桶：`SECTION_AS_CLASS_OPAQUE / OMM / ALPHA / WATER`，且非索引化、顶点数为 3 的倍数；
- **排除 LOD/DH**：`pad1` 是 lodMarker / `-(3+材质)`（折进 +64 会被解成"雨面"）、`pad2 = -1` 哨兵与折叠撞车（`-1+64=63 < 64`）、RGB 已乘 shade ⇒ RGBA8 有损；
- 不能只靠 class 位：`LOD_AS_CLASS_ALPHA == SECTION_AS_CLASS_OMM`、`LOD_AS_CLASS_WATER == SECTION_AS_CLASS_ALPHA`。唯一可靠判据是保留 Y 段标记 `VOXY_LOD_RESERVED_Y`；
- **排除实体/本地玩家/粒子**：Java 侧按 48B 写，语义不同。

## 3. 验收（已实机跑过一局：2026-10-21 22:48，结论＝生效）

### 3.0 首局实测（`run/rt_debug.log`，生产档）

无崩溃（无新 `hs_err`）、无 `Failed shader`、无验证层报错。启动行：
`[RTX] shaders: set=shaders target=...\run\shaders copied=4` —— 那 4 个是 `volumetric_clouds*.comp.spv` 的旧 compute（`extractBundledShaders` 会把 build 里的新版推过去），与 32B 无关。

该局世界没铺满（1170 → 675 段），绝对数不能直接和 1335 段的基线比，看**比率**：

| 时刻 | meshSec | meshMB（CPU 镜像） | asBlasMB | asTrueMB | ΣvertexBytes | Σvertex / meshMB |
|---|---|---|---|---|---|---|
| 改动前基线（48B） | 1335 | 383.2 | 194.1 | 574.1 | 380.0 | 0.99 |
| 22:48:19 | 1170 | 366.5 | 187.5 | 436.1 | 248.6 | **0.68** |
| 22:49:28 | 675 | 128.8 | 69.4 | 154.6 | 85.2 | **0.66** |

⇒ GPU 顶点字节 ≈ CPU 镜像的 2/3，**32B 已生效**。同规模（1335 段）下 `asTrueMB` 应落在 445~455；该局没铺满，所以没直接观测到 447 这个绝对值。

另：8 个 SPV 用当前源 + 规范口径重编后与部署逐一 `MATCH`（部署即最终代码）。

### 3.1 内存口径（`run/rt_debug.log` 的 `[RT MEM]`，每 600 帧一行）

改动前基线（21:49:32，生产档，1335 段）：`asTrueMB=574.1 asBlasMB=194.1 meshMB=383.2 asChunkMB=862.8 vramUsedMB=6507.3/6952.3`。

| 指标 | 预期 |
|---|---|
| `asTrueMB` | **574 → ≈447**（ΣvertexBytes 380 → 253） |
| `asBlasMB` | 194.1 不变 |
| `meshMB` | **不变** —— 它是 CPU 侧镜像（`:63321` `sizeof(Vertex)`），改 GPU 布局不动它，别误判成没生效 |
| `asChunkMB` / `asWasteMB` | 随 slab 2 的幂桶取整另行变化（可能再降） |
| `vramUsedMB` | 余量增加约 130MB 量级 |

### 3.2 画面（重点看这五处）

1. 近场地形颜色/材质与改动前一致（含染色玻璃的 tint、树叶背光）；
2. **雨面水洼**：`pad1` 折叠的唯一消费者，雨天地表应有反光斑块；
3. **玻璃连通掩码**：玻璃边框抑制（`pad2`）仍在，玻璃四周不应多出亮边/黑边；
4. 水面波面与反射、水下/贴水面视角；
5. **远场（LOD/DH）不应出现"雨面"高光**、不应出现块状色带。

### 3.3 反例对照

| 现象 | 说明 |
|---|---|
| 整片地形偏色 / UV 错位 / materialId 全错 | DLL 与 SPV 没配对（一侧开了 32B、一侧没开） |
| 远场 LOD 出现雨面高光或色带 | gate 没排 LOD（判据被改坏） |
| 玻璃四周一圈亮边 | `pad2` 折叠污染 |
| 地形整体发灰/数量级跳变 | `asTrueMB` 没降 ⇒ 打包没生效，先查 `packedSectionVertices` 与表项 `.w` |

### 3.4 档位与产物自检（任何时候都可做）

```
python tools/audit_spv_freshness.py
```
只看 `rt_test*.spv` 这 8 个：应当**既无"漂移"也无"未部署"**。
已知噪声（与本次改动无关，既有状态）：`volumetric_clouds*.comp.spv` 4 个漂移、`rt_test_pomdeferred.rchit.spv` 漂移，以及一批 compute 的"未部署"。

## 4. 回退

**方式 A（最快）**：把 `.bak_pre_vertex32b_20261021` 覆盖回去 —— 六处 DLL + 四处 SPV，一次到位。

**方式 B（改源）**：
1. `SECTION_VERTEX_32B_ENABLED = false`，重编 DLL，六处覆盖；
2. 8 个 SPV 去掉 `-DSECTION_VERTEX_32B=1`（保留 `-DRT_BUILD_PRODUCTION_MODE`）重编，四处同步；
3. 自检：`rt_test.rchit.spv` 应回到 143508B / `83063BAA69`（若掉到 9.6KB 说明连档位都回退错了）。

两者必须同时回退：只回退一侧 ⇒ 每个顶点按错 12B 的偏移读 ⇒ 颜色/UV/materialId 整体错位（不会崩）。

## 5. 已知噪声 / 未处理（都不是本次引入）

- `rt_test_sharc_update.rgen.spv` **不在** `build.gradle` 的 `rtxRuntimeShaderFiles` 清单里（grep `sharc` 无命中）。dev 走 `build/resources/main/shaders` 不受影响，但发行包会缺这个文件，SHARC update 路径启用时会抛 "Failed shader"。属既有缺口。
- refit（`ORION_AS_REFIT=1`）与 32B 兼容：步长判据已对齐，但该路默认关闭，本次未实测。
- 索引化地形（`SECTION_INDEXED_TERRAIN_ENABLED = false`）与 32B 互斥：`indexedGeometry` 段一律不打包；将来若同时开启，refit 分支里传的是 `false`，需要补等价判据（代码里已留注释）。

## 6. 历史教训（勿重复）

1. **索引化地形（2026-10-21）**：只改 native、忘了 shader 仍按 `primitiveIndex * 3` 取顶点 ⇒ 彩色噪点 + 黑区 + 乱闪。见 `HANDOFF-2026-10-21-INDEXED-TERRAIN-TODO.md`。
2. **"脚手架 ≠ 接线"**：32B 的 native 脚手架曾齐全且编译通过，但唯一调用点没传参数 ⇒ 打包分支从未执行。本次把判据下沉进 `createSectionBLAS`，从结构上消除"忘记接线"。
3. **档位判定不能看注释/文件名**：同一份 `rt_test.rchit` 生产档 143KB、延迟档 9.6KB，只能**重编比哈希**。旧脚本注释（"生产档必崩"）曾直接导致交接文档写错档位。

一般化判据：ABI / 档位改动要同时给出 ①代码锚点 ②可观测的运行时证据（计数器/日志）③现场口径的产物指纹（重编比哈希）。

## 8. 2026-10-21 追加修复：静止时玻璃上的彩色/白色噪点（与 32B 无关）

**现象**：静止时彩色玻璃上慢慢积累彩色噪点、无色玻璃上是白色噪点；相机一移动就干净。

**根因**：Any-Hit 用 `payload.albedo *= 透射色` 累积玻璃染色（`rt_test.rahit:716` / `rt_test_translucent.rahit:931`，注释 "T1/T9/T3 colored visibility through glass"），这个累乘的初值必须是 `vec3(1.0)`。
主射线 trace 前只播种了 `state`（`rt_test.rgen:18356`），于是起点是**上一条射线**留在 payload 里的 `albedo`（反射/AO/GI 的命中色），而每像素每帧的射线条数随 RR 采样变化 ⇒ 玻璃透射色逐帧不同 ⇒ 静止时被 DLSS-RR 当信号累积；噪点颜色被玻璃 tint 加权，所以"白色玻璃出白噪点、染色玻璃出彩噪点"。
玻璃**可见性**射线早在 `rt_test.rgen:6410` 就播了 `hit.albedo = vec3(1.0)`，主射线是漏掉的那一处。

**修复**：`rt_test.rgen` 主射线 trace 前加 `hit.albedo = vec3(1.0);`（生产档 `#define hit payload`）。

| 产物 | 新字节 / md5 |
|---|---|
| `rt_test.rgen.spv` | 554728 / `7A19A31D` |
| `rt_test_ser.rgen.spv` | 567420 / `AA52F1C9` |
| `rt_test_sharc_update.rgen.spv` | 570164 / `EC3548C1` |

四处同步；备份后缀 `.bak_pre_glassnoise_20261021`；DLL 不变（纯 shader 修复）。

**可见副作用**：玻璃透射色从"被上一条射线污染"变回正确值（可能略亮/更纯）——这是修正，不是新问题。

**未覆盖**：`PASSTHROUGH` / 玻璃内部等其它 trace 入口同样没有播种 `albedo`。若修完仍有残留噪点，下一步就是把这些入口补齐（它们只影响透射的二次段，主射线已覆盖画面主路径）。
