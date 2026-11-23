# HANDOFF 2026-10-24：半分辨率 GI（方案 C）

> 交接原因：本会话已用掉大半上下文，无法在**一次**内安全完成「5200 行 rgen 段定位 +
> 原生 700 行 patch + 冷编 15 分钟 + 部署校验」。按约定改开交接文档，让下一次会话
> 一次做完。文档里所有**已核实**的事实都带行号；**待核实**的单独标出。
>
> 关键约定：**整个评估阶段只允许一次冷编**（见 §7「一次冷编」），后续 A/B 靠运行时
> 切管线，不再重编 SPV。

---

## 1. 目标与非目标

**目标**：把漫反射 GI 段搬到 1/2 分辨率执行，主 raygen 全分辨率读取并做深度感知上采样。
预期内部 707×400 配置下 `rt` 段 −1.6ms、重场景 −2.6ms（见 §2 数据）。

**非目标**：
- 不动 glossy 反射（19%）、RTAO（6%）、体素 DDA（15%）。
- 不做时间抖动/重投影（那是方案 B；C 的卖点就是无历史依赖）。
- 不改 GI 的物理语义（弹射数、RR、NEE、SHARC 查询全部照旧，只是像素密度 ÷4）。

## 2. 立项依据（实测，均来自 `run/rt_debug.log`）

| 事实 | 数值 | 来源 |
|---|---|---|
| 内部 707×400，整帧 | 10.7ms | 近 300 帧 `[PERF L10 GPU]` 平均 |
| 其中 `rt` 段 | 7.2ms（67%） | 同上 |
| `noGi` 剥离 | rt −6.12ms（**50%**） | 707×400 剥离扫描（base 18.2ms 那一段） |
| `shadow1of2`（阴影采样 2→1） | rt 仅 −0.64ms（5%） | 同上 ⇒ **少发阴影射线没用** |
| `noGlossy` / `noVoxelDda` / `noRtao` | 19% / 15% / 6% | 同上 |
| SHARC 打开 | `sharcup` 单独 **5.35ms**，净亏 | 707×376 `sharcOn` 窗口 n=186 |

结论：**加缓存（SHARC/NRC）在本工程不成立**；只有"直接减少 GI 段的工作量"能拿到帧。
C 就是这条路里唯一无历史依赖的实现。

## 3. 设计（已定稿）

```
[P1] GI raygen（新变体，dispatch = 半分辨率）
     自跑主射线 + 材质求值 + GI 回路
     写 giHalfImage：rgb = 辐照度（解调后），a = 主命中视深度
[P2] 主 raygen（现有 rt_test.rgen.spv，唯一改动）
     不再跑 GI 回路（编译期去掉）
     litColor 之前从 giHalfImage 做 3×3 深度感知双边上采样 → indirectLight
     远场尾部（diffuseGILodWeight / farSlopeOcclusion）照旧作用在它上面
```

**为什么没有 composite pass**：上采样放进主 raygen 内联（9 次纹理读 / 像素，@707×400
约 0.05–0.1ms），省掉一个全屏 pass 和一张全分辨率图。SHARC 的教训（新增 pass 固定开销
不便宜）说明这一步很值。

**为什么 GI pass 自己重发主射线**（而不是读 G-buffer）：GI 回路需要 `matId` /
`brdfWeight` / POM / 玻璃分类等主命中局部状态，G-buffer 重建会把分类误差变成间接光
漏光。半分辨率重发主射线只多 0.25×主命中成本，换来与现状**逐语义等价**的求值。

**像素映射**：GI pass 的 invocation 覆盖四个象限之一：
`pixelCoord = ivec2(gl_LaunchIDEXT.xy) * 2 + phase`，`phase = (frameCount & 3)` 取
`(0,0)/(1,0)/(0,1)/(1,1)`（四帧轮换覆盖四个子像素）。这需要 2×2 的 jitter 与相位一致，
否则时间累积会退化成固定象限。

## 4. 已核实的代码锚点（改前先复核这些行号）

### `cpp/shaders/rt_test.rgen`（24,728 行）

