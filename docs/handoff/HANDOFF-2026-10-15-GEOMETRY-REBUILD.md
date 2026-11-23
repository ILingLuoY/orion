# HANDOFF — 2026-10-15 — Section 几何重建管线：流式卡顿归因收口与三项后续工程

写给接手的人（或下一个会话的我）。**请先读完第 0、1、2 节再动手**——本轮最有价值的部分
是**已排除的路径**和**已装好且验证过读数的仪器**。所有结论都有 `file:line` 与当轮日志
数字支撑；带 **OPEN** 的地方是尚未定案的，不要当成事实引用。

本轮一行代码的业务语义都没改（三处 native 改动全部带 env 闸门、默认开、`=0` 可回退），
纯诊断 + 两处"照作者已有模式"的闸门/仪表。DLL/SPV/Java 均已编译部署并核对过导出表。

---

## 0. 一句话现状

**用户症状"早期流畅、加载到后边就开始卡"已归因到流式期渲染线程重建管线的三段饱和，
每一段都已定位到具体代码与具体数字；剩余工作就是下面第 4 节的三项工程，按 A→B→C 顺序，
每项都已有先例模式和 A/B 手段，但都需要专门一局对照（Δ>3ms 铁律）。**

主线程（sim 段/任务泵/交换链/Reflex）本轮已全部归因闭合，**不再是嫌疑方向，不要回头查**。

---

## 1. 症状与本会话时间线

用户体感（2026-10-15，多局）：早期流畅；飞进新地形/加载到后边开始卡；帧率掉。

| 时间 | 动作 | 产出 |
|---|---|---|
| 上午 | Java 三段仪表：`[FRAME TASKS]`（主线程任务泵）、`[SECTION COMPILE]`（worker 每 section 六相）、`[FRAME JNI HEAD]`（tickHead 三个 JNI） | 全部验证出数 |
| ~12:30 局 | 读数 | 任务泵排除（≈0.03ms）；`simMs ≈ beginStreamlineFrame`（Reflex 睡眠，设计内）|
| ~12:56 局 | 读数 | `jniMs` 重载段 15-26ms/section（max 196）——worker 在等 `g_chunkMutex` |
| 下午 | native：`ORION_UPLOAD_RAW_MOVE_GATE`（raw 副本拷贝外移） | jniMs 无显著变化 ⇒ **锁主因不是 uploadChunk 的拷贝** |
| 下午 | native：锁争用仪表 `upWaitMs/upHoldMs/capHoldMs` 进 `[RT GEOMETRY] counters` | `upWaitMs ≈ capHoldMs` 全窗口成立 ⇒ **捕获线程是锁的持有者**（capHold 峰值 74.8ms）|
| 下午 | native：`ORION_CAPTURE_TIME_GATE`（捕获时间闸，同款模式 :58296）+ 原子预算镜像 | 中等负载 capHold ≤12 ✓；爆发窗仍见 capHold=47.4（单块超预算，见 §4-B）|
| ~13:47 局 | `[RT GEOMETRY SLOW]` 细分读数 | tail 内部分段定位（§4-A）|

---

## 2. 已排除的路径（**不要回头**）

1. **`textureUploadQueueIdle`（已关闭）**：仅 frame=0（启动/资源重载）出现，游戏内 0 次
   （多局 `[PERF L10 STALL]` 统计：最近完整局 1 次/121ms，全部 atFrame0）。
2. **主线程 simMs（已闭合）**：`simMs ≈ beginStreamlineFrame` 的 JNI 耗时，即
   `slReflexSleep`（VulkanCore.cpp:39448-39556，:39515-39518）。这是 **Reflex 低延迟的
   设计内节流**，不是 bug。移动时 15-21ms、原地 ~0.1ms。要帧率就关低延迟
   （`NativeVulkan.setReflexMode` :41502 含 `ReflexMode::eOff`），不要改代码。
3. **主线程任务泵（已排除）**：`[FRAME TASKS]` 全程 `runAllTasksMs ≈ 0.006-0.03`、
   `maxTaskMs ≤ 0.2`。区块结果消费不走主线程任务队列。
