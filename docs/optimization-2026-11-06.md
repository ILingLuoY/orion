# 2026-11-06 优化记录：构建提速 + 几何/显存 + VLI

本文档只记录**实机验证过、且已合入代码**的改动，以及**验证后判定无效/危险并回退**的尝试。
每条都给了证据来源（日志字段或测量口径），便于以后复核或回退。

基线：`1e12949`（2026-11-02/03，含 TU 拆分）。
测试平台：RTX 4070 Laptop（8 GB），16 区块视距，DLSS-RR + FG，内部分辨率 707×376 / 输出 2122×1129。

---

## 0. 架构级重构（本会话开始前已完成，已在上一个提交）

- 7.1 万行单文件 `cpp/VulkanCore.cpp` → `cpp/backend/` 下 **36 个 TU** + `OrionInternal.h`（约 5.6k 行：include/类型/原型/全局 extern）+ `State.cpp`（约 2k 行：约 500 个全局变量定义）。拆分契约见 `cpp/backend/README.md`，函数体逐字未变。
- **本会话为其补上的构建侧收尾**：`cpp/CMakeLists.txt` 增加 `/MP` + `target_precompile_headers(OrionInternal.h)`，`bc7enc.cpp` 用 `SKIP_PRECOMPILE_HEADERS`（它不 include 该头，否则 C1010）。

| 指标 | 拆分后（无 PCH） | 现在 |
|---|---|---|
| 全量构建 | ~60 s | **8.5 s** |
| 单 TU 增量 | ~2.3 s | **1.3 ~ 2.4 s** |
| 导出符号 | 201 | **201（Compare-Object 零差异）** |

---

## 1. 手部层指纹判等（`Entities.cpp`）

**问题**：`handChanged = !g_handFrameVertices.empty()` —— 只要第一人称手里有东西（常态）就置脏，经 `Frame.cpp` 的 `g_entityRebuildRequested` **绕过** 10 帧静默窗直接调 `rebuildWorldGeometry(24)`，整轮世界几何重建退化成每帧执行。

**改法**：复用一直存在但从未使用的 `g_handCachedHash` / `g_handCachedHashValid`，改为哈希 + 尺寸判等；手部层消失时删表项。

**证据**：`[HAND LAYER] frames=300 changed=0 unchanged=300` 连续 15 个窗口（4500 帧全命中）。

| 指标 | 改前 | 改后 |
|---|---|---|
| CPU `geometry` 段 avg | 1.62 ms | 1.32 ms |
| CPU `geometry` 段 max | **13.00 ms** | **1.64 ms** |
| CPU `geometry` 段 p90 | 2.70 ms | 1.46 ms |

---

## 2. 显存治理：修「区块死活加载不出来」（`Sections.cpp` / `WorldGeometry.cpp`）

**现象**：`meshSec=6062` 但 `blas=2038`，远景大片区块在 RT 里不可见；日志刷 `page capacity exhausted`。

**根因链**（全部有日志指纹）：

```text
budgetMB 由 7180 动态缩到 6671 → ratio 越过 highWater(0.72) → over=1
over 的退出条件是 ratio < lowWater(0.62)，但驱逐只释放 BLAS（asBlasMB≈144MB / usedMB≈5077）
⇒ ratio 只能降 2.8%，永远到不了 0.62 ⇒ over 永久为真
⇒ 每 4 帧继续踢段 ⇒ nonResident 4637 且持续增长 ⇒ 视野内 90 格外的段全被踢走
```

**四处改动**：

