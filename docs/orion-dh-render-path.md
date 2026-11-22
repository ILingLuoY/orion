# Orion DH 远场 LOD 当前完整渲染路径全景图

调查日期：2026-11-19。仓库：D:\fabric-example-mod-26.2（mod 子目录 `fabric-example-mod-26.2\`，下文路径均相对该子目录）。只读调查，未改任何文件。
所有行号均以当前磁盘文件为准；证据等级：一级=当前源码（文件:行），二级=源码内注释/文档，三级=推断（显式标注）。

## 0. 生产档旗标与总开关状态

| 旗标 | 值 | 效果 | 证据 |
|---|---|---|---|
| `-Dorion.dh.lodMesh` | true | Java 抓 DH LOD 网格并上传 RT（本图主线） | src/client/java/com/example/client/compat/dh/DhFlags.java:37；DhFarFieldUploader.java:112 |
| `-Dorion.dh.disableRendering` | true | 关掉 DH 自己的光栅绘制 | DhFlags.java:20；DhApiImpl.java:537（`want = !DISABLE_DH_RENDERING`） |
| `-Dorion.dh.farLight.mode` | -1 | DH 图像合成总开关关闭（rgen 合成块短路） | DhFarFieldGeometry.java:58；rgen_main_body.glsl:8116-8128（`dhFarModeRequested >= 0` 才进合成） |
| `-Dorion.dh.matrices` | false（默认） | Java 不推 DH 矩阵/模式，C++ 侧 `g_dhFarFieldMode` 停留默认 -1 | DhFlags.java:32（默认 false）；DhFarFieldGeometry.java:248（`if (!DhFlags.MATRICES ...) return`）；State.cpp:1519（默认 -1.0f） |

生产档净效果（关键推论，二级证据 = 源码注释 + 代码推导）：`g_dhFarFieldMode = -1`（Textures.cpp:365 只有 Java push 才会改它），因此 WorldGeometry.cpp:3210 的 DH mask 特判不生效，**DH 远场实例保持默认 mask 0xff，主射线（mask 0x01）直接命中远场并走 material_resolve 的 DH 专属着色分支（方案 c 完全体）**；DH 图像合成块（rgen_main_body.glsl:8127-8129 双门控 `dhFarFieldParams.x>0.5 && dhFarMode>=0`）与 rt_sun_shadow_far 远场阴影 pass（Shadow.cpp:694 auto 判据 `g_dhFarFieldMode>0.5`）都关闭。

## 1. 伪节数据流：uploadDhLodSection → BLAS/TLAS

### 1.1 Java 采集与上传
- DhLodMeshSink.java:171/181：`enabledCount()` / `renderCut()`——mixin（DhRenderListHookMixin 等）从 DH 渲染清单抓出已捕获节；「画 = 已捕获 ∩ 清单」，无距离分档（DhLodMeshSink.java:31 注释）。
- DhFarFieldUploader.java:103/111：`tick()`→`tickInner()`；:112 只在 `DhFlags.LOD_MESH` 开时工作；:143-146 增量节流（每 120 帧 / 移动 >32 格 / 首次，DhFlags.java:43/46）；:164 `sync()`→:166 `renderCut()` 得 desired 清单；:174-182 对比 `UPLOADED` 表按版本号（`serial*2+textureBit`）挑增量；:202 `dispatch()` 在单线程 WORKER（:91）里逐节调 **NativeVulkan.uploadDhLodSection(pos, version, originX/Y/Z, width, quads)**（:216/220），撤下走 `removeDhLodSection`（:231）。
- DhFarFieldUploader.java:282 `convert()`：把 DhLodMeshSink 的顶面/侧墙 quad 打成 **64 字节/quad**（4 顶点 × 16B：pos u16×3 + uv u16×2 + 顶点色 rgba 字节 10-13 + material 字节 14 + tileId 字节 6-9）。

### 1.2 JNI 接收与伪节落库（cpp/backend/Lod.cpp）
- Lod.cpp:460 `Java_..._uploadDhLodSection`：拷贝字节数组（:53 `copyVoxyLodArray`）→ `appendDhLodQuads`（:384）展开为 `std::vector<Vertex>` opaque/alpha 两桶 → 加 `g_chunkMutex` 锁（:486）：
  - Lod.cpp:344 `allocateDhLodSpatialKeyLocked`：`mixVoxyLodKey(sectionKey ^ DH_LOD_SALT)`（DH_LOD_SALT=0x44484c4f44484c4f44 见 OrionInternal.h:2613，实际值 `0x44484c4f444c4f44`）& 低 44 位，或上 `VOXY_LOD_RESERVED_Y`（OrionInternal.h:1729，`0x7ffeull<<44`）作 DH/voxy 共用的"LOD 空间键"命名空间；冲突线性探测，DH/voxy 各有 owner 表。
  - Lod.cpp:384-459 `appendDhLodQuads`：字节 14 材质，`MATERIAL_AIR=14` 丢弃（:393）；`MATERIAL_WATER=12` → alpha 桶 + materialId=3.0（:400-404）；其余 opaque + materialId=0。**pad1 = lodMarker = -(3.0+dhMaterial)**（:431，即材质编码 0..15）；tileId≠0 → textured，pad2=0.996，否则 pad2=-1.0（:435-437）；无纹理模式（rendererMode=DISABLED）下全部 tileId=0（rchit_helpers_atlas.glsl:413 注释）。
  - Lod.cpp:496-509：`g_chunkMeshes[spatialKey]=opaque`、`g_chunkMeshes[LOD_AS_CLASS_ALPHA|spatialKey]=alpha`（GEOMETRY_CLASS 层位在键高 4 位，OrionInternal.h；等类常量见 Geometry.cpp:1090-1091 的 packOne）；`g_dhLodSections[section]={version,spatialKey}`（:506，map 定义 State.cpp:2282）；`g_dirtySectionKeys.insert(spatialKey)` + `markWorldGeometryDirty()`（:508-509）。
- Lod.cpp:513 `removeDhLodSection` / :549 `clearDhLodSections`：反向摘除（擦三类键 + 标脏）。Cleanup.cpp:824 世界卸载时清表。
- Lod.cpp:953 `rebuildLodCoverage(orderedSections)`：遍历 TLAS 内全部 RTSectionAS，只对 role==SECTION_ROLE_TERRAIN 的段统计 chunk 柱范围，写 binding 20 的 NEAR_READY 位图（`LOD_COVERAGE_FLAG_NEAR_READY`）——供 shader 判「远场命中是否被近场冗余表示」（lod_coverage.glsl:81-100）。

### 1.3 捕获 → BLAS（伪节）
- WorldGeometry.cpp:11 `bool rebuildWorldGeometry(size_t maxSectionUpdates)` 是唯一构建入口。:548 每次先 `refreshDhLodSpatialSnapshotLocked()`（Lod.cpp:372-383，把 `g_dhLodSections` 的 spatialKey 排序快照进 `g_dhLodSpatialSnapshot`，供 :3211 `isDhLodSpatialKey` 二分查询，Lod.cpp:363-371）。
- 增量批次：按 `g_dirtySectionKeys` 枚举脏 spatialKey（WorldGeometry.cpp:721-724 插入、Geometry.cpp:1999-2067 主枚举循环：解键、视锥/距离过滤、`geometryCaptureSectionLocked(spatialKey, split)`，Geometry.cpp:2030-2067）；实体的动态快路见 WorldGeometry.cpp:251/473/509。
- Geometry.cpp:1102 `geometryCaptureSectionLocked`：**分类路由唯一实现**（注释 :1094-1098 列三个消费者：捕获线程 geometryCaptureWorkerLoop / 增量批次 / forceReset 全量），按 16 个 layer 从 `g_chunkMeshes` 取 `(layerIndex<<60)|spatialKey` 分桶打包（:1090-1091 packOne opaque/alpha/water）。
- Sections.cpp:2660 `createSectionBLAS(section, sectionKey, vertices, opaqueGeometry, omm, motion, allowUpdate)`：每节一个 BLAS（伪节 = 一个 DH spatialKey 的顶点桶）；内部分段计时注释（:2664-2666）：**单段实测 1.7~3.0ms**；indexed 判定 `sectionUsesIndexedGeometry`（:2707；与 Geometry.cpp:1052 注释同源）。调用点 WorldGeometry.cpp:1369。
- 角色表：Sections.cpp:3962 `writeSectionRoleSlot(section)` 把 `sectionRoleOf(section)`（Geometry.cpp:3054-3068：`isDhLodSpatialKey(instanceSpatialKey) ? SECTION_ROLE_DH_FAR(2) : VOXY/LOD 判定…`，角色常量 OrionInternal.h:1088-1090）写入 binding 19 的 GPU 表 `[vertexAddressSlot]`；带影子数组去重 write-combined 写（Sections.cpp:3976-4008，注释实测 ord 段 3.04ms/6000 段）。槽位容量 SECTION_VERTEX_ADDRESS_SLOT_LIMIT=1<<18（OrionInternal.h:1087）。

### 1.4 TLAS 实例
- WorldGeometry.cpp:2078 `MAX_TLAS_CUSTOM_INDEX_COUNT` 与 :2088「Section count exceeds instanceCustomIndex 24-bit capacity」硬限。
- 实例循环（世界段）WorldGeometry.cpp:3200-3250：
  - :3200 `instance.instanceCustomIndex = section.vertexAddressSlot`（shader 侧 slot = instanceCustomIndex + geometryIndex，lod_coverage.glsl:28-30）；
  - :3202 默认 `instance.mask = 0xff`；
  - **:3210-3214 DH 特判**：`g_dhFarFieldMode > 0.5f && isDhLodSpatialKey(...)` → `mask = 0xfe`（⚠ 生产档 mode=-1 不触发）；
  - :3219-3226 LOD 键之外 alpha 类 → 0xfb；:3228+ 玩家 0x02、手 0x04、粒子 0xf7；:3230 `primaryMaskCull` 时 `mask &= 0xfe`；
  - SBT 偏移按几何类选 SBT_RECORD_OPAQUE/OMM/…（:3246 之后）。
- TLAS 构建：WorldGeometry.cpp:3590-3635 build info；**:4074-4131** `dstAccelerationStructure` + `updateTlas = tlasCanUpdate` 时 mode=UPDATE（复用），否则 BUILD；诊断日志 :4131。非在册段写 `inactive.instanceCustomIndex=0` 的哑实例（:2849/:3372），排序稳定性 :3406-3408。

## 2. 远场几何参与的 pass / 射线类型与 mask 语义

### 2.1 mask 语义表（实例掩码 × 射线掩码，命中条件 = 按位与≠0）
| 实例掩码 | 谁设置 | 含义 |
|---|---|---|
| 0xff | WorldGeometry.cpp:3202 默认 | 所有射线可见 |
| 0xfe | :3213（仅 farLight.mode>0.5 时 DH） | 排除 bit0 ⇒ 相机主射线不可见、次级射线可见；rgen_main_body.glsl:8404-8408/8717 注释「实例掩码 0xfe ⇒ 相机不可见、次级射线可见」 |
| 0xfb | :3226 alpha 类 | 排除 bit2（手） |
| 0x02 | :3233 本地玩家 | 只被含 bit1 的射线看见 |
| 0x04 | :3240 手 | 只被 0x05 主相机手部射线命中（:3236-3238 注释） |
| 0xf7 | :3246 粒子 | 排除 bit3（雨条射线族 0x08，rgen_main_body.glsl:3793-3797） |
| primaryMaskCull &0xfe | :3230 | 主射线剔除档 |

射线掩码：主射线 **0x01**（rgen_main_body.glsl:426/439 `traceRayEXT(... 0x01 ...)`，SER 同参）；次级/GI/反射/水/本地光 **0xff**（rgen_main_body.glsl:5845/5851 GI、rgen_glossy.glsl:288/301/1136/1244、rgen_receiver_gi.glsl:56/69、rgen_secondary_lights.glsl:463/479、rgen_local_lights.glsl:3155/3171、rgen_water_receiver.glsl:134/526、rgen_glass_primary.glsl:67）。雨条 0x08（rgen_main_body.glsl:3797）。

### 2.2 生产档（mode=-1）下各 pass 的远场参与情况
- **主射线（不透明像素）**：直接命中 DH 实例（mask 0xff）→ rchit → callable `material_resolve.glsl` DH 分支（§3）。
- **GI**：rgen_main_body.glsl:5845/5851 mask 0xff → 远场参与 GI 弹射（material_resolve 注释 :610-612：「自动参与 GI / 阴影 / 反射 / 体积光」）。
- **太阳直射阴影**：rgen_sun_lighting 的阴影射线 mask 0xff（同一 TLAS），可被远场遮挡；专用远场阴影 pass `rt_sun_shadow_far.comp` 由 Shadow.cpp:670-707 `sunShadowFarEnabled()` 门控——未设 ORION_SUNSHADOW_FAR 时 auto = `g_dhFarFieldMode>0.5`，**生产档自动关**（注释 :674-676：没 DH 时 1024² 射线全 miss 纯开销）。
- **反射（glossy）/水面/玻璃**：rgen_glossy / rgen_water_receiver / rgen_glass_reflection 0xff → 远场可命中。
- **本地光/次级光 NEE**：rgen_local_lights / rgen_secondary_lights 0xff。
- **雨条**（0x08）：DH 实例含 bit3 → 会被雨条射线命中（若启用）。
- **DH 图像合成块**（rgen_main_body.glsl:8075-8790，旧 stage-B 路径）：门控 `dhFarFieldParams.x>0.5（=dhTexturesAvailable，Frame.cpp:487）&& dhFarModeRequested>=0（Frame.cpp:498 推 mode）&& dhFarFieldOurs>0.5（主射线 miss，rgen_main_body.glsl:494-501）`。生产档两个门都关（mode=-1 且 textures 未发布）。mode 语义全表见 DhFarFieldGeometry.java:255 push() 日志：-1 总关 / 0 直通 / 1 误差图 / 2 法线 / 3 重打光 / 4 重打光+大气 / 5-11 诊断 / 12 直通+大气 / 13 方向采样；rgen_dh_far.glsl:53-56 同源。
- **冗余剔除**：lod_coverage.glsl:81-100 `lodHitIsRedundant`——role==DH_FAR 且命中点所在 chunk 柱已 NEAR_READY（binding 20，Lod.cpp:953 写）→ 调用方 ignoreIntersectionEXT（近场已就绪时远场让位）。

## 3. 远场命中后的着色路径（material_resolve DH 分支）

入口：`material_resolve.glsl:620` `if(isDhLod && !isParticleMaterial && !isEntityMaterial)`。
- **识别**：material_resolve.glsl:225-237——角色表回答 isDhLod（不再靠 pad1 暗号）；`dhLodMaterial = clamp(round(-voxyLodMarker)-3, 0, 15)`，voxyLodMarker = 三顶点 pad1 重心插值（:220-223）。角色表消费端 `lodInstanceRole()`：lod_coverage.glsl:16-33（slot=instanceCustomIndex+geometryIndex，越界回 TERRAIN）。callable TU 用 rchit_helpers_pom.glsl:1053 的 materialLodRole() 等价实现（lod_coverage.glsl:23-26 注释）。
- **材质分类** material_resolve.glsl:624-629：0 UNKNOWN / 1 LEAVES / 4 METAL / 6 LAVA / 8 SNOW / 12 WATER / 15 ILLUMINATED，其余默认档。
- **albedo**：:637-643 `dhLodAlbedo(floor(texCoord.x+0.5), floor(texCoord.y+0.5), hitPos, geoNormal, interpColor.rgb)`（texCoord 由 appendDhLodQuads 打包的 (tileId, normalIndex) 来，Lod.cpp:442-443）。实现 rchit_helpers_atlas.glsl:521-540：
  - tileId==0（无图集/未发布）：顶点色 + `dhFakeTextureNoise`（:439-462，复刻 DH applyNoise：世界坐标格点噪声，强度 0.18/步数 8/1024 格淡出，刻意超出 DH 默认 0.05/4，注释 :411-415）；
  - tileId!=0：DH blockRatioAtlas（binding 49，C++ ensureDhTextureDescriptors 绑 47+，Lod.cpp:787/862），`dhBlockMip`（:388-393）按方块屏幕尺寸取 mip、`dhFaceUv`（:393-403）定位 tile；
  - 两种都过 `dhLodColorCompensation`（:501-517）：96→768 格从增益 0.85/饱和 1.0 过渡到 0.65/1.45（:492-497），补偿 DH 顶点色线性均值偏亮。
- **法线**：:646-667 背面翻转；水且 |n.y|>0.5 顶面用 `evaluateWaterWaveSurface`（:672-691，与近场同一波浪函数，防远近波相错位）。
- **BSDF 参数**：:706-733——水 roughness 0.08；岩浆 0.92 + `emission=albedo*4.0`；发光方块(15) 0.86 + `albedo*4.0`（注释：真命中辐射参与 GI，夜间远场光源真实照亮周围）；金属 0.35 + F0=230/255（LabPBR 金属码）；树叶 0.92（近似 porosity）；雪 0.45。
- **baked light（烘焙方块光）**：:740 `payload.emission = dhEmission + dhBakedBlockLight(dhAlbedo, interpColor.a)`；dhBakedBlockLight 定义 rchit_helpers_atlas.glsl:569，输入是顶点 alpha 字节（顶点字节 13 = blockLight，Lod.cpp:447 打包）。
- **payload 契约**：:693-708 写 hitDistance/parallaxRayTravel/pom*/normal/albedoPacked/glassAlpha/state——水 → `RT_HIT_WATER` + 水面交接枚举（RT_WATER_AIR_TO_WATER 等），非水 → `RT_HIT_OPAQUE`。玻璃路径同款分支在 material_resolve_glass.glsl:337（dhLodAlbedo 复用）。

## 4. TLAS/BLAS 每帧重建/更新的开销点

- **调度（每帧两腿）** Frame.cpp:
  - :1975 动态实体腿（dynRebuildDue）每帧调 `rebuildWorldGeometry(SECTION_REBUILD_MAX_SECTIONS_PER_BATCH)`，失败置 `g_entityRebuildRequested` 下帧重试；
  - :2028 世界腿：仅当 revision 变化后满足 `SECTION_REBUILD_QUIET_FRAMES(10 帧) 静默窗` 或 `SECTION_REBUILD_MAX_DEFERRAL_FRAMES(60 帧) 硬限`（Frame.cpp:1994-2026；常量 OrionInternal.h:317-318）或 interrupted 续跑才调；rtxGeometryRebuildMs 计时 :1983-1987。
- **批次上限**：SECTION_REBUILD_MAX_SECTIONS_PER_BATCH = 24（OrionInternal.h:319）；自适应 `g_sectionRebuildBatchCap`：完成一批 +2、封顶 24（WorldGeometry.cpp:1743-1749；初值 State.cpp:106）。
- **锁竞争**：WorldGeometry.cpp:80-120 pre-loop `g_chunkMutex.try_lock_for(geometryPreLoopLockWaitMs)`，5 次 skip 后回退 SECTION_PRELOOP_LOCK_FALLBACK_MS——与 Java `uploadDhLodSection` 持同一把锁（Lod.cpp:486），DH 上传高峰会直接吃掉这段预算。
- **fence 门**：WorldGeometry.cpp:36-77 上一批 retirement fence 未 ready（等待上限 SECTION_REBUILD_FENCE_WAIT_MS=2ms，OrionInternal.h:320）→ 整帧 defer（`g_geometryFenceDeferredCount`）。
- **捕获成本**：geometryCaptureWorkerLoop 后台线程（WorldGeometry.cpp:24 `ensureGeometryCaptureWorker`；Geometry.cpp:1770-1800 唤醒式循环 + split 池 :350/456 注释「take 推迟到捕获前」防饿死），捕获与 BLAS 构建解耦。
- **BLAS 构建**：Sections.cpp:2660-2740，每段 1.7~3.0ms（:2664 注释），内分 buffer/vkBuf/prep/omm/req/sizes/mem/as/addr 九相计时（:2669-2677），慢段走 [RT SECTION SLOW] 归因；[RT BLD BREAKDOWN] 日志 WorldGeometry.cpp:1700-1711。
- **角色表 WC 写**：Sections.cpp:3962-4008 影子去重（否则 6000 段 ≈ 每次 0.5µs × 段数的纯 WC 刷写，注释实测 ord 段 3.04ms）。
- **实例写 + 剔除遍历**：每帧对候选段做「解 spatialKey → 求中心 → 太阳阴影保留盒（9 次乘加）→ 视锥 5 平面」（WorldGeometry.cpp:2413 注释；平面缓存 :2093-2185）；DH 实例额外过 `isDhLodSpatialKey` 二分（:3211，快照由 :548 刷新）。
- **TLAS**：WorldGeometry.cpp:4074-4131——instance 数组变化才 BUILD，否则 UPDATE（`tlasCanUpdate`）；哑实例占位保持 count 稳定（:2849/:3372）。
- **远场阴影纯开销防线**：Shadow.cpp:670-707——mode=-1 时 rt_sun_shadow_far（1024² 射线）不派发，UBO w 置 0、rgen 远场分支全短路（:665-666 注释）。

## 5. 把远场从 TLAS 摘出改走光栅：需要动的挂载点

保留不动：Java 采集/上传链（DhLodMeshSink → DhFarFieldUploader → uploadDhLodSection JNI；或反向：直接重开 DH 自绘，见路线 B）。

**路线 A：仍用我方网格，改为光栅栅格化（新 GBuffer/光栅 pass）**
1. cpp/backend/Lod.cpp:460-512 `uploadDhLodSection`——不再把顶点写进 `g_chunkMeshes`（RT 顶点流），改写独立光栅顶点/索引缓冲（pad1/pad2 材质编码可原样带过去）；:508 `g_dirtySectionKeys.insert` 与 `markWorldGeometryDirty()` 改为光栅侧脏标。
2. cpp/backend/Geometry.cpp:1102 `geometryCaptureSectionLocked`（注释 :1094-1098）与 :1999-2067 增量枚举——按 `isDhLodSpatialKey`（Lod.cpp:363）跳过 DH 键，DH 不再进 BLAS。
3. cpp/backend/WorldGeometry.cpp:548 快照刷新与 :3210-3214 DH mask 特判——随之删除/短路。
4. cpp/backend/Sections.cpp:3962 `writeSectionRoleSlot` + Geometry.cpp:3054 `sectionRoleOf`——DH 角色不再出现在 binding 19。
5. cpp/backend/Lod.cpp:953 `rebuildLodCoverage` 与 shaders/lod_coverage.glsl:81-100 `lodHitIsRedundant`——远场冗余剔除随 RT 摘除而失效（光栅需等价的近场让位逻辑或删除）。
6. shaders/material_resolve.glsl:620-742 DH 命中分支（及 material_resolve_glass.glsl:337、rchit_helpers_atlas.glsl:521/569 两个助手）——退役或移植成光栅片元着色器（albedo/噪声/补偿/baked light 公式可整体复用）。
7. 相机可见远场由新光栅 pass 输出：需接 RT 合成后端的 GBuffer/深度（Frame.cpp 派发处），并把 rgen_main_body.glsl:426/439 主射线 miss 后的远场兜底语义对齐（主射线不再打远场 ⇒ miss 像素须由光栅深度判归属，rgen_main_body.glsl:8116-8129 的合成门是现成挂点）。
8. 阴影：Shadow.cpp:694 `sunShadowFarEnabled()`——若希望远场继续投影，需 ORION_SUNSHADOW_FAR=1 强制开或新增光栅 shadow map；否则远场只留 GI/反射遮蔽贡献也一并消失，需在 §2.2 各 0xff 射线消费者评估回退。

**路线 B（现成路径，零新代码）：退回 DH 图像合成路**
- Java：DhFlags.java:32/20 改 `-Dorion.dh.matrices=true` + `disableRendering=false`（或保留 disable 并只开 MATRICES；DhApiImpl.java:211 纹理发布需 PUBLISH_TEXTURES）；DhFarFieldGeometry.java:255 `farLight.mode` 设 4/12/13。
- C++ 侧自动接通：Textures.cpp:365 接收 push；Frame.cpp:487-501 打包 dhFarFieldParams/dhFarFieldLight/矩阵进 UBO；rgen_main_body.glsl:8127-8790 合成块（重打光/大气/护栏兜底）与 rgen_dh_far.glsl 反投影恢复；TLAS 里的 DH 实例可同时保留（mode>0.5 时转 0xfe 退居次级射线，WorldGeometry.cpp:3213）或用 lodMesh=false 摘掉（DhFlags.java:37）。

## 6. 待证项
- `dhBakedBlockLight`（rchit_helpers_atlas.glsl:569）内部公式未逐行核（是否含 *15 归一化/平方衰减），引用时按「顶点 alpha 即 blockLight」处理。
- WorldGeometry.cpp 实例循环中 DH 键的「视锥/距离剔除」是否对 LOD 键有特殊放宽（Geometry.cpp:1833-1834 注释提到 voxy/DH 与实体统一哈希槽位处理），细节未逐行验证。
- SECTION_AS_CLASS_* 各类到 SBT record 的完整映射（:3246 之后）未读完，与本图主线无冲突。
