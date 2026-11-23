# HANDOFF 2026-10-26：RTXDI 式直接光时域重用 + 稀疏渲染

> 状态：**代码已全部落盘、四个 rgen 变体冷编通过、尚未实测**。
> 本文档是给下一次会话（或用户自测）的完整上下文：设计、搬运对照、已核实锚点、
> 已证伪项、验收判据、编译/部署清单。
>
> 关键约定：本工程**不删除被取代的实现**（用 `#if 0` / 宏分支保留），默认档必须
> 逐位等于改动前。SHaRC 由另一个智能体负责 —— 本次**没有**改任何 SHaRC 查询/更新
> 代码、`recordSharcZeroFillIfNeeded`、`sharcMode()` 的门控。

---

## 1. 目标与非目标

**目标（两项运行时开关，下一帧生效，不重建资源）**

1. **直接光采样模式** `CameraUBO.sparseParams.x`：
   - `0` = 传统单帧 RIS（每像素 16 提案 + 1 根可见性射线）—— **出厂，旧画面**；
   - `1` = RTXDI 式时域 reservoir：重投影上一帧 reservoir → 灯身份/修订号验证 →
     WRS 合并 → **只对最终选中项打一根可见性射线**。
2. **稀疏渲染采样率** `sparseParams.y`（%）：白噪声逐像素抽签，非活跃像素跳过
   RTAO / 太阳软阴影 / 局部光 RIS / 局部光二级 DDA / 光泽反射 / diffuse GI；
   **深度、运动矢量、各类引导图始终密写**。
3. **稀疏 × 时域重用联动** `sparseParams.z`：非活跃像素是否仍合并/老化 reservoir
   （`1` = 是，只跳过着色射线，对齐 Remix 的
   `rtx.sparseRendering.enableRtxdiReuseForInactivePixels`）。

**非目标**

- 不改 SHaRC（查询、更新、`sharcMode()`、零填充）。
- 不动 NRD/RR/Streamline 的接口；不改 DLSS 的 MV 语义。
- 不做逐灯 light hash（本工程的 block emitter 没有稳定 light index，见 §3.4）。
- 不试图修「半透明静止噪点」（那是 `translucent-static-noise-probe` 的独立问题；
  本改动与它无因果关系，别把两者混在一起验收）。

---

## 2. 立项依据与源码来源

- 用户批准的方案：运行时二选一 + 稀疏抽签；SHaRC 部分剔除（另一智能体负责）。
- 用户指令：**「RTX REMIX 的源码在仓库里，能有的代码直接搬，没有的才自造」**。
  源码在 `_ref/dxvk-remix/`（只读参考，不在构建路径里）。
- ⚠ 该仓库的 `submodules/rtxdi|sharc|rtxcr|nrc` 是**空目录**（未初始化），所以
  RTXDI SDK 的内部实现拿不到；本工程按 Remix **调用点**的契约复刻（见 §3.3）。

### 搬运对照表

| Remix 源 | 搬了什么 | 本工程落点 |
|---|---|---|
| `src/dxvk/shaders/rtx/utility/noise.slangh` | `uintHash` / `whiteNoise`（hash-prospector 双乘子） | `rt_test.rgen` 的 `uintHash` / `vptWhiteNoise`（`asfloat`→`uintBitsToFloat`） |
| `src/dxvk/shaders/rtx/utility/packing.slangh` | `unorm23ToFloat` | 同上 |
| `rtx/pass/sparse_rendering/per_pixel_rate.slangh` | 抽签判据 `noise <= samplingRate`、默认 WhiteNoise 源 | `vptSparsePixelIsActive` |
| `rtx/pass/rtxdi/rtxdi_temporal_reuse.comp.slang` | 读历史 reservoir → temporal resampling（position/normal/revision 阈值）→ 存回 | `evaluateVPTDirectLightReservoirTail`（重投影 + 身份判据 + 合并） |
| `rtx/pass/rtxdi/rtxdi_initial_sampling.comp.slang` | 空 reservoir → 采样 → `RTXDI_CombineReservoirs` → 延迟可见性 | 现有 RIS 提案循环 + tail 合并 + **单根**可见性射线 |
| `rtx/pass/sparse_rendering/sparse_rendering.h` | `enableRtxdiReuseForInactivePixels` 的语义 | `sparseParams.z` + `runPrimaryDirectLight` 的门控 |

