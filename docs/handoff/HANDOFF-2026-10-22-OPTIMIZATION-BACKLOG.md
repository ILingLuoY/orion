# 优化清单（2026-10-22 记录）

> 本文**只记录，不改任何源码**。每条标注 `[实测]` / `[代码事实]` / `[推断]`，三者不可混读。
> 依据：本轮对各 shader / `VulkanCore.cpp` / 设置项的源码核对，加上已有文档里的实测值
> （`deliverables/降成本清单-2026-10-11.md`、`HANDOFF-2026-10-22-CPU-THREADING.md`）。

---

## 0. 口径与当前配置

| 项 | 值 | 来源 |
|---|---|---|
| 内部 RT 分辨率 | **707×376** | 降成本清单 §2（`dispatch=` 18710 条） |
| rt 段 | 5.75 ms（轻）/ 13.05 ms（重） | 同上，`rt_debug_baseline_20261007` / `A_ablation` |
| 整帧 | 15.0 / 22.9 ms | 同上 |
| 单帧换算 | 15.0 ms ↔ 66.7 fps；每 0.1 ms ≈ 0.44 fps | 计算 |
| A/B 可信度 | 交错 A/B 的 sd ≤ 2.5 ms ⇒ **只有 Δ > 3 ms 可采信** | 降成本清单 §5 |
| 当前配置 | `run/config/rtx-settings.json`（2026-10-22 17:04） | 直接读文件 |

当前取值（本轮所读）：`giRoulette=1`、`sunMarchFarDistance=128`、`rtSpp=1.0`、`giBounces=1`、
`aoNear=1`、`aoMid=1`、`sunShadow=1`、`localLightRisCandidates=1`、`localLightAreaSamples=1`、
`waterReflectionInterval=1`、`glossyFullDistance=16`、`cloudQuality=2`、`godRaySteps=32`。

---

## 1. 本轮已核实的事实

| # | 事实 | 证据 |
|---|---|---|
| F1 | `giRoulette` **已经是 1（开）**，与 Java 出厂默认一致 ⇒ 降成本清单里 A1 那条「你设 0」的建议**已过期** | `RTXSettingsConfig.java:1596`、`run/config/rtx-settings.json` |
| F2 | `sunMarchFarDistance` Java 出厂默认 = **32**，当前是 128；但 `src/main/resources/rtx-settings.default.json:180` 写的是 128，**两份默认值不一致** | `RTXSettingsConfig.java:1640`、resources json |
| F3 | 近场太阳透射 march 已从 6 步改成 **3 步**（2026-10-15）⇒ 降成本清单里「6→1 省 4.0 ms」的实测**口径已不成立**，现在远距简化最多省 2 步 | `rt_test.rgen:645-651`（`WATER_SUN_MARCH_STEPS 3`） |
| F4 | RT 段采样类旋钮全部到底（`giBounces/aoNear/aoMid/sunShadow/localLightRis/localLightArea` 全为 1） | 降成本清单 §A4 + 当前配置 |
| F5 | NRD 整链**永久关停**（`kNrdLocalDiEnabled=false`）；当前本地光降噪是自研链（reproject → moments → 5 级 à-trous），vendored NRD 是 **4.18** 且带 RELAX，`NRD_LOCAL_DI_USE_RELAX=0` | `VulkanCore.cpp:5661`、`30179-30181`、`31980-32042` |
| F6 | 太阳 NEE 是 delta 近似（`sunDir` 单方向 × 3.2），而 `rmiss` 另画了一个日盘（辐亮度 20，被 `clamp(...,2.5)` 压到 2.5），GI 未命中时按 `albedo*0.6` 计入 ⇒ **同一条连接被两种策略各算一次** | `rt_test.rgen:20920+`、`22008-22011`、`rt_test.rmiss:942-1003`、`:1327` |
| F7 | 路径引导的 MIS 权重写法已在（`p_bsdf/p_mix`），但 guide 编译期关闭 | `rt_test.rgen:11050-11192`、`:125-126` |
| F8 | 本地光的 RIS 提案 pdf **不含 1/area 因子**（固定 2×2 撒点）⇒ 它不是立体角 pdf，无法与 `p_bsdf` 组成 MIS | `rt_test.rgen:2970-2983`、`3001-3021` |
| F9 | 命中自发光（`nextUnifiedEmission`）与顶点的本地光 NEE 是同一份能量的两个估计量；源码自述"duplicate" | `rt_test.rgen:22635-22657`、`rt_test.rchit:935-942` |
| F10 | MegaLights 是 UE 引擎内系统，**无可链接 SDK**；仓库全目录 `megalight` 零命中 | 本轮全仓搜索 + Epic 官方文档 |

