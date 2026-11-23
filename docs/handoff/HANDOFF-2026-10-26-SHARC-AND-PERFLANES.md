# HANDOFF 2026-10-26 — SHaRC 对齐、性能车道接通、体积光调研

> 一句话现状：**主 trace 仍是唯一瓶颈（4 跳 GI 下 13.19ms / 70%），但它现在可以被真正调参了**（GUI 的 8 条性能设置第一次接到渲染端）；SHaRC 从"纯亏"变成净赚 3.2ms；空气体积光已退役、待用 froxel+ReSTIR 重做。

---

## 一、已落地改动（全部已编译部署，行为可回退）

| # | 改动 | 位置 | 实测 | 回退 |
|---|---|---|---|---|
| 1 | SHaRC update 改**块级 dispatch**（满屏 230k 线程 → 每块 1 线程） | `rt_test.rgen` `sharcUpdateBlockOffset()` + `VulkanCore.cpp` `sharcUpdateBlock` | `sharcup` **5.35 → 1.39ms**（−74%） | 恢复 `rt_test.rgen.bak_pre_sharcblock_*` |
| 2 | SHaRC update 弹射**钳到 2**（`SHARC_PROPAGATION_DEPTH`） | `rt_test.rgen` `giBounceLimit` 后 | update 写入 2616→6165/帧 | 删掉那段 `#if SHARC_UPDATE` |
| 3 | SHaRC `SHARC_UPDATE_BLOCK` **5→3→2** | `rt_test.rgen` + `VulkanCore.cpp`（**两处必须同步**） | block=3：命中率 26.7→**35%**，`trace` 17.67→**13.19**，净赚 **+3.2ms**；**block=2 未验证** | 改回 `5`/`3` 两处 |
| 4 | 水面**风源关闭**（视觉交给 FFT 法线） | `VulkanCore.cpp` `waterWindStrength()`（`ORION_WATER_WIND=1` 恢复） | `wsim` **20.62 → 0.06ms**，观感几乎不变 | 环境变量 |
| 5 | 水面模拟降频 30→15Hz | `waterSimUpdateSeconds()`（`ORION_WATER_SIM_HZ`） | **无效**（单次成本随场景变，总帧时不变），保留不删 | 环境变量 |
| 6 | **8 条性能 lane 接通**（GUI → JNI → UBO） | `VulkanCore.cpp:61115` 附近写 `perfAoNearSamples…perfGlossyFullDistance` | `[PERF LANE] giBounces=4` 已确认生效 | 删掉那段赋值 |
| 7 | GI 跳数脱离死车道 | `rt_test.rgen` `perfL10GiBounceCap()`：UBO 优先、`GI_BOUNCE_CAP_DEFAULT=2` 兜底 | 0 跳 → 可配 1..4；每跳 ≈ **1.04ms** | 宏 |
| 8 | `caus` scope 拆分（新增 `cloudpipe` 边界） | `VulkanCore.cpp` 枚举 + `perfL10CpuPhaseAdvance(P_RT)` 之后 | `caus` 20.51 → **0.00**（原来混入了云/天空管线创建） | 删枚举项 |
| 9 | MV `mvecScale` 曾改 **{2,2}**，实测**拖影**，已回退 `{1,1}` | `VulkanCore.cpp:66055` | **结论：UV 位移与 {1,1} 是正确配对**（`sl_consts.h` 的 "normalize to [-1,1]" 不能反推出 2 倍） | 宏 `g_mvScaleMode` |
| 10 | `[PERF LANE]` / `[SHARC STATS]` 诊断日志 | 同上 | 值变化时打一行 | — |

**编译状态**：`rtx_backend.dll` 六处一致；SPV 四处同步（`cpp/shaders`、`run/shaders`、`src/main/resources/shaders`、`build/resources/main/shaders`）；`tools/audit_spv_freshness.py` 报告 `漂移 0`。

---

## 二、本轮关键发现（每条都可复核）

1. **`perfPrimary.z`（GI 跳数）原本恒 0** —— Java 有 `giBounces` 设置、JNI 也存进了 `g_perfGiBounces`，但**没有任何一处把它写进 CameraUBO**。整条链路断在最后一步。现已接通（`[PERF LANE]` 可证）。附带复活：AO 近/中距采样、太阳阴影采样、本地光 RIS 候选数、面积采样、水面反射间隔、光泽距离。
   - 注：`rt_test.rgen:2724` 的注释说"全仓 grep 无命中"，是因为它搜的是 `perfPrimary`，而实际存储叫 `g_perfGiBounces`——**这条注释误导了后续排查**。