**没有从 Remix 搬的**：SDK 的 `RTXDI_CombineReservoirs` 本体（空子模块）。
合并数学按 ReSTIR/WRS 的定义与调用点契约重写（§3.3），并刻意使用与本工程
RIS 尾部**同一份**贡献/衰减/玻璃染色函数，避免两套能量口径。

---

## 3. 设计

### 3.1 UBO 通道

`CameraUBO` 尾部新追加 `vec4 sparseParams`（**offset 1712，sizeof 1728**；
C++ `static_assert` 在 `VulkanCore.cpp:4631`）。四格语义：

| lane | 名称 | 语义 |
|---|---|---|
| `.x` | direct light mode | `0` 传统 RIS / `1` 时域 reservoir |
| `.y` | sparse rate % | `0` 关 / `N∈[0,100]` 活跃像素比例 |
| `.z` | rtxdi reuse inactive | `1` = 非活跃像素仍老化 reservoir（默认） |
| `.w` | 保留 | `0` |

宿主每帧写入：`VulkanCore.cpp:55287-55293`；`updateCamera` 之外的默认值
`{0,0,1,0}` 在 `18595-18600`。历史缓冲（`run/shaders` 侧）无需改动：
`binding 26/27` 是 ping-pong，每像素结构体从 3×vec4 扩到 4×vec4
（`VPTDirectHistoryGPU`，`VulkanCore.cpp:17610-17635`，64B，`sizeof` 驱动分配）。

### 3.2 历史第 4 lane（关键约束）

```
v0 = (selectedSample.xyz, emission*8 + typeCode)   // 复用于 EMA 的 radiance 通道
v1 = (W, M, lightRevision, tag)                    // tag 2.0 = WRS；EMA 写 1.0
v2 = (receiverPosition.xyz, valid)                 // 与 EMA 同义
v3 = (receiverNormal.xyz, 0)                       // 仅 WRS 使用
```

- **只能追加第 4 lane**：v0/v1/v2 的 EMA 语义保持逐位不变（透明接收面的
  `evaluateVPTCandidateDirectTemporal` 依赖它们）。曾试过整块复用 → 透明路径全花。
- 两类条目靠 `v1.w` 区分：WRS 判据要求 `abs(v1.w-2.0)<0.5`；EMA 的
  `vptPrimaryHistoryMatches` / `evaluateVPTCandidateDirectTemporal` 增加
  `v1.w < 1.5` 的守卫，互不串味。
- **位置键存的是 `hitPos`（不含 POM 位移）**，与 WRS 自己的判据同源。注意与
  透明路径的历史键问题（`rt_test.rgen` 5073 附近的长注释）不是同一件事：
  那条路径存 `hitPosition`（含 POM），已被实测证明**不能**改成 `matchPosition`。

### 3.3 合并数学（与 Remix 调用点等价）

```glsl
previousWeight = W_prev * M_prev * target_prev(current receiver)   // 把 W 换算回 weightSum
totalWeight    = weightSum_current + previousWeight
M_merged       = min(M_current + M_prev, 24.0)                     // RTXDI maxHistoryLength 语义
select previous with prob previousWeight / totalWeight
W_out          = totalWeight / (M_merged * target_selected)
```

- `target_prev` 由 `vptLocalEmitterContributionAt()` 按**当前接收面**重算，
  与提案循环的 scale/余弦/几何面上界逐项一致；距离闸与提案循环同口径（量
  发射体中心，不量采样点）。
- 合并后**先写历史、后打射线**（Remix temporal reuse 也是先存 reservoir，
  可见性只影响着色），被遮挡的样本下一帧还能复用。
- 稀疏关闭时的旧行为：非活跃像素直接返回 0、不写历史（`tail` 内的守卫）。