| 位置 | 内容 |
|---|---|
| `18174-18196` | `main()` 入口、`size = imageSize(image)` 越界检查、`pixelCoord = ivec2(gl_LaunchIDEXT.xy)`（**唯一一处**像素坐标来源，加 ×2+phase 的守卫就放这里） |
| `19093` / `19099` | `vec3 hitPos` / `float matId`（主命中） |
| `19924` | `float roughness` |
| `20179` | `vec3 bounceDir` |
| `20220` | `vec3 brdfWeight` |
| `21846-21851` | glossy 段结束（GI 之前） |
| **`21852-23557`** | **GI 块**：`vec3 indirectLight = vec3(0.0)`（21855）→ while 循环 → `23557` 闭合 |
| `22170-22173` | GI 唯一的 `traceRayEXT`（SER 分支 / 普通分支） |
| `22184` / `23070` | SHARC `SharcUpdateMiss` / `SharcQuery` 挂点 |
| `23135` | 本地光 emitter NEE 累加 |
| `23561-23562` | `indirectLight *= diffuseGILodWeight` |
| `23564-23690` | 远场尾部（farSlopeOcclusion、环境底），**仍改 `indirectLight`** |
| `23972-24060` | guide 写入（`depthImage` / `rrDiffuseAlbedo` 等，用 `pixelCoord`） |
| `24921-24930` | 最终 `imageStore(image, pixelCoord, …)` |

> ⚠ **GI 变体必须在 23972 之前 return**，否则会与主变体同时写同一批 guide（数据竞争）。

### 原生 `cpp/VulkanCore.cpp`（70,061 行，2.6MB）

| 位置 | 用途 |
|---|---|
| `6103` | `g_rayTracingPipeline` |
| `25814` / `25842` / `25911` | `g_sharcUpdatePipeline` / `ensureSharcUpdatePipeline()` / `VkRayTracingShaderGroupCreateInfoKHR groups[5]` ← **新 pipeline 的模板** |
| `26063` 起 | `recordSharcUpdate(cmd,w,h)`：选点 + dispatch 的模板（`trace(...)` 在 26033 附近） |
| `50738-50845` | 主 pipeline 创建（`groups[5]`、`g_rayTracingPipeline = fresh`） |
| `53775` | SBT 构造区 |
| `61031-61044` | 主 trace 录制点：`recordSkyAtmosphere(cmd)` → `trace(cmd, &g_raygenRegion, …)` ← **GI pass 插在 sky 之后、主 trace 之前** |
| `70660` 附近 | 描述符写区（`terrainHeightTlasWrite` 等，新 binding 的 `VkWriteDescriptorSet` 加在这类地方） |

### 脚本

| 文件 | 说明 |
|---|---|
| `_deploy_stale_spv.py` | 编译 + 部署。`BASE_FLAGS = ['-DRT_BUILD_PRODUCTION_MODE', '-DSECTION_VERTEX_32B=1']`（25 行），SER 变体 101-106，SHARC 变体 108-115 ← **新变体照抄一段** |
| `tools/audit_spv_freshness.py` | 与 BASE_FLAGS 必须逐项一致 |

## 5. 改动清单（按顺序，每项都可独立编译）

### 5.1 shader（一次改完，只编一次）

1. **新 binding**：`layout(binding = 72, rgba16f) uniform image2D giHalfImage;`
   （rgen 已用到的最高是 71：`[0,1,2,6..19,22..32,38,39,42,46,47,48,54,58,61,62,69,70,71]`）
2. **`pixelCoord` 守卫**（18195）：
   ```glsl
   #if GI_HALFRES
       ivec2 pixelCoord = ivec2(gl_LaunchIDEXT.xy) * 2 + giHalfPhase();
   #else
       ivec2 pixelCoord = ivec2(gl_LaunchIDEXT.xy);
   #endif
   ```
   `giHalfPhase()` 从 UBO 取（宿主写 `frameCount & 3`），**不要**在 shader 里自己算帧号。
3. **GI 块守卫**（21852 前 / 23557 后）：
   ```glsl
   #if GI_HALFRES
       <原 21852-23557 原样>            // 不做任何改写
       imageStore(giHalfImage, ivec2(gl_LaunchIDEXT.xy),
                  vec4(indirectLight / max(brdfWeight, vec3(1e-4)), viewDepthMeters));
       return;                          // 必须在 guide 写之前
   #endif
   ```
   ⚠ 主变体（`GI_HALFRES` 未定义）**整段不编译** —— 这才是收益来源。
