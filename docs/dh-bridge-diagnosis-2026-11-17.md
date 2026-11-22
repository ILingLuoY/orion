# Distant Horizons 集成诊断报告（Orion RT × DH 3.3.x）

- 日期：2026-11-17；工作区：D:\fabric-example-mod-26.2（mod 在子目录 fabric-example-mod-26.2\，下文路径均相对该子目录）
- 性质：只读诊断，未改动任何项目文件。
- 方法：通读 compat/dh 全部 Java、FarFieldGate/FarFieldWindow/VoxyLodManager、NativeVulkan DH 段；C++ Lod.cpp/Geometry.cpp/WorldGeometry.cpp/Frame.cpp/Shadow.cpp 与 rchit_helpers_atlas.glsl/material_resolve.glsl/lod_coverage.glsl；历史证据取自 .idea/runConfigurations/Minecraft_Client.xml 注释、docs/、CHANGELOG.md、run/logs/latest.log。近两周日志（11-14~11-17）均无 DH 会话（DH 不在 run/mods），历史性能数字以代码内注释记录的 10-01 A/B 为准。

## 0. 运行形态确认（先钉死前提）

1. 生产档是"我方 TLAS 画远场"：Minecraft_Client.xml 的 VM_PARAMETERS = `-Dorion.dh.lodMesh=true -Dorion.dh.lodHook=true -Dorion.dh.disableRendering=true -Dorion.dh.ownAtmosphere=true -Dorion.dh.publishTextures=true -Dorion.dh.matrices=false -Dorion.dh.farLight.mode=-1`。即：DH 只管网格化，几何进我方 TLAS，DH 自身光栅渲染关闭，stage-B 图像合成（mode 12/13）关闭。
2. 当前本机 DH 通路整体关闭：run/mods 无 DH；3.3.2 在 run/_disabled_mods，3.3.4 被放在 run/shaders（不是 mod 加载目录，不会生效）。latest.log（2026-11-17 09:32）："…[DH-BRIDGE] 未检测到 Distant Horizons，远场通路保持关闭"。要复测症状必须把 3.3.4 移回 run/mods（3.3.4 ≥ API 7 达标，DhBridge.MIN_API_MAJOR=7，DhBridge.java）。
3. 依赖隔离正确：build.gradle:103 DH API 为 compileOnly；:120-124 mixin 编译用 run/mods（缺省回落 cpp/shaders）的 DH core jar；未装 DH 时 DhMixinPlugin 整套跳过（DhMixinPlugin.java:36-44）。
4. "内容指纹没变就跳过、任一节变了全量重来"的旧失效路径已不存在：10-01 重构为 per-section 增量（DhFarFieldUploader.java 类注释与 sync()/dispatch()），每节独立 spatialKey/BLAS（Lod.cpp:344-511），C++ 按 version 拒旧（Lod.cpp:494-498）。

## 1. 问题清单——①明显卡顿

**P1｜DH 远场以 487 万 quad / 数千 BLAS+实例常驻 TLAS，所有射线类型遍历变贵（高置信）**
- 证据：Minecraft_Client.xml 10-01 A/B 注释："A 臂 = lodMesh=true（远场 487 万 quad 在 TLAS 里）；B 臂 = lodMesh=false → rt median=3.21ms"。A 臂的实测值未记录（当时中止）。
- 证据：WorldGeometry.cpp:3204-3215——生产档 `farLight.mode=-1` ⇒ `g_dhFarFieldMode>0.5` 不成立 ⇒ DH 实例 `mask=0xff`，参与主射线/阴影/GI/反射全部射线类型（仅 stage-B 合成档才把 DH 实例从相机射线摘出 mask=0xfe）。
- 证据：TLAS 有"实例集未变则跳过"（[TLAS SKIP]，WorldGeometry.cpp:4340-4391 附近），但 DH 增量上传每次都改实例集 ⇒ 移动期间 TLAS 频繁重建，实例规模由远场撑大。
- 优化（改动面：中）：a) 把 DH 实例对次级射线降权——阴影/GI/反射射线用独立 mask 把 ≥某距离的 DH 实例排除或降采样；b) 远场环半径硬上限 lodMaxRadiusChunks 24（=784 格）先砍半做 A/B；c) 合并 BLAS：把同 detail level 相邻 4~8 节合成一个 BLAS，把实例数从数千压到数百（TLAS 构建与遍历双收益）。

