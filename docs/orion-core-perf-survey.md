# Orion 非 DH（本体）路径 Java 侧 CPU 热点实测调查

调查日期：2026-11-17。工作区：D:\fabric-example-mod-26.2（mod 根 = fabric-example-mod-26.2\）。
问题句：**排除 DH（compat/dh\*、[DH-LODMESH]）与原版光栅本身后，mod 本体在 Java 侧还有哪些有实测数字的 CPU 热点，哪些值得作为「本体 Rust 加速」的目标？**
只读不改；本文档为唯一产出。

---

## 0. 范围与排除口径

| 项 | 口径 |
|---|---|
| 含 | mod 本体 Java 代码在帧路径上的成本：EntityRtCapture、FrameBudget 计量段、VulkanCommandEncoderMixin（recordFrame）、captureHead、GC 抢占（本体帧路径上的 CPU 消耗，虽分配源是原版） |
| 排除 | ① DH：`src/client/java/com/example/client/compat/dh/**`、`[DH-LODMESH]`/`[DH-TEX]` 日志（DH 侧已单列 Rust 试点，见 HANDOFF-2026-11-17-DH-OPT-AND-RUST.md §5/§7）；② 原版光栅本身：prepChunk/terrainDraw、原版 ItemFeatureRenderer.prepareGroup 自身耗时；③ GPU 侧（trace 13~17ms、材质 9.12ms 等全部为 GPU 时间）；④ 远场：无 DH 时 FarFieldGate 处于 `meshes=0 / providing=false / erased=0`（handoff-20261111 §7），非 DH 远场 Java 路径本身无可测成本，远场软影 GPU pass 也已按帧自适应关闭（HANDOFF-2026-10-24 §2.2） |

无 DH 时「远场」在 Java 侧只剩 FarFieldGate 的判读调用，无独立实测数字 —— 本清单覆盖的是**本体帧路径**（其中实体/物品捕获是远低于远场、但每帧必经的本体路径）。

## 1. 结论摘要

本体 Java 侧唯一 50~130 ms 级热点是 **H1：EntityRtCapture 逐顶点捕获链**（物品密集图；普通图 3~33 ms）；第二档是 **H2：recordFrame 的 Java 侧录制墙钟**（1.5~5 ms，随视距线性）；其余本体段（captureHead 稳态 0.13 ms、submitEntities 0.03 ms）可忽略。GC 抢占（H4）实测存在且规模大，但分配源在原版（本体四段合计 0.02 MB/帧），Rust 无法消。所有数字均为本机实测，出处与置信见下。

## 2. 热点清单（编号，按对结论的支撑强度排序）

### H1. EntityRtCapture 逐顶点捕获（实体 + 物品）— 本体最大 Java CPU 热点
- **证据 1（A/B 实测，`ORION_ENTITY_CAPTURE=0`）**：featurePrep 208.05 → 83.97 ms（**Δ=124.1**）；Item 桶 181.82 → 77.45（**Δ=104.4**，max 553→84）；Entity Model 桶 20.55 → 0.19。出处：docs/handoff-20261111.md §12.10 表。
- **证据 2（原始日志，2026-11-11 当晚多窗口）**：`run/logs/2026-11-11-6.log.gz` 21:40:04 行：
  `[LEVEL PHASES FEAT] featurePrep=284.94 sort=0.03 featGroup=278.83 upload=6.05 rest=0.03 | ours(capture)=17.06 ours(generic)=11.49 mtn=18.87 jni=35.37 verts/f=2238356 submits/f=559172(item=559082) | Item=260.96(max 330.29) Entity Model=17.49`。
  featGroup 占 featurePrep 的 98%；submits/f 中 item 占 99.98%。