2. **空气体积光已于 2026-10-19 退役**（`VulkanCore.cpp:5262`）。`enabled` 现在只控制水下分支；`VL_AIR_GODRAYS=1` 才开旧空气路径。所以 `[PERF L10 GPU]` 没有 volumetric 段是正常的。

3. **SHaRC 之前"净亏"的真因是填充量不足**，不是 query 失效。`[SHARC STATS]`：`update=2616/帧` vs `query=95985/帧`（1:37），命中率 26.7%。调到 block=3 + 弹射钳 2 后写入翻倍、命中率 35%、`trace` 降 4.48ms。

4. **Remix 的体积光实现文件在公开仓库里就不存在**：`rtx/algorithm/volume_integrator.slangh`、`rtx/pass/volumetrics/volume_restir.slangh` —— `git log --all` 零命中（`origin = NVIDIAGameWorks/dxvk-remix`）。**无法逐位照抄，只能按 ReSTIR 论文自研**。可用的只有 binding 布局（`volume_integrate_binding_indices.h:29-39`：4 进 4 出 ping-pong）。

5. **Remix 的 ReSTIR 只用在两处**：RTXDI（直接光选灯）和体积光。**间接光默认是 SHaRC，不是 ReSTIR GI**（`integrateIndirectMode` 默认 SHaRC）——这是"ReSTIR 默认关"印象的来源。

6. **Remix 没有 AO pass**（全仓 `rtao/ambientOcclusion` 零命中）。它的间接光完全来自路径追踪。我们的独立 AO 射线可能与 4 跳 GI 重复计算，待消融验证（`alt:1`）。

7. **MV 单位偏差 2 倍**：我们写 UV（`(prevNdc-curNdc)*vec2(0.5,-0.5)`），而 Streamline 要求 MV 归一化到 NDC [-1,1]（`sl_consts.h:204`）。配 `{1,1}` 等于只给了一半位移。

8. **跨局测量不可比**：帧时间局间波动 ±1.5ms，与要测的量同量级。**只有同局 `alt:` A/B 可信**。

---

## 三、当前性能基线（高压场景、4 跳、SHARC block=3、风源关）

```
frame=18.88  trace=13.19(70%)  sharcup=3.00  resolve=0.22  dlss=1.58
wsim=0.06  caus=0.00  cloudpipe=0.00  cloud=0.12  snear=0.17
[SHARC STATS] update=6165/frame  queryHits=37043/frame  到达=105973  → 35.0%
[TLAS SKIP] instances=1119~1761（每帧全量 BUILD，`updateTlas=false`）
```

---

## 四、待办（按优先级，每项都给了第一步）

| # | 事项 | 第一步 | 验证判据 |
|---|---|---|---|
| 1 | **SHARC block=2 验证**（已编译部署，未测） | 起一局站 90s | `update`≈14k、命中率 40%+、`frame` < 18.88 才继续加压 |
| 2 | ~~MV {2,2} 验证~~ **已完成：拖影，已回退 {1,1}** | — | 拖影消失 = 结论 |
| 3 | SHARC 时域 10/64 验证（已改，未测） | 同上 | 命中率↑、无间接光闪烁 |
| 4 | **AO 是否与 GI 重复** | `ORION_PERF_ABLATION=alt:1` 一局 | `trace` 大降且画面不变 ⇒ 砍掉 AO |
| 5 | **ReSTIR 体积光 S1~S5** | S1：8 个 handle + 3 张 3D 图 + 空 `recordVolumeRestir`，开关 `ORION_VOL_RESTIR` 默认 0 | 编译通过、行为不变 |
| 6 | `local_light_grid` cell **64→16 格** | `local_light_grid_common.glsl` + `rt_test.rgen` **两份同步** | 每格候选降 64 倍，8 盏截断才够用 |
| 7 | 给 NEE 加 reservoir（RTXDI 思路） | 先读 `sampleLightRIS` 的 WRS + inversePdf | 密集灯区噪声下降 |
| 8 | 发行包白名单缺 SHaRC SPV | `build.gradle` 的 `rtxRuntimeShaderFiles` | `ORION_SHARC=1` 不再静默降级 |

### ReSTIR 体积光设计（已定稿，S1 待写）

