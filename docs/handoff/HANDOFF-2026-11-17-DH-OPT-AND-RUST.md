# HANDOFF 2026-11-17 — DH 集成修复计划与 Rust 加速启动

## 0. 本 handoff 的产出

- `docs/dh-api-reference.md` — DH API 7.2.0 全类清单 + 34 个关键类 javap + mod jar 自绘渲染类名 + Wiki 摘录
- `docs/dh-bridge-diagnosis-2026-11-17.md` — DH 桥只读诊断全文（P1–P10 编号问题：证据 文件:行、置信分级、改动面、待证项、A/B 判据）
- `rust/orion-farfield/` — Rust 试点脚手架（M1，未接入 gradle，**未编译验证：本机无 rustup**）
- 本文件：定策、版本结论、修复优先级、Rust 里程碑

## 1. 定策（2026-11-17 用户确认）

远场 LOD **只走 Distant Horizons**，Voxy 路线弃用。`VoxyLodManager` 保留为共享底座/历史参考，不再喂新数据。

## 2. 版本与合规（确定，javap/Modrinth 实证）

- API 7.2.0 编译自 DH 3.3.2（`ModInfo.VERSION="3.3.2", API=7.2.0`）；DH 3.3.4 内嵌同一 API 7.2.0（仅配置格式号 4→5）⇒ **3.3.4 不需要换 API jar**；仅当 DH 升 API 8.x 才须换。
- MC 26.3 兼容线：DH 3.3.0 / 3.3.2 / 3.3.3 / 3.3.4（Modrinth，fabric+neoforge 合体 jar）。
- 许可 **LGPL-3.0-only**；API jar 保持 `compileOnly`（build.gradle:103 现状正确）、不分发即合规。
- **部署修正（复测第一步）**：本机 DH 通路当前整体关闭——run/mods 无 DH，3.3.2 在 `_disabled_mods`，3.3.4 被误放 `run/shaders`（不是加载目录）。latest.log 2026-11-17 09:32：「[DH-BRIDGE] 未检测到 Distant Horizons，远场通路保持关闭」。恢复：

```
move run\shaders\DistantHorizons-3.3.4-26.3-fabric-neoforge.jar run\mods\
```

## 3. DH 修复优先级（按性价比；全部证据见 diagnosis 文档）

| 序 | 问题 | 修法 | 改动面 |
|---|---|---|---|
| 1 | P2 卡顿：dispatch 一笔上传全部变更节（DhFarFieldUploader.java:202）× C++ 每帧只消化 24 节（OrionInternal.h:302），TLAS SKIP 逐帧失效 | dispatch 按 8~16 节/帧分帧派发（近处优先）+ 帧批合并 JNI（一次锁内提交 N 节，减少 g_chunkMutex 争用） | 小-中 |
| 2 | P1 卡顿：487 万 quad 常驻 TLAS 且 `mask=0xff` 参与全部射线（WorldGeometry.cpp:3204-3215；B 臂 rt=3.21ms，A 臂未实测） | 次级射线按距离 mask 摘远场；环半径 lodMaxRadiusChunks 先砍半 A/B；相邻同层节合并 BLAS（中期） | 中 |
| 3 | P7 变亮机制根因：远场吃与近场同套直射+GI，但无 AO 洼地、DH 顶点色未阴影、软影/GI 距离降档，现有补偿仅增益 0.85/0.65 | 远场专用环境项（解析 AO/天空可见近似，或把 DH lodShading 折回 albedo）；用 mode 9/11 现成分屏档校准 | 中 |
| 4 | P8/P9 变亮次因：发光/岩浆 `emission=albedo×4` 逐粗 quad 生效；图集 tint×2 亮纹理过曝 | emission 按覆盖面积稀释；×2→≤1.5 | 小 |
| 5 | P4 接线缺口：shouldSuppress 只认 VoxyLodManager.coversSection ⇒ DH 供数时恒 false，「缝外擦原版」从未生效 | 给 DH 实现等价覆盖表（按 DH 启用清单×64 格 cell），或 FarFieldGate 按 provider 分发 | 中 |
| 6 | P3/P5 顺手修：makeRoom 全表扫 O(N²)（DhLodMeshSink.java:294）；探针在 disableRendering 下白跑 ~1800 帧 | makeRoom 换按距离预排序弱堆；disableRendering 跳过 color/depth 探针 | 小 |

**待证实测（一切调参的前提）**：① 装 3.3.4 后取 A 臂 rt 实测（PERF L10，ORION_PERF_TIMESTAMPS=1）；② 移动 32 格后的变更节批量（latest.log grep `[DH-LODMESH] 增量`）；③ binding 49 图集三态（rt_debug.log grep `[DH TEX]`）。

## 4. API 面的落地机会（替换/收窄反射桥，详见 docs/dh-api-reference.md）

- **配置级抑制 DH 自绘**：`configs.graphics().renderingEnabled().setValue(false, "orion")` 或 `renderingMode()=EDhApiRendererMode.DISABLED`（7.2.0 新增带署名 setValue）——替代现有系统属性+探针组合，顺带修 P5；逐帧手段=取消 `DhApiBeforeRenderEvent`；透明拆分=`renderProxy.setDeferTransparentRendering(true)`。
- **数据直读**：`DhApi.Delayed.terrainRepo`（`IDhApiTerrainDataRepo`）按 `detailLevel=0` 取方块级数据点（blockLight/skyLight/bottomY/topY/BlockState/biome），带 SoftReference 缓存——为 Rust 转换核心提供正确输入源。
- **事件**：`DhApi.events` / `DhApiEventRegister.on/off`：World/Level Load+Unload、ChunkModified/ChunkProcessing、BeforeRender（可取消）等；事件在**触发线程同步派发**（渲染事件在渲染线程），handler 内勿碰 GL。
- **线程**：`DhApi.isDhThread()` 判别；预算走 `multiThreading.threadCount / threadRuntimeRatio`。
- ⚠ `EDhApiRenderingApi={VULKAN, OPEN_GL}`：DH 3.x 已有 Vulkan 后端，与自研 RT 的上下文/资源共享**公开 API 无接口**——动它之前先读 distant-horizons-core 3.3.4 源码（GitLab 主仓库 core 子模块有 3.3.4 分支）；且切 Vulkan 后 `getDh*TextureGlId` 会 fail，纹理通道需改数据直读。