---

## 2. 帧数类（纯省，不改观感）

| # | 项 | 预期 | 状态 | 锚点 / 备注 |
|---|---|---|---|---|
| P1 | `rtSpp` 1.0 → 0.5（棋盘交错） | rt 段 **×0.5~0.6**（与场景无关） | **可用，但先看 M1** | `rt_test.rgen:18197-18225`；回退 = 固定 1.0 |
| P2 | `sunMarchFarDistance` 128 → 32 | 量级 **~1 ms**（按 F3 折算，**未重测**） | 可用 | `rt_test.rgen:652+`；0 = 关 |
| P3 | `giRoulette` | 已开，无剩余空间 | 已吃 | F1 |
| P4 | `cloudQuality` 2→1 | 步数 −17~33%（无实测） | 可选 | 云占整帧 ~49%，是最大单项 |
| P5 | `godRaySteps` 32→16 / 遮挡 6→2 | 未知（云段无子计时） | 需先测 | 降成本清单 §B3 |
| P6 | MIS 本身 | **≈0 或微负**（+0.02~0.2 ms） | — | 见 §3；采样预算已到底，换不出帧 |

> `rtSpp=0.5` 与 MV 缺陷叠加会**放大拖影**，见 M1。开 0.5 前请先读 M1。

---

## 3. 画质类：MIS 与本地光

### 3.1 MIS 三个候选（成本从低到高）

| # | 项 | 射线 | 估算 Δrt | 收益 | 风险 |
|---|---|---|---|---|---|
| Q1 | **delta 日盘剔除**（GI 射线未命中时不计日盘，只保留日冕瓣） | **0** | ≈0 | 去掉 ~1.5 量级的单点萤火虫；同时消掉 F6 的重复计入 | **✅ 已落地 2026-10-22**（见 §8；开关 `GI_SKIP_SUN_DISK`，默认 1） |
| Q2 | 天空 mixture MIS（复用 `g2bSampleDiffuseDirectionMIS` 的单样本权重，`p_sky` 概率选天空 CDF 方向） | **0** | +0.02~0.10 | 高粗糙度金属/水反射的方差 | 前置：参与 MIS 的两项必须同一能量基准，现在是 `0.6 / 1.8 / 3.2 / 0.7 / clamp 2.5` 各自缩放 |
| Q3 | 本地光真 MIS | **0** | +0.01~0.05 | 修正 F9 的重复估计 | 需先解决 F8（1/area → 立体角 pdf），**能量口径会变**，所有本地光强度旋钮要重标 |

**不做**：给每个 GI 顶点加一条环境 NEE 射线。1 跳时等于 +1 条/像素，按 RTAO 的实测锚点（1~2 条射线 = 1.10~3.81 ms）折算 **+2~3.5 ms**，即 −4~−15 fps。

### 3.2 本地光（比 MIS 更值钱的是这两条）

| # | 项 | 射线 | 说明 |
|---|---|---|---|
| L1 | **可见光列表的屏幕空间复用**（借 MegaLights 的 8×8 瓦片思路） | 0 | 现状：RIS 选中 1 盏灯 → 1 条可见性射线，被挡则该像素本地直射归零，下帧重掷。瓦片表（89×47×8 项 ≈ 34 KB）+ 一个 gather/sort pass + 采样时重投影查表。**这是 MegaLights 里唯一值得抄的部分** |
| L2 | 解析化面积光 + 真实 1/area pdf | 0 | 修 F8，也是 Q3 的前置；代价是亮度整体变化，需接受一次观感重标 |
| L3 | 本地直射**独立 GPU 计时** + "本地直射归零" A/B 档 | 0 | 现在这一项被合进 `rt=`，没有独立读数；不先量就无法判断 L1/L2 值不值 |

### 3.3 MegaLights：结论是不做整链替换

- 官方口径（Epic 文档 + SIGGRAPH 2025）：随机直接光照，**固定每像素射线数**，质量随光照复杂度下降；核心是 **8×8 屏幕瓦片的可见光列表 + 上帧重投影**；方向光**默认关闭**且官方建议太阳继续走 Deferred+VSM；不支持水面/云/体积雾/前向。
- 本工程缺的只是「多采样 + 屏幕空间可见性复用」，骨架已有（section source directory + alias 表 + 1 条可见性射线 + à-trous 降噪）。
- 真按 K=4 采样实施：+3 条可见性射线/px ≈ **+2~5 ms**（−10~−20% 帧率），而 Minecraft 的光照分布（每像素 1~6 个发光方块）撑不起 MegaLights 的规模优势。
- ⇒ **只抄 L1，不做整链替换。**

