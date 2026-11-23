# 交接：CPU / 线程 / 几何管线优化（2026-10-22）

> 档位：**生产档**（材质在命中着色器里求值；延迟档已作废）。
> 状态：7 条改动已落地、编译、部署；其中 **5 条有实测支撑**（索引化地形的同场景 A/B 已完成，见 §2.2）；
> 「forceReset 分批捕获」**已实施**（2026-10-22 12:55，见 §4.6；仍缺一次进世界实测）。
> 上一份相关文档：`HANDOFF-2026-10-21-VERTEX-32B-TERRAIN.md`、`HANDOFF-2026-10-21-INDEXED-TERRAIN-TODO.md`。

---

## 0. 产物指纹

| 项 | 值 |
|---|---|
| DLL | `822EB824D937559A57C89986D0137EB4`，2,609,486 B 源码编译产物，六处一致 |
| SPV | 8 个（索引化地形版本）：rchit ×2、rahit ×2、rgen、ser.rgen、sharc_update.rgen、pomdeferred.rchit |
| 源码 | `cpp/VulkanCore.cpp` 2,609,486 B / 58,900 行；花括号 5474/5474 |
| 回退包 | `.bak_pre_chunklock_20261022`、`.bak_pre_indexed_20261022`、`.bak_pre_entityhash_20261022`、`.bak_pre_capturedirty_20261022`（各六处 DLL） |
| 损坏留档 | `.tmp/broken_20261022/VulkanCore.cpp.truncated`（见 §3） |

---

## 1. 改动清单（按落地顺序，锚点为最终文件行号）

| # | 条目 | 问题（实测依据） | 改动 | 锚点 |
|---|---|---|---|---|
| 1 | 尾部无限 fence | `vkWaitForFences(UINT64_MAX)` 等上一批 build；历史 STALL 485ms | 改零超时 `vkGetFenceStatus`；未信号才给 2ms 有界等待 + 一次性诊断，最后才退回旧行为 | `sectionBuildFenceWait` 尾部块；入口闸门 |
| 2 | TLAS 容量地板 | 实例数在 730~772 抖动，反复越界换 TLAS 句柄 ⇒ `vkQueueWaitIdle` 排空 5~55ms | 容量 = `max(需求×1.5+4096, 上次×2)`，向上取 64 | ~`TLAS_CAPACITY_SLACK_MIN = 4096u` |
| 3 | 32B 预打包搬出渲染线程 | `staging=` 200 慢帧样本均值 2.97ms / 峰值 72.61ms | `SectionGeometrySplit` 增 4 个 packed 缓存；捕获线程**锁外**打包；staging 段只剩 memcpy，带 `size()` 校验 | `packSectionSplitBucketsLocked` :15019、`pendingPack` :15318+、staging 快路 |
| 4 | 锁改为有界公平 | `pre` 尖峰 82.7ms 全在旧兜底 `g_chunkMutex.lock()`（自旋 3ms 抢不到 → 无界等） | `std::mutex` → `std::timed_mutex`；入口 `try_lock_for(3ms)`；兜底 `try_lock_for(12ms)`，再失败就 `return false` 让位（skip 计数不清零） | `g_chunkMutex` :14219、`SECTION_PRELOOP_LOCK_FALLBACK_MS` :64185 |
| 5 | 捕获唤醒上限 | `capHoldMs=8.2`（单次唤醒霸占锁 8ms） | `GEOMETRY_CAPTURE_MAX_PER_WAKE` 8 → 4 | :14905 |
| 6 | 实体/玩家判等 | `endEntityFrame` 每帧在锁内对两份载荷各做一次全量 `memcmp` | 内容哈希（8B/步）+ 缓存（唯一写入者即该函数自己）；O(1) 比较 | `hashVertexPayload` :14674、`endEntityFrame` :55395 |
| 7 | 索引化地形（原清单第 7 条） | 每 quad 6 顶点展开；索引化单独打开是**零收益**（4×48B = 6×32B） | 索引化与 32B **同时**成立（128B/quad）；唯一判据 `sectionUsesIndexedGeometry`；表项 `.w` 最低位作"索引布局"标志（33）；4 个 TU 按图元号算术推导三个角，**无需新增描述符** | :14169、:19304、`writeSectionVertexAddressSlot`、`material_resolve.glsl` / `material_resolve_glass.glsl` / `rt_test(. _translucent).rahit` |