## 5. Rust 加速计划

**目标（已定）**：把 `DhLodMeshSink` 的逐节转换核心（DH 列数据 → packed quad/顶点缓冲 + 材质/发光标记）移入 Rust cdylib，批量 API，替代「每节一次 JNI+锁」。理由：批量数值变换正是 Rust 甜区，且可与 P2 修法合流（帧批一次调用）。P3 的 O(N²) 先用 Java 小修解决，不值得为它起 Rust。

- **M0（前置，需本机操作）**：安装 rustup-msvc（`winget install Rustlang.Rustup`，默认 host triple 即 x86_64-pc-windows-msvc，MSBuild/VS18 已在）→ `cargo --version` 通过。
- **M1（本次已完成）**：`rust/orion-farfield/` 脚手架——
  - `Cargo.toml`：cdylib，release lto/codegen-units=1
  - `src/lib.rs`：`off_abi_version` / `off_sum_u32` 基线导出（catch_unwind 纪律 + M2 契约 TODO）
  - `build_rust.bat`：cargo build → 拷贝 `dist\orion_farfield.dll`（与 build_dll.bat 并列，不接 gradle）
  - `bench/OrionFarFieldBench.java`：Java FFM 加载 + 基线调用（JDK 26 单文件运行，验证 FFI 通路）
  - 状态：**未编译验证**；装完 rustup 后先 `cargo check`。
- **M2（移植）**：镜像 DhLodMeshSink 逐节转换。所有权规则：输入只读；输出缓冲由调用方分配传容量、返回实际写入数；任何 panic 必须 catch_unwind 吞掉返回错误码；`#[repr(C)]`。**先对拍**（Rust 输出 vs Java 输出逐位一致）再 A/B，判据：`[DH-LODMESH] 增量`批耗时 + PERF L10 CPU 段。
- **M3（可选）**：`DhLodBufferHookMixin` 的字节拷贝通道改 `off_pack_*` 直写，削减 DH 构建线程上的暂存量（P6）。

## 6. 防回退教训（diagnosis P10，勿重蹈）

- 远场太阳阴影图路线 A 曾把远场整片压黑（表面采样点深度比较恒判全遮挡、背面不剔除）⇒ `ORION_SUNSHADOW_FAR` 保持 0；重启用前必须先改 `sun_shadow.glsl` 生产者契约（向阳侧反向追踪）。
- A/B 两臂 GPU 成本不同会驱动 rr_ae 自动曝光摆动（2026-10-14 教训）⇒ 校准 P7–P9 时不要与消融开关同开。

## 7. Rust 执行记录（2026-11-17 下午，M0/M2 已完成）

### M0 工具链（完成）
- winget 不存在，直接 rustup-init 装 **Rust 1.100.0**（stable-x86_64-pc-windows-msvc）。
- 位置：`D:\fabric-example-mod-26.2\.toolchains\{rustup,cargo}`——**C 盘仅剩 0.3 GB，工具链无法装 C 盘**（首次安装在 C 盘解压报 os error 112，已清理残留）；D 盘根目录写权限受限，故落在工作区。
- 调用前必须设 `RUSTUP_HOME`/`CARGO_HOME`（`rust/build_rust.bat` 已内置；全局可用可自行 setx + PATH）。MSVC 链接器直接命中 VS18，无需 vcvars。

### M2 转换核心（完成，未接生产）
- `off_convert_dh_section`：`DhFarFieldUploader.convert` 的 Rust 镜像 + **单趟融合**（Java 因定长分配被迫两趟；Rust 契约是调用方给缓冲 ⇒ 边过滤边写，读取减半，输出逐位一致）。
- 字段映射与冻结契约见 `rust/orion-farfield/src/lib.rs` 文档（16B→16B：x/y/z/tileId/normalIndex 低 4 位/RGB/light 原样/material；x/z 超限 quad 过滤；textureEnabled=0 ⇒ tileId=0）。
- ABI 决策：**全 32 位标量**（Windows 上 C long 是 32 位，JAVA_LONG 有平台差异坑；JAVA_INT 无歧义）；缓冲长度上限 2^32-1；错误码：0=无有效 quad、0xFFFFFFFE=容量不足、0xFFFFFFFF=panic。
- **对拍：40/40 逐位一致 PASS**（blockWidth∈{1,2,16,64,512}×textureEnabled×quads∈{0,1,7,331,4096}，确定性 PRNG 合成流）。
- **A/B（256 节×8192 quad ≈ 134MB，每节一次调用 = 生产形态）**：java best 53.9–64.9 ms（两轮有机器噪声）vs rust best 45.2–46.9 ms ⇒ **≈1.15–1.43×**。
- 诚实结论：转换核心本身收益中等（gather/scatter 型，JIT 不弱）；生产级收益主要来自 M3 调用形态改造 + P2。SIMD（pshufb 定点 shuffle）留作可选后手，不建议现在上。