---

## 4. MV 专项（用户报「到现在还是有问题」）

> **2026-10-22 用户报：MV 已修好。** 本节转为历史记录，不再作为待办；M1/M2 若要复核请重跑 §4.3 判据。
>
> 本节只记录已核实的代码事实与可疑点，**没有定位到根因**。

### 4.1 已确认的设计与近期修复

| # | 事实 | 证据 |
|---|---|---|
| M0a | rgen 写进 `dlssMotionImage` 的是**像素位移**（`deltaNdc * 0.5 * imageSize`），宿主 2026-10-22 已把 `mvecScale` 改成 `(1/W, 1/H)`，两侧现在一致；探针实测 `maxAbs=5.875px` | `rt_test.rgen:18872-18881`、`VulkanCore.cpp:61480-61539`、MV 探针 `24000+` |
| M0b | `cameraMotionIncluded=false`、`motionVectorsJittered=false` ⇒ MV 只允许含**物体自身运动**，静态面必须恰好 0 | `VulkanCore.cpp:61546-61553`、`rt_test.rgen:18804-18850` |
| M0c | 玩家自身层已不发位移段（C1 修复：否则 closest-hit 无条件补 Δcam ⇒ 走动时该层双倍位移） | `VulkanCore.cpp:15222-15234` |
| M0d | 位移段只在**主射线**的命中着色里插值（2026-10-21 收益优化 #2），次级命中不写 | `material_resolve.glsl:155-200` |
| M0e | 4 档 `mvecScale`：0/1 = `(1/W,1/H)`、2 = `(1/W,-1/H)`、3 = `(1,1)`；档 3 是**故意错的 A/B 对照片** | `VulkanCore.cpp:61515-61539` |

### 4.2 可疑点（按性价比排序）

**M1 ★ `rtSpp=0.5` 时 MV 完全不写（代码与注释矛盾）** `[代码事实]`
- `rt_test.rgen:18217-18225` 的 `if(!rtSampleThisPixel) return;` 在 MV 的 `imageStore`（`:18941`）**之前**。
- 但 `:18934-18935` 的注释断言"无论是总开关关着、还是 spp=0.5 棋盘跳过像素，这一格都不会留下旧值"——**与实现矛盾**。
- 影响：现在 `rtSpp=1.0` 不触发；一旦按 P1 开 0.5，未采样像素的 MV 会保留 1~2 帧前的值，RR 拿它重投影 ⇒ 快速物体拖影加倍。
- 候选修法：a) 加一个只处理棋盘未采样像素的 3×3 邻居拷贝 pass（与 M2 的 dilate pass 可以合并）；b) 干脆不开 0.5。**不要**试图在早退前算 MV（MV 依赖主命中）。

**M2 ★ 没有 dilate MV** `[代码事实 + 标准做法]`
- `constants.motionVectorsDilated = eFalse`（`VulkanCore.cpp:61555`；`sl_consts.h:238-239` 的语义是"MV 是否已 dilate"）。
- 前景/背景边缘像素本帧与上帧命中不同物体 ⇒ 1px 级错位，表现是**轮廓抖边/毛刺**，不是整屏鬼影。
- 做法：全屏 compute，3×3 取**最近深度**邻居的 MV 写回，然后置 `motionVectorsDilated=true`。代价 ≈0.03~0.08 ms（707×376，读 MV+depth）。
- 判据：站在栅栏/树叶/玻璃边缘平移视角，看轮廓是否稳定；再对比 `motionDebugView=2`（有向色）。

**M3 `prevViewProj` 每像素现算一次 4×4 求逆** `[代码事实]`
- `rt_test.rgen:18854-18855`：`inverse(cam.prevViewInverse * cam.prevProjInverse)` —— 一次 mat4 乘 + 一次 mat4 求逆，**每个命中像素**算一遍。
- 这是**性能项**（265k 像素 × mat4 inverse，量级 0.05~0.1 ms），不是正确性项。可提到主循环外/UBO 一次算好。⚠ 不能跨帧缓存（prev 每帧变）。
- 这也是"能优化的"里最干净的一条：不改语义、可逐位对照。

**M4 Δcam 修正只对"有位移段的顶点"生效** `[代码事实 + 推断]`
- `material_resolve.glsl:189-194`：`if(any(notEqual(hitWorldOffset, vec3(0))))` 才补 `prevViewInverse[3]-viewInverse[3]`。
- 若某个动态物体的位移段**恰好全 0**（静止实体、或 Java 侧传 0），则 MV=0，而 RR 认为它该有相机运动 ⇒ 相机移动时该物体残留。
- 判据：对着**静止**的实体/掉落物平移相机，看是否有残影。