4. **交换链（早已排除）**：`acquireMs ≈ 0.02 / presentMs ≈ 0.24`。
5. **uploadChunk 锁内 raw 拷贝（已外移，但不是锁主因）**：`ORION_UPLOAD_RAW_MOVE_GATE`
   已落地（:47618-47631 闸门；存储段 :47975+ 拷贝移到取锁前，锁内只剩 memcmp 判等 +
   两次 move）。字节语义与旧路径逐位相同。`upHoldMs` 实测 ≤3.4ms ⇒ uploadChunk 自身
   锁内活不重。
6. **历史日志挖 ablation（不可信）**：n=8-353 vs 基线 n=17134 且方向反向，混杂陷阱
   （项目文档已记录）。rt 定价必须受控消融 `ORION_PERF_ABLATION=alt:3,2,5,11,12,15,16`。

**当前锁争用仪表读数方法**：`[RT GEOMETRY] counters` 行的
`upWaitMs / upHoldMs / capHoldMs / capWaitMs`（声明与判读表在 VulkanCore.cpp:12669-12747）。
判读：`upWait ≈ capHold` ⇒ 捕获线程持锁；`capHold` 大且 `capWait` 大 ⇒ 两者都有；
`capHold ≈ 预算(≤12)` 且 `capWait` 大 ⇒ 排队等 worker/渲染线程。

---

## 3. 本会话落地的三处 native 改动（全部带闸门）

| 闸门 | 默认 | 内容 | 位置 |
|---|---|---|---|
| `ORION_UPLOAD_RAW_MOVE_GATE` | 开（=0 回退）| uploadChunk 的 raw 副本分配+拷贝移出 `g_chunkMutex` 临界区；存入字节逐位不变 | 闸门 :47618；存储段 :47975 附近 |
| `ORION_CAPTURE_TIME_GATE` | 开（=0 回退）| 捕获线程唤醒循环加逐 Section 时间闸（照渲染路径同款 :58296-58303，同一份自适应预算）| 闸门 :13148；循环 :13503 附近 |
| （仪表，无闸门）| 常开 | `g_chunkLockMaxWait/HoldNanos` + `g_captureLockMaxWait/HoldNanos`（CAS-max，零新锁）+ `ChunkLockTimingGuard`（RAII）| :12685-12747；counters 行 :58460-58470 |

配套：`g_sectionRebuildBudgetMirrorMs`（:1784 原子镜像）——渲染线程每次算预算时发布，
捕获线程只读镜像，避免与**非原子**的 `g_perfL10GpuPassWindow.samples`（:1630，渲染线程
每帧重写）形成跨线程数据竞争。镜像未发布（=0）时按文档回退值 5.0ms 处理
（`SECTION_REBUILD_TIME_BUDGET_MS` :1663）。

⚠ 新增 JNI/改 `extern "C"` 相关代码时务必遵守 :38821-38830 的规则（插入物必须在
`extern "C"` 之上；改完扫导出表核对 `Java_com_example_vulkan_NativeVulkan_*` 未改名）。

---

## 4. 三项后续工程（按优先级 A→B→C）

### A. tail 段的 staging memcpy 与 TLAS scratch createBuffer（帧尖刺的最大单项）

**机制（已定量）**。tail 无任何时间预算——全函数仅两处闸（捕获段 :58296、构建循环
:58761-58770），注释 :1710-1711 与 :59104-59115 明确排除 tail。`[RT GEOMETRY SLOW]`
（:61518-61566）在 2026-10-15 13:47 局给出三种交替形态：

```
frame=1787 total=59.2 sections=24  tail=48.4  staging=46.9   ← 批量 staging memcpy
frame=1789 total=96.4 sections=25  bld=56.6   tail=34.5  preCmd=31.8 (tlas=31.3) ← TLAS scratch createBuffer
frame=1793 total=67.6 sections=17  tail=60.2  staging=59.0
frame=1833 total=26.7 sections=19  bld=20.4   tail=3.1     ← 良性对照
```