### M3 接入生产（待做，需先实测 P-待证①②；已由用户定序挂起，先做本体——见 §8）
1. 批量导出 `off_convert_sections`（N 节一次调用），与 P2「8–16 节/帧分帧派发 + 帧批合并 JNI」合流：一次 `g_chunkMutex` 内提交一批，替代每节一次 JNI+锁。
2. `DhBridge` 加版本门（off_abi_version ≠ 2 拒载）+ 生产对拍开关 `-Dorion.dh.rustConvert=true/false`（A/B 判据：`[DH-LODMESH] 增量` 耗时 + PERF L10 CPU）。
3. 复用 bench 的确定性数据做生产前对拍开关（首帧比对 Java/Rust 输出，不一致即回退 Java 路径并打日志）。

## 8. 本体 Rust 第一枪（2026-11-17 晚，已实施）

### 目标选择（三路子代理调研，报告已入库：docs/orion-core-{perf-survey,java-surface,native-boundary}.md）
- **最大热点（实测 Δ124ms）＝实体/物品逐顶点捕获链**（EntityRtCapture；ORION_ENTITY_CAPTURE=0 省 124.1ms，verts/f=223.8 万）——但 §12.12 已证卡点是图元数，正解是**模型级批量化设计**（与 §13.6 第 3 步合流），不是逐顶点移植 ⇒ 留给设计路线，本轮不动。
- **本轮 S 级目标＝粒子天气内核**（ParticleRtCapture.buildRtWeather+emit 打包）：20Hz、seed→输出全确定、除两个可前置探针（getHeight/getPrecipitationAt）外零 MC 对象耦合。
- A 级候选（后续复用本管线）：SectionCompilerMixin VPT 反照率累积/玻璃内面剔除/OMM 逐三角分类、实体 MV 循环。
- 原生边界：208 个 JNI 导出、每帧重活全在 rtx_backend.dll；第二个 DLL 走 natives/windows 资源提取模式。

### 实现（全部落地）
- `rust/orion-farfield/src/lib.rs`：新增 `off_weather_plan`/`off_weather_build`（ABI 升 3）；`WeatherParams` 37 词冻结（anchor 由 Java double 域算好以 i32 传入，避免 f32 化错格）；Java 溢出 int 字面量用 u32 位模式常量（H1–H6）；JOML transform 矩阵式复刻 + rotateZ 后乘（javap 1.10.9 实证）；sin/cos/floor 走 double 域逐位对齐。
- `src/client/java/com/example/vulkan/RustBridge.java`：FFM 桥（Java 25），版本门 off_abi_version==3，资源提取 `/natives/windows/orion_farfield.dll`（与 rtx_backend 同目录）+ `orion.rustDll` 开发覆盖，任何失败永久回退 Java 路径。
- `ParticleRtCapture.java`：`-Dorion.rustWeather`（默认 false）开关；`buildRtWeatherRust()`＝plan→Java 探针（同量同序）→build 写快照；`endFrame` 快照拷贝路径（重建帧/非重建帧都从快照搬，语义等价于逐帧 emit）；.bak 已留（.bak_20261117_rustweather）。
- `rust/build_rust.bat`：新增部署腿（拷 DLL 进 src/main/resources/natives/windows/）。

### 对拍与 A/B（bench/OrionFarFieldBench.java，JOML 1.10.9 孪生）
- **DH 回归 40/40 逐位 PASS**（ABI 升 3 未破坏旧核心）。
- **天气对拍 4/4 场景 PASS**（满雨/满雪/混合/空）：plan 坐标 0 差异；非 XYZ 字段逐位一致；XYZ 最大差 3e-6（门限 2e-4，纯 JOML 公式 ulp；纯雨路径连 JOML 旋转都逐位同）。
- **A/B（1764 格满雨）：Java 孪生 0.31ms vs Rust plan+build 0.04ms ＝ 7.34×**（build 单独 0.02ms）。诚实注记：孪生含逐雪花 new Quaternionf 分配，生产 Java 复用向量 ⇒ 生产倍数会低于 7.34×，但量级成立。

### 待办
1. ~~编译验证~~ ✅ 已完成（2026-11-17）：compileClientJava **BUILD SUCCESSFUL**（javac 25.0.4.1 工具链，ParticleRtCapture.class / RustBridge.class 均由新源码产出）。根因备忘：exec/CLI 环境无 GRADLE_USER_HOME ⇒ gradlew 回落 C:\Users\马帝\.gradle（C 盘近满、9.6.0-bin 半成品）⇒ 在线下载；已清 C 盘半成品（25.8MB），后续 CLI 调用须显式 `$env:GRADLE_USER_HOME='D:\GTX'`；JDK 25 工具链已从本地 zip（voxy-26.3-dev/jdk-25_windows-x64_bin.zip）解至 D:\GTX\jdks\jdk-25.0.4.1 并登记进 gradle.properties installations.paths，离线可用。
2. 游戏内 A/B：`-Dorion.rustWeather=true/false` 双臂（雨天帧耗时 + PERF L10），可选首帧生产对拍日志。
3. A 级候选移植排队（VPT 反照率累积优先：28B stride 纯缓冲变换，与 DH 同型）。
4. 实体捕获模型级批量化设计评审（真正的大头，Δ124ms）。

## 9. 本体 A 级第一枪：VPT 反照率累积 Rust 化（2026-11-17 晚，已实施）

