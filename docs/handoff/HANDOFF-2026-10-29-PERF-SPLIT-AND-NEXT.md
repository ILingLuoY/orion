# HANDOFF 2026-10-29：`trace` 成本拆分结论 + 下一步「分箱延迟材质」方案

> 本文档自包含，可直接作为新会话的起点。所有数据均来自 2026-10-29 当晚的实机测量，
> 所有结论都标注了是哪一局、用什么开关测出来的。

---

## 零、一句话状态

`trace` 的 67% 是次级射线（反射 36% + GI 34%），**遍历只占 7%**；因此 BVH 侧投入（CLAS/Morton/分块）已判定无价值。
下一步唯一有意义的方向是「次级命中的材质求值」，做法必须是**分箱排序后再求值**（2026-10-21 那次不分箱的尝试实测慢了 60%，详见 §5）。

---

## 一、实测数据（4 张表，按测得顺序）

### 1.1 `trace` 三段分解（20:19 局，`ORION_PERF_ABLATION=alt:0,2,3`）

| 臂 | `trace` 中位 | 相对 base |
|---|---|---|
| `base` | 17.51 ms | — |
| `noGlossy` | 11.22 ms | **−6.29 ms (−36%)** |
| `noGi` | 11.49 ms | **−6.02 ms (−34%)** |

与下一张表交叉：主射线 5.03 + 反射 6.29 + GI 6.02 = 17.34 ≈ base 17.51 ⇒ **三项基本可加，不存在大块共享下游**。

### 1.2 遍历占比（20:11 局，`alt:0,4,16`）

| 臂 | `trace` | 相对 base |
|---|---|---|
| `base` | 15.46 ms | — |
| `noRtaoGlossyGi`（只剩主射线） | 5.03 ms | **−10.43 ms (−67%)** |
| `noVoxelDda`（关体素 DDA 遍历） | 14.37 ms | **−1.09 ms (−7%)** |

⇒ 能单独归因的**遍历成本只有 1.09 ms / 7%**。

### 1.3 射线数量类降本全部无效（20:15 局，`alt:0,9,10,11,12,13`）

| 档 | `trace` | 相对 base |
|---|---|---|
| `march1` | 15.38 | −0.38 (−2%) |
| `tierHalf` | 15.53 | −0.23 (−1%) |
| `rrGi` | 15.54 | −0.22 (−1%) |
| `ao2of4` | 15.72 | −0.04 (−0%) |
| `shadow1of2` | 15.74 | −0.02 (−0%) |
| `base` | 15.76 | — |

⇒ **次级射线的成本不由“射线条数”驱动**，减半/砍步数/开轮盘都换不回时间。
（注：当前配置本来就有 `aoSkip=1`，`ao2of4` 无效也属正常。）

### 1.4 refit 同局 A/B（19:01 局，marker 热切换，**结论已采纳**）

| 指标 | 基线 | refit 开 |
|---|---|---|
| `asCreateN` / `asRetireN` | 2214~2228 | **614~751 (−72%)** |
| `asCreateMs` | 279~282 | 259~265 (−7%) |
| `asChunkMB` / `asWasteMB` | 512.4 / 170.5 | 510.4 / 170.0（**无内存代价**） |
| `frame` 中位 | 19.51 | 19.41 |
| `trace` 中位 | 15.55 | 15.27 |

⇒ refit **中性偏正、零内存代价**，已改为默认开。

---

## 二、代码现状（哪些改了、哪些生效、怎么回退）

### 2.1 `cpp/VulkanCore.cpp`

