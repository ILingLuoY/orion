# Orion M3 实施规格 — DH 分帧派发（P2）+ Rust 批量内核 + P3 makeRoom 修复

- 基线仓库：`D:\fabric-example-mod-26.2`（mod 子目录 `fabric-example-mod-26.2\`），本文仅提取规格，未改动任何仓库文件。
- 诊断源：`docs/dh-bridge-diagnosis-2026-11-17.md`（P2 :22-26、P3 :28-30）与 `HANDOFF-2026-11-17-DH-OPT-AND-RUST.md` §3/§5/§7。
- 行号核验（2026-11-19）：诊断文档之后 C++ 侧有 +17 行平移（OrionInternal.h 302→**319**；Frame.cpp 1961/2014→**1975/2028**；WorldGeometry.cpp:3204-3215→**3210** 附近）；Java 侧无漂移（DhFarFieldUploader :202/:214/:282、DhLodMeshSink :214/:294 均与诊断一致）。Lod.cpp 的 `g_chunkMutex` 在 **:489**，`[TLAS SKIP]` 日志在 WorldGeometry.cpp:**:4376**。

---

## 1. dispatch() 的精确当前行为（一手代码核验）

**触发链**：`DhBridge.tick()`（渲染线程，DhBridge.java:112→131）→ `DhFarFieldUploader.tick()` → `tickInner()`，每帧一次。总闸 `-Dorion.dh.lodMesh`（DhFlags.java:29，默认 **false**）。

**同步触发条件**（tickInner :140-156）：
- `due` = 首次 || `frame - lastSyncFrame >= LOD_REFRESH_FRAMES`（DhFlags.java:35，默认 **120 帧**）；
- `movedEnough` = 与上次同步点切比雪夫距离 ≥ `LOD_MOVE_THRESHOLD`（DhFlags.java:39，默认 **32 格**）；
- `needData` = 场上尚无网格（`uploadedQuads < 0`）。
- 任一成立才进入；随后 `if (JOB_IN_FLIGHT.get()) return;`（:149）——在飞期间**整帧跳过且不补触发**（lastSyncFrame/lastSyncX/Z 已在派发前置位 :153-156）。
- `enabledCount()==0` 时只 `prune()` 后返回（:158-161）。

**差集计算**（sync :163-193）：`prune()` → `renderCut()`（已捕获 ∩ DH 启用清单 − 内洞）→ 版本号 `serial*2+textureBit` 与 `UPLOADED` 比对 ⇒ `toUpload`（**全部变更节，无节数/字节上限**）与 `toRemove`；无变更则 `skippedUnchanged++` 返回。

**dispatch（:202-244）**：`JOB_IN_FLIGHT.compareAndSet(false,true)` 后把**整单**丢给单线程 worker（`DhLodFarField`，daemon）。worker 循环：
1. 逐节 `convert(section, textured)`（:214，Java 两趟转换）；
2. 逐节 `NativeVulkan.uploadDhLodSection(...)` JNI（空节传 EMPTY_BYTES）；
3. 全部上传后逐节 `removeDhLodSection`；
4. 每步检查 `GENERATION` 变化即中止（换世界作废在飞单）；`finally` 清 `JOB_IN_FLIGHT`（:242）。

**convert（:282-341）**：DH 16B/顶点 ×4 顶点 = 64B/quad → 我方 16B/顶点布局。两趟（先数有效 quad 再填充）；x/z u16 > blockWidth+1 的 quad 过滤；`textureEnabled==0 ⇒ tileId=0`；normalIndex 取字节 13 低 4 位；light 字节 13 原样拷贝；material 字节 12→14；字节 15 恒 0。

**与 C++ 24 节/帧的配合（失配点）**：
- C++ 每帧至多一次 `rebuildWorldGeometry(SECTION_REBUILD_MAX_SECTIONS_PER_BATCH=24)`（OrionInternal.h:**319**；Frame.cpp:**1975** RT-DI 动态路径 / **2028** 常规路径；常规路径另有 QUIET_FRAMES=10 / MAX_DEFERRAL_FRAMES=60 延迟窗，:316-318）。
- batchCap = `min(maxSectionUpdates, effectiveSectionRebuildBatchCap())`（WorldGeometry.cpp:283-284；后者 = 环境变量覆盖或默认 `g_sectionRebuildBatchCap=24`，Perf.cpp:198-208、State.cpp:106），cursor 模式 FIFO 只取 `min(available, batchCap)`，余量留队下一帧（:292-316，回流 :330-337）。
- Java 一次派发数百节 ⇒ 每节 JNI 内各自拿 `g_chunkMutex`（Lod.cpp:489）+ `markWorldGeometryDirty()`（:509）⇒ C++ 以 24 节/帧追赶，拖多帧 BLAS churn；期间实例集每帧都在变，`[TLAS SKIP]`（WorldGeometry.cpp:4376，"实例集未变则跳过"）持续失效。
- 注意 24 节/帧是**全局共享预算**：同一 batchCap 还要喂近场 capturedBatch 与 dirty keys（WorldGeometry.cpp:339-342），DH 派发率必须低于 24，否则近场活跃期反向积压。

## 2. 分帧派发设计（P2 修法）

**派发上限**：默认 **12 节/帧**，可调 `-Dorion.dh.dispatchSectionsPerFrame`（推荐域 8–16，低于 C++ 24 的消化能力留出近场份额）；叠加字节预算 `-Dorion.dh.dispatchBytesPerFrame`（默认 8MB，防超大节拖帧），双限取先到。

**优先级**：切比雪夫距离升序（与 `DhLodMeshSink.distance` 同语义：AABB 到视图中心）；同距离 `serial` 降序（最新状态先走，避免先传旧版再传新版）。`toRemove` 不参与排序，随切片捎带提交（撤下无转换成本）。

**数据结构**：新 `PENDING = ConcurrentHashMap<Long, Section>`（pos → 最新 Section；DH 重构建换新对象时以新替旧，天然去重）。每帧 `tickInner`：
- `JOB_IN_FLIGHT` 仍 true → return（背压留在 PENDING，变更照常入队，不再丢失触发机会）；
- 否则取 PENDING 快照排序（≤数千条，渲染线程 O(P log P) 可接受；仅在新增入队或玩家移动 ≥8 格时重排，缓存结果），切前 `min(cap, byteBudget)` 节 + 全部 toRemove 成一帧切片；
- `sync()` 只做差集并入队，不再整单派发。`reset()/release()` 须同步 `PENDING.clear()`（防换世界残留）。

**JOB_IN_FLIGHT 语义不变**（同时只一单在飞），但单的内容从「全部积压」缩为「本帧切片」——这是与现状的唯一行为差。

**C++ 配对项（帧批合并 JNI）**：新增 `uploadDhLodSectionsBatch(long[] keys, long[] versions, int[] originX/Y/Z, int[] widths, int[] lens, byte[] blob)`：字节拷贝 + `appendDhLodQuads` 展开（Lod.cpp:475-486 同款）在**锁外**做，`g_chunkMutex` 单次锁内提交全部映射更新 + dirty + revision（Lod.cpp:489-509 同款一次）——把 N 次锁合并为 1 次，削渲染线程 `try_lock_for` 与 2ms 栅栏等待命中（诊断 P2 证据 :25）。

## 3. Rust 批量内核 off_convert_sections 的 ABI

**决策：N 节一次调用（批量），不采用每节循环调用。** 依据：(a) 与 P2「一次 g_chunkMutex 提交一批」形态合流（HANDOFF §7 M3 既定）；(b) Java 侧切片本来就要装填连续 blob 供批量 JNI 用，批内核免 N 次 FFM downcall 与 N 次缓冲装换；(c) 离线 A/B 已证单节内核收益有限（1.15–1.43×，HANDOFF §7），生产收益主要在调用形态。

**布局（调用方分配、Rust 零分配零锁、与批量 JNI 共用）**：
- `src_blob`：切片各节 DH 64B/quad 流连续拼接（Java 侧 direct ByteBuffer 池装填，memcpy 级）；
- `spans`：`#[repr(C)] OffSpan { src_off: u32, src_len: u32, block_width: u32, texture_enabled: u32 }`（16B/节）× N，第二个 direct buffer；
- dst 与 src **1:1 同偏移**：过滤只减不增 ⇒ 每节输出 ≤ 输入，dst_cap = src_len 恒足，无需独立 dst 布局；
- 签名：`unsafe extern "C" fn off_convert_sections(src: *const u8, blob_len: u32, spans: *const OffSpan, span_count: u32, written: *mut u32) -> u32`；返回成功条数；`written[i]` = 第 i 节输出字节数，失败条目回填 `ERR_PANIC=0xFFFFFFFF` / `ERR_CAP=0xFFFFFFFE`（错误码沿用 lib.rs:33-34）。