### 实现
- `rust/orion-farfield/src/lib.rs`：新增 `off_vpt_plan`/`off_vpt_build`（ABI 保持 3，纯增量）。两段式「槽位树」ABI：plan 只出几何可判定的探针槽（每 quad 6 槽 × 8B：kind u8 + pad + 坐标 i16×3，可出界 → i16），Java 探针循环按生产同款控制流补谓词位（`0x80|isAir/leafFlower/canOcclude`，未探=0xFF，未发=0x00；getBlockState 次数与生产完全一致），build 吃位做全部数学：albedo 累加（含 front-solid/back-air 分支、图集钳位、像素指针读）+ 就地 byte27 植被(25)/雨洼(23)打标（顺序与生产一致，23 可覆盖 25）。
- `RustBridge.java`：新增 vptPlan/vptBuild 句柄；**累加器走 ThreadLocal 本地缓冲拷入拷出**（FFM 下行不收堆段，ofArray 被拒；顺带解决多线程 builder 下静态缓冲竞争）；顶点缓冲视图 duplicate().clear() 全容量（生产用绝对索引）。
- `SectionCompilerMixin.java`：`-Dorion.rustVpt`（默认 false）开关 + `accumulateVptAlbedoRust()` + 调用点回退分支（.bak_20261117_rustvpt 已留）；`build_rust.bat` 部署腿把 DLL 送进 natives/windows。

### 对拍与 A/B（bench VPT 段：3 场景 × 4096 quad，含平 X/Y/Z、退化、出界坐标）
- **DH 40/40 + 天气 4/4 回归 PASS**（ABI 3 未破坏旧核心）。
- **VPT 3/3 PASS**：plan 坐标 0 差异；采样数完全一致（677/677、690/690、0/0）；累加器四数组逐位一致；顶点字节逐位一致。
- **A/B（4096 quad）：Java 全流程 0.76ms vs Rust plan+build 0.32ms ＝ 2.42×**（探针留在 Java 侧＝生产真实形态；诚实注记：探针与 getBlockState 的成本没动，收益来自纯数学部分卸载）。
- 天气 AB 复测：8.71×（0.30→0.034ms）。

### A 级其余项（子代理规格已入库 deliverables/orion-a2-a4-port-spec.md + docs 副本）
- **玻璃内面剔除/半透明标记**：与 off_vpt_* 同型（谓词前置成节周边体素属性表），下一优先。
- **实体 MV 循环**：每顶点仅 ~15 flop，单独移植被 FFI 摊薄 ⇒ 并入捕获链批量化设计评审，不单做。

### 待办
1. 游戏内双臂 A/B：`-Dorion.rustWeather` + `-Dorion.rustVpt`（perf L10 / 雨天帧耗时 / 节编译耗时）。
2. 玻璃剔除移植（复用本管线）。
3. 实体捕获批量化设计评审。

## 10. 游戏内冒烟验证与三连 bug 修复（2026-11-18，已实施）

### 冒烟故障链（三次启动，全部定位并修复）
1. **Session 1（13:01）**：VPT 路径 worker 线程 panic 风暴（index==len）→ 13:03:16 原生崩溃（VCRUNTIME140 写越界，Worker-Main-5）。根因 A：**/28 钳制错误**——vertexBuffer 容量按「每 quad 28B 记录」计，而 drawState.vertexCount 是 4× 展开数，quad 上限必须 /112（4×28）；根因 B（更深）：**静态共享缓冲跨 worker 线程互覆**。
2. **Session 2（13:11）**：JVM malloc failed（99KB）——**系统提交内存耗尽**（C 盘 0.23GB 页面文件无法增长 + 多个 gradle 守护进程）；gradle --stop 释放后正常。环境红线，非代码问题。
3. **Session 3（13:29→13:57）**：桥加载成功（懒初始化，进世界才触发 ✓）→ 进世界后 VPT panic 复现（/28 钳制不够）→ 13:57:35 **VK 信号量 5s 超时崩溃（BEGIN_RENDER_PASS Item in hand）＝10 月已知引擎崩溃形态**，被竞争拖死的 worker 触发；归因未能完全定界（12:26 另有无关的 nvoglv64 驱动崩溃记录）。

### 修复（全部落地，compileClientJava BUILD SUCCESSFUL）
- **钳制修正**：quad 上限 = min(vertexCount/4, bufferBytes/112)（/28 是按顶点数算的，错 4 倍）。
- **ThreadLocal 化**：vptParamsBuf/vptPlanBuf/vptBitsBuf → ThreadLocal（SectionCompilerMixin），section 编译在多 worker 线程上，static 共享会跨线程互覆——**这是 panic 风暴与垃圾坐标的真正根因**（诊断实证：quad=2 的 slot0 读到别的 section 的 lz=256；albedo 槽的 bits 出现只有 foliage 探针才会写的 bit1）。与 SectionScratch 同一模式。
- **weatherPlanBuf.flip() 移除**：Rust 经 segment 写入不推进 Java position，flip 把 limit 归 0 → 探针循环全线下溢（14:35:17 BufferUnderflow 实证）→ 已修（position 本在 0，直接读）。
- **诊断武装**：plan 失败/成功各打印 vc/cap/lim/pos/stride/quadsClaimed/quadsRust（前 3 次）；Rust 内部 index≥4096 时打印整个 quad 现场（kind/coords/bits/verts）并 ERR_CAP 回退；启动行加 `-Dorion.rustDll` 直指 dist 绕过提取（本次实证 dist 加载正确）。