- **staging 47-59ms**：:59129 `g_vertexStagingPool.tryAcquire(totalStagingBytes, …)` →
  :59161-59171 未命中则 createBuffer → :59186-59267 `vkMapMemory + memcpy`（顶点+运动矢量）。
  一次拷 24 个 section 的全部顶点（几十 MB 量级），落在 HOST_VISIBLE 内存上——写合并
  带宽 ~1-2GB/s ⇒ 几十 ms。**OPEN：staging 字节数未单独记录**（先补"每批 stagingBytes"
  打印即可定量验证）。
- **tlas=31.3ms**：:60610-60619 TLAS scratch `createBuffer`。实例数随探索增长，超过当前
  2 的幂槽位就真分配一次（build-buffer 池 :4685-4695 起步 1MB、指数取整、best-fit
  :4701-4733、adopt 顶小留大 :4737-4804、**无槽位上限**）。这是"_INSTANCE 数跨档"时的
  一次性大尾巴，与 `allocMs` 尖峰同源（slab 池 :15525-15527 原话："这些 miss 是
  结构性的，池化治不掉"；单次分配平均 0.24ms 但显存吃紧后 WDDM 换页尾巴"个别一次
  几十毫秒"，:15529-15530）。

**候选方案（均需先加仪表再动）**：
1. **staging 分帧**：把"一次 memcpy 全批"拆成"每帧 ≤N 字节/≤M 个 section"，其余排队
   下帧。作者已有 batched staging 设计（:59129-59267）与 fence/停泊机制
   （:61189-61231 park），扩展它而不是新造路径。⚠ 阈值不自造——N/M 的取法必须从
   stagingBytes 实测分布导出，或做成 env（对齐 `ORION_GEOMETRY_BUDGET_MS` 先例 :1790）。
2. **TLAS scratch 预放档**：实例数逼近当前槽位时提前建大一号的槽（异步建、本批仍用旧的
   ——TLAS scratch 只服务本批 build，需先验证能否双缓冲，**OPEN：未验证**）。
3. 先做零风险的：给 `[RT GEOMETRY SLOW]` 行补 `stagingMB=` 字段（:61518 附近一行改动），
   下一局就能确认 staging 字节↔耗时线性关系。

**A/B**：任何改动配 `ORION_TAIL_*` 闸门；对照读 `[RT GEOMETRY SLOW]` 的 tail/staging/tlas
三列 + counters 的 `tailMs`。

### B. 捕获线程单 Section 拷贝可达 ~47ms

**机制**。`geometryCaptureSectionLocked`（:13344-13459，"分类路由的唯一实现"）对每个
Section：16 次 `g_chunkMeshes.find()`（:13349-13357）+ 每层 `g_chunkMotion` 查询 +
构建 `SectionGeometrySplit`（7 顶点桶 + ommInput + 3 motion 桶）。reserve 策略：普通桶
**几何增长**（:13266-13285，`reserve(max(required, cap*2))`，可能 realloc），只有半透明
混装层做**精确** reserve（两遍全层顶点 :13431-13456）。时间闸只在 **Section 之间**检查
（:13505-13508 "第一个 Section 无条件做"）——单块无论多贵都得做完（与 :1709 构建循环
同一约束）。

47ms 的构成拆不开：纯 memcpy 级拷贝 2-6MB 应 <5ms ⇒ 嫌疑是 **a) 巨型 Section（玩家
建筑区，多层几十万顶点）**、**b) 桶 realloc churn**、**c) 内存带宽与 6 个 worker +
渲染线程争抢**。**OPEN：三项占比未定量。**

**候选方案**：
1. 先加仪表（零行为变化）：`geometryCaptureSectionLocked` 返回/打印单 Section 拷贝的
   顶点数与毫秒（超 3ms 才记，复用 PERF_L10 风格），下一局就知道是 a/b/c 哪个。
2. 若是 realloc churn：把精确 reserve 推广到全部桶（第一遍统计 + 第二遍拷贝；半透明层
   已有现成两遍先例 :13431-13456，照抄模式）。风险低，代价是每 Section 多一遍读。
3. 若是巨型 Section：无解于"块间中断"约束，只能靠 A（staging 分帧）把下游吞掉的速度
   提上来，或考虑 O(1) 的拷贝外移设计（需作者参与——涉及 split 生命周期与 retire 环）。

**A/B**：仪表先上；改动 2 配闸门（建议名 `ORION_CAPTURE_EXACT_RESERVE`，对齐现有命名）。