4. **主变体取 GI**（21908 之后、23561 之前，即远场尾部之前）：
   ```glsl
   #if !GI_HALFRES
       vec3 indirectLight = vec3(0.0);
       if (cam.giHalfEnabled.x != 0u) {          // 运行时 A/B 开关
           indirectLight = upsampleGiHalf(pixelCoord, viewDepthMeters);
       }
   #endif
   ```
   `upsampleGiHalf()`：3×3 taps、深度权重 `w = 1/(1e-3 + |z0 - z1|)`、材质一致时才用；
   写在新函数区（建议放 `rt_test.rgen` 的 7000 行附近工具函数区，或直接放 GI 块前）。
5. **不要动** `perfRayStatAdd(2u)` 的调用（GI 变体里它统计的是半分辨率射线，语义变了，
   在注释里写明"统计口径 = 半分辨率"）。

### 5.2 原生

1. 新图 `g_giHalfImage/View/Memory`：`w/2 × h/2`、`VK_FORMAT_R16G16B16A16_SFLOAT`、
   `STORAGE` + `SAMPLED`，随 render size 变化重建（照 `g_rrDiffuseAlbedoImage` 的模式，
   见 `6028` 一带）。
2. 描述符布局 + 写：binding 72（storage image，主变体侧是 sampled image —— **一个 binding
   两用要拆成两个**：72 = storage（GI 变体写），73 = combined image sampler（主变体读）。
   简化做法：单一 storage image + `imageLoad`，免 sampler；主变体的 3×3 手动 `imageLoad`）。
3. 新 pipeline `g_giHalfPipeline` + SBT raygen 记录：整段照抄 `ensureSharcUpdatePipeline()`
   （25842-26059），把 shader 名换成 `rt_test_gi_half.rgen.spv`。
4. `recordGiHalfPass(cmd, w, h)`：整段照抄 `recordSharcUpdate()`（26063 起），dispatch
   `(w+1)/2 × (h+1)/2`，插在 `61031` 的 `recordSkyAtmosphere(cmd)` **之后**、主 `trace(...)`
   之前。
5. UBO：新增 `giHalfEnabled`（0=旧路径 / 1=新路径）+ `giHalfPhase`。**两个变体共用同一
   UBO**，`CameraProperties` 尾部还有空位（`historyPadding0` 已被 giMax 占用，别抢）。
6. 环境开关 `ORION_GI_HALF`（默认 `0`），按 `docs/env-switches.md:93-98` 的约定写
   `static const` lambda + 补一行登记。

### 5.3 部署

`_deploy_stale_spv.py` 加一个 target：`('rt_test.rgen', 'rt_test_gi_half.rgen.spv', ['-DGI_HALFRES=1'])`，
并同步 `tools/audit_spv_freshness.py` 的 BASE_FLAGS 校验。变体 8 → 9 个：
`rt_test.rgen.spv` / `rt_test_ser.rgen.spv` / `rt_test_sharc_update.rgen.spv` /
**`rt_test_gi_half.rgen.spv`** / `rt_test_pomdeferred.rchit.spv` 等。

## 6. 已知坑（每条都已有证据，别踩）

1. **半分辨率的 RR 轮盘**：`rt_test.rgen:22115` 的 survival 下限 `0.20` 是按"每像素独立
   样本"调的（2026-10-09 为治"静止噪点"）。密度 ÷4 后每帧独立样本少 4 倍，这个下限要
   重扫；否则静止画面可能回到椒盐状。
2. **本地光 emitter scan / NEE 在 GI 回路内**（`23135`）：半分辨率会让地面火光斑点变粗。
   若观感不可接受，把这一段搬回主 raygen（代价 +8~10 行全分辨率工作）。
3. **SHARC 查询在回路内**（`23070`）：密度 ÷4 会改变缓存填充率；SHARC 默认关，但
   `ORION_SHARC=1` 的对照臂语义要同步更新文档。
4. **玻璃/水面**：回路内含玻璃穿透与水探针。上采样权重必须带材质一致性（否则 2×2 跨
   玻璃边缘会漏水）；宁可退化成最近邻也不要双线性漏光。
5. **远场尾部**（`23561-23690`）留在主变体 —— 它作用在 `indirectLight` 上，而主变体的
   `indirectLight` 已是从半图采样来的，因子照常生效，**不要**把这段也搬进 GI pass。