**P2｜Java 侧单笔上传任务无节内上限，C++ 侧每帧 24 节的重建上限追赶不及（高置信）**
- 证据：DhFarFieldUploader.java:202-232——dispatch() 把本轮全部变更节塞进一个 worker 任务，逐节 convert + JNI，无节数/字节上限；触发条件为每 120 帧（DhFlags.java:35）或移动 ≥32 格（:39）或场上无网格；JOB_IN_FLIGHT 只防堆积，不切分。
- 证据：OrionInternal.h:302 `SECTION_REBUILD_MAX_SECTIONS_PER_BATCH = 24`；Frame.cpp:1961/2014 每次重建最多消化 24 节 ⇒ 数百节的变更要拖 10+ 帧的 BLAS churn，期间实例集每帧都在变，[TLAS SKIP] 失效。
- 证据：每节 JNI `uploadDhLodSection` 都要拿 `g_chunkMutex`（Lod.cpp:488）并 `markWorldGeometryDirty()`；worker 串行大批量上传 ⇒ 渲染线程 rebuildWorldGeometry 入口的 `try_lock_for` 预算与 `SECTION_REBUILD_FENCE_WAIT_MS=2ms` 栅栏等待（WorldGeometry.cpp:70-100、OrionInternal.h:303）开始命中。
- 优化（改动面：小-中）：dispatch 按 8~16 节/帧分帧派发（或按字节预算），近处节优先；把"每节一次 JNI+锁"改为帧批合并接口（一次锁内提交 N 节）。

**P3｜DhLodMeshSink 超预算路径 O(N²)（中置信）**
- 证据：DhLodMeshSink.java:87 预算 512MB；:294-312 makeRoom 每入一节全表线性扫；:214-240 prune() 全表收集+排序。仅在超预算时发生，但 DH 重构建风暴期恰是超预算期。
- 优化（改动面：小）：makeRoom 用"按距离预排序的弱堆/索引"替代全表扫；预算 512MB 对 256 视距可下调（1024 格外本就 d≥4 粗层，字节占比小）。

**P4｜近场压制链路对 DH 空转（接线缺口，高置信——不是卡顿源，但压住了远场收益/成本平衡）**
- 证据：FarFieldGate.java:243-256（shouldSuppress）与 :315/:325（updateNearby 恢复/擦除判据）都调用 `VoxyLodManager.coversSection(...)`；而覆盖表只由 Voxy 上传喂（VoxyLodManager.java:101-103、141-155 refreshCoverage 只看 VoxyCompat.meshes/UPLOADED_VERSIONS）。Voxy 已弃 ⇒ coversSection 恒 false ⇒ DH 在供时近场 section 依旧全量进 RT。
- 后果：不产生同深度打架（内洞 DhLodMeshSink.insideInnerHole + binding 19/20 角色表与覆盖位图去重兜底，lod_coverage.glsl:68-73 anyhit ignore），但"缝外擦原版、由 LOD 接管"的成本置换从未发生——近场原版几何与 DH 远场几何同时常驻 TLAS。
- 优化（改动面：中）：给 DH 通路实现等价 coversSection（按 DH 启用清单×64 格 cell 生成），或把 FarFieldGate 的覆盖判定改为按 provider 分发。

**P5｜探针与每 tick 抄清单的固定开销（低置信，非主因）**
- 证据：DhApiImpl.java:621-660 probeHandles 每 30 帧重探、上限 60 次；disableRendering=true 时 color/depth 永不就绪 ⇒ 前 ~1800 帧持续反射 dumpObject + System.err 打日志。DhRenderListHookMixin.java:69-79 每 tick 全量拷 DH 启用清单（千级 long[]）。
- 优化（改动面：小）：disableRendering 时跳过 color/depth 探针；清单拷贝改增量/复用缓冲。

**P6｜DH 自身线程争用（低置信，量级待测）**
- 证据：DhLodBufferHookMixin.java:41-56 挂在 LodBufferContainer.tryMakeAndUploadBuffersAsync HEAD，当场拷字节（池化 buffer 必须立刻拷）；拷贝发生在 DH 的缓冲构建线程，量级 = 暂存字节总量（≤512MB/全场，分摊）。未发现 Java 侧锁竞争渲染线程的路径（共享状态均为无锁结构，DhLodMeshSink.java"线程"注释）。
- 待证：DH 3.3.2→3.3.4 的构建线程数与批次大小变化。

## 2. 问题清单——②LOD 区域异常变亮