| # | 改动 | 效果（实测） |
|---|---|---|
| 1 | `vramGovernorHighWater` 0.72 → **0.80** → **0.82**（仍低于注释记录的 DLSS `eWarnOutOfVRAM` 线 0.849） | ratio 0.76~0.81 不再误触发 over |
| 2 | `evict` 增加 `nonResident` 饱和保护（默认 3000，`ORION_NONRESIDENT_GUARD`） | `nonResident` 4637 → **0**，`blas` 2038 → **4427** |
| 3 | `over` 在「驱逐已饱和」时也退出 | 整局 `overEnters=0` |
| 4 | `evict` 帧节流 4 帧（`ORION_VRAM_EVICT_INTERVAL`）+ 候选容器 `static` 复用 | `evict` avg 5.03 → 3.76 ms，max 15.43 → **8.63 ms** |

> 关键认识：**驱逐不释放显存**（页号/槽位被复用），所以用它做「降 ratio」的手段本身无效；over 的滞回带假设「驱逐能降 used」是设计缺陷。

---

## 3. Section role 表「变化才写」（`Sections.cpp`）

**问题**：`writeSectionRoleSlot` 每帧对全部段（~6000）各做一次**离散 4B 写**，而 `g_sectionRoleMapped` 是主机可见（write-combined）映射 —— 离散写每次刷一次 WC 缓冲。

**改法**：主机侧影子数组比较（缓存内读），命中则整帧零写入；影子以映射指针为代际（重建时整体失效重填）。

**证据**：`ord` 段 2.00 ms → **0.19 ms**（`preCmd` 7.64 − evict 3.50 − inst 3.95）。

---

## 4. VLI（体素光注入）八项

| # | 问题 | 改法 | 效果 |
|---|---|---|---|
| 1 | `applyVLISectionVoxelsLocked` 每段调一次 `rebuildVPTCandidateNeighborhoodLocked`，而它是 5×5×5=125 个邻域 Section、每个再内层扫 125 ⇒ 单段约 15625 次哈希查找 | 批量路径改为**按 pageIndex 去重**收集，循环结束统一重建一次 | `apply` 324.84 → **0.23 ms**；`[RT VLI SLOW]` 215 条 → **2 条**，325 → **29 ms** |
| 2 | `refreshVPTEmitterSectionFromLocalLights`（Java 逐 Section 回调）持 `g_vliMutex` 做同样的 125×125 | 锁内只收集受影响页，帧内 `recordVLIUpdates` 统一重建（`s_hasDeferredVptNeighborhoodPages` 原子标志避免空闲帧白拿锁） | `wait` 8~17 ms → **0.0001 ms**，`site=workerApply` 从锁热榜消失 |
| 3 | 池增长 2x：一局 2 次，每次锁内整池迁移 + `vkQueueWaitIdle` 17~22 ms | 步长 **2x → 4x**（1024→3253 一步到位） | 一局 **1 次**，23 ms 一次性 |
| 4 | 池顶到上限后新 Section 被**永久丢弃**（只在 `g_vramGovernor.over` 时才允许淘汰最远页） | 判据从「显存是否超预算」改为**「最远页是否比新段更远」**（`evictFarthestVLIPageForIncomingLocked`） | 池不再死钉上限；`vliLivePages 3248 / freePages 5` 持续回收 |
| 5 | 上述丢弃日志每帧刷（实测 1211 次 / 2.5 min） | 限频每 600 条一条 | 1211 → **6** |
| 6 | 淘汰每次在锁内**全表扫** 3253 页 | 等距抽样上限 256（`VLI_EVICT_SCAN_LIMIT`） | `wait` 7~10 → **0** |
| 7 | `chunkUnloadScan` 单次持锁 **42 ms** | 探针查明 98% 在 `g_vliSections.erase()`（节点内联 `VLISectionCpuPage`，180 µs/次，锁内析构大块）⇒ **先 move 出大对象、锁外析构** | 3043 次列卸载、8059 页摘除，**一次都没超过 1 ms**，该 site 从 `[VLI LOCK]` 彻底消失 |
| 8 | 淘汰路径同样的 erase 代价（一局 5887 次） | 同样 move-out 到 `thread_local` 容器 + 每帧锁外 drain | `pending` max 3.65 → **2.69 ms** |
| 9 | **第 4 项改完后的回归**：判据变「距离」后淘汰从"偶尔跑"变成"快速移动时持续跑"，但 `evictFarthestVLIPageImpl` 尾部**仍是锁内同步 125×125** | 与第 2 项同一机制：只收集受影响页，帧内统一重建 | `rest` 8.89 ms（max 37.9）与 `wait` max 47.5 消失；末尾 `fps` 150→22.8 的塌陷不再复现 |