| 改动 | 默认 | 回退开关 |
|---|---|---|
| refit 默认开（`sectionBLASRefitEnabled`） | **开** | `ORION_AS_REFIT=0` 硬关；`run/as_refit.off` 热切换（600 帧探测） |
| refit 判据口径修复（索引化 `(verts/4)*2`、32B 判据传真实 indexed） | 生效 | 随 refit 开关 |
| `geometry.flags` | **已回退为原样的 `0`** | — |
| `effectiveSectionRebuildBatchCap` override 可放大 | 生效 | `ORION_GEOMETRY_BATCH_CAP` / `ORION_GEOMETRY_BUDGET_MS` |
| 捕获队列读游标 `g_capturedSectionsHead` | 开 | `ORION_CAPTURE_CURSOR=0` |
| 诊断字段 `lockSkips` / `lockWaitMs` / `logMs` / `queued` | 常开（只读） | — |
| `logDeviceFaultDiagnostics`（GPU 挂死诊断） | 常开 | 只在 `vkDeviceWaitIdle` 失败时触发 |
| `rtxMemPriority*`（`VkMemoryPriorityAllocateInfoEXT`） | 开 | `ORION_MEM_PRIORITY=0` |
| `applyHdrStaticMetadata`（`vkSetHdrMetadataEXT`，日志前缀 `HDR-2.5`） | 开 | 仅 HDR 激活时调用 |
| 次级 mip bias 写 `historyPadding0` | **0.0f（无变化）** | `ORION_SECONDARY_MIP_BIAS=0~3` |

### 2.2 `cpp/shaders/material_resolve.glsl`

`pbrLod` 计算之后新增：次级命中（`purpose != RT_PURPOSE_PRIMARY_SKY`）加 `uintBitsToFloat(cam.historyPadding0)` 级 mip，上限仍是 `pbrMaxLod`（不跨 sprite、不串色）。
**默认 0 ⇒ 与改动前逐像素一致。**

### 2.3 Java 侧

- `VulkanBackendMixin.java`：新增 12 项可选扩展（device_fault / checkpoints / memory_priority / pageable / shader_clock / subgroup_rotate / quad_control / maximal_reconvergence / expect_assume / float_controls2 / raw_access_chains / hdr_metadata）+ CLAS 两项（`VK_NV_cluster_acceleration_structure`、`VK_NV_partitioned_acceleration_structure`）+ `rtx$logClusterAsLimits()`。
  回退：`ORION_EXT_OPTCAP=0`（一次全关，CLAS 也在其内）；`ORION_EXT_PAGEABLE=0` 单关 pageable。
- `SectionCompilerMixin.java`：玻璃内部面剔除的越界守卫（`|center| <= 32` 才做探测，否则 `drop=false` 保留几何）。
- `docs/env-switches.md`：已同步新增开关。

### 2.4 已删除

**Morton quad 重排（T3-10）—— 已整段删除**，代码/文档/DLL 三处均无残留。
原因：加入后 `[RT MEM] meshSec` 从 954 掉到 **3**（几何几乎全缺），整段删除后恢复。

### 2.5 部署状态

- native：`cpp/build/Release/rtx_backend.dll` MD5 **`90DCB0D17B4D1C016589FB6CEAB88131`**，已同步 `run/`、`src/main/resources/natives/windows/`、`build/resources/main/natives/windows/`
- SPV：`rt_test.rchit.spv` = **`3bfa7e06`**（含 mip bias 逻辑），已部署三处；`tools/audit_spv_freshness.py` 报 **漂移 0**
- ⚠ 既有遗留：`rt_test.rmiss.spv` 在 `run/` 里是旧版（`cpp=53f0ceb4` vs `run=d8414c1b`），今天之前就这样，未处理

---

## 三、下一步：①-1「反射的分箱延迟材质」

### 3.0 ⚠ 预期修正（2026-10-29 晚追加，**先读这段再决定做不做**）

**SER 已经拿走了“重排”这一类收益的绝大部分**，代码里有完整实测（`VulkanCore.cpp:2719-2724`）：

```
// ORION_SER_LADDER 一局 5 档实测（185 窗口 / 36 个完整周期，按周期内配对）：
//   基线 SER off  rt = 33.64 ms
//   mode 1 −34.9%  mode 4 −33.2%  mode 5 −36.8%  mode 6 −35.6%
//   36/36 个周期 SER 全胜
```

