# Orion 客户端每帧 CPU 工作面地图（Java 侧 → Rust 移植候选分级）

- 生成：2026-11-17。只读调查，未改任何源文件。
- 范围：`D:\fabric-example-mod-26.2\fabric-example-mod-26.2\src\client\java\com\example\` 下 `NativeVulkan.java` 全部 native 方法 + Java 调用点；排除 `compat\dh` 包、`VoxyLodManager`（DH/Voxy 另有 Rust 试点，见 HANDOFF-2026-11-17-DH-OPT-AND-RUST.md §5/§7：`DhFarFieldUploader.convert` 的 Rust 镜像 M2 已完成，A/B 实测 1.15–1.43×，对拍 40/40 PASS）。
- 证据等级：全部行号为当前仓库一手代码；H 为项目交接文档（二手，已与代码对齐）；「推断」单独标注。

## 1. Native 方法 → 调用频率分类

调用点全部位于 `src\client\java\com\example\client\`（mixin 子包在内）。native 声明集中 `src\client\java\com\example\vulkan\NativeVulkan.java`（行号括注）。

| 频率 | 方法 | 调用点 |
|---|---|---|
| 每帧 | updateCamera(19) | mixin\VulkanCameraMixin.java:499 |
| 每帧 | setInWorld | VulkanCameraMixin.java:279 |
| 每帧 | beginEntityFrame / uploadEntityBatch / endEntityFrame | EntityRtCapture.java:249 / 1469 / 383 |
| 每帧 | beginParticleFrame / uploadParticleBatch / endParticleFrame | ParticleRtCapture.java:188 / 359 / 366 |
| 每帧 | uploadDynamicLights | DynamicLightCapture.java:151 |
| 每帧 | setVisibleSections | mixin\LevelRendererMixin.java:172（帧末传 null 于 141/147/154） |
| 每帧 | updateWaterBoats / queueWaterImpulse | VulkanCameraMixin.java:604 / 573,579 |
| 每实体/每模型 | （Java 内部，不发 JNI）resolveBatchMotion / captureVertex | EntityRtCapture.java:810 / 1601 |
| 每粒子（原版提取逐 quad） | （Java 内部）ParticleRtCapture.capture | ParticleRtCapture.java:~340 |
| 事件驱动（每 section 编译） | uploadChunk / uploadCutoutOmmSection / uploadVoxelLightSection | mixin\SectionCompilerMixin.java:491 / 526 / 2382 |
| 事件驱动 | clearChunkLayers | compat\FarFieldGate.java:283,326 |
| 事件驱动 | updateEntityTexture / updateEntityPbrTexture | EntityRtCapture.java:2820 / 2752 |
| 事件驱动 | updateParticleTexture / 雨痕 / 星图 / 末地图集 | ParticleRtCapture.java:724 / 749,781 / 867,977 / 898 |
| 事件驱动 | updateAtlas / removeLightChunk | mixin\VulkanRenderPassAtlasHijackMixin.java:37 / mixin\ClientChunkCacheMixin.java:31 |
| 一次性 | uploadPBRAtlases（图集重载） | mixin\TextureAtlasPBRMixin.java:849 |
| 一次性 | clearWorldGeometry | mixin\MinecraftLevelLifecycleMixin.java:53 |
| 设置页一次性 | 全部 setXxx/getXxx 旋钮、FG/Reflex 查询 | gui\RTXSettingsScreen.java（约 40 处，737–2900+） |
| 【排除】DH | updateDhFarField / publishDhTextures / publishDhBlockRatioAtlas / uploadDhLodSection / removeDhLodSection / clearDhLodSections | compat\dh\DhFarFieldGeometry.java:362,382；DhTextureHandles.java:179,207；DhFarFieldUploader.java:216,220,231,369 |
| 【排除】Voxy | uploadVoxyLodAtlas / uploadVoxyLodMesh / removeVoxyLodMesh / clearVoxyLodMeshes | compat\VoxyLodManager.java:651,667,823,836,876 |

JNI 薄封装判定：settings 类 setter 全部是「纯状态拷贝，不产生 Vulkan 调用」（NativeVulkan.java:236/315/331 等注释）；updateCamera 只写 CameraUBO；纹理句柄类的真活在 C++（mip 生成见 uploadPBRAtlases 注释 NativeVulkan.java:56-63）。这些均不入 Rust 候选。

## 2. 每帧 Java 侧纯计算循环（文件:行 + 量级）

### P1 粒子天气几何生成（buildRtWeather）
- ParticleRtCapture.java:418-570。格网 WEATHER_GRID_SIZE=round(16·√7)=42，MAX_RT_RAIN_DROPS=42²=**1764 格**（:50-56）；MAX_RT_WEATHER_QUADS=3528（:56）。
- 每格：2–3 次 32 位 hash（hashWeatherSeed :646-654 纯函数）+ 相位/抖动/smoothstep 数学（纯）；1 次 `level.getHeight` + 1 次 `getPrecipitationAt`（MC 耦合探针 :462-466）；雨格追加 2 quad、雪格 2 片雪花 1 quad/片。
- 重建节奏：雨中每 50ms 一次（WEATHER_RAIN_REBUILD_INTERVAL_NANOS :139，≈20Hz，即每 3 帧一次）。
- 量级：1764 格 ×（~10 次整数 hash + ~20 flops）+ ≤3528 quad 追加 / 重建。

### P2 粒子顶点流打包（endFrame→emitQuad→vertex/applyWeatherWind→emit）
- ParticleRtCapture.java:351-367（endFrame）→ emitQuad :369-394 → vertex :664（四元数 rotate+scale+平移）→ applyWeatherWind :670 → emit :695（12 float=48B/顶点打包）。
- 每 quad：4×四元数旋转 + 4×风斜 + 6×emit（两三角形）= **6 顶点 × 48B = 288B/quad**。
- 上限：MAX_RT_PARTICLES=256（:45）+ 3528 天气 quad = **3784 quad → 22,704 顶点 ≈ 1.09 MB/帧**（满雨推断，缓冲按此分配 :102-108）。每帧执行；Quad 是自有 record，内核零 MC 对象。

### P3 粒子 nearest 堆（capture）
- ParticleRtCapture.java:~340-352：256 容量最大堆 siftUp/siftDown（O(log256)/粒子），由原版粒子提取逐 quad 调用（天气时每帧数千次）；particlePrevRel WeakHashMap 运动记录 :248-285。MC 耦合高（Particle 对象、ThreadLocal）。

### P4 实体批次运动矢量循环（uploadBatch 内）
- EntityRtCapture.java:1300-1336：每顶点读 3 float（48B 顶点步长 :46）→ 历史差分（vertexHistory）或 3×4 位姿 delta 变换 → 写 3 float MV + 回写 3 float 历史。每帧对全部实体批次执行；顶点数无硬上界（缓冲 768 顶点起、倍增 :3118）。批量、确定性，纯数组/ByteBuffer 操作。
- 量级：全帧实体顶点数 N ×（~15 flops + 24B 读写）。**N 无现成统计（待证）**，常见场景推断 10³–10⁴ 顶点/帧。

### P5 实体顶点打包（capture/emit/共面层偏移）
- EntityRtCapture.java:3218-3253（capture：quad 凑 4 → applyCoplanarLayerOffset :3230-3315 叉积/法线/HashMap coplanarQuadCounts/量化键）→ emit :3332-3357（12 float 打包 + AABB 累积）→ ensureCapacity :3366-3374（倍增 + 全量拷贝）。
- captureVertex :1601-1660：每顶点 hand 3×3 或 projective UV fract；被 CapturingVertexConsumer :3380+ 逐顶点调用；MC 耦合高（Submit/PoseStack/纹理槽）。
- 组缓存命中时整段跳过（skipGroupCapture :1603-1606；groupCache :2373-2406）。

### P6 近水水面探针（findNearbyWaterSurfaceY）
- VulkanCameraMixin.java:140-197：水平半径 96 格、步距 8 → 25×25 列 × 垂直 21 层 = **≤13,125 次 getFluidState/次探针**（:131-134 常量）；500ms 缓存（:135，≈每 30 帧 1 次），命中水柱且距玩家 ≤32 格即早退；另有每柱 ≤64 层爬顶循环 :163-172。方块访问耦合高。

### P7 每帧实体扫描 ×2（水交互 + 动态光）
- VulkanCameraMixin.tickWaterInteractionEntities :513-604：全 entitiesForRendering 扫描，过滤船/掉落物，状态 HashMap，≤8 船 × 5 float 打包（:598-604）。
- DynamicLightCapture.collectEntityLights :236-272：同款全实体扫描（查表未命中 continue）+ collect :181-198 id 排序 + :152-163 ≤32 条 × 32B 打包（1KB/帧）。

### P8 可见 section 打包
- LevelRendererMixin.rtx$uploadVisibleSections :130-176：visibleSections → int[3n]（scratch 预分配 3×2048 :149）；几百 section × 3 int ≈ KB/帧。

### P9 section 编译批处理（事件驱动，突发大；移动时常态数节/帧）
SectionCompilerMixin.java（一次 section×layer 编译内顺序执行）：
- accumulateVptAlbedoAndTagRainPuddles :1675-…：每 quad 质心/叉积法线/归一化 + ≤2 次 region.getBlockState 探针 + 图集像素读（MemoryUtil.memGetByte）+ 16³ voxel 累积；stride 固定 28B（:1691 校验）。
- tagTranslucentQuads :930 / fillUntaggedTranslucentQuads :890：逐 quad 顶点缓冲扫描/标记。
- cullInteriorGlassFaces :1359-1672：逐 quad 邻接判定 + 压缩输出拷贝。
- SectionUploadDedup.hash（调用点 :479-487）：全缓冲哈希（MB 级/section）。
- OmmCutoutClassifier.buildForMinecraftQuads（access\OmmCutoutClassifier.java，调用点 :526）：逐 CUTOUT 三角 4 态分类。
- 量级：每 section 每 layer 数千 quad ×（~30 flops + 1–2 方块查询）；世界加载 = 数百~上千 section 突发；随后移动中常态每帧几节。

### P10 遮挡剔除（异步，非渲染线程）
- cull\EntityVisibilityCuller.java：每 5 帧采集候选（CAPTURE_FRAMES :91）、世界 tick/相机位移触发、守护线程 10ms 轮询（SLEEP_MS :90）、1s 可见保持（:92）、相机 32/20 格迟滞；DDA 核心 OcclusionCullingInstance（LogisticsCraft 移植，:27-29）。已在独立线程 ⇒ 渲染线程收益≈0。

### P11 实体运动配对（每模型）
- EntityRtCapture.resolveBatchMotion :810-908：同类型 tracker 线性配对（×2 轮）+ writePoseDelta :730-765（3×4 矩阵乘）；attachVertexHistory :912。每帧每模型一次，量小。

### P12 相机与状态下发
- VulkanCameraMixin.onRenderLevel :394-509：JOML 2×mat4 求逆 + get 到 float[]（:466-490）、时相/雨量读取；updateCamera :499（19 float）；findPlayerWaterSurfaceY 单柱探针；handRotation 转发 :471-473。µs 级。

## 3. S/A/B/C 分级表

打标三轴：(a) 批量确定性 (b) MC 对象耦合度 (c) 数据量×频率。

| 级 | 候选 | (a) | (b) | (c) | 理由（文件:行） |
|---|---|---|---|---|---|
| **S** | 粒子天气几何生成内核：buildRtWeather 格循环 + endFrame emitQuad/emit 打包 | 高（seed→输出全确定，hash/unitFromSeed/smoothstep 纯函数可逐位对拍） | 低（内核仅 Quad record；getHeight/getPrecipitationAt 两探针可前置数组化） | 大×高（≤3528 quad/重建 ×20Hz + 每帧 22.7k 顶点×48B ≈1.09MB 重打包） | ParticleRtCapture.java:418-570, 351-367, 369-394, 664-707 |
| **A** | VPT 反照率累积 + 雨洼标记 | 高 | 低（getBlockState×≤2/quad 可前置；图集直读指针已平坦） | 大×事件突发（世界加载数百节 + 移动常态） | SectionCompilerMixin.java:1675-1790 |
| **A** | 玻璃内面剔除 + 半透明 quad 标记/填充 | 高 | 低（纯缓冲变换+压缩拷贝） | 大×事件突发 | SectionCompilerMixin.java:890-928, 930-1000, 1359-1672 |
| **A** | CUTOUT OMM 逐三角分类 | 高 | 低（顶点缓冲直读） | 中×事件突发 | access\OmmCutoutClassifier.java（调用点 SectionCompilerMixin.java:526） |
| **A** | 实体批次 MV 循环（历史差分/3×4 变换+历史回写） | 高 | 中（紧贴 uploadEntityBatch 调用点，需缓冲交接改造） | 中×每帧（N=全帧实体顶点，待证） | EntityRtCapture.java:1300-1336 |
| **B** | 近水水面网格探针 | 中（确定性但依赖方块流） | 高（getFluidState 深耦合） | 中×2Hz（≤13,125 查询/次） | VulkanCameraMixin.java:140-197 |
| **B** | 遮挡剔除 DDA | 高 | 高（世界体素访问） | 中×异步（已不占渲染线程） | cull\EntityVisibilityCuller.java（+OcclusionCullingInstance） |
| **B** | 粒子 nearest 堆 + 提取期运动记录 | 中 | 高（Particle/WeakHashMap/ThreadLocal） | 中×每帧（天气时数千次/帧，单次 O(log256)） | ParticleRtCapture.java:~340-352, 248-285 |
| **B** | 动态光采集/排序/打包 | 中 | 高（全实体遍历） | 小×每帧（≤1KB） | DynamicLightCapture.java:140-272 |
| **B** | 实体共面层偏移 + emit 打包 | 中（HashMap 计数/纹理槽查表在环内） | 中 | 中×每帧 | EntityRtCapture.java:3218-3357 |
| **C** | 相机矩阵求逆 + updateCamera 状态下发 | — | — | µs×每帧 | VulkanCameraMixin.java:466-509 |
| **C** | setVisibleSections 打包 | — | — | KB×每帧 | LevelRendererMixin.java:130-176 |
| **C** | 船/掉落物波源、水脉冲 | — | — | ≤40B×每帧 | VulkanCameraMixin.java:513-604 |
| **C** | 纹理句柄/图集上传（真活在 C++ mip/blit） | — | — | 事件 | EntityRtCapture.java:2752,2820；TextureAtlasPBRMixin.java:849 |
| **C** | settings 全量 setter/getter、FG/Reflex | — | — | 设置页一次性 | RTXSettingsScreen.java 各处 |
| **C** | 生命周期清理（clearWorldGeometry/clearChunkLayers/removeLightChunk） | — | — | 一次性/事件 | MinecraftLevelLifecycleMixin.java:53；FarFieldGate.java:283,326；ClientChunkCacheMixin.java:31 |

补充：H 级证据——DH 转换核心已有 Rust M2 实测 1.15–1.43×（HANDOFF-2026-11-17 §7），说明「gather/scatter 型顶点变换」在本项目 JIT 下收益中等；A 级批量项的预期收益与之一致，S 级粒子项因「每帧全量重打包 + 重建」频率更高，预期收益更大（推断）。

## 4. 待证项（未定位/值得跟进）

1. **实体每帧顶点总量**：EntityRtCapture.takeCapturedVertexCount(:2092) 与 LevelPhaseProbe 有探针，跑 PERF L10（ORION_PERF_TIMESTAMPS=1）采样即可量化 P4 占比——决定 A4 是否升 S。查法：跑一局读 `perf/` 输出。
2. **buildRtWeather 中 getHeight/getPrecipitationAt 耗时占比**：profile 后若 <20%，S 级候选可整块移入 Rust；若高，需先做高度/降水数组前置。查法：JFR 或手工 nanoTime 分段。
3. **满雨时天气 quad 是否实际达到 3528 上限**：读 `[RTX-WEATHER]` 类诊断（若无，加临时计数日志）。
4. SectionUploadDedup.hash 的算法与缓冲规模（access\SectionUploadDedup.java）未逐行核——若为 xxhash 变体，Rust 侧可直接对拍。

## 5. 建议下一步

- 先做 S 级（粒子天气内核）+ A 级三项 section 批处理（与 DH P2「帧批合并 JNI」修法同型，可共用 `off_convert_sections` 式批量 ABI，见 rust/orion-farfield/ 既有契约）。
- A4（实体 MV）等待证① 数据出来再定级；B 级不值得当前投入。