### 3.4 失效与身份

- **revision**：`localLightBuffer.header.y` = 宿主 `g_localLightRevision`，
  只在灯内容重建时递增（内容不变不涨）。历史 revision 不匹配 ⇒ 丢弃。
  误差上限 1 帧（开关灯）。
- **接收面**：位置容差 `vptJitterCompensatedPositionEpsilon(hitPos)`，
  法线余弦 ≥ `0.92`（`VPT_DIRECT_WRS_NORMAL_COSINE`），身份法线用几何面法线
  （POM 浮雕不参与身份判据）。
- **为什么不做逐灯 hash**：block emitter 每次重建后索引会变；整表 revision +
  世界坐标/类型/发射强度重建是本工程可用的最强身份。若将来灯表稳定化，
  可替换为逐灯 hash，判据结构不用动。

### 3.5 稀疏抽签

```glsl
samplingRate = clamp(cam.sparseParams.y, 0, 100) * 0.01;
rate >= 1 或 <= 0  ⇒ 全活跃（旧行为）
active = vptWhiteNoise(pixel, cam.frameCount, 0u) <= samplingRate
```

- 白噪声按 Remix 默认（`PerPixelRateNoiseSource::WhiteNoise`），不用蓝噪声：
  蓝噪声会随相机运动形成固定纹理。
- **不做 1/rate 的能量补偿**：Remix 对 MC 信号也不做；补偿只会把稀疏噪声放大
  ×1/rate。非活跃像素的 RT 项按 0 交给历史/RR 重建。
- 门控点（都不是早退，保证引导图/自发光/天空照常写）：
  `aoSamples=0` / `shadowSamples=0`（由现有归一化给出 `shadowTransmittance=0`）/
  `runLocalSecondary=false` / `runGlossyRT=false` / `runDiffuseGI=false` /
  局部光直射按 §3.3 处理。
- 动态（移动）光源**没有** reservoir：非活跃像素直接跳过
  （`evaluateVPTRISDirectLightingPrimary` 里的 `if(!shadingActive) return voxelDirect;`）。

### 3.6 三级优先级（env / UI）

宿主解析：**UI 覆盖原子量(-1=未覆盖) → `ORION_*` 环境变量 → 出厂默认**。

- `ORION_DIRECT_LIGHT_MODE`、`ORION_SPARSE_RATE`、`ORION_RTDI_REUSE_INACTIVE`
  （`VulkanCore.cpp:6905-6985`）。
- ⚠ Java 启动恢复**已经**加了环境变量守卫（`NativeVulkan.java:1976` 附近）：
  env 非空时不下发配置值，否则启动恢复会永远压住 env。在界面上点「应用」
  才算一次显式覆盖。

---

## 4. 已核实的代码锚点（改前先复核）

### `cpp/shaders/rt_test.rgen`

| 行（本次落盘后） | 内容 |
|---|---|
| `1112-1122` | `VPTDirectHistory` 结构体 + 第 4 lane 注释 |
| `2779` | `evaluateVPTDirectLightReservoirTail` 前向声明 |
| `3180-3273` | RIS 提案循环（`selectedEmission/selectedTypeCode` 在此赋值） |
| `3281-3298` | WRS 分支插在 `weightSum <= 0` 早退**之前** |
| `3750-3774` | `evaluateVPTRISDirectLightingPrimary`（唯一允许 WRS 的入口；动态光稀疏门控） |
| `6157-6262` | `vptLocalEmitterContributionAt` / `vptLocalEmitterSampleTransmittance` |
| `6264-6425` | `evaluateVPTDirectLightReservoirTail`（合并 + 历史 + 单根射线） |
| `7649-7676` | Remix 白噪声三件套 |
| `18885-18892` | `vptSparsePixelIsActive` |
| `19973-19975` | `vptPixelActive`（深度/MV 写完之后才算） |
| `21022` / `21290` / `22518` / `22695` / `23089` | 五个稀疏门控点（AO/太阳阴影/本地二级/光泽/GI） |
| `22450-22495` | 运行时二选一（`runPrimaryDirectLight` + `sparseParams.z`） |