### 探针实测（第 7 项的依据）

```text
[VLI UNLOAD BREAK] sections=7 totalMs=1.2809
  tableMs=0.0049   mapEraseMs=1.2597   hashForgetMs=0.0087
  tableSlots=8192  livePages=2772
```

即：**不是**哈希表删除、**不是**嵌套取锁，而是 `unordered_map::erase` 析构大对象。

---

## 5. 其他运行期小改

| 改动 | 说明 |
|---|---|
| `Glass.cpp`：稀疏掩码 pass 门控 | 采样率 ≤ 0 时整条 pass 早退（默认档 30%，故默认无收益，仅「0=关」时生效） |
| `Device.cpp` / `Sharc.cpp`×2：管线缓存写盘去抖 | 三处同步 `persistSharedPipelineCache()` 改为 `countdown=60`，交给 `maybePersistSharedPipelineCache` 去抖 + 大小去重 |
| `PipelineCache.cpp`：恢复后去重 | 加载成功后设 `g_pipelineCachePersistedBytes = initialData.size()`，消除启动后把**与磁盘完全相同**的 184 MB blob 再写一遍（实测 120 ms 空写） |
| `vol_restir_composite.comp`：`froxelW` 恒等化简 | `(viewZ-near)/maxViewZ ≡ pow(sliceT,e)`，`pow(pow(t,e),1/e) ≡ t` ⇒ 每帧省约 6600 万次 `pow` + `clamp` + 除法 |
| `Sharc.cpp`：update 闸门 / resolve 隔帧 | `ORION_SHARC_RESOLVE_PARITY`（默认 2）；仅在「间接光模式 = SHaRC」时生效，默认档为关 |

---

## 6. 捕获链：split 缓冲池化 + 驱逐半径修正（本轮决定性改动）

### 6.1 `SectionGeometrySplit` 缓冲池化

**问题**：捕获线程在 `g_chunkMutex` 内为每个 Section 新建十几个 vector（最多 16K 顶点 / 几百 KB）。首次触碰触发缺页，`CAPSLOW` 的**墙钟 / CPU 周期比高达 14~25 倍**（frame=9660：墙钟 35.96 ms，实际 CPU 仅 ≈1.45 ms），单段持锁 `capHoldMs` 8.4~9.4 ms —— 这是 `pre`（等锁）与 `upWaitMs`（上传线程等锁）的共同根因。

**改法**：split 在消费端（build 循环）只被**读**（`&update.second.opaque` 这类取指针），没有 move 走，所以用完可连同容量回收复用。

| 文件 | 改动 |
|---|---|
| `OrionInternal.h` | 声明 `takeSectionSplitFromPool()` / `returnSectionSplitToPool()` |
| `Geometry.cpp` | 池实现（上限 24、只回收有容量的、复位 `hasOmmInput`/`ommInput`）；捕获线程改为取热 buffer |
| `WorldGeometry.cpp` | RAII guard 在 `rebuildWorldGeometry` 返回时归还全部 split（覆盖所有 early return）；两处 `SectionGeometrySplit{}` 改为从池取 |

**实测**（双方都是 `capPerWake=1`，唯一差别是池）：

| 指标 | 无池 | 池化 |
|---|---|---|
| `capHoldMs` | 8.4 ~ 9.4 ms | **2.5 ms** |
| `upWaitMs` | 14.2 ms | 11.6 ms |
| `geometry` avg | 5.64 ~ 8.58 ms | **3.73 ~ 5.39 ms** |
| **`fps`（10 分钟 / 107341 帧）** | 165 → 71 → 58 → 48（持续衰减） | **165.5 恒定** |