**索引映射（必须与 `sectionQuadIndexAddress()` 的图案逐项一致）**

```
三角形 t（= gl_PrimitiveID），q = t >> 1：
  t 偶 ⇒ 4q+0, 4q+1, 4q+2      （对应展开布局 v0,v1,v2）
  t 奇 ⇒ 4q+0, 4q+2, 4q+3      （对应展开布局 v0,v2,v3）
```

shader 侧先取 `stride & ~1u` 作真实步长，再按最低位决定走哪条映射；否则 33 会被
`loadSectionVertex` 当成"非 32B"按 48B 读。

---

## 2. 实测与结论

### 2.1 成立

| 项 | 读数 | 判读 |
|---|---|---|
| 第 4 条 | 12:26 局两条慢帧 `pre=15.22 / 13.74` | 正好 = 3ms 首轮 + 12ms 兜底上限；旧代码这两帧是 80ms 级 |
| 第 5 条 | `capHoldMs 8.2 → 4.9`、`upHoldMs 0.7 → 0.3` | 单次持锁砍半 |
| 第 1 条 | 两局 `sectionBuildFenceWait` = 0、`worldGeometryQueueIdle` = 0 | 尾部等待与 TLAS 排空都不再触发 |
| 第 6 条 | 无 STALL、`asRetireN` 正常 | 日志侧无法直接验证，只剩画面判据（实体是否跟手） |

### 2.2 已验证：第 7 条索引化地形（A/B，2026-10-22）

做法：同一份源码，只改 `SECTION_INDEXED_TERRAIN_ENABLED`，各编一个 DLL（无需重编 SPV ——
着色器按表项 `.w` 最低位在运行时分辨两种布局），同一存档各跑一局、站定等 `dirty=0 queued=0`。

| 轮次 | meshSec | meshMB | rawMB | **meshMB / rawMB** | 折算 B/quad | asTrueMB/blas |
|---|---:|---:|---:|---:|---:|---:|
| ON（索引 + 32B） | 537 | 183.9 | 80.8 | **2.276** | 255 | 0.433 |
| OFF（展开 + 32B） | 268 | 97.2 | 37.3 | **2.606** | 292 | 0.432 |

两点结论：

1. **索引化确认生效**：每 quad 网格字节 292 → 255（**−12.7%**）。按字节模型反解
   （OFF 每 quad 192B = 6×32B，ON 每 quad 128B = 4×32B）⇒ **地形部分正好 −33%**，
   地形约占 mesh 总量 **38%**，其余为实体/粒子/LOD（不受这条影响）。
2. `asTrueMB` 每 BLAS **不变**（0.433 → 0.432）—— 符合预期：三角形数不变，只有顶点缓冲变小。
   GPU `rt` 21.58（ON）vs 21.81（OFF）在噪声内 ⇒ **这条的收益是显存/带宽，不是帧时间**。

**归一化方法（必须遵守）**：跨局比较只能用 **`meshMB / rawMB`**。
`rawMB` 是 Java 上报的 28B 原始载荷（= quad 数 × 112B），与顶点布局、与"每段多少 quad"都无关。
`meshMB / meshSec` **不可用**：它受每段 quad 数影响（本 A/B 两轮段数 537 vs 268，
单看 per-section 会得出 "OFF 只比 ON 高 5.9%" 的错误结论）。

**口径说明（对外表述）**：`meshMB` 含非地形层 ⇒ 本场景**整体降幅 ≈ 12~13%**。
"−33%" 只能用于"地形顶点字节"这一项，不能说成"网格显存降 1/3"。
受益随地形占比缩放：纯原版地形、实体少的场景接近 −30%；实体/粒子/LOD 重的场景会被稀释。
满载 Broville 那种场景的绝对收益仍是 −45MB 量级。

### 2.3 重要更正：`pCap` 的口径