**M5 depth 与 MV 是否同源（待查）** `[推断]`
- `cameraMotionIncluded=false` 时，DLSS 用 **depth** 自己反推相机运动。若 POM（`primaryParallaxRayTravel`，`rgen:18811-18814`）、水面、玻璃三类像素写出的 depth 与 MV 描述的不是同一个表面，相机重投影就会错。
- 症状会是"移动视角时整片抖/闪"，与 M2 的轮廓抖边不同。
- 待查点：`rt_test_pomdeferred.rchit`、水面 walker、玻璃分支三处的 depth 写法；宿主日志里的 `sizeMatch=`（`VulkanCore.cpp:61651+`）应恒为 1。

**M6 单位是"两处开关对"** `[代码事实]`
- shader 侧 `MOTION_VECTOR_UV_UNITS` 是**编译期** `#define`（`rt_test.rgen:179`），宿主侧 4 档是运行期。
- 档 2/3 只有在同时重编 shader 宏时才自洽；现在档 3 = "像素写 + (1,1)" 是故意错的。
- 优化：把单位也做成 UBO lane（0=像素 / 1=UV），4 档全在宿主闭环，一帧生效、不重编 SPV。**这也能避免下次再踩"改了档位没改宏"。**

### 4.3 MV 的复验判据（不依赖主观）

1. `motionDebugView=1`（幅值热图）：**静态世界与天空必须纯黑**。任何整屏灰 ⇒ 相机成分漏进 MV（M4/M0b）。
2. `motionDebugView=2`（有向色）：中性灰 0.5 = 零位移；颜色反了 ⇒ 符号错（M6 档位）；整屏彩色噪声 ⇒ 单位错。
3. `[MV PROBE]` 回读：`maxAbs` 应在"物体真实像素位移"的量级（走动时个位数 px，不是 700 量级）。
4. Host 日志 `sizeMatch=1`、`motion=%ux%u` 与 `input=%ux%u` 必须同尺寸。

---

## 5. 未测量 / 诚实边界

1. 本地直射的独立 GPU 耗时 —— **未知**（合并在 `rt=` 内）。
2. `cloud=` 段内部拆分（volume march / resolve / atrous / composite / god ray）—— 无子计时，占整帧 ~49%。
3. SHaRC 的净成本 —— 从未测到（`perf/*.log` grep sharc 全空）。
4. A2 的现值收益 —— 口径已变（F3），**必须重测**。
5. MIS 的噪声收益 —— 需要"静止机位 temporal std"指标，现有 A/B 的 sd(2.5 ms) 测不到。
6. MV 根因 —— 本节只列可疑点，**未定位**；M1 是唯一可直接从代码判定的缺陷。

> **2026-10-22 A1 已落地（纯观测，不改行为）**：`[FRAME BUDGET]` 新增一行
> `[FRAME TAIL SPLIT] renderFrameInnerMs= postRenderFrameMs= submitMs= submitCount=`。
> 来源：`FrameBudget`（三个新读数）+ `MinecraftReflexMixin` 的 `renderFrame(Z)V`
> INVOKE/Shift.AFTER + `VulkanCommandEncoderMixin` 的 `submit` HEAD/RETURN。
> 依据：242 个 60 帧窗口实测 `renderFrameTailMs` 中位数 **9.22 ms**，而
> acquire(0.015)+present(0.197) 仅 0.21 ms ⇒ 约 9 ms 无归属。
> 判读：`submitMs ≈ frameTail` ⇒ 队列等待/帧节奏（减 GPU pass 才有 fps 收益）；
> `renderFrameInnerMs` 大而 `submitMs` 小 ⇒ 原版 renderFrame 自己的活；
> 两段都小 ⇒ 卡在原版 Java 侧（HUD/GUI/窗口事件）。
>
> **首轮结果（22:02 那一局，325 个窗口）`[实测]`**：
> - 稳定态（后 1/3）：`interval 7.35 / sim 3.09（其中 beginStreamline 3.04）/
>   render 1.34 / frameTail 3.47（inner 2.84、post **0.00**）/ submit 1.45`，
>   `submitCount = 1 次/帧`。残差 0.00 ⇒ 账对平。
> - **§5 那个"9.22 ms frameTail"是流式加载期的现象，不是稳态问题**：该局早期窗口
>   `inner = 14.92 / 14.66 / 14.18`，稳定后落到 2.2~2.8。上一份日志的 242 个窗口
>   中位数 9.22 是因为那局 21:23 才进世界、21:26 就退出，全程都在流式期。
> - `postRenderFrameMs = 0.00` ⇒ `renderFrame` 是 `runTick` 的最后一个动作，
>   frameTail **全部**是 renderFrame 内的剩余，没有"原版 Java 偷偷干活"的部分。
> - 稳定态最大的非计算项是 `[FRAME JNI HEAD] beginStreamlineMs`（3.04ms），
>   即 `BeginStreamlineFrame()` 里的 `slGetNewFrameToken` / `slReflexSleep`。
>   ⚠ 该局日志里**既没有** `[REFLEX SLEEP] ACTIVE`（成功只打一次）**也没有**
>   `[REFLEX SLEEP] FAILED` ⇒ 那 3ms 到底在 token 还是 sleep 上，**尚未钉死**。
>   钉法：在 `BeginStreamlineFrame()`（`VulkanCore.cpp:44767`）给两个调用各埋一个计时。
> - 下一步最省事的判别：**带 `ORION_PERF_TIMESTAMPS=1` 跑一局**，直接拿
>   `[PERF L10 GPU]` 的 14 段 GPU 时间戳（`VulkanCore.cpp:1853` 的开关，零代码改动、
>   不引入同步点）⇒ 一次回答"GPU 是不是瓶颈、该减哪个 pass"。