- froxel 网格 `ceil(w/8) × ceil(h/8) × 64`，深度指数分布 `exp=2.0`，**覆盖 96 格**（不是 Remix 的 20，Minecraft 雾更远）；
- restir 网格 = froxel÷4，层数 64（与 froxel 对齐便于重投影）；
- reservoir **16B/RGBA32_UINT**：`x`=lightIndex(16b)+flags(2b)+age(8b)、`y`=weightSum、`z`=W、`w`=M；
- 5 个 dispatch：initial(32 RIS) → visibility → temporal(M clamp 2) → spatial(3D 邻域 8) → integrate(全分辨率 froxel，沿视线 slices/4 步 + 蓝噪声抖动)；
- 资源约 **5.2MB**（radiance RGBA16F + age R8 + reservoir，各 ping-pong）；
- 旧 `volumetric_light.comp` **保留**（水下分支仍在用），新增独立开关 `ORION_VOL_RESTIR`。

---

## 五、测量方法论（本轮踩过的坑，务必沿用）

1. **只用同局 A/B**：`ORION_PERF_ABLATION='alt:1'` / `alt:1,2,3` 让窗口在 base 与消融级间逐 60 帧交替，同位置站 90s，取两组均值相减。**跨局比较已两次得出错误结论**。
2. **位置固定**：A/B 交替期间走动会把场景差异混进差值。
3. **被丢弃的窗口**：日志出现 `boundaries incomplete, missing index=N` 说明某帧少了边界 ⇒ 该帧被丢。新增边界**必须放在每帧必走的位置**（本轮 `CLOUDPIPE` 放在条件执行的云管线 block 里，导致 0 个窗口）。
4. **测量前清进程**：客户端未完全退出就重启会触发 JVM native OOM（`hs_err_pid*.log`，`arena.cpp:193`），看起来像代码崩，其实只是内存不足。

---

## 六、其它坑

- **回滚要按项回滚**：本轮曾用 `sharc_ubo.glsl.bak_pre_temporal_*` 整个文件恢复以撤掉 clamp，结果把无关的时域对齐（10/64）也一起撤了，造成"叫你对齐又没对齐"。**下次改多项时用带前缀的分项标记，别靠整文件 bak**。
- **`SHARC_UPDATE_BLOCK` 是编译期宏**，C++ 的 `ceil(w/block)` 与 shader 的 `block*idx+offset` 必须同时改，否则块坐标错位。
- **shader 冷编 ~15 分钟**：所有 shader 侧改动必须攒批一次编，不能拿它做试错。
- **杀进程前等 `rssMB` 归零**（日志 `[RT MEM]`），否则内存叠加。

---

## 七、环境变量速查

| 变量 | 默认 | 作用 |
|---|---|---|
| `ORION_PERF_TIMESTAMPS` | 0 | 开 `[PERF L10 GPU]` 分段（必须 1 才有数据） |
| `ORION_PERF_ABLATION` | — | `alt:N` / `alt:1,2,3` / `pin:N`（0 base、1 noRtao、2 noGlossy、3 noGi、4 三者合、8 gi1Bounce、14 rrOff） |
| `ORION_SHARC` | 0 | SHaRC 总开关 |
| `ORION_SHARC_ENTRIES` | 22(4M) | 元素数 2 的幂（21 = 2M = 88MB，显存紧张时用） |
| `ORION_SHARC_UPDATE` | 跟随 SHARC | 只关 update 保留 resolve，用于分成本 |
| `ORION_WATER_WIND` | 0 | 1 = 恢复常驻风源水面 |
| `ORION_WATER_SIM_HZ` | 15 | 水面模拟更新频率（15..60） |
| `ORION_GI_BOUNCES` | — | 计划中（当前走 GUI） |

---

## 八、备份清单（按时间）

```
cpp/VulkanCore.cpp.bak_pre_sharcblock_*
cpp/VulkanCore.cpp.bak_pre_lanename_*
cpp/VulkanCore.cpp.bak_pre_windoff_*
cpp/VulkanCore.cpp.bak_pre_block2_*
cpp/VulkanCore.cpp.bak_pre_mvec_*
cpp/shaders/rt_test.rgen.bak_pre_sharcblock_*
cpp/shaders/rt_test.rgen.bak_pre_sharcclamp_*
cpp/shaders/rt_test.rgen.bak_pre_block3_*
cpp/shaders/rt_test.rgen.bak_pre_block2_*
cpp/shaders/rt_test.rgen.bak_pre_perflanes_*
cpp/shaders/sharc_ubo.glsl.bak_pre_temporal_*
cpp/shaders/sharc_ubo.glsl.bak_pre_sharc_temporal_fix_*
```

（备份文件较多，确认某项稳定后可清理；`.tmp/patch_*.py` 里保留着每次改动的完整脚本与注释。）