**纪律沿用**（lib.rs:11-19 FFI 纪律）：`catch_unwind` **逐条**包（一条坏节不拖全批）；全 32 位标量 + 指针；输入只读；`#[repr(C)]` 冻结。**ABI 版本保持 3**（纯增量导出，RustBridge.java:37 `EXPECTED_ABI=3` 不动）；旧 DLL 无此符号 ⇒ `lib.find("off_convert_sections")` 失败即软回退逐节 `off_convert_dh_section`（句柄已备，RustBridge.java:88-93、包装 :189-204）。

**每节转换语义冻结不变**（tileId 门 / normalIndex 低 4 位 / light 原样 / material / x-z 超限过滤 / textureEnabled=0⇒tileId=0，lib.rs:109-155 已镜像并对拍 40/40）。批量内核新增对拍用例：N 节组合（含 0 节、含空节、含全过滤节、ERR 注入），判据逐位一致。

## 4. Java convert() 调用点改造

- `convert(:282)` 为 private static，**唯一调用点**是 dispatch worker 循环 :214（全仓引用面核验：DhBridge / NativeVulkan / RustBridge 均不直接调它）。
- worker 循环改三段：①切片 src 装入 blob（+spans 表）；②`RustBridge.convertDhSections(blob, spans, written, n)`（新包装，仿 :190-204 模式，direct ByteBuffer 段下行）；③`uploadDhLodSectionsBatch` 提交（空节跳过，保持 Lod.cpp:501-504 的 erase 语义）。
- **回退面全保留**：`-Dorion.dh.rustConvert=false` 或 Rust 加载失败/对拍失败 ⇒ 原路径（Java convert + 逐节 `uploadDhLodSection`）。首切片生产对拍：Rust 批输出与 Java 逐节输出逐位比对，不一致 ⇒ 永久回退 + `[RUST-DH] MISMATCH` 日志（HANDOFF §7 M3 第 3 条既定）。RustBridge 侧版本门已有（:74-77），任何失败永久回退的模式沿用 §8 实装纪律。