### 验证（Session 4/5/6 + 完整一局收官，日志实证）
- 桥生产加载 ✓（dist 直指，158720 字节 = /112+诊断版）。
- **VPT 路径全绿**：`[RUST-VPT] ok: vc=3292 cap=92176 cutout=true quadsClaimed=823 quadsRust=823`（cutout 层）、`vc=3428 quadsClaimed=857 quadsRust=857` 等，**零 panic 零回退**，quads 全对齐（cap=quads×112 证实缓冲为标准的 4×28/quad 布局，此前「容量不足」均为竞争假象）。
- **完整一局收官（2026-11-18 14:33–14:40，2.7MB 日志全量统计）**：VPT Rust 路径全程零失败零 panic、原生崩溃 0、正常存盘退出（Saving chunks → Stopping!）；天气回退 3815 次（雨中每 50ms 重建一次的下溢回退，恰好是 flip 修复必要性的生产级实证，下一次启动生效后归零）。
- **测量局收官（2026-11-18 15:43–15:45，flip 修复已加载，日志 = run/rt_client_out_rust.log）**：全 session `RUST-VPT failed=0`、`RUST-WEATHER failed=0`、`panicked=0`、`[VPT-DIAG]` 越界金丝雀=0、正常 Stopping 退出；fusedMs 稳定 4.6–6.6ms 区间。VPT Rust 路径生产验证**收官 ✓**；天气路径 flip 修复已加载且零失败（本局未降雨，雨触发验证遇雨自验即可）。
- 天气路径：flip bug 已修并编译，**待下次启动验证归零**（本局优雅回退 Java，不影响玩）。

### 待办
1. 重启验证 RUST-WEATHER（flip 修复已编译，雨天触发）。
2. 双臂 A/B（基线臂 = 注释两行 -D）。
3. 玻璃剔除移植。
4. 实体捕获批量化设计评审。

### 环境红线（重申）
C 盘 0.23GB（页面文件无法增长）+ 提交内存曾耗尽：起游戏前 gradle --stop、关大内存应用；中期把页面文件挪 D 盘。

## 11. DH 接入定稿：分帧派发（已实施）+ G-buffer 光栅光影（设计定稿，2026-11-18）

用户定序（2026-11-18）：DH LOD 走光栅——不是原版光栅，而是 **G-buffer 光栅光影**；加载 DH 时降帧最少、卡顿最少；参考既有 Rust 管线。

### 三路调研（规格已入库 docs/）
- `docs/orion-dh-render-path.md` — DH 远场当前渲染路径全景：单链 Java 抓网格 → JNI 上传 → C++ 伪节 → BLAS/TLAS → 命中着色；farLight.mode=-1 时 mask 0xff 参与全部射线；**改光栅的七个挂载点已定位**（①Sections.cpp:1999/1102 脏键枚举跳 DH 键；②Geometry.cpp:1999/1102 isDhLodSpatialKey 跳过；③WorldGeometry.cpp:3210 特判删除；④Sections.cpp:3962/Geometry.cpp:3054 角色不入表；⑤material_resolve.glsl:620-742 DH 着色分支→片元移植；⑥lod_coverage.glsl:81 冗余剔除失效；⑦Shadow.cpp:694 远场阴影去留）；零代码替代路：`-Dorion.dh.matrices=true`+mode=4/12/13 重开 DH 图像合成（DhFarFieldGeometry.java:255）。
- `docs/orion-gbuffer-survey.md` — **引擎无任何 Vulkan graphics 光栅管线**（全 compute+RT，vkCmdDraw 0 命中，graphics 路径已删 Frame.cpp:3295 注明）；「G-buffer」= RT rgen 主 pass 写的 6 张 denoiser guide 图（createRRGBufferImages Sharc.cpp:545；albedo RGBA16F×2/normal+rough/hitDistance/stableRadiance/transparencyHint；写入方 rgen_main_body.glsl:7994 imageStore；消费方 NRD/Streamline）；DH 远场当前合成点 = K1 合成块（rgen_main_body.glsl:8114-8612，DH 色写进 RR G-buffer 8619-8642）；雾接入在 fog_froxel/fog_screen_apply 前（光栅深度参与 froxel 雾）；新光栅 pass 可仿水焦散 compute 骨架（Water.cpp:1319-1385 无深度 render pass / 1441 compute）。
- `docs/orion-m3-dispatch-spec.md` — 分帧派发与 P3 修复的完整规格与验收判据（本轮已实施，见下）。

### 阶段一（已实施，compileClientJava BUILD SUCCESSFUL）
- **分帧派发（P2）**：DhFarFieldUploader 新增 `PENDING`/`PENDING_REMOVES` 并发队列 + `syncDiff()`（差集入队，不再整单派发）+ `dispatchFrameSlice()`（每帧近处优先切片，默认 12 节/8MB 双限，可调 `-Dorion.dh.dispatchSectionsPerFrame/BytesPerFrame`；排序缓存仅规模变化或移 ≥8 格才重排；撤下全部捎带；`-Dorion.dh.frameBatch=false` 回旧整单路径做基线）。
- **P3 堆修复**：DhLodMeshSink.makeRoom 由 while+全表扫（O(k·N)）改为单趟建堆逐 pop（O(N + k·log N)）；prune 全表排序改复合键堆（同键序语义等价）。
- 诊断：`[DH-LODMESH] 帧批：PENDING 剩 N 节` 日志；plan 失败/成功各打印 vc/cap/lim/pos/stride（前 3 次）。