### C. 流量账：生产端无限流 × 消费端预算饱和

**账目（13:35 爆发窗实测）**：生产 ~118 sections/s（captured 20 秒 +2356）；消费 =
`min(batchCap 自适应 1..24, budget clamp(5,12))`，且**实体路径每帧插队**
（:51540-51556，绕过静默窗口；请求源 :49456-49461/:49703-49708）；
`fenceDeferred=42`、`adopted=11524 vs captured=12219`。Java 生产端
（SectionCompilerMixin:281-282 @compile RETURN）**无任何限流/合并**，唯一跳过是
FarFieldGate（:288-292）。

**关键事实**：`g_sectionRebuildBatchCap`（:1687，中断减半/批内完成 +2）**没有 env 覆盖**
（全部出现点 1687/58118/58418/58707/58788/59109，无 getenv）——而 budget 有
（`ORION_GEOMETRY_BUDGET_MS`）。爆发期两条腿都顶格：buildLoop 41.6-56.6ms（24-25 块/批，
单块 maxSection 只有 2.7-3.4 ⇒ 量的问题不是单块贵）。

**候选方案**：
1. **给 batchCap 加 env**（如 `ORION_GEOMETRY_BATCH_CAP`，照 `ORION_GEOMETRY_BUDGET_MS`
   :1790-1807 的写法）——零风险 A/B 手段，先量"消费速率对帧尖刺/加载完成速度"的权衡
   曲线，再谈改默认值。**不要直接改 24 这个默认**：加载变慢是用户可感的退化。
2. Java 侧限流（上传节流）：**不建议**——动原版编译语义，风险高收益不明。
3. 实体每帧插队 vs 世界区块抢占：爆发期两者抢同一预算；若 A/B 数据显示实体路径
   让世界区块饿出尖刺，可考虑把实体重建的优先级做成 env 可调。**OPEN：未定量。**

---

## 5. 仪表与读数速查（本轮新增/已有）

| 标签 | 落点 | 读法 |
|---|---|---|
| `[FRAME BUDGET]/[FRAME SPLIT]/[FRAME SIM]` | latest.log | interval = sim + render + frameTail（闭合校验）|
| `[FRAME TASKS]` | latest.log | 主线程任务泵（已排除，恒 ≈0）|
| `[FRAME JNI HEAD]` | latest.log | beginStreamline=Reflex 睡眠（simMs 大头）|
| `[SECTION COMPILE]` | latest.log（worker 线程打）| 256 个一报；avg/max + 六相（tag/fused/jni/omm/clear/voxel）|
| `[RT GEOMETRY] counters` | rt_debug.log | `upWaitMs/upHoldMs/capHoldMs/capWaitMs`（锁争用）+ `preLoopMs/buildLoopMs/tailMs/maxSectionMs` + 池计数 |
| `[RT GEOMETRY SLOW]` | rt_debug.log | 慢批细分：pre/pCap/bld/tail（staging/preCmd(ord/evict/inst/tlas)/record/waits/submit/park/qIdle）|
| `[PERF L10 STALL]` | rt_debug.log | 阈值 3ms 的等待：worldGeometryQueueIdle / sectionBuildFenceWait / … |
| `[RT MEM]` | rt_debug.log | rss/mesh/raw/vram 口径；**判读看是否收敛**（探索变大属正常，:57239-57243）|

挖日志模板（PowerShell）：
`Select-String -Path run\rt_debug.log -Pattern 'upWaitMs=' | Select-Object -Last 12`（其余同理）。

## 6. 铁律与禁区（历史教训，全部仍然有效）

1. **Δ>3ms 的消融差值才算数**；跨局对比必须同负载姿态，最好同局 A/B（env 闸门为此而生）。
2. **阈值/守卫公式不自造**：仓库里有先例就照抄（本轮三处全部照抄：闸门模式
   :46200-46209、时间闸模式 :58296、镜像=文档回退语义）。
3. **注释可能滞后于代码**：动任何结构前以当前代码为准（本轮验证过 `markWorldGeometryDirty`
   2 个原子、`sameVertexPayload` 纯 memcmp、捕获线程有 MAX_PER_WAKE 限流——都与注释一致，
   但这个习惯不能省）。