## 5. P3 makeRoom O(N²) 的修法

**现状**：`makeRoom`（DhLodMeshSink.java:294-312）= `while(超预算){ 全表 O(N) 线性扫找"最远非在用"victim; 淘汰一个 }` × k 次淘汰 ⇒ O(k·N)，风暴期 k 大即 O(N²)；`prune`（:214-240）超预算时全表收集 + 复合键全排序 O(N log N)。两者仅在超 512MB 预算时触发（:87），但 DH 重构建风暴期恰是超预算期（诊断 P3）。

**修法（数据结构选型）**：**per-call 堆选择**替代重复全表扫：
- `makeRoom`：单趟 O(N) 遍历建大顶堆（键=距离，仅收非在用、非 incoming 节），随后逐 pop 淘汰至预算内 ⇒ O(N + k·log N)。手写 `long[]` 双数组堆（key=dist，val=pos）或 fastutil `LongHeapPriorityQueue`；scratch 数组走 **ThreadLocal**（`onBuilt` 跑在 DH 缓冲构建线程，可能多条，类注释 :109-112 明示）。
- `prune`：同法，复合键堆（非在用先、距离降序），pop 到 `target`（0.85×512MB）即停，替代全排序。
- **不选持久化有序索引**：距离键依赖 volatile 视图中心（`setView` 每帧变）+ live 集每 tick 换（`enabled[]`），持久结构持续失效，重建成本 ≥ 收益；per-call heapify 简单且无失效问题。
- 行为等价性：同距离 victim 次序可能与 CHM 迭代序不同（原实现取迭代中第一个最大者），任选均合法；对拍只锁不变量（只淘汰非在用；被迫动在用必打日志 :240），不锁淘汰序。
- 可选：`enabled[]` 引用未变时复用 live 集+堆；预算 512MB 对 256 视距可下调（诊断 P3 :30 建议）。