`pCap = rtxSegMs(perfL10RebuildStart, rtxPreLoopMarkLog)` —— **从函数入口到捕获段结束**，
即「取锁等待 + 锁内捕获/分类工作 + `updateVramGovernor()`」，不是纯等待。
10-21 与 10-22 早间注释里"整帧尖峰全部是取锁等待"的结论**是错的**（当时只看了 `pre ≈ pCap`）。

**本行原判读（已作废）**：原文写"12:26 局 frame=31 `pre=91.46 / pCap=91.46` 而构建循环只有
`sections=1`，同期 counters `forceResets=1` ⇒ 那是 `forceReset` 的锁内全表捕获"。
**这条推理不成立**：`sections=` 是 `sectionUpdates.size()`（:68636），而 forceReset 分支自己的日志
在同一次进世界的每一局都报 `synchronous full rebuild of 1 section(s)` ⇒ 重置当帧只捕获了 **1 段**，
一段的分类拷贝不可能是 91ms（一段实测 0.1~6ms）；`forceResets=1` 只说明窗口内发生过重置，
不能证明那条 SLOW 行是重置帧。

**真因（2026-10-22 12:55 局直接证据）**：`[CPP] 12:55:40 [TEX SCRATCH] releasing resident upload
staging MB=768.06` 与 `[RT GEOMETRY SLOW] frame=30 total=99.86 pre=99.43 pCap=99.43 sections=1`
紧邻、同一次 `updateVramGovernor()` 之内。该函数在 `:65022` 被调用、位于
`rtxPreLoopMarkLog`（`:65030`）之前 ⇒ 完全落在 `pCap` 窗口里；其
`releaseHugeTextureUploadStaging()`（`:10337`）会先 `vkWaitForFences(纹理上传 fence, UINT64_MAX)`
再 `destroyTextureUploadStaging()`（vkbuffer 销毁 + `vkFreeMemory`）。
旧 DLL 三局同形：重置行后面紧跟 `[TEX SCRATCH] … MB=1536.06`，随后 `pre=122.00 / 133.35 / 148.03`。
⇒ 世界加载那一帧的 ~100~150ms 来自**一次性归还 768MB/1536MB 纹理上传常驻暂存**，
与 forceReset 捕获无关。§4 的分批捕获仍然正确（重置帧成本从 O(顶点字节) 降到 O(键数)），
但它**不可能**把这条 preLoop 尖峰拿掉 —— 见 §6 第 1 条。

---

## 3. 事故记录（必须知道）

今天做第 7 条时 `cpp/VulkanCore.cpp` 被**截断**：2,578,781 → 2,189,799 B（尾部约 390KB 丢失，
含 `recordFrameCommand` 后半与整段 `rebuildWorldGeometry`），编译报 `C1075 { 未找到匹配令牌`。
磁盘尚余 29.4GB，**不是空间问题；触发原因未定位**。

恢复方式：整文件从 `.tmp/before/VulkanCore.cpp`（2,577,722 B，10-21 22:18:55）恢复，
再逐条重放第 1/2/3 条改动，校验花括号配平 + 尾部符号 + 编译通过。
损坏文件留档 `.tmp/broken_20261022/VulkanCore.cpp.truncated`。

教训：**改这种单文件 2.6MB 的工程，每次改动后立刻 `cmake --build` 并检查文件大小/花括号**，
不要连着做五六处替换再编译。

---

## 4. forceReset 分批捕获（**已实施**，2026-10-22 12:55；实测见 §4.6）

### 4.1 病灶（**成立，但不是那次 91ms 的成因**，见 §2.3）

`forceReset` 分支在 `g_chunkMutex` 内遍历 `g_chunkMeshes` 全表，对每段做
`geometryCaptureSectionLocked`（O(顶点字节) 分类拷贝）。722 段 × 0.3MB 量级时它是
**单帧最大的锁内工作量**，应当去掉；但 12:26 局那条 `pre=91.46` 已查明是纹理暂存释放
（§2.3），不是这段捕获。

### 4.2 实施前必须读的三段（不读完不落笔）