- **证据 3（规模）**：verts/f = 1.59M~2.24M（三窗口），每顶点 12 float=48B ⇒ 约 65~90 MB/帧顶点流；单价实测 **35~61 ns/顶点**；其中 MV 计算 11.3~18.9 ms（≈8 ns/v，`mtn`），JNI 提交 20.2~35.4 ms（≈15 ns/v，`jni`）。出处：handoff-20261111.md §12.11/§12.12 + 上行日志。
- **组成拆分**：`ours()` 四项探针合计 ≈ 63~83 ms，但 Item A/B 增量 104~124 ms ⇒ 差额 ~40~60 ms 是插在**原版写顶点循环里逐顶点跑**的 VertexConsumer 包装（handoff §12.10 结论 1 原文）。探针覆盖不了它，A/B 才暴露。
- **代码位置**：src/client/java/com/example/client/EntityRtCapture.java:107-136（`captureVertex` 逐顶点入口、全局镜像 + `ActiveBatchSlot.owner` 线程校验）、:522 起 `beginModel`；逐 quad 入口在 RenderTypeFeatureRendererMixin 的 `getVertexBuilder`（每帧 64 万次、只落在 24~25 个 group，vbPerGroup≈2.3 万，2026-11-11-6 日志 21:39:42 行）。
- **量级**：物品密集图 104~124 ms/帧增量（普通 Entity Model 16.6~19.5 ms；普通图 featurePrep 3.2~33 ms 视地图而异）。
- **置信：高**（同日 4+ 窗口原始日志、开关 A/B、文档三方一致；本调查直接核对了 gz 原始行）。
- **Rust 适配性**：格式转换 + MV 矩阵 + 打包 = 批量定长数值变换，与已完成的 DH 转换核心 Rust 试点（off_convert_dh_section，Java 53.9~64.9 vs Rust 45.2~46.9 ms，**1.15~1.43×**）同型；试点诚实结论是"转换核心收益中等，生产收益主要在调用形态批量化"。handoff-20261111 §13.6 第 3 步已规划"`emit()` 输出模型局部空间顶点 + 每实例变换"。⇒ 若做本体 Rust，真正的赢面在**把每顶点 Java 回调改成模型级批量输出 + 实例矩阵**（省掉逐顶点 wrapper + 逐帧重复转换），而不是逐顶点函数的微优化（§12.12 已定论"卡的不是 ns/顶点，是图元数量"）。

### H2. recordFrame —— 每帧 RT 命令录制的 Java 侧墙钟
- **证据**：rd=8 med **1.53 ms** / p90 3.24 / 尾段 inner+submit 2.06 ms（docs/handoff-20261111.md §2.1，65 窗口中位）；rd=12 → 5.21 ms（§2.2，随视距近线性）；rd=4 → 1.30 ms。物品密集+GC 帧 `maxRecordFrameMs` 82~229 ms（2026-11-11-*.log.gz FRAME TAIL SPLIT 行）；轻场景稳态 1.4~2.4 ms（2026-11-12-3.log.gz 21:04 系列）。
- **代码**：入口 src/client/java/com/example/client/mixin/VulkanCommandEncoderMixin.java（handoff §9："真正的每帧 RT 录制入口"）；计量在 FrameBudget.java:637（recordFrameMs）。
- **置信：高**（数量级与线性趋势多局一致）。细分成分（JNI 封送 vs native 内部）未拆 —— native 段归 C++ 调查，不在本次范围。
- **Rust 适配性**：单次调用、每帧一次，非批量循环 ⇒ 不是 Rust 甜区；除非后续拆出 per-pass 录制循环。

### H3. captureHead（LevelRenderer.render HEAD 的 tick/捕获/上传）
- **稳态**：med 0.13 ms，可忽略（handoff §2.1）。**尖峰**：世界首载帧 avg 13.7~15.2 / max **795~879 ms**（2026-11-11-1/2/6.log.gz FRAME TAIL SPLIT：`captureHeadMs=15.246 maxCaptureHeadMs=878.799` 等，3 条独立窗口）。
- **归因**：尖峰对应 C++ 侧 staging 222 MB/帧与 entity 层整层哈希（handoff §12.13，`[PERF L10 STALL] tag=entityFrameVertexHash` 4933 条、ms≈32~34，哈希并行化已修）。Java 侧本身只是触发点。
- **置信**：稳态高；尖峰中（Java 侧未单独归因，尖峰主体在 native）。
- **Rust 适配性**：无（本体 Java 侧无可拆的循环）。