## 6. 验收判据

**日志标记（新增）**：
- `[DH-LODMESH] 帧批：派发 k/N 节（待派=… 耗时=…ms）`（每帧或每 30 帧汇总）；
- `[RUST-DH] batch ok: spans=k convMs=…` / `batch fail:…` / `MISMATCH:…`；
- `[DH-SINK] makeRoom scans=N evict=k`（`-Dorion.dh.sinkStats=true` 时开）。
- 沿用现有：`[DH-LODMESH] 增量：上传…`（syncDone :246-253）、`[TLAS SKIP]`（WorldGeometry.cpp:4376）、PERF L10（`ORION_PERF_TIMESTAMPS=1`）、`[RUST-BRIDGE]`。

**A/B 方法（双臂同脚本，参照 §10 测量局模式）**：场景 = 固定种子新世界 + 进世界静止 120 帧 + 定速前进 32 格 + 停 300 帧；输出 `run/rt_client_out_{a,b}.log`。
- A 臂（基线）：现 HEAD 或 `-Dorion.dh.rustConvert=false`；B 臂：分帧派发 + rustConvert=true。
- 指标：①移动窗口 >30ms 帧尖峰计数；②`增量`单批耗时与单批节数（A=一笔全量，B ≤cap/批）；③移动结束→PENDING 清空的追赶时长；④`[TLAS SKIP]` 命中率与 TLAS 重建次数；⑤PERF L10 CPU 段（`rtxGeometryRebuildMs` 已埋，Frame.cpp:1979-1980）。
- **通过线（建议）**：帧尖峰 −50% 以上；`增量`单批 ms ≤5ms；追赶 ≤3s（24 节/帧消化下数百节 ≈ 10–25 帧）；转换对拍 100% 逐位一致；零原生崩溃、零 panic 回退风暴。
- P3 判据：远距传送往返风暴 session 内 makeRoom 扫描总量与 hitch 计数下降；无新增「淘汰启用清单内」日志。
- Rust 侧独立判据：离线 bench（256 节×8192 quad 同数据）批量内核 ≥ 逐节循环；首切片生产对拍逐位一致。

## 7. 环境前提与风险

- DH jar 须在 `run/mods`（HANDOFF §2 部署修正）；`-Dorion.dh.lodMesh=true` 才有 A/B 对象（DhFlags.java:29 默认 false）。
- FFM 启用前须把 `--enable-native-access=ALL-UNNAMED` 进 run config（HANDOFF §10 对账 #2）；CLI 编译显式 `GRADLE_USER_HOME=D:\GTX`；Rust 工具链 `RUSTUP_HOME/CARGO_HOME` 指向 `.toolchains`，构建走 `rust/build_rust.bat`。
- 风险：派发率 > C++ 消化率时 PENDING 无界（reset 清空兜底 + PENDING>2000 连续 5s 告警日志）；blob 装填是新增 memcpy（字节预算兜底，占比 >1ms/帧则降 cap）；24 节/帧为全局共享预算，近场活跃期 DH 排队延长是预期行为；Rust 不可用路径全保留，任何失败永久回退。