当前 pin 的是 mode 5。把三组数据叠起来看：

| 事实 | 数据 |
|---|---|
| SER 已吃掉的发散成本 | 33.64 → 15.46 ms（约 −9 ms，−36.8%） |
| 减少射线数 | `rrGi` −0.22 / `ao2of4` −0.04 / `shadow1of2` −0.02（几乎无效） |
| 遍历 | `noVoxelDda` −1.09 ms（仅 7%） |

⇒ 剩下的 15.46 ms 是**每条命中固定的 ALU 工作量**（POM 步进、PBR/GGX、图集采样次数），
**不是访存发散、也不是相干性问题**。而分箱优化的是访存相干性 ⇒ **对固定 ALU 无效**。

**修正后的预期：①-1（分箱）收益 ≤ 0.5 ms，甚至可能为负**（多一次 hit buffer 写读往返）。
原估的 20~40% 来自公开案例，而那些案例普遍**没有 SER**。

真正还有空间的是**降低每 hit 的 ALU**：
- 次级命中简化 BRDF（跳过 GGX、Lambert + 常数 specular）
- 减少图集采样次数（`_n`/`_s` 合并或按需）
- POM 步数（用户已明确不接受在次级命中砍 POM，需重新拍板）

这些都动画质，**必须先由用户拍板取舍**，不是纯工程优化。

**建议**：在动 ①-1 之前，先做一次零改动验证 —— `ORION_SER_LADDER` 关掉 SER 跑一局，
确认在当前 build（含今天的改动）上 SER 仍然贡献 −30% 以上。如果 SER 贡献已经变小，
说明剩下的确实是 ALU；如果 SER 贡献仍很大，说明还有发散空间，分箱才值得试。

### 3.1 为什么必须先分箱

`HANDOFF-2026-10-26-RTXDI-AND-TRANSLUCENCY.md:119` 原文：

> **2026-10-21 删掉的 `RT_DEFERRED_MATERIAL` 是"延迟材质求值"（第二遍重新采样图集），实测 17.66ms → 28.26ms**

即：**不分箱的 deferred 比内联慢 60%**。原因是没有排序/分箱，第二遍采样是完全随机的，多了一次数据往返且访问更散。

对照标准做法（Selle, *Sorted Deferred Shading for Production Path Tracing*）：命中点按 meshID / faceID 分箱排序，**同箱内材质一致、纹理访问连续**。

当前源码状态：`RT_DEFERRED_MATERIAL` 的代码路径**已被整段删除**（`hit_payload.glsl:6`、`material_resolve.glsl:5`、`rt_test.rgen:2` 只剩注释），`HitIdentity` 与 `HIT_IDENTITY_GLASS_BIT` 均已删除。
`.tmp/retired_deferred_20261023/` 里只有那套旧 SPV 二进制 + `_build_deferred.bat`，**源码不可复用**。

### 3.2 设计（一次编对的最小切片）

| 项 | 设计 |
|---|---|
| **HitPoint 结构** | `vec3 pos; vec2 uv; uint materialId; uint primId; uint instIdx; uint packed;` ≈ 32 B |
| **写入** | `rt_test.rchit` 只写 HitPoint 到 SSBO（按像素线性索引），`atomicAdd` 桶计数 |
| **分箱** | 按 `materialId`（0~15）× 面朝向（6）分 96 桶；**counting sort**（前缀和 + 重排索引），不上 GPU radix sort |
| **求值 pass** | 新 `material_resolve_binned.comp`：逐桶 dispatch，桶内调原 `resolveHitMaterial`（参数化版本已存在，可直接复用） |
| **只做反射** | 仅对 `RT_PURPOSE_*` 的反射射线启用；主射线与 GI 保持内联（降低风险与工作量） |
| **A/B 开关** | 走 UBO lane：`cloudParams5.x`（注释标为 reserved）。**不用 `historyPadding0`**（已被 mip bias 占用） |

### 3.3 改动清单（下一轮按此执行，**只冷编一次**）