帧率分布（1789 个窗口）：`p10=163.7 / avg=159.9 / max=178.8`；`rssMB=8648` 而 `peakRssMB=8653`（零增长），`nonResident=0`、`secEvicted=0`、`gap=-16`。

### 6.2 驱逐 / 补回半径修正（修「很多区块怎么也不加载」）

**现象**：`meshSec=6682` 而 `blas=4447`（差 2235）、`nonResident=2665`、`secEvicted=3004` 但 `secRehydrated=230`。

**根因**：`vramEvictKeepRadius()` 默认 **96** ⇒ over 时 keep=72、**驱逐半径 90**、**补回半径 72**，而 16 区块视距是 **256 格**。视野内 90~256 格那一大片被踢出 BLAS 后**永远回不来**。

**改法**（`Settings.cpp`）：

| 参数 | 改前 | 改后 | 效果 |
|---|---|---|---|
| `vramEvictKeepRadius()` | 96 | **288** | over 时驱逐半径 → 270、补回 → 216，视野内不再被误踢 |
| `vramRehydratePerPass()` | 16 | **64** | 万一积压，排空速度 ×4 |

**实测**：`gap=-4 ~ -19`、`nonResident=0`、`secEvicted=0`，区块不再缺失。

### 6.3 捕获节流 `ORION_CAPTURE_PER_WAKE=1`

每次唤醒的段数 4 → 1：`bld` 3.44 → **0.51 ms**，`geometry` 12.24 → **8.58 ms**（叠加 6.1 后到 3.73~5.39）。
**代价**：`upWaitMs` 9.0 → 11.6~14.2（上传线程等得更久），区块上传变慢。

---

## 7. 验证后**回退**的尝试（避免以后重复踩）

| 尝试 | 结果 | 原因 |
|---|---|---|
| **段数/距离驱动驱逐**（`ORION_SECTION_MAX_COUNT` 默认 4000） | **负优化，默认改回 0** | 16 区块视野内真实段数就是 ~6000，4000 上限让系统永远超限 ⇒ 每 4 帧全表扫 + 持续驱逐且降不下来：`asRetireN` 1130→2519、`upWaitMs` 12.8→19.2、`fps` 69.6→47.6。机制保留但默认关闭 |
| **VLI 池增长改 fence 延迟回收** | **GPU 挂死，已回退** | 首次触发增长即 `5s timeout reached when waiting for VK semaphore` → `0xC0000409`。恢复 `vkQueueWaitIdle` 同步版后同条件稳定。结论：该路径是 8 个长期驻留 buffer 的整体切换，异步化需要 timeline semaphore 在帧边界注入，收益（一局 2×20 ms）不值风险 |
| ~~**`ORION_CAPTURE_PER_WAKE=1`**~~ | **先前判错，已改为采用**（见 6.3） | 早先的结论是"只是把工作切片、与持锁时长无关"。补充 6.1 的池化后重测：它确实把 `bld` 3.44 → 0.51、`geometry` 12.24 → 8.58，代价是 `upWaitMs` 上升。真正没用的是"只切小段、不解决缺页"——两者必须一起上 |
| **TLAS UPDATE**（`updateTlas=false` 打开） | **不做** | `[RT GEOMETRY SLOW]` 实测 `tlas` 段 avg 0.24 / max 0.37 ms，收益仅 0.1 ms |

---

## 8. 环境变量开关（本会话新增/使用）