---

## 6. 如果只做一件事

**先把 M1 与 M2 一起修（MV 侧），再谈 P1（`rtSpp=0.5`）。**
理由：P1 是当前唯一还有整段量级收益的性能项（rt ×0.5~0.6），而它会把 M1 从"潜在缺陷"变成"每帧可见的拖影"。
M1+M2 可以合并成**一个** 3×3 全屏 compute（未采样像素拷贝邻居 MV + 边缘 dilate），代价 <0.1 ms，且不改任何语义。

---

## 7. Vulkan 扩展盘点（2026-10-22，依据本机 vulkaninfo + Khronos 官方文档）

### 7.1 本机实测环境 `[实测]`

```
Vulkan Instance Version: 1.4.341
device apiVersion  = 1.4.351
driverVersion      = 617.42.0.0   (NVIDIA proprietary)
deviceName         = NVIDIA GeForce RTX 4070 Laptop GPU
```

**当前已启用**（`VulkanBackendMixin.injectStreamlineExtensionsAndFeatures` 与 Streamline 需求）：
`VK_KHR_push_descriptor`、`VK_NVX_binary_import`、`VK_NVX_image_view_handle`、
`VK_KHR_buffer_device_address`、`VK_EXT_buffer_device_address`、`VK_NV_optical_flow`、
`VK_KHR_external_{semaphore,memory}[_win32]`、`VK_KHR_timeline_semaphore`、`VK_KHR_maintenance4`、
`VK_KHR_acceleration_structure`、`VK_KHR_ray_tracing_pipeline`、`VK_KHR_ray_query`(门控)、
`VK_KHR_deferred_host_operations`、`VK_EXT_opacity_micromap`(门控)、`VK_EXT_ray_tracing_invocation_reorder`(门控)。

### 7.2 官方侧的新动向 `[官方文档]`

- **Vulkan 1.4.340（2026-01-23）** 新增 `VK_EXT_descriptor_heap`（**EXT、未定稿**，目标是取代 descriptor set，先收反馈再转 KHR）。
- **Vulkan Roadmap 2026** 六项从"可选"转"强制"：VRS、**着色器时钟查询**、**主机图像拷贝**、
  **计算着色器导数**、多项交换链改进、更高的描述符/着色器接口上限。
- **`VK_KHR_pipeline_binary` 已 Ratified**（#484），依赖 Vulkan 1.4 / `VK_KHR_extended_flags` / `VK_KHR_maintenance5`
  —— 本机 apiVersion 1.4.351 ⇒ 依赖已满足。作用：**按单个 pipeline** 捕获/还原二进制，
  `VkPipelineBinaryInfoKHR` 注入创建，`VkDevicePipelineBinaryInternalCacheControlKHR` 可关掉驱动内部缓存。

### 7.3 能用在自家资源上的候选（按"对本工程是否有意义"排序）