4. `g_chunkMutex` 锁序约定：`g_chunkMutex -> g_capturedSectionsMutex`（:13473 附近）。
   新代码不得引入反向持锁。
5. `extern "C"`/JNI 导出规则（:38821-38830）+ 改完扫导出表。
6. 部署管线：`tools\build_and_deploy.bat`（编译→六处 DLL→SPV 重编→审计）；游戏必须关着
   （run\rtx_backend.dll 会被占用）。本会话 bat 在 agent shell 里报 255，需手动四步
   （cmake build → 6×copy → `_deploy_stale_spv.py` → `audit_spv_freshness.py`），命令见
   git 历史或直接问。
7. Gradle：`JAVA_HOME=D:\RTX` 才能起（daemon 目录是空壳）；`run/mods` 为空 ⇒ 游戏走 dev
   classpath，Java 改动 `gradlew build` 即部署。

## 7. 下一步（接手即做）

1. 先跑一局正常飞行，读 §5 仪表确认三项数字仍在（基线：staging 47-59 / 单块捕获 47 /
   118 sections/s）。
2. 按 A.3 → B.1 → C.1 顺序上"纯仪表"改动（都是零行为风险），拿到 stagingMB、单块捕获
   毫秒、batchCap 曲线三份数据。
3. 数据齐了再回来定 A/B/C 的正式方案与阈值（阈值从数据导出，不自造）。
4. 别忘了还欠的 rt 受控消融（`ORION_PERF_ABLATION=alt:3,2,5,11,12,15,16`）——
   render 段 12-17ms 的账还没拆。

---

## 8. 执行记录（2026-10-15 下午会话）：仪表数据 + B 结案 + 顺带修掉一个既有崩溃

### 8.1 三处仪表已落地并出数

| 项 | 内容 | 位置 |
|---|---|---|
| A.3 | `[RT GEOMETRY SLOW]` 行补 `stagingMB=`（/1048576，与 poolKeptMB 同口径） | `VulkanCore.cpp` SLOW 行 |
| B.1 | 捕获线程单 Section 仪表 `[RT GEOMETRY CAPSLOW] frame= key= ms= Mcycles= verts=`（阈值 = `PERF_L10_STALL_LOG_THRESHOLD_MS` 3ms；锁内只记录、锁外补打——RTLog 的 I/O 不落在 g_chunkMutex 持有段） | `geometryCaptureWorkerLoop` |
| B.1b | `Mcycles=` = `QueryThreadCycleTime` 差值（只数运行时周期）——判别"CPU 被饿"vs"真搬内存" | 同上 |
| C.1 | `ORION_GEOMETRY_BATCH_CAP` env（照 `ORION_GEOMETRY_BUDGET_MS` 写法；**取用时压上限**，自适应状态机不动；counters 行 `cap=` 改打有效上限） | `:1705` 起 |

### 8.2 数据结论（14:48-14:50 流式局）

1. **A（staging）结案：流式期无罪。** 本局 staging 最大 **2.8ms / 15.9MB**（≈5.7GB/s）。
   旧日志里 staging>10ms 的 132 行全部是**启动期**（frame=15-17 的 forceReset 大批）。
   "staging 分帧"对"加载到后边卡"不是解，降级为启动期可选。
2. **B（单块捕获 ~47ms）结案：是"等"，不是"搬"。** CAPSLOW 502 个慢样本，
   **ms 与 verts 无关**（0-3k 桶均值 5.4ms vs 25k+ 桶 6.1ms，全平）⇒ (a) 巨型 Section、
   (b) realloc churn 都被排除（都应随字节增长）。Mcycles 判别：`ms=7.35 Mcycles=0.13`、
   `ms=3.88 Mcycles=1.83` ⇒ 线程只跑了 wall 的 0.5%~15% ⇒ **CPU 饿/硬页错误等待**，
   与崩溃局的内存压力（sysAvail 仅 6.7GB、private 21GB）同一根源。捕获时间闸本身工作
   正常（capHold ≤26，Java jniMs 均值 0.34ms）。