| 变量 | 默认 | 作用 |
|---|---|---|
| `ORION_VRAM_EVICT_KEEP_RADIUS` | **288** | 保护半径（格）。**必须 ≥ 视距**，否则视野内会被驱逐且补不回（见 6.2） |
| `ORION_VRAM_REHYDRATE_PER_PASS` | **64** | 每帧补回段数上限 |
| `ORION_VRAM_EVICT_INTERVAL` | 4 | evict 帧节流；=1 回到逐帧（A/B） |
| `ORION_VRAM_EVICT_PER_PASS` | 16 | 每帧驱逐段数上限 |
| `ORION_NONRESIDENT_GUARD` | 3000 | 被驱逐段数上限，达到后停止驱逐；=0 关闭 |
| `ORION_SECTION_MAX_COUNT` | 0 | 段数驱动驱逐上限；**默认关闭**（见第 7 节） |
| `ORION_SECTION_KEEP_RADIUS` | 384 | 段数驱动时的保留半径（格） |
| `ORION_CAPTURE_PER_WAKE` | 4 | 捕获线程每次唤醒处理的段数；=1 可降持锁（见 6.3，代价是 upWait 上升）。**实测对 `pCap` 无效**，见 10.1 |
| `ORION_VLI_GATHER_PAGES` | 384 | VLI gather 每帧最多搬运的源页数（每页 16 KB）。稳态建议 128（见 10.2） |
| `ORION_VLI_BUDGET_MS` | 4 | VLI apply 每帧时间预算 |
| `ORION_VLI_SECTIONS_PER_FRAME` | 2 | VLI apply 每帧段数上限 |
| `ORION_SHARC_RESOLVE_PARITY` | 2 | SHaRC resolve 隔帧 |
| `ORION_VLI_GROW_DEFER_FRAMES` | 30 | 池增长的安静窗推迟上限 |

---

## 9. 已知未解决问题（下一步）

1. **退出期挂死（比 AV 更严重的新症状）** —— 连续三局在退出时 `Responding=False`、持续烧 CPU（5 秒涨 4.9 s）、10 GB 不退，只能强杀；窗口已销毁（`FG-3 SWAPCHAIN/SURFACE DESTROY`）但进程不结束。更早的多局是 `0xC0000005 at nvoglv64.dll+0xA6639A`（15:23 / 17:43 / 18:12 / 18:30 / 19:31 / 19:36 / 20:28 同一签名）。两者都是退出路径，**均未修**。已给 `Frame.cpp` 帧首加了 6 个 `RTXMarkPhase` 打点（`frame:prio/hash/phaseCpu/persist/hotreload/atlas`），下次崩溃时 `[CRASH PATH]` 可直接定位到具体调用。
2. **`preLoopMs` 11.4 / `tailMs` 9.4（窗口极值）** —— 预循环里 `orderedSections` 构建+排序、脏键候选扫描、`inst` 视锥剔除都是 O(段数)（16 区块约 6000~6900 段）。当前 `rebuildCalls` 仍 ≈1 次/帧，而 `asCreateN` 只有 ~2200，即**大量 rebuild 没有实活却照付 O(N) 尾部**，是下一个结构性目标。
3. **`ORION_CAPTURE_PER_WAKE=1` 的代价**：`upWaitMs` 从 9.0 抬到 11.6~14.2，区块上传变慢。若体感加载变慢，可折中取值 2。
4. **仓库根目录散落 177 个未跟踪 shader 文件**（`.rahit.spv`、`bloom_*.comp`、`camera_ubo.glsl` 等），疑似脚本误拷贝，**未提交**，建议清理。
5. **部署一致性**：`src/main/resources/shaders` 曾被 gradle 用旧版覆盖，导致 RT 管线缓存键漂移、每次冷编 8~17 分钟。改动 shader 后必须同步 `cpp/shaders` → `src/main/resources/shaders` → `build/resources/main/shaders` → `run/shaders` 四处。

---

## 10. 2026-11-07 续：VLI gather 预算、帧外时间归因

### 10.1 两个新探针