1. `cpp/shaders/hit_payload.glsl` — 新增 `HitPoint` 结构与 hit SSBO 声明
2. `cpp/shaders/rt_test.rchit` — 反射命中改为写 HitPoint（保留内联路径作为 `#else` 分支，用宏 `ORION_BINNED_REFLECTION` 切换）
3. `cpp/shaders/rt_test.rgen` — 反射部分改为记录桶偏移，不再直接消费 payload
4. **新增** `cpp/shaders/material_resolve_binned.comp` — 分桶求值
5. `cpp/VulkanCore.cpp` — hit SSBO + 桶计数 buffer + 前缀和（可用小 comp 或 host 侧读回，**优先 host 读回避免再加一个 comp**）+ pipeline + pass 插入点 + `cloudParams5.x` 写入
6. 一次冷编：`rt_test.rchit` + `rt_test.rgen`（含 SER 变体）+ `material_resolve_binned.comp`，**四处部署**（`cpp/shaders`、`run/shaders`、`src/main/resources/shaders`、`build/resources/main/shaders`），改完立刻跑 `tools/audit_spv_freshness.py` 确认漂移 0

### 3.4 判据与终止条件

- 只比 `[PERF L10 GPU] maxMs ... trace=`
- **≤ 0 收益就终止**，并记录结论「此场景下分箱固定成本 > 缓存收益」——这也是有价值的结论，避免以后重试
- 有正收益才推广到 GI（①-3）

---

## 三-补、官方原做法对齐 + 本仓库的三处结构冲突（2026-10-29 21:15 补）

### 三点一 官方原文（Eisenacher, Nichols, **Selle**, Burley, EGSR 2013, *Sorted Deferred
Shading for Production Path Tracing*, §3.2）

- hit point 记录 = **differential geometry + mesh ID + face ID + face UV**；
- 排序两级：**先按 mesh ID 分组（parallel radix sort），组内再按 face ID 预排序**，
  使着色顺序与 per-face 纹理的磁盘顺序一致；
- 收益机制原文：*"Because each mesh face is touched at most once when shading a ray
  batch, all shader inputs, including texture maps, are only accessed once. This
  amortizes per-file texture costs (opening the file over the network), and per-face
  texture costs (reading and decompressing a block of texels) perfectly for each batch."*
- 成本：35 min 总渲染里 **hit point sorting 只占 1.5 min**（shading 15 / traversal 12.5）；
- 明确否定项：*"Sorting just the hit points ... yields no benefits for traversal."*

**与本仓库的映射**：meshID ↔ TLAS instance（section），faceID ↔ section 内 quad 的图集 tile。
⚠ 但官方的主要收益机制（per-face 纹理文件从磁盘/网络打开、解压）**在这里不存在** ——
Minecraft 是单张图集、全驻显存。剩下的收益只有 L2/L1 局部性与分支一致性，量级远小于论文。

### 三点二 三处结构冲突（HANDOFF §3.3 的改动清单据此**不成立**）

| # | 冲突 | 证据 |
|---|---|---|
| a | 反射**不是单段**，`tracePrimaryGlossyReflectionRadiance` 是最多 16 段的循环，每段消费上一段的材质结果。只延迟"第一段"就必须把路径状态（origin/dir/throughput/accumulatedRadiance/镜像层数/水界面计数/seed…）序列化到 SSBO，并**新增第二遍 raygen** 从 pathStep=1 续跑 | `rt_test.rgen:18689-18778`（MAX_PATH_TRACE_STEPS=16），`18785-18840`（每段 traceRayEXT + 立即读 payload） |
| b | `material_resolve` 内部**含光追**（POM / POM 自阴影），所以 §3.3 第 4 项的 `material_resolve_binned.comp` **不可行**：compute 管线没有 RT pipeline，跑不了 `traceRayEXT`。求值 pass 必须是**新的 raygen 变体** | `material_resolve.glsl:1391 traceParallax(...)`、`:1885 tracePOMSelfShadow(...)` |
| c | rchit **拿不到 ray index**（没有 `gl_LaunchIDEXT`），无法把 hit 写到"哪个像素的槽"。必须在 `HitPayload` 里加字段 ⇒ 共享 include 改动会让 **6 个 TU 全部重编** | `hit_payload.glsl:17-62`（payload 无 rayTag），`rt_test.rchit:1997-2020` |