### `cpp/VulkanCore.cpp`

| 行 | 内容 |
|---|---|
| `4631-4638` | `offsetof(CameraUBO, sparseParams) == 1712` / sizeof 1728 的 static_assert |
| `6905-6985` | 三级解析：`directLightMode()` / `sparseRatePercent()` / `rtxdiReuseInactiveEnabled()` |
| `17610-17635` | `VPTDirectHistoryGPU` 4×vec4 + `static_assert(sizeof==64)` |
| `18595-18600` | UBO 默认值 `{0,0,1,0}` |
| `28297` / `28310` | `g_localLightRevision` 与其写入 `localLightBuffer.header.y` |
| `50569-50634` | JNI set/get（纯状态拷贝，下一帧 `updateCamera` 生效） |
| `55287-55293` | 每帧写 `sparseParams` |

### Java

| 文件 | 行 | 内容 |
|---|---|---|
| `NativeVulkan.java` | `386-402` | 五个 native 声明 |
| `NativeVulkan.java` | `1976-1996` | 启动恢复 + **env 守卫** |
| `RTXSettingsConfig.java` | `1802-1809` / `2205-2230` / `3287-3290` / `3951-3959` / `4163-4166` / `4535-4539` / `5041-5045` | 字段 / getter+clamp / 两处出厂重置 / JSON 读 / JSON 写 / DTO |
| `RTXSettingsScreen.java` | `451-452` / `757-760` / `1185-1192` / `3022-3037` | pending / 构造初始化 / 两行按钮 / 应用块 |

---

## 5. 已证伪 / 不要再试（本轮踩过的坑）

| 想法 | 结果 / 原因 |
|---|---|
| 用 MV 图（binding 62）做重投影 | 本工程 MV 对相机运动为 0（相机走矩阵），重投影会全屏错位。用 `vptReprojectPreviousPixel` + 抖动补偿 epsilon |
| 整个 `VPTDirectHistory` 复用为 reservoir | 破坏 EMA v0/v1/v2 语义 ⇒ 透明接收面直接退化。只能追加第 4 lane |
| 复用 `evaluateVPTCandidateDirectTemporal` 做主表面 WRS | 那条路径是透明/水下接收面的 EMA 历史，语义完全不同；已另写 tail |
| 稀疏按 1/rate 补偿能量 | Remix 对 MC 信号不补偿；补偿只会把噪声放大 ×1/rate |
| 非活跃像素直接 `return` | 会漏写深度/运动矢量/引导图/GI 半分辨率图 ⇒ DLSS/RR 看到洞。必须只把 RT 项置 0 |
| 直接改 `shadowTransmittance` | 现有代码在 `shadowSamples=0` 时的归一化已经给出 0，改公式反而破坏旧路径 |
| 拿 Remix 的 SDK 头文件 | `submodules/rtxdi` 为空，只有调用点；按契约重写（§3.3） |
| 把本改动当「半透明静止噪点」的修复 | 无关。那条线见 `translucent-static-noise-probe` 与 `glass-rr-guide-artifact` |

---

## 6. 编译（冷编，四个 rgen + DLL）

> 用户自己编，产物先落 `cpp/.tmp/`，**等「拷」/「加载修复版」再部署**。
> 规范口径必须带 `-DRT_BUILD_PRODUCTION_MODE -DSECTION_VERTEX_32B=1`
>（见 `cpp/.tmp/rtgen_sync_20261021/build_canonical.py` 的 BASE_FLAGS）。