| 探针 | 位置 | 作用 |
|---|---|---|
| `[CHUNK LOCK] site=… held=…` | `Geometry.cpp`（`g_chunkMutex` 的两处主要持有点：`capture` / `uploadErase`） | 超 3 ms 才打。实测**只有 `capture` 触发**，`uploadErase` 一次都没有 |
| `[FRAME GAP] n=120 avg=… max=…` | `Frame.cpp` 帧首 | 量相邻两次 `recordFrameCommand` 的间隔，与 `[PERF L10 CPU]` 的 `workAvgMs` 相减即得「帧外时间」 |

**结论 1**：帧外时间占大头。典型一帧 `avgMs=17.77 / workAvgMs=6.03`，即约 2/3 不在后端代码里（Java 侧 + 驱动提交 + 呈现）。
**结论 2**：`pCap`（等 `g_chunkMutex`）的唯一来源是捕获线程；`capPerWake=1` 已开仍无改善 ⇒ 单段成本是主因，不是"一次抓太多段"。

### 10.2 VLI gather 每帧预算

`gather` 把 `g_vliDirtySourcePages` 每页 4096×u32（16 KB）拷进上传 arena，页多时单次 3.3 ms / 16 ms 峰值，把 worker 饿到 `wait` 8.6 ms / 45.8 ms 峰值。

改动（`Vli.cpp`）：
- 每帧最多搬 `ORION_VLI_GATHER_PAGES`（默认 384）个源页，搬完的逐页 `erase`，剩余留到下一帧；
- 其余 6 个脏集合（table / solve / emitter / emitterAlias / candidate / sourceDirectory）**无条件清空**。

> 踩过一次坑：第一版写成「源页排空才清那 6 个集合」，想避免"表项已更新、源数据未同步"。但稳态下脏页持续多于预算 ⇒ 源页永不排空 ⇒ 那 6 项每帧全量重做，`gather` 从 n=2 恶化到 n=179。已改为无条件清空。

实测（`ORION_VLI_GATHER_PAGES=128`）：

| 指标 | 384 | 128 |
|---|---|---|
| `gather` 次数 / max | 179 / 14.24 ms | **70 / 7.30 ms** |
| 几何慢帧 `pCap` max | 35.17 ms | **20.24 ms** |
| `geom_slow` 条数 | 254 | **94** |
| `cpuFps` 末段 | 34.4 | **77.8** |

### 10.3 回退：`orderedSections` 签名缓存

曾给「收集 + 排序 + role 写 + LOD 覆盖」加签名缓存（key + vertexAddress + blasAddress 的 FNV），命中则复用上一帧顺序。实测命中率只有 **31%**（`[GEOM ORD CACHE] hit=2453 miss=5347`）——签名含全部段，任意一段重建就整帧 miss，而 miss 时还要白付每段 3 次 FNV。**已回退**。

### 10.4 当前剩余瓶颈

| 项 | 数值 | 说明 |
|---|---|---|
| **`pCap`（等 `g_chunkMutex`）** | avg 8.50 / max 20.24 ms | 捕获线程单段持锁 ~4.8 ms × 排队；`capPerWake=1` 无效 |
| `VLI wait`（等 `g_vliMutex`） | avg 7.55 / max 31.24 ms | `gather` 已限流，但 `pending` 与 `vliGrow` 仍在 |

已排除 `CAPSLOW` 的"计算"假设：`Mcycles` 换算只有 ~1.45 ms，而墙钟 35.96 ms ⇒ 96% 在等内存（每顶点 300~600 ns，是 memcpy 的数百倍）。"捕获是否还有漏掉的冷分配"已在 10.5 用探针回答：冷分配确实存在，但归因不是漏加的层，而是 split 池的稳态规模被峰值并发钉死。

### 10.5 split 池的真实命中情况（**推翻**此前的"基本没命中"结论）

上一版据 `grow=22144 / growMB=4385` 推断"池命中率接近 0"。这个推断是错的，错在两处口径：