⇒ 完整实现的实际工作量 = 改 payload ABI（全量冷编）+ rgen 状态序列化 + 新 raygen 变体 +
VulkanCore 侧三段 buffer/两个 pipeline/分箱前缀和。**不是一次"最小切片"，是一整天的改造。**

### 三点三 先量价格：消融档 17 `noMatEval`（已实现并部署）

在 rchit 的 `main()` 里（`rt_test.rchit:2020` 附近）加了一个分支：命中后**跳过
`resolveHitMaterial()`**，只把 payload 填成中性灰（albedo 0.18 / 法线 = -rayDir /
roughness 0.5），让 rgen 继续按"命中普通 opaque 面"走。

- 用法：`ORION_PERF_ABLATION=alt:0,17`（同局内交替，±60 帧窗口，逐对相减）
- 判据：`trace(base) − trace(noMatEval)` = **材质求值的价格上界**（连主射线也算进去、
  且玻璃/水退化成 opaque 使路径变短 ⇒ 上界偏大）
- **终止线**：若这个上界 < ~2 ms，分箱延迟材质（一次额外 raygen pass + 计数/前缀和/
  双倍 SSBO 往返）**必然亏**，按 §3.4 直接终止并记结论；
  若上界 > ~5 ms，才值得按上面 a/b/c 三项改造，并优先只做"材质属性"那一半。
- 部署：DLL `28DDBEAE`（六处一致，含 `noMatEval` 字符串）、`rt_test.rchit.spv`
  `781BA57E`（四处一致）；`rt_test.rchit.bak_pre_matprobe_20261029` 与
  `VulkanCore.cpp.bak_pre_matprobe_20261029` 为回退点。
- 生产档（stage 0）走 else 分支，逐像素零变化。

### 三点四 实测结果与结论：**①-1 终止**（2026-10-29 21:5x，两局，同局配对）

**第 1 局 `ORION_PERF_ABLATION=alt:0,17`（32 窗口，A/B 接对）**

| 臂 | `trace` 中位 | n |
|---|---|---|
| `base` | **16.69 ms** | 24 |
| `noMatEval` | **9.24 ms** | 8 |
| 配对 Δ（相邻 8 对） | **7.27 ms** | 8/8 全胜，极差 7.19–7.61 |

⇒ 材质求值 = trace 的 **43.5%**，上界 **7.3 ms**。

**第 2 局 `ORION_SER_LADDER=1`（191 窗口，5 档轮换，同局配对）**

| 档 | `trace` 中位 | n |
|---|---|---|
| SER off（mode 0） | **28.05 ms** | 38 |
| mode 1（自定义 key 融合） | 15.66 | 39 |
| mode 4（role-table instance class key） | 16.26 | 38 |
| mode 5（**现网 pin**，driver key 全融合） | 16.02 | 38 |
| mode 6（= mode 1 + DISTANCE_ONLY 走 rgen 查询） | **15.50** | 38 |

⇒ **SER 价格 = 12.04 ms（42.9%）**。

**结论（三条互相独立，方向一致）**

1. **算术**：`base(SER on) 16.69 = 非材质 9.24 + 材质 7.45`；`base(SER off) 28.05` ⇒ SER off 时材质部分
   约 **18.8 ms**。即 SER 已经把同一份材质求值从 ~18.8 ms 压到 7.45 ms（**−60%**）。
   软件分箱是 SER 的**替代品而不是叠加品**，它最多做到与硬件重排同级，而那一级已经达到
   ⇒ 增量 ≈ 0，而固定成本（额外 raygen pass + 双倍 SSBO 往返 + 计数/前缀和）恒为正。