### H4. GC 抢占渲染线程 —— 实测存在，但分配源在原版，非本体
- **分配速率（ThreadMXBean 精确计量，`MB/f:` 字段）**：level 段 med **3.49 MB/帧**（rd=8 普通图，handoff §4.1）；物品密集图 **66.8 MB/帧**（原始日志 2026-11-11-6.log.gz 21:39:28 起 4 行 `MB/f: level=66.8xx record=0.0 capture=0.006 submit=0.081`）。**本体四段合计 0.02 MB/帧**（capture/submit/record；handoff §4.1）⇒ 分配不在本体代码里。
- **GC 形态（.tmp/gc.log，本调查重新统计）**：Pause Young **56 次**，med 7.53 / max 30.80 ms，合计 441 ms；Concurrent Mark Cycle **14 个周期**（28 行含起止），med 127.08 ms，合计 **1219 ms**。与 handoff §4.2（med 6.37/124，合计 565/1219）基本一致（该文件含两次启动拼接，口径差 ≤2 ms）。
- **单窗口爆发**：runclient_gap.log:731（15:45:27）`[FRAME GAP/GC] gcMs=308 gcCount=34`。
- **机制链**（handoff §4.2）：分配速率高 → Young GC 频繁 → 触发并发标记 → 与渲染线程抢 CPU → p90 71.68 / max 274 ms。
- **置信：高**。**Rust 适配性：无** —— 分配源是原版（PreparedGroup 列表重建、动态顶点缓冲等），本体已≈0；Java 侧能做的是减原版分配（接管/缓存，均已试、均否）或堆参数，不是 Rust 化对象。

### H5. submitEntities（实体捕获提交）
- med **0.03 ms**（handoff §2.1）；分配 0.01~0.07 MB/f。**可忽略**。置信高。

### H6. 已修复、不再构成热点的本体 Java 项（防止重复立项）
- 图集绑定 127 槽线性扫 → 1 次比较（`ORION_TEXBOUND_CACHE`，handoff §5 第 9 条）。
- 探针未门控事故：`getVertexBuilder` 每 quad 两次 `System.nanoTime()` 曾凭空加 20~40 ms/帧，已改 `probeEnabled()` 门控（handoff §13.8）——对新增热点探针的直接教训。
- 逐顶点日志 `[RTX-HANDUV]`/`[TEXT PERF]` 曾 30 MB/局，已默认关（handoff §5 第 10 条）。

### H7. 帧外残差（Java tick/sim + 驱动提交 + 呈现）—— 规模最大但不可 Rust 化的 Java 侧时间
- `[FRAME GAP] n=120 avgMs=17.77 / workAvgMs=6.03` ⇒ **约 2/3 帧时间在后端代码之外**（docs/optimization-2026-11-06.md §10.1 原文："Java 侧 + 驱动提交 + 呈现"）。
- 外部线程采样佐证：perf/threads2_l_arm.csv（top 线程 ~2.0 s CPU / 2 s 窗口）与 perf/threadmod_p_arm.csv（top 线程模块归因 = **nvoglv64.dll** 2.97 s/3 s 窗口，次线程 ucrtbase.dll 0.27~0.3 s）⇒ 窗口内 CPU 大头在显卡驱动提交路径，不在 JVM。
- latest.log（2026-11-17 局，RT 未激活窗口）也呈 `avgOutsideMs≈16.3 / avgRenderMs≈0.4` 的形态（run/logs/latest.log:446-1248 多行）。
- **置信：中** —— "帧外"三成分（tick/sim、驱动、呈现）未逐项归因；驱动占大头有模块采样佐证但无分时拆解。

## 3. 未实测 / 推测项（不得作为结论）

| # | 项 | 现状 | 出处 |
|---|---|---|---|
| U1 | 实体/粒子捕获每帧分配（`new Matrix4f`×2、每 quad 装箱） | 「量级未测，不建议盲改」 | HANDOFF-2026-10-22-CPU-THREADING.md §6.6 原文 |
| U2 | 主线程任务泵 `BlockableEventLoop.runAllTasks` | FrameBudget 已埋点**从未取到数据**（dev 启动脚本不写 Java 日志） | 同上 §6.5 |
| U3 | Item 桶中未经 `ours()` 插桩的构成（物品走 RenderTypeFeatureRendererMixin，未单独计 wrapper vs 原版） | 「很可能同样以我们的捕获为主，需要下一步补测」 | handoff-20261111.md §12.9 末段原文 |
| U4 | 帧外残差的 tick/sim 子段 | 无逐段归因 | §H7 |