### 阶段二（设计定稿，待实施）
1. **C++ 摘除**：按七挂载点把 DH 键从 BLAS/TLAS/角色表中摘除（远场不再参与任何射线）。
2. **compute 投影 pass**：仿水焦散骨架新建 compute pass，把 DH LOD quad 记录（C++ 已有的 28B/quad 展开数据）投影到屏幕空间，写进 RR G-buffer 图（albedo/hitDistance/normal，深度测与近场合成，替代原命中路径）；着色分支⑤从 material_resolve 移到该 pass；雾⑦按光栅深度接 froxel。
3. **Java/Rust 侧**：off_convert_sections 批量内核（ABI 已定）+ 分帧派发（阶段一已完成）对接。
4. **验收**：TLAS 无 DH 实例（[TLAS SKIP] 命中率）、K1 合成块改读光栅 G-buffer、A/B 双臂（帧耗时 −P1 的 TLAS 份额、卡顿计数）。
### 对账声明（本轮三路子代理 3 usable / 0 failed / 0 stopped / 0 empty，无重做范围）
未覆盖项：①G-buffer compute 投影 pass 未实施（阶段二，设计已定稿）；②C++ 七挂载点摘除未实施（需引擎侧改动，规格已列）；③DH jar 部署与 run config 的 --enable-native-access 仍待用户操作；④实机变更节批量分布与 A 臂 rt 基线仍为待证项（需游戏实测）；⑤**rtx_backend.dll 与源码同步性未核**（子代理 B 遗留待证：run\ 下部署的 DLL 是否与当前源码一致，动阶段二引擎侧改动前需先确认基线）；⑥子代理 A 渲染路径全景的未核项详见 docs/orion-dh-render-path.md 待证节（以函数名锚定为准）。

## 12. 阶段二第一片：DH LOD 投影 compute pass（2026-11-19，已实施并编译通过）

按用户定序（2026-11-19）：先做不动 rgen、不需要起游戏的部分（另一智能体在修主 bug，引擎 C++ 侧暂不碰；rgen 侧也未动——用户更正后明确先不动 rgen）。

### 已落地
- **`dh_lod_raster.comp`（mod 根，源 + .comp.spv，glslangValidator VulkanSDK 1.4.309.0 编译通过，vulkan1.3 目标 + GL_EXT_shader_8bit_storage）**：DH LOD 投影 compute pass——读 C++ 伪节的 64B/quad 记录 SSBO（与 uploadDhLodSection 收到的字节流同构），逐 quad 解码（有效性判据与 C++ appendDhLodQuads 同款：x/z ≤ width+1、material≠14 空气剔除）→ 世界坐标 → MVP 投影（worldToClip = inverse(projInverse)×inverse(viewInverse)）→ DH 翻转约定屏幕坐标 → 两三角形覆盖填充 → 写 **RGBA8 颜色图 + R32F 深度图**（未覆盖=深度 0；水 material=12 写 alpha 区分）。
- 派发粒度：每 section 一次（与 uploadDhLodSection 同粒度），push constants 传 section 原点/宽度/quad 数/图尺寸；绑定 set0：0=cam UBO、1=quad SSBO（uint8_t 数组，8bit storage 扩展）、2/3=输出图。
- v1 语义如实记录：逐 quad 无序写入（远场地形屏幕重叠极少，last-writer-wins）；颜色取 vertex0（quad 内四顶点颜色一致）；深度 = 本 MVP clip.z/w（mode=14 时 Java 需把本引擎 MVP 推进 dhFarFieldMvp 供 K1 重建）。
- **off_convert_sections 批量内核（M3 ③）**：N 节一次 JNI；批量对拍 64 节逐位一致 PASS；AB（256 节×8192 quad，128KB src）Java 逐节 55.85ms vs 批量 47.73ms ＝ 1.17×（JNI 次数缩减在派发层收益更大）。
- **Java 接线**：RustBridge.convertSectionsBatch（FFM，ThreadLocal 中转缓冲，失败返回 null 回落逐节 Java convert）；DhFarFieldUploader 帧批 worker 优先走批量（-Dorion.dh.rustConvert=false 回基线）。compileClientJava BUILD SUCCESSFUL。

### 待实施（阶段二余下）
1. **C++ 七挂载点摘除**（等主 bug 修复后动引擎侧，规格见 §11）。
2. **C++ 接线**：本 pass 的管线创建/每帧派发（每 section 一次）/SSBO+图像描述符/输出图创建（W×H 同 DH 图）。
3. **rgen mode=14 分支**（用户解封 rgen 后）：K1 块读本项目图（替代 DH 贴图源），复用既有 gamma/强度/雾机制；Java 侧 mode=14 时把本引擎 MVP 推进 dhFarFieldMvp。
4. **off_convert_sections 批量内核**（M3 ③，规格已入库 docs/orion-m3-dispatch-spec.md）。

### 验证
- glslang SPIR-V 编译通过（vulkan1.3 目标）。
- 游戏内验证待主 bug 修复后进行（本 pass 未接线前为死代码，无风险）。

## 13. 引擎侧整合实施收官（2026-11-19，执行者子代理，三绿）

### 已落地（20 文件，每文件 .bak_20261119_rasterlod，全量记录 docs/orion-raster-lod-impl.md）
1. **输出图+管线**：`createDhLodRasterResources`（Lod.cpp）按 DH 图尺寸建 RGBA8 color + R32F depth storage image（复用 createRRGuideImage），仿 Shadow.cpp 远场阴影范式建 compute 管线加载 dh_lod_raster.comp.spv，专用 set0 = cam UBO + 记录 SSBO + 两图，push constants ivec4×2。
2. **每帧派发**：`recordDhLodRasterPass`（Frame.cpp，挂在 recordSunShadowFarMap 后、traceK1K2 前）：清屏→逐节拷贝记录→逐节 push+dispatch→RT 屏障。**记录数据 CPU 保留新增**（g_dhLodSections；原实现在 uploadDhLodSection 展开后即丢弃）+ 单一大 device-local SSBO（256B 对齐 bump+free-list）+ 常驻 staging，渲染线程帧内拷贝，上传线程零 Vulkan 调用。
3. **rgen mode=14**：新绑 **binding 96（rgba8 color）/97（r32f depth）**（全 RT 闭包占用扫描后的空闲 slot；RT 池 STORAGE_IMAGE 配额 23→25 的必炸点自查已修）；K1 外门放宽、颜色/深度源按 mode 切换，gamma 2.2/强度/增益/大气透视全部镜像 mode=4；Java 经 VulkanCameraMixin→EntityRtCapture→DhFarFieldGeometry 推本引擎 inverse MVP 进载荷 [0..15]。
4. **七挂载点**（行号按当前版本重核）：②Geometry.cpp:1115、③WorldGeometry.cpp:3216、⑦Shadow.cpp:725（远场阴影自适应关断，显式 env 仍可强开）均 g_dhRasterLod 门控；①④随②结构性摘除；⑤material_resolve/⑥lod_coverage 走结构性不可达+注释（TU 无相机 UBO，旗标无法低风险到达）。旗标经 updateDhFarField reserved lane [22] 下发，关闭时全路径首行短路、载荷字节不变。