| 段 | 位置 | 要确认 |
|---|---|---|
| reset 全量捕获循环 | `cpp/VulkanCore.cpp:64462` 起 | 是否**只**做分类捕获；有没有顺带填 `ommInput` 之类"必须同帧就位"的依赖 |
| 循环后的脏集处理 | `cpp/VulkanCore.cpp:64839-64872` | `forceReset` 时 `g_dirtySectionKeys.clear()` 之后谁负责把工作重新排上；`capturedPending` 的发布 |
| 构建循环的 reset 分支 | `cpp/VulkanCore.cpp:65107`、`:65293` | `!forceReset` 的预算守卫在 reset 帧被跳过 ⇒ `sectionUpdates` 变空后哪些分支会失去隐含前提 |

### 4.3 方案

```
forceReset 时：
  1) 仍执行 g_sectionBLAS 的全量 retire + clear（现状）
  2) 清空 g_capturedSections（现状，需确认时序在 worker 早退门之前）
  3) 不再同步捕获，改为：g_dirtySectionKeys = 全部 spatial 键（从 g_chunkMeshes 键去层位）
  4) 发布 g_worldGeometryDirty；本帧 sectionUpdates 为空，不建 BLAS
  5) 后续帧由捕获线程按"最近优先"分批搬（4 块/唤醒、12ms 预算）
```

### 4.4 判据

- `[RT GEOMETRY] counters` 的 `preLoopMs` 从 91.5 降到 ≤12；
- 加载后 `captured` 持续增长（不是停在 0）；
- 首屏表现为"最近的区块先出现、其余几十帧内补齐"——这是预期，不是 bug；
- 不得出现 `queued` 长期为 0 而屏幕仍空（那说明脏键没排上）。

### 4.5 不变量（违反即回退）

1. OMM 输入与顶点必须同批就位（`g_cutoutOmmInputs` 按 layer 键）；
2. 退役槽 `g_sectionVertexAddressFreeSlots` 只在 fence 信号后回收；
3. `g_worldGeometryDirty` 必须在释放 `g_chunkMutex` 前发布；
4. worker 的 `!g_forceSectionASReset` 早退门与 `g_forceSectionASReset = false` 的时序不能倒。

### 4.6 实施记录（2026-10-22 12:55）

| 项 | 内容 |
|---|---|
| 改动 1 | `rebuildWorldGeometry` 的 `forceReset` 分支（`cpp/VulkanCore.cpp:64580-64645`）删掉锁内全表 `geometryCaptureSectionLocked`，改为：清队列 → 遍历 `g_chunkMeshes` 把全部 spatial 键（去层位、去重）标脏 → `sectionUpdates` 保持为空 |
| 改动 2 | 脏集清理（`:64956-64965`）去掉 `if(forceReset) g_dirtySectionKeys.clear()`，两种情形合并为"只摘本批处理过的键"（reset 帧天然空操作） |
| 未改 | retire + `g_sectionBLAS.clear()` 仍在原处（`:65225` 起）照旧执行；`g_forceSectionASReset = false` 与 `g_worldGeometryDirty` 发布位置未动 |
| 为什么能继续走完整条尾部 | 本帧 `sectionUpdates` 为空 ⇒ 构建循环空转；`orderedSections` 为空早有专用分支（`:66790` "Keep Binding 0 valid while no world Section is loaded"），TLAS 退化成 mask=0 哑实例，因此**照旧拿到一次 fence 提交**，退役槽回收时序不变（不新增回收点） |
| 新日志 | `[RT GEOMETRY] force reset: deferred capture of N new section key(s) queued (dirtyTotal=M), BLAS retired this frame, frame=K` |
| 回退 | 按注释里的"【回退】"两处还原（标脏循环 ↔ 逐键捕获；erase 循环 ↔ reset 时 clear） |
| 产物 | 我这轮编出的是 `D89D1AAEFFAD7BB2D93691CB4A4590EB`（5,788,160 B）；**随后被并发的"查找优化"会话重编覆盖** —— 现七处统一为 `45FE2739AB089F83BEF6B433C70A05EF`（12:52:31，含 `opts=`/`nbrN`/`nbrUs` 埋点）。12:55 与 13:01 两局跑的就是后者，日志里出现 `deferred capture of` ⇒ §4 改动确实在运行的那份里。SPV 无改动 |
| ⚠ 并发编辑 | 2026-10-22 下午有另一个会话在**同一份** `cpp/VulkanCore.cpp` / 同一 handoff 上工作（新增 `opts=`/`nbrN`/`nbrUs`、重建 DLL）。行号随时会漂：改动前必须重新 grep 定位，不要用本文件里的绝对行号当锚点 |
| 补齐速率 | 由既有节流决定：每批 ≤24 段（`SECTION_REBUILD_MAX_SECTIONS_PER_BATCH`），批间隔 6 帧（`SECTION_REBUILD_MIN_FRAME_INTERVAL`，被打断时按 `CONTINUE_FRAMES=3` 续），722 段 ≈ 30 批；实体/玩家每帧动会走 `g_entityRebuildRequested` 快路，实际逐帧补齐 |