## 4. 材料核对清单（本调查实际读到）

- docs\：benchmarking.md（bench 开关与守卫）、optimization-2026-11-06.md、handoff-20261111.md（核心）、env-switches.md
- mod 根：HANDOFF-2026-10-22-CPU-THREADING.md、HANDOFF-2026-10-29（trace 拆分，GPU）、HANDOFF-2026-10-24、HANDOFF-2026-10-30（GPU）、HANDOFF-2026-10-21×2（native/GPU，两份均明示"CPU 侧与 Java 侧零改动"）、HANDOFF-2026-11-17-DH-OPT-AND-RUST.md（交叉核对用）
- perf\：arm_*.csv 列结构 = `time,commitMB,dMB,MBps`（内存采样，非 CPU）；threads2_l_arm.csv / threadmod_p_arm.csv = 线程 CPU + 模块归因；A_ablation.log / A_sweep.log = 整局原生日志（5000+ 行，[PERF L10 GPU] 等，GPU 侧）；procsample_*.csv = 进程内存
- _perf\：deferred_base_world.csv / prod_base_world.csv / prod_same_spot.csv = `frame,blit,caus,cloud,dlss,far,fg,rt,sdr,setup,sky,tail,vli,wsim` 的 **GPU pass** 时间序列（deferred 24.9~29.7 vs prod 10.6~17.8 的对比，GPU 侧）
- src\client\java\com\example\client\bench\：BenchmarkSession.java = 逐帧 `{"i","frameMs","renderMs"}` 落盘 + valid 判定（开销：开时每帧 2 次 nanoTime + 缓冲 JSON；关时一次 static final 判断）——**只测帧间隔与 render() 总墙钟，不做分段归因**；CameraPath* = 可复现机位。分段归因由 FrameBudget.java（五段墙钟 + `getThreadAllocatedBytes` 分配，:535/:602-647）与 LevelPhaseProbe（[LEVEL PHASES]/[LEVEL PHASES FEAT]）承担
- 日志：run\logs\latest.log（2026-11-17 局）；2026-11-11-1~6、2026-11-12-1~6 .gz 原始计量行（本调查解压核对）；.tmp\gc.log；runclient_gap.log；runclient_simsplit3.log

## 5. 给「本体 Rust 加速」的目标排序（基于上述证据）

1. **H1 逐顶点捕获链的批量化** —— 唯一 50~130 ms 级本体 Java 热点。方向不是"逐顶点函数改 Rust"（§12.12 已证卡的是图元数量），而是模型局部空间一次输出 + 实例矩阵（与 §13.6 第 3 步合流），顺带消掉插在原版循环里的 wrapper。预期量级：物品密集图省 50~80 ms/帧（我们侧增量）+ 原 version 41~77 ms 需另行接管。
2. **H2 recordFrame** —— 1.5~5 ms，收益中等，暂缓。
3. **H3/H4/H7** —— 主体在 native/驱动/GC，非 Java Rust 对象；H4 的治本只能靠减原版分配（工程已试并否）。

## 6. 附：关键原始日志行（可直接复核）

```
run/logs/2026-11-11-6.log.gz @21:40:04
[LEVEL PHASES] n=60 indirect=true takeover=0 level=286.58 submit=0.43 featurePrep=284.94 prepChunk=0.16 terrainDraw=0.02 unaccounted=1.03
[LEVEL PHASES FEAT] featurePrep=284.94 sort=0.03 featGroup=278.83 upload=6.05 rest=0.03 | max featGroup=330.29 calls/f=12.0
  | ours(capture)=17.06 ours(generic)=11.49 mtn=18.87 jni=35.37 verts/f=2238356 submits/f=559172(item=559082 model=90)
  | Item=260.96(max 330.29) Entity Model=17.49
[FRAME TAIL SPLIT] … MB/f: level=66.791 record=0.0 capture=0.006 submit=0.081 maxRecordFrameMs=164.06

.tmp/gc.log（本调查统计）：Pause Young 56 次 med 7.53ms 合计 441ms；Concurrent Mark Cycle 14 周期 med 127.08ms 合计 1219ms
runclient_gap.log:731（15:45:27）：[FRAME GAP/GC] gcMs=308 gcCount=34
```