6. **guide 写竞争**：GI 变体必须在 `23972` 之前 return。
7. **`pixelCoord` 的越界检查**（`18177`）用的是 `imageSize(image)`（全分辨率），
   半分辨率 dispatch 天然通过；但 GI 变体写 `giHalfImage` 时要用 `gl_LaunchIDEXT.xy`，
   不要用 `pixelCoord`。
8. **`perfRayStatAdd` 口径**：统计脚本会失真，注释里标清楚。

## 7. 一次冷编（关键约束）

用户约束：每次改 SPV 冷编约 **15 分钟**。因此：

- **只编一个新变体**（`rt_test_gi_half.rgen.spv`），主变体**不改**（保留旧 SPV 作基线）。
  等等 —— 主变体必须去掉 GI 回路才有效益，所以主变体**也要重编一次**。这就是"一次冷编"
  的含义：**一次把主变体 + 新变体一起编完，之后不再重编**。
- A/B 靠运行时：`ORION_GI_HALF=0` 时主变体走"读半图"分支但宿主**不去 dispatch GI pass**
  → 需要半图里有旧内容的问题 ⇒ 所以 `ORION_GI_HALF=0` 时主变体必须回退到**自己的 GI 回路**。
  ⇒ 结论：主变体里 GI 回路**不能编译期删除**，要用运行时分支包住：
  ```glsl
  if (cam.giHalfEnabled.x == 0u) { <原 GI 块> }
  ```
  **代价**：主变体的寄存器占用/occupancy 仍按含 GI 回路算，收益会打折（需实测）。
  **如果实测打折严重**，第二次冷编再改成编译期删除（那时不再需要 A/B 基线，因为已有
  数值记录）。
- 建议：**第一次冷编就用运行时分支**，先拿到"总量能省多少"的上界；确认值得后再做
  编译期删除。

## 8. 必测判据（按顺序，任一条不过就停）

1. **空跑门禁**：GI pass 只写常量图（不追射线）时，新增耗时 **< 0.5ms**。
   （依据：SHARC update 4% 像素花了 5.35ms ⇒ 新 pass 的固定开销必须先量。）
2. **收益**：`rt` 段下降 ≥ 1.5ms（707×400），`orion.bench` 固定相机路径跑 3 次取 p50；
   任一次 `valid=false` 作废。
3. **画面**：树下 / 门框边 / 玻璃后 / 水面倒影 / 黄昏低角度五个定点，无块状漏光、
   无「远处变平」；`ORION_GI_HALF=0` 时必须**逐位**回到当前画面。
4. **静止噪点**：站着不动 10 秒录屏/截图，不得出现椒盐。

## 9. 回退

- 运行时：`ORION_GI_HALF=0`（默认）。
- 编译期：删掉 `_deploy_stale_spv.py` 的新 target 与 `GI_HALFRES` 分支，主变体回到
  `giHalfEnabled` 恒 0（等价旧行为）。
- 基线产物：`cpp/shaders/rt_test.rgen.spv` 与四处部署副本，改动前先备份为
  `*.spv.bak_pre_gihalf_20261024`。

## 10. 待核实（下一会话开工前先读这几处）

1. `18195` 之后 `pixelCoord` 到主射线方向的那段（`18400-18500`）—— 确认没有第二处
   `gl_LaunchIDEXT` 直读。
2. `23557-23570`：GI 块闭合与 `indirectLight *=` 之间的确切边界（决定守卫落点）。
3. `23960-24000`：guide 写的确切起点（决定 GI 变体 return 的位置）。
4. `src/client`：`CameraProperties` 尾部还有哪些空闲 lane（`historyPadding0` 已被 giMax 用，
   `perfAblationStage` 已用）—— 新 lane 别抢已占用的。
5. 描述符布局数组的确切构造点（预计在 `53775` 前后，`VkDescriptorSetLayoutBinding …[]`），
   确认 binding 72/73 未被别处占用（rgen 未用，但宿主可能预留）。

---

## 11. 【已完成】实现落地记录（2026-10-24 同一日，第二会话）

§1~§10 的代码改动**已全部写完并编译通过**，剩下的是实机判据（§8）与 `run/` 的
DLL 覆盖（当时游戏占用文件）。

### 11.1 §10 待核实项的实际答案（都是用户原文猜错/需修正的地方）