| 扩展 | 本机支持 | 现状 | 对本工程 | 结论 |
|---|---|---|---|---|
| `VK_EXT_calibrated_timestamps` / `VK_KHR_` | rev2 / rev1 | 未启用 | 把 GPU 时间戳与 CPU 时钟对齐 ⇒ 补上 §5 的"云段无子计时""本地直射无独立计时" | **值得**，低风险（只读写时间戳） |
| `VK_KHR_pipeline_binary` | rev1 | 未启用 | 现在是单块 56 MB `VkPipelineCache` 整存整取；per-pipeline 二进制可缩缓存、加快启动 | **值得**（启动/内存，不是 FPS），中风险 |
| `VK_EXT_memory_priority` | rev1 | 未启用 | 配合已有 `VK_EXT_memory_budget` 的 VRAM governor，给 section 缓冲分优先级 | 可选，低风险小收益 |
| `VK_KHR_shader_subgroup_rotate` / `VK_KHR_shader_quad_control` / `VK_KHR_shader_maximal_reconvergence` | rev2 / rev1 / rev1 | 未启用 | **只在新写的 wave 级代码里才有收益**（例如 §3.2 L1 的 8×8 瓦片光源列表 gather） | 等有对应 shader 再开，单独启用零收益 |
| `VK_KHR_shader_float_controls2` / `VK_EXT_extended_dynamic_state3` | rev1 / rev2 | 未启用 | 自家管线的优化/精简，量级很小 | 可选 |
| `VK_EXT_descriptor_buffer` | rev1 | 未启用 | 描述符直写显存，省 CPU；但 2026-10-04 的 **RT 描述符环形实验曾出现"视角冻结"**（疑驱动 per-set 状态）⇒ 这套栈对描述符改动敏感 | **不建议**（风险 > 收益） |
| `VK_KHR_ray_tracing_maintenance1` / `VK_KHR_ray_tracing_position_fetch` | rev1 / rev1 | 未启用 | position fetch 只在 **ray query** 里可用（closest-hit 里没有该指令）；本工程 RT 主体是 `traceRayEXT` ⇒ 用不上；仅太阳阴影/云 march 的 ray query 侧理论可用 | 低收益 |
| `VK_NV_ray_tracing_motion_blur` | rev3 | 未启用 | 可让实体 BLAS 带运动变换、免逐帧重建；但几何是每帧 CPU 捕获的，收益存疑 | 待评估 |
| `VK_EXT_host_image_copy` | rev1 | 未启用 | 本工程热点是**顶点/实例缓冲**上传（buffer），不是图像上传 | 用不上 |
| `VK_KHR_fragment_shading_rate` + `VK_NV_fragment_shading_rate_enums` | rev2 / rev3 | 未启用 | 云 / 光柱 / 降噪全是 **compute**，VRS 只作用于 fragment；只剩最终 composite，收益≈0 | 不做 |
| `VK_KHR_present_wait` / `VK_KHR_present_id(2)` / `VK_{KHR,EXT}_swapchain_maintenance1` | 全部 rev1 | 未启用 | **交换链归 Minecraft 所有**，mod 不持有 | 做不了（除非改 MC） |
| `VK_EXT_descriptor_heap` | rev1 | 未启用 | EXT 未定稿、目标是取代 descriptor set；描述符布局全在 Minecraft 手里 | **不做** |
| `VK_KHR_cooperative_matrix` / `VK_NV_cooperative_matrix2` | rev2 / rev1 | 未启用 | 只有上神经降噪才有意义 | 不做 |

### 7.4 结论

**没有任何一个扩展能给出整段量级的帧数收益。** 当前两个大头（`cloud=7.4 ms`、`rt=5.75/13.05 ms`）
都是自家 pass 的算法成本，扩展只能改"怎么提交/怎么缓存/怎么测量"，改不了算法。
如果只做三件，选：**calibrated_timestamps（先能测）+ pipeline_binary（启动与缓存）+ memory_priority（显存）**。

启用位置与硬约束：`VulkanBackendMixin.injectStreamlineExtensionsAndFeatures`
（`src/client/java/com/example/client/mixin/VulkanBackendMixin.java:118`）。
**每个扩展都必须先 `physicalDevice.hasDeviceExtension(...)` 门控再 `deviceExtensions.add`**，
否则 `vkCreateDevice` 直接失败（现有 ray_query / OMM / SER 都是这个写法）；带特征结构体的扩展
还必须同时 `vulkanFeatures.add(new VulkanFeature(new VulkanPNextStruct(...), ...))`，否则特征未使能。

### 7.5 LSS（Linear Swept Spheres / 线性扫掠球）—— 三重否 `[官方文档 + 实测]`

- 官方：`VK_NV_ray_tracing_linear_swept_spheres`，注册号 **430**、Revision 1、**Not ratified**，
  依赖 `VK_KHR_ray_tracing_pipeline` + SPIR-V `SPV_NV_linear_swept_spheres`；
  新增 `VK_GEOMETRY_TYPE_SPHERES_NV` / `VK_GEOMETRY_TYPE_LINEAR_SWEPT_SPHERES_NV` 两种图元与
  `VkAccelerationStructureGeometryLinearSweptSpheresDataNV`。定位是**毛发/绒毛**（代表案例 Indiana Jones）。