1. `counters` 行里的 `poolHit/poolMiss` 是 **SectionMemory 显存池**，与 split 缓冲池无关；
2. `captured` 只统计**捕获线程**的产出，渲染线程每帧自己的同步捕获不计入 ⇒ "grow / captured" 会高估扩容率（1.13 就是这么算出来的）。

补上真正的探针（`spHit/spMiss/spRet/spRej/spDrop/spStarve/spSize` 与 `growEmpty/growSmall`）后实测稳态：

```text
spHit=3051 spMiss=737 spRet=3046 spRej=151 spDrop=0 spStarve=482 spSize=13
growBk=261/215/0/182/95/63/71/0  growEmpty=481  growSmall=406
```

- 命中率 = 3051/(3051+737) = **80.6%**（不是 0）；
- 扩容率 ≈ **0.34 次/捕获**（不是每次）；
- 扩容归因：`growEmpty`（拿到空槽）**56%**、`growSmall`（槽不够大）**44%**。

**池为什么长不大**（这是真正的问题）：带容量的槽只能由「冷分配 → 填充 → 归还」产生，所以**池的稳态大小 ≈ 峰值并发在途段数**（实测 13~24），与 `SECTION_SPLIT_POOL_LIMIT` 设多大无关——实测把它从 24 提到 96 完全没有效果。峰值并发 = 捕获线程一次唤醒连捕的段数（队列上限 `GEOMETRY_CAPTURE_MAX_QUEUED=256` 内可一口气抓上百段）+ 渲染线程一帧捕获的段数（~6）。

本轮改动（`Geometry.cpp` / `WorldGeometry.cpp`）：

1. **take 推迟到捕获前**。原先"选 key"阶段就按 `batchCap` 一次 take 24 个，而时间预算只让 1.4 个真正捕获 ⇒ 22 个槽整帧滞留池外（`spSize` 被压到 0，捕获线程随即 miss）。
2. **max-fit 取槽**（取总容量最大的），池满时**以大换小**（原先 LIFO、池满直接丢弃）。
3. **捕获线程池空时不冷分配**：结束本轮唤醒，4 ms 后重试（连续 32 次唤醒才兜底冷分配，避免世界加载被饿住）。
4. 被时间预算截断的条目**先归还再 `resize`**（原先直接析构，容量一起丢）。

实测：`capHoldMs` 8.2 → **3.0~7.5**，`cpuFps` 63.3~66.4（基线 62.3~63.4）。扩容总量没有显著下降——剩余 miss 来自上面那条结构性原因，不是取槽策略。

**下一步（按收益排序）**：

1. 降低峰值并发才是根治：捕获完立刻把顶点拷进帧级 arena，split 随即还池，使并发降到 1~2（需要把 staging 与 split 生命周期解耦）。
2. 不做第 1 条的话，命中率会在 80% 附近封顶，`growEmpty` 稳定在 ~480/600 帧。

### 10.6 根治：capture arena —— split 持有窗口缩到「一次捕获」（已上线）

既然池的稳态大小 = 峰值并发在途段数，扩池无用；根因是 split 的持有窗口横跨整个
`rebuildWorldGeometry`（build → staging → TLAS），一帧捕获 6 段就锁住 6 个槽。
所以把数据从 split 里搬出来，split 立刻还池。

改动（`Geometry.cpp` / `WorldGeometry.cpp` / `Sections.cpp` / `OrionInternal.h`）：

1. 新增 `CaptureArenaSegment`（`std::span<const Vertex>` 视图）+ 分块 arena：
   每块 4MB、**段不跨块** ⇒ 块地址稳定，`span` 不会因扩容悬垂；超过一块的段走 bigChunk。
2. 三个捕获点（捕获线程 / 渲染线程脏段 / forceReset 全量）统一改成：
   本地 split 填 → `packSectionSplitBucketsLocked` → `captureArenaCommit` →
   `returnSectionSplitToPool`。split 的持有窗口缩到一次捕获。