3. **C（batchCap）**：env 就绪但 A/B 局还没跑（用户局 cap=24 全程）。
   下次要 A/B 时启动前 `$env:ORION_GEOMETRY_BATCH_CAP='8'`。

### 8.3 崩溃：`allocate Section memory slab failed VkResult=-2`（既有 bug，已修）

- **与本次改动无关**：同一失败全日志 6 次，12:18/13:47 两次发生在本会话部署之前
  （对应 `hs_err_pid5164/18140`）。
- **机理**：流式期 `usedMB 7.4G > budgetMB 6.8G`（ratio 1.06-1.10 持续越线；超支差额
  相当部分是**其它进程**——vkLiveMB 只有 3.5G）。闸门 mode=2 已尽力（secEvicted=1811、
  vliEvicted=1225、slabKept=0、freed 冻结在 1536MB）仍压不下去 ⇒ 某次 Section slab
  `vkAllocateMemory` 返回 `VK_ERROR_OUT_OF_DEVICE_MEMORY` ⇒ `check()` 抛 runtime_error
  ⇒ 构建侧 `catch(...){ destroy; throw; }` 重抛 ⇒ 无人接 ⇒ terminate。
- **修法（照 OMM 路 pool-acquire-failed ⇒ log + fallback 的先例，只动失败路径）**：
  1. `createSectionMemorySlab` 失败不再 throw：`[SECTION OOM]` 一行日志 + 返回 false
     （签名改 `bool(..., uint32_t& outSlabIndex)`）；
  2. `acquireSectionMemory` 未命中建 slab 失败 ⇒ `return false`（OMM 调用方原本就会处理）；
  3. `createSectionBLAS` 的 acquire 失败改抛专用类型 **`SectionMemoryUnavailableError`**；
  4. 构建 lambda 新增 `catch(const SectionMemoryUnavailableError&)`：destroy + 计数
     `g_sectionMemoryOomSkipCount` + 进 `g_nonResidentSections`（补传配额会重建）
     + `[SECTION OOM] build skipped` 日志 + return（跳过本块）。**其余异常仍 `catch(...)`
     照旧终止**——真 bug 不许吞。
  5. `[VRAM GOV]` 行加 `oomSkips=`。
  判读：出现 oomSkips>0 = used/budget 长期 >1 且其它进程占着差额；观感 = 那块地形
  迟到（与被闸门驱逐一致），而不是崩。
- 产物：`rtx_backend.dll` 六处同哈希 `BC8C660D5310BAAA`（上一版 `C0769086054090A0`，
  含仪表）。着色器未动。

### 8.4 遗留

1. C 的 A/B 局已跑（2026-10-15 15:16，`ORION_GEOMETRY_BATCH_CAP=8`）：**无收益也无恶化**
   —— 重载窗口两局重叠（基线 37-41.5ms vs A/B 36-43ms），perCall 两局都是 3-4（cap 与
   budget 都没真正卡死），队列正常排空（captured 5407 / queued=0，消费 84/s vs 90/s）。
   **默认 24 不动**。⚠ 该局有混杂变量：启动 shell 漏了 `ORION_PERF_TIMESTAMPS` ⇒
   GPU 时间戳整局缺席 ⇒ `budgetMs` 回落 5.0（基线 12.0）。以后**替用户拉游戏必须带全
   平时 shell 的 env**。要干净曲线需重跑。
2. "帧数好低"复核：基线局完整时间线显示重载段本来就掉到 25fps（41.5ms 窗口出现过两次），
   与 A/B 局同形 ⇒ 不是 cap=8 引入。重载段大头在 `Outside`（30-36ms，render 仅 4-7ms），
   仍是未归因的主线（与 §7.4 的 rt 消融是同一笔账）。