### 三绿验证
- glslang dh_lod_raster.comp.spv exit=0；_deploy_stale_spv.py 96 目标/重编 24/失败 0、三目录哈希一致；MSBuild 冷编译 BATEXIT=0；gradlew compileClientJava --offline BUILD SUCCESSFUL；新 DLL 已部署 run\。

### 遗留待证（需游戏实测，本轮不起游戏）
实机成像/变体自选收敛/水 alpha/SSBO 扩容频率。**建议开启组合**：`-Dorion.dh.rasterLod=true -Dorion.dh.matrices=true -Dorion.dh.farLight.mode=14 -Dorion.dh.lodMesh=true`（关闭时全路径首行短路、载荷字节不变、与 HEAD 逐位一致）。完整记录（20 文件清单、slot 语义、复跑命令、回退清单）＝ docs/orion-raster-lod-impl.md。

### 对账声明（本轮引擎侧整合子代理 1 usable / 0 failed / 0 stopped / 0 empty，无重做范围）
交付物完整可用（三绿 + 20 文件 + DLL 部署 run）。未覆盖项即上方遗留待证四项（全部需游戏实测，本轮不起游戏）＋两项小额遗留：①rtx_backend.dll 与源码同步性仍未核（前轮遗留，动引擎侧后续改动前建议先核）；②`--enable-native-access=ALL-UNNAMED` 已在 _launch_rust.bat 启动行内，但 IDE/run config 路径未同步（用户用 IDE 启动时需自查）。两波前期委派（本体调研三路 + A2/A4 规格）亦全部 usable，无静默丢弃。

### 对账声明（无失败/中止/空回执，无重做范围）
两波委派全部可用：上轮本体调研三路 3 usable；本轮 A2/A4 规格提取 1 usable（产物已入库 docs/orion-a2-a4-port-spec.md，同时存在 deliverables/ 副本与 C:\Users\Public 原件）。未覆盖项及原因：
1. **游戏内双臂 A/B 未跑**：需启动带 `-Dorion.rustWeather`/`-Dorion.rustVpt` 的游戏会话，本轮只交付开关/管线/离线对拍。
2. **`--enable-native-access=ALL-UNNAMED` 未进 run config**：JDK 25 默认 WARN 不拦，启用开关前需加参数（未改 run config，留用户决定）。
3. **玻璃剔除「收益未量化」**：规格子代理判定代码层可移植置信高、但收益层置信低（剔除判定有「同 block 不同 state 且双方完整方块」等复杂前提，探针前置表的实测收益未测）⇒ 移植前先跑该项的实测基线。
4. **实体 MV 循环未移植**：规格判定每顶点 ~15 flop、FFI 摊薄后无利；且其历史差分依赖 `entry.frame == entityMotionFrame-1` 帧号连续性，对拍需逐位复刻 float 运算顺序与历史回写时机——两个前提使独立移植成本高于收益，并入批量化设计评审。
5. **A2/A4 规格行号已平移**：规格提取期间 SectionCompilerMixin.java 被本轮 VPT 接入并发修改（2447→2597 行），规格中行号以函数名锚定为准。
6. **编译验证范围**：compileClientJava 通过＝本两轮全部 Java 改动（RustBridge/ParticleRtCapture/SectionCompilerMixin）可编译；运行时行为验证依赖第 1 条的游戏内 A/B。

### 未覆盖与待证（对账声明，全部有因，无静默丢弃）
三个调研子代理全部成功（3 usable / 0 failed / 0 stopped / 0 empty），无重做范围。以下为本轮未覆盖项及原因：
1. ~~编译验证~~ 已解决：根因＝执行环境缺 GRADLE_USER_HOME（回落 C 盘半成品）+ JDK 25 工具链未就位；已用 D:\GTX 发行包 + 本地 JDK 25（D:\GTX\jdks\jdk-25.0.4.1，已登记 installations.paths）离线编译通过（BUILD SUCCESSFUL）。
2. **游戏内双臂 A/B 未跑**：需启动带 `-Dorion.rustWeather` 的游戏会话（且本机 DH 未装回、无自动跑批环境），本轮只交付开关与管线。
3. 子代理 B 的四个待证（详见 docs/orion-core-java-surface.md）：①实体每帧顶点总量（决定 A4 是否升 S）；②buildRtWeather 内方块探针耗时占比；③满雨是否触 3528 quad 上限；④SectionUploadDedup.hash 算法核对——均需运行时数据或反编译，本轮未做。
4. 子代理 C 的两个待证（详见 docs/orion-core-native-boundary.md）：orion_farfield 进 natives/windows 后 dev/生产双提取路径未实测（本轮只读未构建 mod）；FFM `--enable-native-access` 注入点未查（JVM 参数需进 run config）。
5. 子代理 A 的推测项（详见 docs/orion-core-perf-survey.md）：实体捕获每帧分配量级、runAllTasks 埋点、Item 桶 wrapper 拆分——均无实测，标「推测」。
6. **子代理报告原始位置**：先产出于 C:\Users\Public\，本轮已拷入 docs/ 入库（上方三份）。