3. 消费链路全部去掉 vector：`CapturedSectionUpdate` / `sectionUpdates` /
   `PendingSectionUpload` / `rebuildGeometryClass` / `createSectionBLAS` 改用 `std::span`。
4. arena 复位：staging 拷完之后，`captureArenaTryReset(queued)`——**同时**要求
   "无写者"（`captureArenaBeginWrite/EndWrite` 包住 commit + 入队）与"队列无在途段"，
   否则会覆盖还没被消费的段。

实测（DLL `499E29AD`，稳态 + 按键移动触发新区块加载）：

```text
spHit=600 spMiss=0 spStarve=0 spSize=2
growBk=0/0/0/0/0/0/0/0   growEmpty=0   growSmall=0
arenaN=600/窗口  arenaRst=600  arenaChunks=8(~32MB 常驻)  arenaSkip=0
capHoldMs=0.0
```

| 指标 | 10.5（max-fit + 96 槽） | 10.6（arena） |
|---|---|---|
| 扩容次数 / 600 帧 | ~1100 | **0** |
| split 池内槽数 | 13~24 | **2** |
| `spMiss` | 599~966 | **0** |
| `capHoldMs` | 3.0~7.5 ms | **0.0~0.1 ms** |
| `workAvgMs` | 7.7~8.7 ms | **1.8~2.2 ms** |

代价：每段多一次 memcpy 进 arena（约 200KB/段，热内存 memcpy 约 3ns/顶点，对比原来
缺页引发的 300~600ns/顶点仍是两个数量级）；arena 常驻约 32MB（8 块 × 4MB，按需增长、不释放）。

后续注意：`growSmall` 归零意味着 split 的容量已经"一次训到最大"（同一个槽被反复复用），
`SECTION_SPLIT_POOL_LIMIT` 现在只需覆盖并发（2~8），不必再调大。

### 10.7 一批"确定性"优化（不改变行为，无需跑局验证）

段数上来以后（`meshSec` 1900 → 5500），`rebuildWorldGeometry` 里有四处大容器是**每帧新建**的，
每帧一次 malloc/free 再加一次首次触碰的缺页；arena 提交则是**每容器一次分配**（一段最多 14 次）。
这些都不涉及语义，直接改掉：

| 改动 | 位置 | 说明 |
|---|---|---|
| `pendingUploads` / `sectionsNeedingBuild` / `orderedSections` / `instances` 改 `static` + `clear()` | `WorldGeometry.cpp` | 容量跨帧保留。`instances` 在 5500 段时是 5500×64B ≈ 352KB，原来每帧 malloc/free 一次 |
| `captureArenaCommit` 一次分配 + 切片 | `Geometry.cpp` | 原先 14 个容器各自 `allocate`（14 次锁内推进、读时 14 段互不相邻的内存）；改成先算总长（逐段 64B 对齐）→ 一次 `allocate` → 依次 `memcpy`。分配次数 14 → 1，整段连续，build/staging 缓存局部性更好 |
| arena 块 4MB → 8MB | `Geometry.cpp` | 最大段（约 5 万顶点 + motion + 32B packed）会超过 4MB 而落到 bigChunk，反复 `new/delete`（实测 374 次）；8MB 覆盖后 bigChunk 只作极端兜底 |
| `g_captureArenaChunks` 改为"当前常驻块数" | `Geometry.cpp` | 原先是累计分配次数，含 bigChunk 的反复分配，读日志时会被误导 |

`rebuildWorldGeometry` 只在渲染线程调用（已有注释约定），`static` 复用不需要加锁；
四个容器都在函数内声明处 `clear()`，early return 不会残留上一帧数据。

这些改动对行为是等价的，跑局时只应看到 `geometry` 小幅下降（省掉每帧堆分配与缺页），
`arenaChunks` 的数值口径也变成"常驻块数"。