**P7｜远场命中走与近场同一套 RT 直射+GI，但遮蔽数据系统性缺失（高置信，机制性根因）**
- 证据：material_resolve.glsl:442-731（DH 分支输出与近场同构的 payload：albedo=补偿后顶点色/图集色，state=RT_HIT_OPAQUE/WATER）⇒ rgen 用同一套直射+环境+GI 照它。
- 证据 a：DH 顶点色是**未阴影**的原始方块色×生物群系染色（DH 自己的 lodShading/亮度/雾全部不跑：disableRendering=true）；近场 albedo 同样未阴影，但近场有 RTAO/GI/软影全量档，远场被距离降档：软影 tap ≥32 格 8→4/2（rgen_main_body.glsl:3045-3072，docs/handoff-20261111.md 第 5 条）、GI ≥64 格降速（docs/env-switches.md ORION_GI_NEAR_RATE 行）。粗 LOD 没有洼地/植被细节 ⇒ AO 射线大面积 miss ⇒ 天空光全额进入。
- 证据 b：现有补偿只压增益不补遮蔽：rchit_helpers_atlas.glsl:496-546 `DH_LOD_NEAR_GAIN=0.85 / DH_LOD_FAR_GAIN=0.65`，注释明说"刻意不压到与近场同亮——只削掉大部分爬升"。
- 优化（改动面：中）：给远场单独的环境项——按顶点 normalIndex（面序）+ 高度差做解析 AO/天空可见近似，或把 DH 的 lodShading 面朝向系数折算乘回 albedo；把 DH_LOD_FAR_GAIN 再降并配合远场专用 ambient 下限，用 mode 9 左右分屏 A/B 校准（Minecraft_Client.xml 注释给的现成判据）。

**P8｜LOD 材质颗粒度扩散把发光/岩浆放大成片（中置信）**
- 证据：material_resolve.glsl:624-629/704-712——dhIsLit(ILLUMINATED=15)/dhIsLava(6) ⇒ `emission = dhAlbedo*4.0`；DH 的材质是列主导方块聚合，粗层一个 quad 可能覆盖 4×4~64×64 格，只要主导材质是发光方块整个 quad 发光；DH_BAKED_BLOCK_LIGHT 加性项（rchit_helpers_atlas.glsl:549-580，DH_BAKED_BLOCK_GAIN=0.25）按顶点字节 13 同样逐 quad 生效。
- 优化（改动面：小）：emission 按 quad 面积/覆盖格数稀释（如 ×16/width），或仅当 tileId 采样命中发光纹理时给 emission；baked blockLight 对 d≥2 层按距离衰减。

**P9｜图集 tint×2 重标定对亮纹理过曝（中置信，条件性）**
- 证据：rchit_helpers_atlas.glsl:533-541——textured 时 `tinted = vertexColor * (tile.rgb * 2.0)`；×2 是对 blockRatioAtlas"占比图"偏暗的拟合，亮纹理（沙/雪）会被推到 clamp(1.0) ⇒ 远场偏白。atlas 未就绪时走 dhFakeTextureNoise 顶点色路径（:524-528），无此项。
- 优化（改动面：小）：把 ×2 改为按 tile 直方图自适应，或上限 1.5；与 P7 的增益一起用 mode 9 校准。

**P10｜已撤回的历史暗坑（防回退记录，确定）**
- 远场太阳阴影图路线 A 曾把远场整片压黑（run config 注释：深度比较对表面采样点恒判全遮挡、背面不剔除）⇒ ORION_SUNSHADOW_FAR=0（Minecraft_Client.xml envs）。当前变亮与它无关，但重启用它前必须先改生产者从向阳侧反向追踪（sun_shadow.glsl 顶部契约）。
- 追加曝光失稳的教训：A/B 两臂 GPU 成本不同会驱动 rr_ae 自动曝光摆动（ORION_PERF_ABLATION 注释 2026-10-14）⇒ 校准 P7-P9 时不要与消融开关同开。

## 3. 待证项（未定位/未量化，附查法）
1. A 臂（far field in TLAS）的 rt 相位实测值：仓库只记了 B 臂 3.21ms。查法：装回 DH 3.3.4 + lodMesh=true 跑 [PERF L10 GPU]（ORION_PERF_TIMESTAMPS=1 已开），对比 far/vli/rt 段。
2. 一次移动 32 格后的实际"变更节"分布（P2 的真实批量）：grep run/logs/latest.log 的 "[DH-LODMESH] 增量：上传 N 节"与"层级 d*=…"。
3. binding 49 DH 图集在生产档的最终状态（"仅 atlas"三态）：run/rt_debug.log grep "[DH TEX]"。
4. DH 3.3.4 相对 3.3.2 的 LodBufferContainer 签名/线程模型是否漂移（mixin 挂点软失效风险）：反编译对照 tryMakeAndUploadBuffersAsync。

## 4. 建议的下一步（按性价比排序）
1. 先把 3.3.4 移回 run/mods 复现两症状，取 P-待证 1/2/3 的实测数（一切调参的前提）。
2. P2 分帧派发（Java，改动面最小、直接削卡顿尖峰）。
3. P1 的实例 mask 分射线 + 环半径 A/B；P1c 合并 BLAS 作为中期项。
4. P7 远场环境项专用化 + P8/P9 两个小修，一起用 mode 9/11 的现成 A/B 档校准。
5. P4 接通 DH 的近场压制（收益最大但动面最宽，放最后）。