**实测（2026-10-22 12:55 局，`run/rt_debug.log` 第 277759~279214 行）**

| 判据 | 结果 |
|---|---|
| 重置帧成本 | `force reset: deferred capture of 0 new section key(s) queued (dirtyTotal=1)` —— 该局重置发生在 frame=1，此时 `g_chunkMeshes` 还是**空的**（Java 侧区块在重置之后才上传），所以**没有可延后的键**；frame=1 未进 SLOW（<15ms） |
| `preLoopMs` 91.5 → ≤12 | **未达成，但与本次改动无关**：本局 `preLoopMs=99.4` 来自 frame=30 的 `[TEX SCRATCH]` 768MB 释放（§2.3）。扣除那一帧后窗口内后续为 2.6 / 4.9 / 6.2 |
| `captured` 持续增长 | 达成：captured 787 → 954 → 1319 → 2372，adopted 712→2212，`queued` 稳态 0 |
| 稳定性 | 3601 帧、零 failed/exception/timeout、forceResets=1、asRetireN 正常，退出正常 |

**遗留：延后捕获这条路径在本次跑测里没有被真正压到**（重置时 `g_chunkMeshes` 为空）。
要压到它需要一个"重置标志被消费时区块已经铺满"的时机（换维度 / 重进存档 / 加载慢到
上传先于首帧重建）。届时判据是新增那行日志的 `N` 应 ≈ 段数、且 `preLoopMs` 仍 ≤12。

---

## 5. 构建 / 部署 / 回退

**native**

```
cmake --build cpp/build --config Release --target rtx_backend
# 产物：cpp/build/Release/rtx_backend.dll（约 40 秒）
```

六处部署：`cpp/build/Release`、根目录、`run/`、`src/main/resources/`、
`src/main/resources/natives/windows/`、`build/resources/main/`、`build/resources/main/natives/windows/`。
（`run/` 在客户端运行时被占用，必须先关游戏。）

**SPV**

```
python _deploy_stale_spv.py        # 只重编"源比 SPV 新"的，自动部署到 run/shaders
# 再把变化的 8 个拷到 src/main/resources/shaders、build/resources/main/shaders
# 编译口径：-DRT_BUILD_PRODUCTION_MODE -DSECTION_VERTEX_32B=1 --target-env vulkan1.2
```

**回退**：按 §0 的 `.bak_pre_*` 覆盖对应六处 DLL；SPV 回退 = 回退 4 个 TU 的源码改动后重编
（脚本不留 SPV 备份）。

---

## 6. 已知未做项（按价值排序，供后续接手）

1. **纹理上传暂存的阻塞式归还**（新增，§2.3 查明）—— 世界加载那一帧唯一的百毫秒级单帧：
   `updateVramGovernor()`（`:65022`）→ `releaseHugeTextureUploadStaging()`（`:10337`）里
   `vkWaitForFences(纹理上传 fence, UINT64_MAX)` + `vkFreeMemory`，实测 768MB→99ms、
   1536MB→122~148ms，且整段落在 `pCap` 窗口内。待做：先把 wait 与 free 分别计时（现在还分不开
   是等 fence 还是 `vkFreeMemory` 慢），再决定是"改成登记制、由 fence 回调释放"还是
   "把 tick 移出 preLoop 窗口"。**不要先挪位置** —— 挪走只是把 99ms 换到另一处。