## 14. 2026-11-19 晚实机验证、bug#1 修复与 mode=15 诊断工具

### 四旗标首局（21:00 起）
- DH 3.3.4 已从 run\shaders 移回 run\mods（前轮遗留项执行）；_launch_rust.bat 追加四旗标（初版丢续行符 `^` 已修，REM 基线臂注释块与 Active 块并存）。
- 首局死于**提交内存耗尽**（hs_err_pid20724：C2 CompilerThread malloc failed, Chunk::new）——`gradlew --stop` 释放后重启成功。C 盘页面文件红线仍在，无声退出先查这个。

### bug#1：首帧 NaN inverse MVP（已修，待回归确认）
- 现象：`[DH-MAT] 本引擎 inverse MVP 已下发 v0=NaN v5=NaN`（首帧相机矩阵未就绪）；C++ 侧本有拒绝（rt_debug.log `已丢弃非有限=1`）但 Java 侧无守卫。
- 修复：双层 finite 守卫——①`EntityRtCapture.setEngineCameraMatrices` 非 finite 拒收（保留旧值/null）；②`DhFarFieldGeometry.push` 乘积非 finite 本帧不推送。备份 `.bak_20261119_nanmat`。复验局 MVP 行打印真实值（v0=0.0406 v5=0.1062 v15=4.88e-4）。
- C++ 接受路径实证：rt_debug.log `[DH MAT] 收到 DH 逆 MVP（第 1..3 次） near=0 far=0 mode=14/15 strength=1 gain=1`。**注意：C++ RTLogFlushed 落 run\rt_debug.log（`[CPP]` 前缀），不在控制台日志里——排查勿再 grep 控制台。**

### 待解：远场成像「原版平光」（H2 实锤）
- 现象（用户目视）：水面有远景倒影（光栅投影链路通，反射采样色图成立）但主视野远场无雾无 GI＝走了 rgen_main_body.glsl:8795 兜底裸色（positionIsWorld=false 或 geometryReady=false 路径）。
- 静态排查全绿：comp 写 forward-Z clip.z/w 与 rgen 矩阵路径自洽；comp 写入与 dhFarTextureUv 读取翻转互相抵消（颜色对齐实证）；Java multiplyColumnMajor 顺序正确（viewInv×projInv=inverse(proj×view)）；lanes 置零后解析式被 lanesUsable 门关掉、矩阵路径优先。断链环节待 mode=15 象限读数定位。

### mode=15 四联诊断（新增工具，冷编绿）
- 语义：＝mode 7 的四象限（Q1 矩阵误差图 / Q2 法线 / Q3 距离来源 / Q4 距离灰度），但深度源走本项目光栅图（原 mode 7 只能看 DH 贴图深度，rasterLod 下看不到自己的链）。Q4 归一改 `dhFarFar>4?dhFarFar:2048`（适配置零 lanes，否则恒白）。
- 改动三处（备份 `.bak_20261119_diagmode`）：①rgen_dh_far.glsl `dhLodRasterModeActive()` 加 15；②rgen_main_body.glsl 外门/象限展开加 15 + Q4 归一；③DhFarFieldGeometry.java 推送门 `(MODE==14||MODE==15)&&RASTER_LOD` + legacy 路径 `MODE==15&&!RASTER_LOD` 钳 0（防未绑定描述符读取）。
- 验证：_deploy_stale_spv 重编 17/失败 0/三目录哈希一致；gradlew compileClientJava BUILD SUCCESSFUL；mode=15 推送被 C++ 接受（rt_debug.log）。读数→断链定位后回 mode=14 复验成像。

### bug#2：DH 自渲染盖住我们的输出（根因实锤，旗标已补）
- 诊断局象限不显形 + 三局「原版画面不变」⇒ 远场像素根本不是我们的 K1 分支画的（mode=15 诊断分支会无条件改写远场颜色，看不到象限＝输出被盖）。
- 根因：四旗标组合漏了 **`-Dorion.dh.disableRendering=true`**（DhFlags.DISABLE_DH_RENDERING，默认 false）——DhRenderListHookMixin 注释明说「我们接管几何时正是把 DH 自己的绘制关掉的」，DH 自渲染（apply 合成回 MC 帧缓冲）一直活着，把 K1/mode=14/15 的输出整层盖住。
- 安全性已核：捕获挂 tryTick（每 tick 无条件，与 rendererMode 无关），disableRendering=true 不影响 DH-LODMESH 捕获（实测关绘后增量上传照常：6 节/42710 quad/25ms）。
- **验证组合更新为五旗标**：`-Dorion.dh.rasterLod=true -Dorion.dh.matrices=true -Dorion.dh.farLight.mode=14 -Dorion.dh.lodMesh=true -Dorion.dh.disableRendering=true`。

### bug#3：推送流挂错了事件源（已修，心跳验证过）
- mode 14/15 载荷原本由 DH 的 before-render 事件驱动；实测进世界 ~21 秒后 DH 渲染停摆 ⇒ 事件停发 ⇒ C++ matrixValid=false（rt_debug.log 21:59:52「收到全零矩阵⇒回落 stage-1」）⇒ 后续整局远场裸色。
- 修复：推送改挂引擎相机每帧入口（VulkanCameraMixin→EntityRtCapture.setEngineCameraMatrices 尾部回调→DhFarFieldGeometry.pushEngineMvp）；全零矩阵拒发（设备重建窗口的未初始化相机不误 invalidate）；1200 帧心跳日志。备份 `.bak_20261119_pushflow`。实测：1200 次推送/13 秒，零 invalidate。DH 事件路径保留给 legacy（mode≤13）。