| 项 | 实际 |
|---|---|
| 1 | **这条猜错了，而且是最致命的**：`gl_LaunchIDEXT` 在 rgen 里共 31 处直读，其中 `18455` 的 `pixelCenter`（主射线方向）和 `18444` 的 `seed` 必须在变体里走 pixelCoord。已改（详见 §11.2 第 4 条）。`19066` 的 `skyPixelCenter`（天空像素的 MV 方向）走的也是 launchID —— 但那段已被 §11.2 第 5 条的守卫一并排除在 GI 变体之外。 |
| 2 | GI 块真正的收尾是 `23671-23673`（while 的 `}` + 我加的一层 `}`），`indirectLight *= diffuseGILodWeight` 在 `23714`。 |
| 3 | guide 写起点是 `24156` 的 `if(!perfL10DisableG2bLocalGuide())`（不是 23972，23972 是透明接收面的 depthImage 写）。 |
| 4 | `CameraUBO` 尾部**只剩 rd 1584 之前没有空位**：最后成员是 `ueCloudB@1552`，我追加 `giHalfParams@1568`，`sizeof` 1568→1584。`historyPadding0` 早已被 POM 深度占用。 |
| 5 | 数组是 `VkDescriptorSetLayoutBinding b[72]{}`（`21844`，**不是 53775**），binding 72 空着，已扩到 `b[73]` + static_assert。 |

### 11.2 与 §5 原方案的三处偏差（都是读代码后必须改的）

1. **守卫不能从 21852 开始**。`indirectLight`(21855)、`dbgLocalBounceAccum`(21858)、
   `diffuseGILodWeight`(22013) 三个 depth-0 声明在块外仍被引用（块内共 17 个 depth-0
   声明，只有这 3 个漏出去）⇒ 花括号下移到 `diffuseGILodWeight` 之后。
   扫描脚本：`.tmp` 里的正则 + 引用计数法，结论见 `rt_test.rgen:22119-22126` 的注释。
2. **miss 分支必须单独早退**。GI 变体原先只有"GI 块末尾 return"这一个出口，而 sky
   像素根本不进 GI 块 ⇒ 会一路走到 guide 写（§6.6 的数据竞争在**天空像素**上发生）。
   现在在 `if (primaryHitDistance < 0.0)` 里写 `vec4(0,0,0,60000)` 后立刻 return。
3. **半图必须显式清零**。`imageLoad` 读未定义内容是 UB，且这张图的 alpha 就是深度
   权重的一半。`clearGiHalfImageIfNeeded()` 在**首次使用那一帧的命令里**做
   UNDEFINED→TRANSFER_DST→clear→GENERAL，不额外等设备空转。

4. **主射线方向根本不看 `pixelCoord`**（§10 第 1 条的答案是"有第二处，而且是最要命
   的那处"）：`18455` 的 `vec2 pixelCenter = vec2(gl_LaunchIDEXT.xy) + 0.5 +
   frameJitter`，紧跟着 `inUV = pixelCenter / vec2(gl_LaunchSizeEXT.xy)`。
   只改 `pixelCoord` 的后果是**把屏幕左上角四分之一的几何写进半图**（射线按 launchID
   走、写回按 launchID*2+phase 走）。已改成 `vec2(pixelCoord) / vec2(size)`：
   主变体里 `pixelCoord == launchID`、`size == LaunchSize`，所以是逐位等价的重写；
   GI 变体里才真正有意义。`seed`（`18444`）同样换成 `pixelCoord`（主变体数值不变）。
   ⚠ `gl_LaunchSizeEXT` 在 GI 变体里是**半分辨率**，任何用它的归一化都会把视锥
   缩小一半 —— 全文件剩下的 `gl_LaunchSizeEXT` 只出现在 `projScale`（= 高度，用于
   投影缩放）与 `21474` 的左右半屏判据，两者都只关心比例，可以不动。

5. **GI 变体不能写 G-buffer**。`if (s == 0)` 那一段（`18916`，`primaryDepth` +
   `dlssMotionImage`）整段加了 `#if !GI_HALFRES`：它和主变体写同一批像素，而两条
   dispatch 之间**没有屏障** ⇒ 同址两写是未定义顺序，DLSS 可能吃到半变体的 MV
   （天空分支那份还是按 launchID 算的）。主变体本来就对每个像素无条件写一遍。

### 11.3 实际落点（行号会随后续编辑漂移，定位请 grep 标识符）