3. B 的"等"如果要在代码侧缓解，方向是降并发压力/调优先级（需作者参与），不是搬拷贝。
4. **rt 受控消融第一轮（2026-10-15 15:32 局，`alt:3,2,5,11,12,15,16`）**：配对差分
   （臂窗口 vs 相邻 base 窗口）唯一稳定 ≥3ms 的臂 = **noGlossy ≈ −5ms**
   （Δframe −5.76/−4.79，Δrt −5.72/−4.96，两窗一致）⇒ `rt=16.3ms` 里镜面反射占 ~5ms。
   其余臂（noGi −2.3 仅 1 窗 / rrGi 两窗反向 / march1、noG2bGuide、noIndirectPom、
   noVoxelDda）均未过线。⚠ 样本极薄：该局只飞了 ~1 分钟（15:32:29 进世界 → 15:34:42
   正常退出），每臂 1-2 窗且全在流式重载段。要拆 GI/RR-GI 的账需要 ≥8 分钟的同配置局。
   另：消融只覆盖 GPU 侧；CPU 侧 Outside 30-36ms 的大头不在其中。

### 8.5 CPU 侧结案（2026-10-15 16:35 局，第八段仪表）：**不存在独立的 CPU 大头**

新增仪表（全部纯读时钟，零行为）：
- 第七段：`FRAME SPLIT` 加 `gapMs=`（runTick RETURN → 下帧 runTick HEAD）+
  `FRAME GAP/GC gcMs/gcCount`（GarbageCollectorMXBean 差分，免 JVM 参数）；
- 第八段：`FRAME SIM SPLIT` 四路细拆 sim 段——packets
  （`PacketProcessor.processQueuedPackets`）/ extract
  （`LevelExtractor.collectPerFrameMainThreadGizmos`，注意两个坑：非 void 要
  `CallbackInfoReturnable`、泛型返回值有桥接方法要写死精确描述符）/ sound / mouse。

16:35 局（8 分钟，正常飞行+流式）逐窗残差表结论：
1. **gap ≈ 0.03ms、GC ≈ 0**（仅 16:35:03 进世界窗口 404ms/56 次的资源重载 GC 突发，
   流式期偶发 10-64ms 小窗）——主循环空档与 GC 排除；
2. **packets ≈ 0.1-1ms**（退出世界 churn 时 3-14ms，属预期）、**extract ≈ 0**、
   sound/mouse ≈ 0 —— sim 段四路嫌疑全部排除；
3. **sim ≈ beginStreamline（帧围栏/GPU 等待）**：流式期 jni 三槽 17-27ms vs sim
   25-28ms，残差 ≈ 0——sim 段闭合。且它与 render() 段此消彼长
   （r 涨到 10-16ms 时 beginStreamline 缩到 7-11ms），合计贴着 GPU 节奏；
4. ⇒ **帧周期 = sim(帧围栏等待) + render()(GPU 帧执行，流式期涨到 20-40ms) +
   tail(2-8ms)**——"CPU 大头"其实是 GPU 受限的等待被摊在两个段里，
   **CPU 本身没有隐藏成本**。低帧的根因就是 GPU 帧时间长（流式期 23-40ms），
   优化战场在 GPU 侧（§8.4 已锁定镜面反射 ~5ms，GI 待加长局）。
5. 后续若有 CPU 侧诉求：`beginStreamline` 的围栏等待时长与 Reflex/ pacing 策略
   可作为调参项，但那是"怎么等"，不是"省时间"。

### 8.6 高压场景卡顿归因（2026-10-15 16:43 局，2 分钟）

Java 侧（50 个窗口，帧分段全闭合）：重载段 iv 48-62ms，构成 = sim
（**beginStreamline 26-45ms**，帧围栏等待）+ r（7-25ms）+ tail 3-7ms；gap≈0、
GC≈0（仅进世界突发）、packets/extract/sound/mouse≈0 ⇒ 与 §8.5 一致，CPU 无罪。

原生侧：
1. **持续低帧主因 = GPU 帧时间 50-58ms**（`PERF L10 GPU` avg）：**rt 24-26 +
   dlss(RR evaluate) 20-25 + cloud 4.6-6.7**。dlss 桶 = sl.dlss_d 的 RR evaluate
   （输出分辨率 ~2122x1129，超高性能档反查），20-25ms 是第二大单项，值得单独审视
   （RR 档位/输出分辨率直接决定它）。
2. **单帧巨刺**：maxR=1268ms（16:44:03）、425ms（进世界 16:43:47）、115ms
   （16:44:25）——伴随 **RT 后端 mid-session 重建**（GPU 帧计数器 16:44:18 前后归零、
   `forceResets=2`）。进世界/重建期一次性尖刺，与持续低帧不同因。
   ⚠ mid-session 重建的触发条件没查（若是维度切换=预期；无故重建=bug，待查）。