- ① **驱动不暴露**：本机 `vulkaninfo` 共 335 个扩展，`spheres`/`swept` 零命中（driver 617.42）
  ⇒ 现在连"启用"这一步都做不到，不是"支持但没开"。
- ② **材质系统不兼容**：本工程命中后的材质求值依赖三角形顶点表
  （`gl_PrimitiveID` + `gl_InstanceCustomIndexEXT` → `sectionVertexAddressBuffer` → 32B/48B 顶点 → UV/法线/LabPBR）；
  LSS 的命中属性是沿段参数 + 球面朝向，**没有顶点表** ⇒ UV 只能合成，法线贴图/反照率/PBR 全断。
  影响面：`rt_test(. _translucent).rchit/.rahit` 四个 TU + `material_resolve(_glass).glsl` +
  BLAS 构建三个入口（`VulkanCore.cpp:16838 / 19728 / 20102` 全是 `VK_GEOMETRY_TYPE_TRIANGLES_KHR`）。
- ③ **场景不匹配**：Minecraft 无毛发；细杆类（栅栏/铁栏/锁链/藤蔓/铁轨）占三角形比例极小，
  LSS 的体积/遍历优势要"百万根发丝"量级才显现；细几何的现有正解是 **OMM**（叶/草 cutout，已启用）。
- 结论：**不做**。触发条件（若要重估）：驱动暴露该扩展 **且** 出现大量细长几何的 RT 表示需求，
  且先解决"无顶点表 ⇒ UV/材质"这一前置问题。

### 7.6 RTX Mega Geometry（CLAS）—— 本机能启用，但现在不该用 `[官方文档 + 实测]`

- 官方：`VK_NV_cluster_acceleration_structure`（**rev 4**）+ `VK_NV_partitioned_acceleration_structure`（rev 1），
  **本机 vulkaninfo 均存在** ⇒ 与 LSS 不同，这个是"能启用"的。
  层级：`TLAS → Cluster BLAS → CLAS ×N`；另有 **Cluster Template**（不含顶点位置、可复用实例化的"模具"）。
- 它优化的是 **AS 构建成本**，不是遍历/着色成本。官方适用场景：大量动画物体、LOD 切换、
  动态细分、设备端驱动（多级间接）构建、簇复用、顶点位置量化。
  ⇒ 本工程两个大头（`cloud 7.4 ms`、`rt` 的遍历+材质）**都不在靶心上**。