```bash
GLSLANG=C:/VulkanSDK/1.4.309.0/Bin/glslangValidator.exe
BASE="-DRT_BUILD_PRODUCTION_MODE -DSECTION_VERTEX_32B=1"

"$GLSLANG" -V --target-env vulkan1.2 -S rgen -e main $BASE \
  -o cpp/.tmp/wrs_check/rt_test.rgen.spv cpp/shaders/rt_test.rgen
"$GLSLANG" -V --target-env vulkan1.2 -S rgen -e main $BASE -DRT_ENABLE_SER \
  -o cpp/.tmp/wrs_check/rt_test_ser.rgen.spv cpp/shaders/rt_test.rgen
"$GLSLANG" -V --target-env vulkan1.2 -S rgen -e main $BASE -DSHARC_UPDATE=1 \
  -o cpp/.tmp/wrs_check/rt_test_sharc_update.rgen.spv cpp/shaders/rt_test.rgen
"$GLSLANG" -V --target-env vulkan1.2 -S rgen -e main $BASE -DGI_HALFRES=1 \
  -o cpp/.tmp/wrs_check/rt_test_gi_half.rgen.spv cpp/shaders/rt_test.rgen
```

**2026-10-26 冷编结果（全部成功）**

| 产物 | 字节 | md5 |
|---|---|---|
| `rt_test.rgen.spv` | 600592 | `a6485d641efaa6ba391ae619c5ab0619` |
| `rt_test_ser.rgen.spv` | 612800 | `404169e2ba62310bf51e3fc74f3906a9` |
| `rt_test_sharc_update.rgen.spv` | 617148 | `8753d3720bcd26ad12d9d41a3af9ef22` |
| `rt_test_gi_half.rgen.spv` | 584856 | `466703d7b2aae7a1c29674b3a6561345` |

⚠ `VPTDirectHistoryGPU` 尺寸变了（48 → 64B）⇒ **DLL 必须一起冷编**，
只换 spv 会读错历史缓冲。CMake 配方见 `build-env-jdk` 记忆条目。

**部署**（用户点头后）：DLL 与四个 spv 要同时写到 `run/` 与 bundle 源
（一次写四处，见 `deploy-target-map` 记忆），只拷 `run/` 会被启动时静默覆盖。

---

## 7. 验收判据（按此顺序，一次只动一个变量）

1. **零回归**：`directLightMode=0`、`sparseRate=0` ⇒ 与部署前**逐位一致**
   （同机位截图 diff；日志无 `[RTX CONFIG] Failed to restore ...`）。
2. **模式 1 静态收敛**：静止在本地光场景（火把/萤石/岩浆），画面应比 0 档
   更快收敛（reservoir 跨帧），无亮斑、无「灯还亮着但已挖掉」超过 1 帧。
3. **模式 1 动态**：快速移动/转身，接收面不应拖出灯影般的残影；开关灯
   （放置/破坏光源）残影 ≤ 1 帧。
4. **稀疏 50%**：`ORION_PERF_TIMESTAMPS=1`，`rt` 段应显著下降；
   画面静态时与原档接近；**相机移动时噪声上升属预期**。若引导图出现洞
   （DLSS 边缘抖动/拖影），说明有人在非活跃分支里漏写了东西，回查五个门控点。
5. **稀疏 + 模式 1 + `z=1`**：相机停住后非活跃像素不应让 reservoir 变「生」
   （恢复全采样后不应出现一次全屏重新收敛的闪动）。
6. **动态光源**：手持发光物/掉落物移动时，直射光跟手且无滞后拖尾（动态光
   走原 RIS，不进 reservoir）。

回退：性能页两个按钮置 0（或 env 置 0），**不需要换 spv/DLL**。

---

## 8. 风险与遗留

- **一帧滞后**：模式 1 在光源/接收面快速运动时理论上多一帧。实测若明显，
  可把 `VPT_DIRECT_WRS_M_CAP` 降到 16 或提高法线阈值；不要关掉 revision 判据。
- **稀疏率的噪声代价**：官方量产档 30~35%；本工程未做逐像素 Rate 的
  spatial reuse，先按单像素历史验证。
- **未接 GUI 的 `sparseParams.z`**：只有 env（`ORION_RTDI_REUSE_INACTIVE`）。
  若实测证明 z=0 更好/更差，再决定是否暴露到界面。
- **文档**：`docs/env-switches.md` 已补三行；本文件是唯一交接入口。