3. **显存超支 11.7%**（used 7542 / budget 6755，mode=2 硬驱逐：vliEvicted 719 页、
   secEvicted 311、nonResident 290）。`oomSkips=0` —— §8.3 的 OOM 降级路径未被
   逼出（闸门驱逐顶住了）。观感是远段 pop-in，不是卡顿。
4. **几何管线干净**：SLOW 仅 2 条（25/24ms，tail 为主）、fenceDeferred=0、
   perCall 1.3-2.2、无 CAPSLOW、budgetMs=12.0（时间戳正常）。
5. 偶发 `worldGeometryQueueIdle` 18-21ms（3 帧）——渲染等几何队列，重建忙时瞬时。

高压局行动项排序：
1. **RR（dlss 桶）20-25ms** —— 查档位与输出分辨率配置，降一档的收益 >10ms；
2. **rt 25ms** —— 加长消融局拆 GI/RR-GI（§8.4 遗留）；
3. mid-session RT 重建触发条件（forceResets=2）——确认是否场景切换所致。

### 8.7 体积光"怎么调都没有"诊断 + 雾耦合接回（2026-10-15 晚）

诊断（用户报"老问题：调雾出不来体积光，怀疑旋钮失效"）：
1. **旋钮链路全程正常**：UI apply → `setVolumetricLightSettings`（JNI :43643 实时
   写原子量）→ `populateGodRayParams` → UBO → 着色器门控。配置值全部正确到位。
2. **真因一（主因）**：`volumetric_light.comp` 在 10-14 体积雾重写时把介质与雾
   密度场的耦合**整块断开**（march 里只剩 `VL_MEDIUM_SIGMA=0.0011/格` 自有薄霾），
   `fogMediumDensity()` 不再被调用——雾的任何旋钮都影响不到光柱。着色器 ：975
   原注释明说是预留接回点（"重写体积雾时若要再让光柱吃雾的介质，从这里接回去"）。
3. **真因二**：`volumetric_light.comp:940` 的 `weatherParams.z` 维度门控——只有
   主世界（z=1）跑，末地（z=3）直接返回中性值；`volumetric_clouds.comp:940` 同样
   门控 ⇒ 末地里体积云与体积光都被硬关。
4. 配置侧无罪；用户配置（enabled=true/intensity=1.15/steps=28）全部正确。

接回实现（`volumetric_light.comp`，SPV 已重编部署 `c593cda0 → 3f3fa886`）：
- 复用历史系数 `VL_FOG_BONUS = 0.0022`（10-04 调定的旧耦合系数，10-14 后闲置）；
- 密度形状 = `fogDensityAt(y, heightBlock, scaleHeight, 1.0)` 归一化剖面
  （`volumetric_fog.glsl:78` 指数高度式**逐字照抄**，含 FOG_MIN_EXPONENT=-4 钳制；
  不 include 整个 fog 文件——`evaluateAnalyticFog` 依赖本文件没有的 hgPhase）；
- 车道 `cloudParams7=(enabled, baseDensity, heightBlock, scaleHeight)`、
  `cloudParams9.z=rainBoost`（新旧布局同车道，旧档 :624/:627 读的同一批）；
- 雨天加厚照新雾口径（`volumetric_fog.glsl:188` 的线性 mix）；
- `baseDensity`（雾自己的 σ0）**有意不进**光柱介质：旧档
  `GODRAY_FOG_SCATTERING × density∈[0,1]` 同样不吃 σ0，量纲由 VL_FOG_BONUS
  承载，否则光柱合成 `scene×a+rgb` 会对雾 pass 已算的消光二次计账；
- 雾关掉时雾项恒 0，逐位回到 10-14 之后的行为。
- 已知边界：**末地仍无体积光/体积云**（维度门控未动）——放开需要先回答"末地的
  方向光源是什么"（末地无太阳），设计决策待作者。

验证方式：主世界 + 体积雾开 + 调雾的高度/标高/雨强 → 光柱跟随；朝太阳方向、
低太阳时段最明显。