2. **工程内已直接试过"手工选 key 分组"**：mode 4 就是"用 role table 自选 instance class 做
   reorder key"，实测 **16.26 ms，比 driver-picked key 的 mode 5（16.02）慢 0.24 ms**。
   手工分组的先验在这个场景下是**负**的。
3. **官方也不做**：NVIDIA 的 SER 博客把"按材质对光线排序"列为被 SER 取代的旧流程步骤
   （"不再需要单独的压缩和排序阶段"）；论文里的主要收益是 per-face 纹理文件的打开/解压
   （15.6 GB out-of-core），本项目是单张常驻图集，机制不存在。

⇒ 按 §3.4 的终止条件记录：**此场景下分箱固定成本 > 缓存收益，①-1 不再重试**；
   ①-3（推广到 GI）同样不做。若将来要重开，唯一有意义的前提是"分箱能拿到 SER 结构上拿不到的
   信息"（现网 driver key 已含 instance 粒度，materialId 粒度更细但收益未知），且必须先证明
   mode 4 ≥ mode 5。

**附带发现（待复测，可白拿）**：这次 mode 1（15.66）与 mode 6（15.50）都**快于现网 pin 的
mode 5（16.02）**，38 个窗口逐周期一致（每周期 s6 < s5）。与 2026-10-12 那次"mode 5 最快
（−0.667 vs mode 1）"方向相反 —— 那次测的是 `rt`、且场景不同。建议单独复测后考虑把 pin 从
5 换成 1 或 6（约 0.4–0.5 ms / trace 的 3%），切换前需确认 pixel-equal 与 SER 语义不变。

---

## 四、A/B 的正确姿势（今天踩过的坑）

1. **必须同局内切换**。跨局对比会被场景差异彻底污染 —— refit 一度被跨局数据误判为「负收益」，同局 marker 切换后翻案。
   可热切换的：`run/as_refit.off`（600 帧探测）。其余 env 都是启动读一次。
2. **消融必须用 `alt:` 或 `pin:`，不要用 sweep**。`ORION_PERF_ABLATION=1`（SWEEP）实测档位**从不推进**（236 个窗口全是 `base`），因为它依赖 `g_perfL10FrameSerial`（见 `VulkanCore.cpp:3552`）；而 `pin:`/`alt:` 走 `g_perfL10AblationStage = request` 的直赋分支。
3. **站定 + 固定视角 + 全屏**。失焦会锁 10fps，把 `framesPerStage` 的时间拉长一倍。
4. 看 `[RT MEM] meshSec` 作为「几何是否正常」的第一判据：正常 900+，异常会掉到个位数。

---

## 五、今天失败的两次（不要再犯）

1. **`replace_all` 误伤**：用 `str.replace` 批量替换 `g_capturedSections.clear();` 时，把 helper 函数**自己内部那行**也换成了函数名 ⇒ 自递归。编译器 `C4717` 救了一次。**教训：批量替换后必须检查被替换位置是否包含定义体本身。**
2. **Morton 重排（T3-10）**：只改 quad 顺序、理论上 shader 零改动，但实机 `meshSec` 954 → 3（几何几乎全缺，TLAS 只剩 3 实例）。
   结论：**`uploadChunk` 是每个 section 的必经路径，任何插进它的代码都必须先做「单层 / 单 section / 恒等排序」三级验证**，不能一次全量默认开。

---

## 六、环境与运行

- 启动：`.\gradlew.bat runClient --offline`（IDE 启动等价）
- 日志：`run/rt_debug.log`（native，全量）、`run/logs/latest.log`（Java + FRAME BUDGET）
- 崩溃日志：`run/hs_err_pid*.log`、`run/crash-reports/`
- 既有崩溃（非本次改动引入）：
  - `nvoglv64.dll+0xeaa43c` —— **退出/切换世界**时的 `recordFrameCommand → createRenderPass`，10-28 起多次出现
  - `SectionCompiler.cullInteriorGlassFaces` 越界 —— 已加守卫