- 本工程最接近它目标的只有一处：**实体/粒子的逐帧 BLAS 重建**（地形 section 是静态 + 编辑时 refit）。
  但那一处的瓶颈实测是 fence/容量抖动/**CPU 预打包与上传**（2026-10-22 的 7 条改动已治），
  而 CLAS 只省 BVH 构建、不省顶点上传，且实体几何是每帧 CPU 捕获的形变数据。
- 接入硬成本：
  1. **命中语义**：CLAS 内 primitive 索引是"簇内局部、可非连续"，提案文档**未定义 `PrimitiveID` 语义**；
     每个 CLAS 另带 32 位 `ClusterID`（命中着色器可读）。本工程材质求值依赖
     `gl_PrimitiveID` + `gl_InstanceCustomIndexEXT` → `sectionVertexAddressBuffer` → UV/法线，
     必须重做或加一层映射，涉及 4 个 TU + `material_resolve(_glass)` + rgen/SBT。
  2. 管线创建需 `VkRayTracingPipelineClusterAccelerationStructureCreateInfoNV`；
     AS 上限（`maxTrianglesPerCluster` / `maxVerticesPerCluster`）与多种对齐需查询。
  3. **当前规范不支持 CLAS 序列化/反序列化** ⇒ 不能像 `pipeline_binary` 那样离线缓存。
- 结论：**可以启用，但不建议现在做**。触发条件：先给"实体/粒子 AS 构建"加独立计时，
  实测证明它确实是显著项且量大到能摊薄改造，再按 RTXMG sample 落地。

### 7.7 RTX IO / GDeflate —— API 在，场景不在 `[官方文档 + 实测]`

- Vulkan 侧对应物是 **`VK_EXT_memory_decompression`**（`VK_NV_memory_decompression` 的别名）
  ⇒ `vkCmdDecompressMemoryNV` / `vkCmdDecompressMemoryIndirectCountNV`，压缩格式 **GDeflate 1.0**。
  **本机实测支持**：`memoryDecompression = true`、`decompressionMethods = MEMORY_DECOMPRESSION_METHOD_GDEFLATE_1_0_BIT_EXT`。
- RTX IO 宣传的两半，只有一半在 Vulkan 有公共 API：
  1. **GPU 解压** → 有（上面的扩展）；
  2. **NVMe 直读进显存（DirectStorage 那半边）** → Vulkan 无公共等价物；而且 Minecraft 的
     资源/存档管线在 Java 侧，mod 只拿到已解压的方块数据 ⇒ 没有"压缩资源包"可解。
- 剩下能用的只有"压缩上传 + GPU 解压"，对象是 section 顶点/索引缓冲。但：
  - 2026-10-22 已把 32B 预打包搬出渲染线程（`staging=` 慢帧 2.97 ms 均值 / 72.61 ms 峰值 ⟶ 只剩 memcpy）；
  - 32B 顶点压缩已把显存/带宽压到 `ΣvertexBytes/meshMB` 0.66~0.68；
  - CPU 热点是 section fuse/打包与锁竞争，**不是 PCIe 带宽**；
  - 解压必须排在 BLAS build 之前 ⇒ 新增"解压完成→建 AS"依赖链，延迟会暴露在 chunk 首帧。
- 额外成本：需要 GDeflate 编码器（仓库内没有）、压缩块布局、解压录制、与 fence/预算交互。
- **替代方案（更划算）**：section 数据是**静态**的，压缩上传只省加载期带宽；
  缩小**常驻顶点格式**（32B → 量化到 24/16B）同时省显存与每帧缓存压力，且不需要任何新 API。
- 结论：**不做**。触发条件：先给"顶点/索引上传"加 GPU 时间戳与 PCIe 占用读数，
  实测证明带宽是瓶颈，再谈压缩上传。

---

## 8. 落地记录：MIS-A（GI 射线剔除太阳日盘）2026-10-22

**改了什么**（两处，均为 `#ifndef GI_SKIP_SUN_DISK`，默认 **1 = 开**）：

| 文件 | 位置 | 内容 |
|---|---|---|
| `cpp/shaders/rt_test.rgen` | `6349` 后 | 新增 `RT_STATE_NO_SUN_DISK_BIT = 1u << 14u`（bit 7/8/12/13 已用，9~11 是 profile，14 空闲） |
| `cpp/shaders/rt_test.rgen` | GI 回路 trace 点（原 `21986`） | `hit.state |= RT_STATE_NO_SUN_DISK_BIT` |
| `cpp/shaders/rt_test.rmiss` | 常量区 `52` 后 / 日盘合成前（原 `985` 后） | 同值常量 + `if((payload.state & BIT) != 0u) sunDisk = vec3(0.0);` |

**为什么这样是"正确的 MIS"而不是近似**：太阳直射在两端都由 NEE 以 delta 近似估计
（主命中 A1、GI 顶点 A3-2），delta 策略对 BSDF 命中日盘那一支的权重就是 0。
只压日盘、**不压** `pow64` 日冕瓣（±8°，非 delta，NEE 覆盖不到）。

**影响面**：GI 回路内发出的全部射线（含水中 `RT_PURPOSE_WATER_MEDIUM` 分支）。
主射线（purpose 7）与 glossy 反射（不在回路内）不受影响 ⇒ 镜面里的太阳仍在。

**产物指纹**（`glslangValidator -V --target-env vulkan1.2 -DRT_BUILD_PRODUCTION_MODE -DSECTION_VERTEX_32B=1`）：

| SPV | 旧 | 新 |
|---|---|---|
| `rt_test.rgen.spv` | `D8F45F44` | `00C31AA1` |
| `rt_test_ser.rgen.spv` | `C87FB7FF` | `652070D1` |
| `rt_test_sharc_update.rgen.spv` | `975B556D` | `590C234B` |
| `rt_test.rmiss.spv` / `rt_test_ser.rmiss.spv` | `D0829039` | `40F4401D` |

已部署：`run/shaders/` 与 `src/main/resources/shaders/`（逐字节比对一致）。
回退包：各处 `*.bak_pre_misa_20261022`；**DIFF 产物必须成套回退**（rgen 与 rmiss 的 bit 语义成对）。
DLL 未改动。

**A/B 判据**：
1. 关闭 = 编译加 `-DGI_SKIP_SUN_DISK=0`（两文件同值），画面应回到旧档；
2. 开启后**整体亮度不应有可见变化**（期望值差 ~1e-4）；若明显变暗 ⇒ 说明某条路径的
   太阳能量原本只由日盘承担，需查该路径的 NEE 是否缺失；
3. 目标收益是"静止时孤立亮点"减少，用 `motionDebugView=0` + 静止机位看间接光区域。