2. ~~forceReset 分批捕获~~ —— **已完成（§4.6）**：重置帧不再做 O(顶点字节) 捕获；但"重置时可延后"
   这条路径在 12:55 局没被压到（重置时区块尚未上传），仍需一次换维度/重进存档的实测；
3. ~~第 7 条同场景 A/B~~ —— **已完成（§2.2）**：地形顶点字节 −33%、整体网格 −12.7%、`asTrueMB` 不变；结论**保留 ON**。后续若要再省，方向是压缩非地形层（实体/粒子/LOD 占 mesh 的 62%）；
4. `g_chunkMeshes` 改 copy-on-write（`shared_ptr<const vector<Vertex>>`）——这是"捕获真正能多线程 / 锁外拷贝"的前提；目前加线程只会增加锁争用（实测 `capHold 4.9ms`、锁是瓶颈）；
5. 主线程任务泵（Java 侧 `BlockableEventLoop.runAllTasks`，`FrameBudget` 已埋点但从未取到数据：dev 启动脚本不写 Java 日志）；
6. 实体/粒子捕获的每帧分配（`new Matrix4f`×2、每 quad 装箱）——量级未测，不建议盲改。

---

## 7. 显存侧（2026-10-22 13:2x 落地，待实测）

### 7.1 病灶（13:01 局实测）

1. **`over` 永久成立**：`ratio=0.858~0.888` 一直 > 高水位 0.72，`over=1` 全程；而
   `[RT MEM]` 同一时刻 `vramUsedMB=6070 / vkLiveDeviceMB=2602.8` ⇒ **3.4GB 不是我们的**。
   `heapUsage` 是整堆用量（含其它进程），本文件 `:17825` 那条崩溃注释早已记过这件事。
2. **越线把留用池清零 → 纯抖动**：`keepCapMB=0` 期间 11 秒内
   `slabAllocMB 14962.5→18795.6`（+3833MB）、`slabFreeMB 14042.1→18002.1`（+3960MB），
   复用率 0；`poolMiss=90~97 次/600 调用`、`allocMs=160~250ms/窗口`（单次峰值 3~19ms）。
   `freedMB=1023.8` 之后就再没动过 —— 归还换不来 `over` 归零。
3. **驱逐/补传迟滞方向相反（可证）**：补传用未收缩的 `vramEvictKeepRadius()`=96，
   而驱逐在 over 时是 `96×0.75×1.25`=**90** ⇒ 90~96 格那一圈的段必然
   「驱逐 → 补传 → 再驱逐」。实测 `secEvicted 31→208`、`secRehydrated 28→34`（36 秒）。

### 7.2 改动

| # | 改动 | 锚点 |
|---|---|---|
| 1 | 补传半径改用**与驱逐同源**的收缩半径（`vramEvictKeepRadiusShrunk()`）；over 时补传 <72、驱逐 >90，中间 18 格是真迟滞带。不引入新阈值 | `vramEvictKeepRadiusShrunk` 定义于 `vramEvictKeepRadius` 之后；调用点：补传入口、驱逐段 |
| 2 | over 时**不再清零**留用池，改降到 `SECTION_MEMORY_POOL_RESTORED_*`（256MB / 4 槽）：`sectionMemoryPoolEffectiveKeepBytes/Slabs` | 同上函数体内 |
| 3 | `[VRAM GOV]` 增 `ourMB`、`overageMB`、`hystR`、`evictR` 四个数（纯日志） | `logVramGovernor` |
| 4 | `VRAM_EVICT_HYSTERESIS` / `VRAM_EVICT_SHRINK_RADIUS` 上移到闸门配置段（三处共用一份） | `VRAM_GOVERNOR_SAMPLE_TICKS` 之后 |