| 文件 | 位置 | 内容 |
|---|---|---|
| `rt_test.rgen` | `261` | `binding 72` giHalfImage（rgba16f） |
| | `18189` | `#ifndef GI_HALFRES` 默认 0 |
| | `18207` | `upsampleGiHalf()`（仅主变体编入） |
| | `18266` | pixelCoord = launchID*2 + (fc&1, fc>>1&1) |
| | `19171` | miss 分支哨兵 + return |
| | `22128` | `if (cam.giHalfParams.x == 0.0f) {` 运行时守卫 |
| | `23673` | 守卫收尾 + GI 变体 `imageStore` + return / 主变体上采样 |
| `camera_ubo.glsl` | 尾 | `vec4 giHalfParams`（偏移 1568） |
| `VulkanCore.cpp` | `6662` / `23332` / `23387` / `26431` / `26641` | `giHalfEnabled()` / `createGiHalfImage` / `clearGiHalfImageIfNeeded` / `ensureGiHalfPipeline` / `recordGiHalfPass` |
| | `61095` / `61692` | 建图调用 / 录制调用（在 `recordSkyAtmosphere` 之后、主 `trace` 之前，随后重绑主管线） |
| `_deploy_stale_spv.py` / `tools/audit_spv_freshness.py` | — | 新增 `-DGI_HALFRES=1` 目标 |
| `docs/env-switches.md` | D 段 | `ORION_GI_HALF` 一行 |

### 11.4 产物（四处同哈希：`cpp/shaders`、`run/shaders`、`src/main/resources/shaders`、`build/resources/main/shaders`）

```
rt_test.rgen.spv              569412  022723C5   ← 主变体（GI 回路仍在，运行时跳过）
rt_test_ser.rgen.spv          581636  9BD79081
rt_test_sharc_update.rgen.spv 584848  88F293E5
rt_test_gi_half.rgen.spv      557904  5CFE8BD4   ← 新变体
rt_test.rchit.spv             147740  78C21707   ← 以下四张因 camera_ubo.glsl 尾部
rt_test_translucent.rchit.spv 108156  487A5621      多了一个未读成员而重编（块结构体
rt_test.rahit.spv              43448  A7FD5C0B      含全部成员 ⇒ SPIR-V 必然变）
rt_test_translucent.rahit.spv  37532  F76E3955
rt_test.rmiss.spv             139700  BED0D43D
```


### 11.5 还没做的（下一会话从这里开始）

1. **`run/rtx_backend.dll` 仍是旧 DLL**（MD5 `16DF96FB`；新的是 `E08F5A50`）。
   当时游戏进程占着文件 ⇒ 退出游戏后：
   `Copy-Item cpp/build/Release/rtx_backend.dll run\ -Force`
   （`src/main/resources/natives/windows/` 与 `build/resources/main/` 已更新）
2. **§8 的五条判据一条都没跑**（空跑门禁 / 收益 / 画面 / 静止噪点）。
3. 第一次带 `ORION_GI_HALF=1` 启动会多一次驱动编译（`[GI HALF] 开始创建…管线` 到
   `管线已创建` 之间的日志，预期数秒~十几秒）。这一次之后才有 A/B 数值。
4. 若收益达标再考虑第二次冷编改成编译期删除（`#if GI_HALFRES` 取代
   `if (cam.giHalfParams.x == 0.0f)`），那时 §7 的"折扣"就消失了。
5. 已知未处理项（都不影响正确性，只影响观感/统计）：
   · `perfRayStatAdd` 口径在半变体里变成 1/4（统计脚本会失真）；
   · `ORION_SHARC=1` 时两个 pass 会各写一遍缓存（§6.3，默认关所以没验）；
   · 四处 `projScale = float(gl_LaunchSizeEXT.y) / …`（`5160/13457/13779/17806`）在
     半变体里拿到的是半分辨率高度 ⇒ 像素足迹算小一半，水面反射焦散的 mip 会偏一档。
     修法要么给这些 helper 传高度，要么在 GI 变体里把 `gl_LaunchSizeEXT.y * 2` 顶上；
     没做是因为它只影响焦散清晰度，先看主收益。
   · `rtSampleParams.x == 0.5`（棋盘交错）与 `ORION_GI_HALF` 同开时，半变体的棋盘
     判据用的是 launchID 奇偶、覆盖的像素集合与主变体不同 ⇒ 有 1/8 屏幕的 GI 会
     隔帧不刷新。两个旋钮都不是默认档，先不处理。