---

## 九、当日下午追加（体积光 / 半透明 / MV / RTXDI）

### 9.1 体积光（froxel + ReSTIR）—— 已能跑，晴天不可见

另一个 agent 实现了完整管线（`vol_restir*.comp` 6 个 + `vol_restir_*Pipelines`），
我修了 3 处导致**驱动级崩溃**的缺陷：

| # | 位置 | 缺陷 |
|---|---|---|
| 1 | `createVolumeRestirOutputPipelines` composite 段 | `depthInfo` 是 `COMBINED_IMAGE_SAMPLER` 但 `sampler = NULL` |
| 2 | `createVolumetricCloudResources` godRay 段 | `godRaySceneInfo` 同上 |
| 3 | 同上 | `writes[]` 里有未初始化槽位（`descriptorType=0`）与 NULL imageView |

**根因共性**：`VkDescriptorImageInfo{}` 零初始化后只填 `imageView`/`imageLayout`，漏 `sampler`。
没有校验层时 NVIDIA 驱动**直接 access violation**，不返回 VkResult。

**新增防御**：`vkGuardUpdate(count, writes, tag)`（在 `createVolumeRestirChainPipelines` 之前），
提交前逐条校验并跳过坏槽位，用 `RTLogFlushed` 落盘。体积光 5 处提交已全部改走它。
（注：`RTLog` 有缓冲，崩溃时内容会丢 —— 这是前几轮"日志一条都没有"的原因，诊断必须用 `RTLogFlushed`。）

**当前状态**：管线全部创建成功（`[VOL RESTIR] ... pipelines ready`），
但**晴天看不到体积光是正确行为**——体积光强绑定介质，Remix 默认雾也只有 `0.999 @ 200m`。
验证必须去**水下 / 雨天 / 下界 / 洞口光柱**，或加调试视图（跳过消光直接输出 froxel radiance）。

### 9.2 半透明：已与官方对齐

`glass_chain_common.glsl`（2026-10-25 为对齐 Remix 引入的透射链契约）里，
数学部分（几何级数，`[Remix brdf.slangh:533]`）**本来就一致**，多出来的只有一条自加硬阈值：

```cpp
#define ORION_GLASS_CHAIN_MAX_FACES 32          // 原 16（对齐 psrRayMaxInteractions=32）
#define ORION_GLASS_CHAIN_MIN_TRANSMITTANCE 0.0 // 原 0.01（官方无硬阈值，全靠 maxInteractions）
```

原值下第二次命中就 `glassChainShouldAbort` 返回 true → any-hit **不调 `ignoreIntersectionEXT`**
→ 该面变实心，即"第一层透明、第二层不透明"。

**遗留未修**：`glassChainBegin()` 只有 1 处调用，而 `glassFaceBudget = 0.0` 有 6 处
⇒ 5 个发起点只清预算、不清介质状态（`glassInside`/`glassEntryT`/`transmittanceAccum`）。
若仍有"某些角度第一层就不透明"，就是这里。

### 9.3 MV：已回退

`mvecScale` 曾按 `sl_consts.h` 的 "normalize to [-1,1]" 推断改成 `{2,2}`，实测**拖影**，已回退 `{1,1}`。
**结论：UV 位移与 `{1,1}` 就是正确配对**，那句注释不能反推出 2 倍。

### 9.4 体积云的 4 个坏绑定（待修）

`[VK GUARD] cloud:` 日志点名：
- `binding 2 / 3 / 5` → `COMBINED_IMAGE_SAMPLER` 缺 sampler
- `binding 8` → NULL imageView

被 guard 跳过（所以不崩），但云缺这几张贴图。属于另一条工作线。

### 9.5 另一个 agent 的改动（默认全关，不影响基线）

| 功能 | 开关 | 默认 |
|---|---|---|
| RTXDI 直接光 | `ORION_DIRECT_LIGHT_MODE` | 0（原 NEE） |
| 稀疏渲染 | `ORION_SPARSE_RATE` | 0 |
| 四肢 MV（逐顶点历史） | `-Dorion.limbMv=0` 关闭 | 开 |

### 9.6 RTXDI 完全对齐计划

见 `docs/RTXDI-ALIGN-PLAN.md`。核心结论：**我们是 SVGF（信号层滤波），官方是 ReSTIR（样本层复用）**，
两条不同路线；完全对齐需 5 阶段（reservoir → WRS → temporal → spatial → shading），
每阶段单独编译验收。阶段 1（reservoir 基础设施）**未开始**。