**A/B 开关**：`ORION_VRAM_POOL_UNDER_OVER=0` 退回"越线即清零"；驱逐/补传整段仍可用
`ORION_VRAM_GOVERNOR=0|1` 关掉（0/1 时 mode<2，第 1 条不生效）。

**回退**：单文件三处替换，见上表锚点；或直接回退 DLL。

### 7.3 产物

DLL MD5 `2889C7EAFC2DF81EA4592D3EFEB2F33A`，七处一致（`cpp/build/Release`、根目录、`run/`、
`src/main/resources/`、`src/main/resources/natives/windows/`、`build/resources/main/`、
`build/resources/main/natives/windows/`）；SPV 无改动。源码花括号 5511/5511。

### 7.4 判据（下一局）

- `[VRAM GOV]`：`keepCapMB=256`、`slabKeptMB` 从 0 涨到 64~256MB；`hystR=72 evictR=90`（over 时）；
- `counters` 的 `poolMiss` / `allocCalls` 从 ~90~97/600 调用掉到个位数，`allocMs` 从 160~250ms 掉到近 0；
- `[RT MEM]` 的 `slabAllocMB`/`slabFreeMB` 斜率从 ~350MB/s 摊平；
- `secRehydrated` 增长放慢（`secEvicted` 不变 —— 驱逐策略本身没动，`nonResident` 可能更大，
  这是"少抖动"的代价，不是 bug）；
- **不期望**：`over` 就此归零 —— 3.4GB 外部缺口不是我们能填的（见 7.1 第 1 条）。

### 7.5 实测（2026-10-22 13:15 局，新 DLL `2889C7EA`）

| 判据 | 改前（13:01 局） | 改后（13:15 局） |
|---|---|---|
| `keepCapMB` | 0 | **256.0** |
| `slabKeptMB` | 0 | **95.0 / 127.0 / 238.9 / 254.8**（4 个采样） |
| `poolMiss` / `allocCalls`（每 600 调用） | 90~97 | **3 / 4 / 7** |
| `allocMs`（每窗口） | 160~250ms | **1.6 / 2.3 / 34.6ms** |
| `slabAllocMB/slabFreeMB` 斜率 | +3833 / +3960MB 每 11s（≈350MB/s） | **+398 / +537MB 每 26s（≈15MB/s）** |
| `secRehydrated`（≈36s 内） | 28~34 | **0**（抖动消失） |
| `secEvicted` | 31→208 | 16→280（策略未动，仍持续放） |
| `hystR / evictR` | 96 / 90（**颠倒**） | **72 / 90** |
| `over` / `ratio` | 1 / 0.858~0.888 | 1 / 0.866~0.903（**未变，符合预期**） |
| `ourMB / overageMB` | 无此读数 | **2398~2714 / 995~1239** ⇒ 缺口里我们只占一部分，且**可动的只有几百 MB 几何** |
| `RT PHASE SLOW` 占比 | 47 次 / ≈5600 帧（0.84%） | **8 次 / 2823 帧（0.28%）** |
| 全段 `PERF L10 STALL` | 2 条（textureUploadQueueIdle 63~66ms） | **0 条** |

结论：留用池保住了（复用率从 0 变成 4453/4460 命中），抖动斜率降了一个数量级，补传抖动归零；
`over` 如预期没归零（`overEnters=1`，全程没退出过），证实缺口主要在外部。

**残余尖峰（下一步的靶子）**：仍全部落在 `geometry`，但形态变了 —— 不再是"多段叠加"，
而是**单次 `vkAllocateMemory` 在 0.9 压力下自己就要 30ms**：

```
frame=2293 [RT GEOMETRY SLOW] bld=35.46  sections=12   ← 构建循环
同窗口      counters: maxSectionMs=0.3  poolMiss=3  allocMs=34.6
```

即 `allocMs(34.6) ≈ bld(35.46)`：这 35ms 是一次新尺寸的 slab 分配（池里没有该档空闲块时会真分配）。
这与本文件 `:10327` 记的旧结论一致（压力下"分配 0.2MB 也要 41ms"）。
13:15 局最差帧 2341 = 45.67ms（geometry），2281 = 26.56ms。
