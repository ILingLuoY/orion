# Distant Horizons API 调查报告
（供 MC 26.3 Fabric 光追 mod 集成参考 · 调查日期 2026-11-17）

## 0. 调查对象与手段
- API jar：`D:\fabric-example-mod-26.2\fabric-example-mod-26.2\libs\DistantHorizonsApi-7.2.0.jar`（内含 155 个 .java 源文件 + .class，已解包到 `C:\Users\Public\dh-api-src\`）
- Mod jar：`D:\fabric-example-mod-26.2\fabric-example-mod-26.2\run\shaders\DistantHorizons-3.3.4-26.3-fabric-neoforge.jar`
- 工具：`D:\RTX\bin\jar.exe` / `D:\RTX\bin\javap.exe`（JDK 26）；网上核查：GitLab 官方仓库/Wiki/Modrinth
- 伴随证据文件（均 UTF-8）：`C:\Users\Public\dh-api-classes.txt`（API jar 全清单 367 行）、`dh-mod-classes.txt`（mod jar 全清单 2196 行）、`dh-mod-render-classes.txt`（渲染相关 369 行）、`dh-api-javap.txt`（34 个关键类 javap）、`dh-modinfo-verbose.txt`、`dh-wiki-pages.json`（官方 Wiki 19 页全文）、`dh-modrinth-263.json`（Modrinth 26.3 版本查询）

---

## 1. 版本配对（置信等级：确定，本地铁证）

| jar | ModInfo.VERSION | API 版本常量 | CONFIG_FILE_VERSION |
|---|---|---|---|
| `DistantHorizonsApi-7.2.0.jar` | `"3.3.2"` | MAJOR=7, MINOR=2, PATCH=0 | 4 |
| `DistantHorizons-3.3.4-26.3-fabric-neoforge.jar` | `"3.3.4"` | MAJOR=7, MINOR=2, PATCH=0 | 5 |

- 证据：两 jar 内 `com.seibel.distanthorizons.coreapi.ModInfo` 常量（`javap -v` / `javap -constants`）。API jar 的 `fabric.mod.json` 仅是占位（id `dh-api`，"Don't try running this jar in Minecraft"）。
- Mod jar `fabric.mod.json`：`"id": "distanthorizons"`，`"version": "3.3.4"`，`depends.minecraft = ["26.3"]`，`java >= 25`，`license: "LGPL-3"`，`contact.sources = https://gitlab.com/distant-horizons-team/distant-horizons/`；内嵌 181 个 `com/seibel/distanthorizons/api/**` 类（含完整 API 与实现）。
- **结论：DH 3.3.2 与 3.3.4 使用同一 API 版本 7.2.0。现有 API jar 7.2.0 编译产物对 3.3.4 完全可用，不需要更新 API jar。**
- MC 26.3 兼容版本线（Modrinth API 实测 `game_versions=26.3`，2026-11-17）：`3.3.4-26.3`、`3.3.3-26.3`、`3.3.2-26.3`、`3.3.0-26.3`，全部 fabric/neoforge 合体 jar。
- API 版本号语义（`DhApi.getApiMajorVersion()` javadoc）：major 只在破坏性变更时更新；minor 在新增方法时更新；patch 为非破坏修复。即 API 7.2.0 编译的代码可运行于 API ≥7.2.0 的任意 DH 版本；只有 DH 未来把 API 升到 8.x 才必须换 API jar。
- 差异提示：CONFIG_FILE_VERSION 4→5，说明 3.3.4 的配置文件格式版本更高（API jar 编译自 3.3.2）；对只读写配置值的 API 使用方式无影响。

## 2. 本地 API 面盘点（`com.seibel.distanthorizons.api.*` + `coreapi.*`）

### 2.0 入口与生命周期
- `com.seibel.distanthorizons.api.DhApi`（since API 1.0.0）：
  - 常驻单例：`DhApi.events`（`IDhApiEventInjector`）、`DhApi.worldGenOverrides`（`IDhApiWorldGeneratorOverrideRegister`）、`DhApi.overrides`（`IOverrideInjector<IDhApiOverrideable>`，绑定 core 行为覆写）。
  - 版本查询：`getApiMajorVersion()/getApiMinorVersion()/getApiPatchVersion()/getModVersion()/getIsDevVersion()/getNetworkProtocolVersion()`。
  - 线程判定：`public static boolean isDhThread()` —— 以线程名前缀 `ModInfo.THREAD_NAME_PREFIX = "DH-"` 判断。
- `DhApi$Delayed`（**DH 首次初始化前全部为 null**）：`configs`(`IDhApiConfig`)、`terrainRepo`(`IDhApiTerrainDataRepo`)、`worldProxy`(`IDhApiWorldProxy`)、`renderProxy`(`IDhApiRenderProxy`)、`wrapperFactory`(`IDhApiWrapperFactory`)、`customRenderObjectFactory`(`IDhApiCustomRenderObjectFactory`)。
  - 安全时机 = 一次性事件 `DhApiAfterDhInitEvent implements IDhApiEvent<Void>, IDhApiOneTimeEvent<Void>`（迟注册也会补发，见 `ApiEventInjector.bind` 对 OneTime 事件的缓存补发逻辑）。

### 2.1 (a) 读取 LOD 数据 —— 支持方块级（置信：确定，javadoc+签名）
接口 `com.seibel.distanthorizons.api.interfaces.data.IDhApiTerrainDataRepo`（由 `DhApi.Delayed.terrainRepo` 获取）：
```java
DhApiResult<DhApiTerrainDataPoint>   getSingleDataPointAtBlockPos(IDhApiLevelWrapper, int x, int y, int z, IDhApiTerrainDataCache)  // since 3.0.0
DhApiResult<DhApiTerrainDataPoint[]> getColumnDataAtBlockPos(IDhApiLevelWrapper, int x, int z, IDhApiTerrainDataCache)            // 自顶向下整列
DhApiResult<DhApiTerrainDataPoint[][][]> getAllTerrainDataAtChunkPos(IDhApiLevelWrapper, int chunkX, int chunkZ, IDhApiTerrainDataCache)   // [relX][relZ][columnIndex]
DhApiResult<DhApiTerrainDataPoint[][][]> getAllTerrainDataAtRegionPos(...)          // 同上，区域粒度
DhApiResult<DhApiTerrainDataPoint[][][]> getAllTerrainDataAtDetailLevelAndPos(IDhApiLevelWrapper, byte detailLevel, int posX, int posZ, IDhApiTerrainDataCache)
DhApiResult<DhApiRaycastResult> raycast(IDhApiLevelWrapper, double ox, oy, oz, float dx, dy, dz, int maxLen, IDhApiTerrainDataCache)
DhApiResult<Void> overwriteChunkDataAsync(IDhApiLevelWrapper, Object[] chunkObjectArray)   // 写入（异步排队）
IDhApiTerrainDataCache createSoftCache()   // SoftReference 缓存，AutoCloseable
```
- **detailLevel=0 即方块级**：`EDhApiDetailLevel` javadoc —— 0=1 块，1=2×2，2=4×4 … 4=chunk(16×16) … 9=region(512×512)；宽度 = 2^detailLevel。
- 数据点 `com.seibel.distanthorizons.api.objects.data.DhApiTerrainDataPoint` 字段：`byte detailLevel; int blockLightLevel; int skyLightLevel; int bottomYBlockPos; int topYBlockPos;`（Y 相对 level 最小高度）`IDhApiBlockStateWrapper blockStateWrapper; IDhApiBiomeWrapper biomeWrapper;`。BlockStateWrapper 提供 `isAir()/isSolid()/isLiquid()/getOpacity()(0-16)/getSerialString()/getMaterialId()`。
- `IDhApiFullDataSource`（API 4.0.0，主要面向自定义 worldgen 写入）：`getWidthInDataColumns()`、`getApiDataPointColumn(relX,relZ)`、`setApiDataPointColumn(relX,relZ,EDhApiWorldGenerationStep,List)`（7.1.0 重载）。
- `DhApiChunk`（create/getDataPoints/setDataPoints）与 `DhApiRaycastResult(pos, dataPoint)` 为配套对象。
- 缓存 `com.seibel.distanthorizons.api.interfaces.data.IDhApiTerrainDataCache extends AutoCloseable`：`clear()` + `close()`；javadoc 明确"无缓存时每次读取都要命中数据库文件→解压→解析，较慢"。
- 世界定位：`IDhApiWorldProxy.worldLoaded()/getSinglePlayerLevel()/getAllLoadedLevelWrappers()/getAllLoadedLevelsForDimensionType(...)/getAllLoadedLevelsWithDimensionNameLike(...)`；`IDhApiLevelWrapper.getDimensionName()/getDhIdentifier()/getLevelType()/getMaxHeight()/getRenderRegister()/getDhSaveFolder()` 等。

### 2.2 (b) 事件系统（置信：确定）
- 注册：`com.seibel.distanthorizons.api.methods.events.DhApiEventRegister.on(Class<? extends IDhApiEvent>, IDhApiEvent 实现)` / `off(eventInterface, handlerClass)`；等价走 `DhApi.events.bind/unbind`。**每个事件类只能绑定一个处理器实例**（重复绑返回 fail）。处理器类需实现 `IBindable`（`coreapi.interfaces.dependencyInjection`）。
- 派发实现：`com.seibel.distanthorizons.coreapi.DependencyInjection.ApiEventInjector.fireAllEvents` —— **同步、在触发线程上直接调用**每个 handler（direct-executor 风格），异常被 catch 记日志不中断；`IDhApiEventParam.getCopyBeforeFire()` 为 true 时逐 handler 克隆参数；`DhApiRenderParam.getCopyBeforeFire()` 固定 false（渲染参数共享不克隆）；cancelable 事件任一 handler 置 cancel 则整体取消。
- 事件类清单（`api.methods.events.abstractEvents`，`EventParam` 见各类内部类）：
  - 生命周期：`DhApiBeforeDhInitEvent`、`DhApiAfterDhInitEvent`(OneTime)
  - 世界：`DhApiWorldLoadEvent`、`DhApiWorldUnloadEvent`、`DhApiLevelLoadEvent`（参数 `levelWrapper`）、`DhApiLevelUnloadEvent`
  - 数据：`DhApiChunkModifiedEvent`（chunk 改动已进 full data source）、`DhApiChunkProcessingEvent`、`DhApiBlockColorOverrideEvent`、`DhApiBlockTextureOverrideEvent`、`DhApiBlockStateWrapperCreatedEvent`
  - 渲染：`DhApiBeforeRenderEvent`（**cancelable：cancel 即 DH 跳过该帧自绘**）、`DhApiAfterRenderEvent`、`DhApiBeforeRenderPassEvent`（不可 cancel，注入 DH pass 用，renderPass=OPAQUE/TRANSPARENT）、`DhApiBeforeRenderSetupEvent`/`DhApiBeforeRenderCleanupEvent`、`DhApiBeforeBufferRenderEvent`、`DhApiBeforeApplyShaderRenderEvent`、`DhApiBeforeDeferredRenderEvent`、`DhApiBeforeFogRenderEvent`、`DhApiBeforeGenericObjectRenderEvent`、`DhApiBeforeGenericRenderSetupEvent`/`DhApiBeforeGenericRenderCleanupEvent`、`DhApiBeforeTextureClearEvent`、`DhApiColorDepthTextureCreatedEvent`、`DhApiAfterColorDepthTextureCreatedEvent`
- 渲染参数 `DhApiRenderParam`（version 2025-12-23）：`renderPass(EDhApiRenderPass)`、`partialTicks`、`nearClipPlane/farClipPlane`、MC 矩阵组 `mcProjectionMatrix/mcModelViewMatrix/mcInverseMvmProjectionMatrix`、DH 矩阵组 `dhProjectionMatrix/dhModelViewMatrix/dhMvmProjMatrix/dhInverseMvmProjectionMatrix`（`DhApiMat4f` 可 copy/invert/multiply）、`worldYOffset`、`clientLevelWrapper`（since 5.1.0）。

### 2.3 (c) 线程模型（置信：确定）
- DH 工作线程统一前缀 `DH-`（`ModInfo.THREAD_NAME_PREFIX`），`DhApi.isDhThread()` 判定。
- 事件派发无内置线程切换：handler 在触发线程同步执行（`ApiEventInjector.fireAllEvents` 源码）。渲染类事件在渲染线程触发（`DhApiBeforeRenderEvent` javadoc："called before DH starts modifying the GL state"）；数据类事件（chunk modified 等）在 DH 后台线程触发 —— **handler 内不得直接碰 GL，GL 调用需转回渲染线程**。
- 后台线程预算可配置：`IDhApiConfig.multiThreading()` → `IDhApiMultiThreadingConfig.threadCount()`(`Integer`)、`threadRuntimeRatio()`(`Double`)；另有 `enums.config.quickOptions.EDhApiThreadPreset` 预设。
- `DhApiResult` 是全部 API 调用的统一返回包装（`success/message/payload`）。

### 2.4 (d) 抑制 DH 自身 GL 自绘（置信：确定；持久抑制推荐走配置，逐帧抑制走事件）
1. **持久关闭 LOD 自绘**（本项目首选）：
   - `DhApi.Delayed.configs.graphics().renderingEnabled()` → `IDhApiConfigValue<Boolean>`，javadoc："Simplified version of renderingMode() that only enables/disables the fake chunk rendering"。
   - `configs.graphics().renderingMode()` → `IDhApiConfigValue<EDhApiRendererMode>`，枚举 `com.seibel.distanthorizons.api.enums.rendering.EDhApiRendererMode = { DEFAULT, DEBUG_TRIANGLE, DISABLED }`；两者联动。
   - 写法（API 7.2.0 新签名）：`configs.graphics().renderingEnabled().setValue(false, "<你的mod名>")`；恢复用 `clearValue()`；`getCanBeOverrodeByApi()` 可预判是否允许 API 覆写。
   - 附带开关：`genericRendering().renderingEnabled()`（通用物体渲染）、`chunkRenderDistance()`、`lodOnlyMode()`、`disableFrustumCulling()/disableShadowFrustumCulling()`、`overdrawPreventionRadius()`。
2. **逐帧抑制**：`DhApiBeforeRenderEvent`（cancelable）javadoc："Canceling the event will prevent DH from rendering that frame" —— 适合与自研 Vulkan RT 逐帧同步/动态混合。
3. **透明物拆分**：`IDhApiRenderProxy.setDeferTransparentRendering(true)` —— 不透明正常时机，透明推迟到 MC 透明 pass 的第二遍。
4. **只停数据不停渲染**（RT 独占数据时不需要，但可用）：`DhApi.Delayed.worldProxy.setReadOnly(true)`（API 4.0.0，"prevent DH from updating or creating new LODs"）。
5. **渲染后端查询**（API 7.0/7.1/7.2.0 新增）：`IDhApiRenderProxy.getRenderingApi()` → `EDhApiRenderingApi = { VULKAN, OPEN_GL }`；`getRenderingEngine()` → `EDhApiRenderingEngine = { AUTO, OPEN_GL, BLAZE_3D }`；`isNativeRenderer()`（是否直接调底层 API 而非 Blaze3D）。**注意：DH 3.x 已内置 Vulkan 渲染 API 选项** —— 与自研 Vulkan RT 并存时的资源/上下文交互，公开 API 层无专门接口（见待证项）。
6. 静态 `DhApiBeforeRenderPassEvent` 不可取消，只用于注入 DH 的 pass（javadoc 明示）。

### 2.5 (e) 配置读写 API（置信：确定）
- 根：`DhApi.Delayed.configs`（`com.seibel.distanthorizons.api.interfaces.config.IDhApiConfig`）→ `graphics()/worldGenerator()/multiplayer()/multiThreading()/debugging()`（logging 组未开放，源码注释：更新器不进 API）。
- graphics 组（`IDhApiGraphicsConfig`）子组：`fog()`(`IDhApiFogConfig`)、`heightFog()`? —— 实际存在 `IDhApiFarFogConfig`/`IDhApiHeightFogConfig`（包内），`ambientOcclusion()`、`noiseTexture()`、`texture()`、`genericRendering()`；另有 `IDhApiLoggingConfig`、`IDhApiDebuggingConfig`、`IDhApiMultiplayerConfig` 类存在（部分未在根接口暴露）。
- 值操作 `com.seibel.distanthorizons.api.interfaces.config.IDhApiConfigValue<T>` 全方法：
  `T getValue()`（生效值）/`T getTrueValue()`（配置文件值）/`T getApiValue()`（API 覆写值，null=未设）/`boolean setValue(T)`（deprecated）/`boolean setValue(T newValue, String apiUserDisplayName)`（**7.2.0 新增**，UI 显示控制者）/`boolean clearValue()`/`boolean getCanBeOverrodeByApi()`/`T getDefaultValue()/getMaxValue()/getMinValue()`/`void addChangeListener(Consumer<T>)`。
- 枚举族：`enums.config.*`（`EDhApiMaxHorizontalResolution`、`EDhApiVerticalQuality`、`EDhApiHorizontalQuality`、`EDhApiTransparency`、`EDhApiBlocksToAvoid`、`EDhApiLodShading`、`EDhApiGLErrorHandlingMode`、`EDhApiGpuUploadMethod`、`EDhApiDatabaseSyncMode`、`EDhApiDataCompressionMode`、`EDhApiWorldCompressionMode` 等）；`enums.rendering.*`（fog/材质/方向）；`enums.worldGeneration.*`。

### 2.6 渲染集成辅助（对自研 Vulkan RT 直接有用）
- `com.seibel.distanthorizons.api.interfaces.render.IDhApiRenderProxy`：
  - `getDhDepthTextureGlId()/getDhColorTextureGlId()/getDhBlockRatioAtlasTextureGlId()` → `DhApiResult<Integer>`（GL 纹理名；非 OPEN_GL 引擎时 fail）—— Iris 即走这些（javadoc 明示 deprecated 方法"needed by Iris"）。
  - Blaze3D 版本：`getDhDepthTextureBlazeWrapper()/getDhColorTextureBlazeWrapper()/getDhBlockRatioAtlasTextureBlazeWrapper()`（`IDhApiBlazeTextureWrapper`）。
  - block ratio atlas：颜色=DH 底色与方块应有色之比，0.5=纯底色；**可能随时重建，须每帧查询后再绑定**。
  - `getDepthRange()/getDepthDirection()`（7.2.0，逐帧可能变化）、`getNearClipPlaneDistanceInBlocks(float partialTicks)`、`clearRenderDataCache()`。
- 覆写渲染（替换 DH 内部渲染件，进阶）：`DhApi.overrides` 注入 `IDhApiOverrideable` 实现，含 `interfaces.override.rendering.*`：`IDhApiShaderProgram`、`IDhApiGenericObjectShaderProgram`、`IDhApiFramebuffer`、`IDhApiCullingFrustum`、`IDhApiShadowCullingFrustum`（方法：`overrideThisFrame()/bind()/unbind()/fillUniformData(DhApiRenderParam)/setModelOffsetPos(DhApiVec3f)/bindVertexBuffer(int)` 等）。
- 自绘小物件（非 LOD）：`DhApi.Delayed.customRenderObjectFactory.createForSingleBox/createRelativePositionedGroup/createAbsolutePositionedGroup` → `IDhApiRenderableBoxGroup`，经 `IDhApiLevelWrapper.getRenderRegister()`（`IDhApiCustomRenderRegister.add/remove`）挂到 DH 渲染器。
- 数学对象：`objects.math.DhApiMat4f/DhApiVec3d/DhApiVec3f/DhApiVec3i`。
- worldgen 覆写：`DhApi.worldGenOverrides`（`IDhApiWorldGeneratorOverrideRegister`）+ `override.worldGenerator.AbstractDhApiChunkWorldGenerator`/`IDhApiWorldGenerator`、`objects.data.IDhApiFullDataSource` 写入。

## 3. DH 3.3.4 mod jar 自绘渲染相关类名（只列名，来自 `jar tf` 过滤 render|shader）

### 3.1 core 渲染主干（`com.seibel.distanthorizons.core.render.*`）
`DhApiRenderProxy`（API 实现）、`LodRenderer`、`RenderBufferHandler`、`RenderParams`、`RenderThreadTaskHandler`、`CameraZoom`、`AbstractBlockTextureAtlas`、`QuadTree/LodQuadTree`、`QuadTree/LodRenderSection`、`QuadTree/QuadTreeTickNodeHolder`、`renderer/BeaconRenderHandler`、`renderer/CloudRenderHandler`、`renderer/FogRenderParamFactory`、`renderer/GenericRenderObjectFactory`、`renderer/RenderableBoxGroup`、`renderer/AbstractDebugWireframeRenderer`、`renderer/StubDebugWireframeRenderer`、`renderer/cullingFrustum/DhFrustumBounds`、`renderer/cullingFrustum/NeverCullFrustum`；几何/缓冲：`core.dataObjects.render.ColumnRenderSource`、`bufferBuilding/{LodBufferContainer, ColumnRenderBufferBuilder, LodQuadBuilder, ColumnBox, BufferQuad, IndexBufferBuilder}`、`columnViews/ColumnRenderView`、`transformers/FullDataToRenderDataTransformer`、`core.util.{RenderUtil, RenderDataPointUtil, RenderDataPointReducingList}`。

### 3.2 OpenGL 自绘路径（`common.render.openGl.*`）
`GlDhMetaRenderer`、`GlDhTerrainRenderer`、`GlDhRenderApiDefinition`、`GlDhDebugWireframeRenderer`、`generic/{GlGenericObjectRenderer, GlGenericObjectShaderProgram, GlGenericObjectVertexContainer}`、`glObject/{GLProxy, GLState, GlDhFramebuffer, GlDummyUniformData}`、`glObject/buffer/{GLBuffer, GLIndexBuffer, GLVertexBuffer, GlQuadIndexBuffer}`、`glObject/shader/{GlShader, GlShaderProgram}`、`glObject/texture/{GlDhColorTexture, GlDhDepthTexture, EGlDh*Format}`、`glObject/vertexAttribute/{GlAbstractVertexAttribute, GlVertexAttributePostGL43, GlVertexAttributePreGL43, GlVertexPointer}`、`terrain/{GlBlockTextureAtlas, GlDhTerrainShaderProgram}`、`util/{GlAbstractShaderRenderer, vertexFormat/GlLodVertexFormat*, GlVertexFormats}`、`postProcessing/{GlScreenQuad, apply/GlDhApplyShader, copy/GlDhCopyShader, antialiasing/{GlDhTaaRenderer, GlDhTaaShader, GlDhTaaSharpenShader}, fade/{GlDhFarFadeRenderer, GlDhFarFadeShader, GlDhFarFadeApplyShader, GlDhVanillaFadeShader, GlVanillaFadeRenderer}, fog/{GlDhFogRenderer, GlDhFogShader, GlDhFogApplyShader}, ssao/{GlDhSSAORenderer, GlDhSSAOShader, GlDhSSAOApplyShader}}`、`test/GlTestTriangleRenderer`。

### 3.3 Blaze3D 自绘路径（`common.render.blaze.*`）
`BlazeDhMetaRenderer`、`BlazeDhTerrainRenderer`、`BlazeDhRenderApiDefinition`、`BlazeDhGenericObjectRenderer`、`BlazeDebugWireframeRenderer`、`apply/{BlazeDhApplyRenderer, BlazeDhCopyRenderer}`、`postProcessing/{BlazeDhFarFadeRenderer, BlazeDhFogRenderer, BlazeDhSharpenRenderer, BlazeDhSsaoRenderer, BlazeDhTaaRenderer, BlazeVanillaFadeRenderer}`、`objects/BlazeGenericObjectVertexContainer`、`wrappers/{RenderPipelineBuilderWrapper(+EDh*枚举), BlazeVertexFormatBuilder}`、`wrappers/buffer/BlazeVertexBufferWrapper`、`wrappers/texture/{BlazeTextureViewWrapper, BlazeTextureWrapper, BlazeBlockTextureAtlas, IDhBlazeTexture}`、`wrappers/uniform/{BlazeUniformBufferWrapper, BlazeLodUniformBufferWrapper}`、`util/{BlazeDhVertexFormatUtil, BlazePostProcessUtil, BlazeUniformUtil}`、`test/BlazeDhTestTriangleRenderer`。

### 3.4 资产 shader（`assets/distanthorizons/shaders/`，每套均有 `gl/` 与 `blaze/` 两份）
`terrain/gl/{vert.vert, frag.frag}`、`apply/gl/apply.frag`、`copy/gl/apply.frag`、`fade/gl/{dh_fade.frag, vanilla_fade.frag, apply.frag}`、`fog/gl/{fog.frag, apply.frag}`、`ssao/gl/{ao.frag, apply.frag}`、`antialias/gl/{taa.frag, sharpen.frag}`、`debug/gl/{vert.vert, frag.frag}`、`generic/gl/{direct,instanced}/{vert.vert, frag.frag}`、`test/gl/*`、`shared/gl/quad_apply.vert`（blaze 侧对应 `.fsh/.vsh`）。

### 3.5 注入 MC 渲染管线的 Mixin（fabric 与 neoforge 同名两套）
`fabric.mixins.client.{MixinLevelRenderer, MixinGameRenderer, MixinFogRenderer, MixinChunkSectionsToRender, MixinImmersivePortalsRenderStates}`（`DistantHorizons.fabric.mixins.json`）。DH 侧还有 `renderPass` 抽象 `core.wrapperInterfaces.render.renderPass.{IDhTerrainRenderer, IDhMetaRenderer, IDhFogRenderer, IDhSsaoRenderer, IDhAntiAliasRenderer, IDhFarFadeRenderer, IDhGenericRenderer, IDhVanillaFadeRenderer, IDhTestTriangleRenderer}`。

## 4. 官方源码 / 文档 / 依赖坐标（2026-11-17 实测可达）
- 主源码仓库：https://gitlab.com/distant-horizons-team/distant-horizons/ （fabric.mod.json `contact.sources` 官方指向；旧仓库 `https://gitlab.com/jeseibel/minecraft-lod-mod` 仍存在）
- 核心子模块（与 MC 版本解耦的代码）：https://gitlab.com/distant-horizons-team/distant-horizons-core （存在 `3.3.4` 分支）
- API 示例工程：https://gitlab.com/distant-horizons-team/distant-horizons-api-example （Forge-ApiDemo / Fabric-ApiDemo）
- API Javadoc（在线）：https://distant-horizons-team.gitlab.io/distant-horizons/ （GitLab Pages，`DhApi` "Start Here"）
- 开发者 Wiki：https://gitlab.com/distant-horizons-team/distant-horizons/-/wikis/home ；关键页：`What is the Distant Horizons API`（API 用途：控制 DH / 读地形数据）、`dependency setup`
- Modrinth 主 mod：https://modrinth.com/mod/distanthorizons ；API jar：https://modrinth.com/mod/distanthorizonsapi （"Don't Download Unless You Are A Developer"）
- Issue：https://gitlab.com/distant-horizons-team/distant-horizons/-/issues
- **Maven 坐标**（Wiki `dependency setup` + 示例工程 build.gradle 一致）：
  - 仓库：`maven { url = "https://api.modrinth.com/maven"; content { includeGroup "maven.modrinth" } }`
  - 编译期：`compileOnly "maven.modrinth:DistantHorizonsApi:7.2.0"`（Fabric 直接用；Forge 用 `fg.deobf(...)`）
  - 运行期：`runtimeOnly/modRuntimeOnly "maven.modrinth:DistantHorizons:<版本>"`（26.3 线最新 `3.3.4-26.3`）
  - 本地 jar 约定：`flatDir libs` + `blank:DistantHorizonsAPI:<ver>`，文件名 `DistantHorizonsAPI-<ver>.jar`；本工程现用 `compileOnly files("libs/DistantHorizonsApi-7.2.0.jar")` 等价合法（API 类不引用 MC 类型，无需 remap —— 与 build.gradle 现有注释一致）。

## 5. 许可（置信：确定）
- **LGPL-3.0-only**（Modrinth 页 "Licensed LGPL-3.0-only"）；mod jar `fabric.mod.json` `"license": "LGPL-3"`；API jar 全部 .java 头注释 "licensed under the GNU LGPL v3 License, Copyright (C) 2020 James Seibel"。三源一致。
- 合规要点（与 build.gradle 现有做法一致）：API jar 仅 compileOnly、不随包分发；链接 DH 无需开放本 mod 源码（LGPL 对链接者宽松），但**修改 DH 本体**才需按 LGPL 开放。

## 6. 与本工程现状的对照（只读核查，供集成参考）
- `build.gradle`：API 7.2.0 `compileOnly files("libs/...")`；DH core jar `compileOnly files(run/mods|cpp/shaders 的 DistantHorizons-*.jar)` —— 因 Mixin 需要 DH 内部类型。
- 已有 DH 引用面（grep `com.seibel.distanthorizons`）：`DhApiImpl`（DhApi、config graphics/fog、RenderProxy 纹理与 DepthRange/Direction、AfterDhInit/BeforeRender/AfterRender 事件）、`DhTextureHandles`（Blaze 纹理包装）、`DhFarFieldGeometry/Solver/Uploader`（RenderParam 矩阵、反射 `core.util.RenderUtil`）、`DhLodBufferHookMixin`（`core.dataObjects.render.bufferBuilding.LodBufferContainer`、`core.pos.DhSectionPos`、`core.wrapperInterfaces.world.IClientLevelWrapper`）、`DhRenderListHookMixin`（`core.pos.blockPos.DhBlockPos2D`、`core.render.QuadTree.LodQuadTree/LodRenderSection`）。
- 结论：**公开 API 覆盖 (a) 数据读取（含方块级）、(b) 事件、(e) 配置、渲染纹理句柄；DH 自绘抑制已可用 (d) 双通道。而逐 quad 几何截取（LodBufferContainer）与渲染列表（LodQuadTree）不在公开 API 内，只能继续走 Mixin（现状如此），升级 DH 时为版本敏感点。**

## 7. 待证项与建议下一步
1. **DH Vulkan 后端细节**：`EDhApiRenderingApi.VULKAN` 已在 API 枚举中（version 2026-3-10），但公开 API 无 Vulkan 资源共享接口。下一步：拉取 `distant-horizons-core` 3.3.4 分支源码，查 Vulkan 渲染器实现与上下文创建方式（是否独立于 LWJGL GL 上下文、是否共享 swapchain）——决定与自研 RT 引擎并存时的互斥/共存策略。
2. **LICENSE 文件原文**：GitLab raw 两次 404（默认分支名不确定）；许可本身已三源确认，如需原文可查仓库 `LICENSE`/`COPYING` 分支文件。
3. **在线 Javadoc 版本滞后性**：GitLab Pages 未见版本号标注，核对 7.2.0 新增项（`setValue(T,String)`、`getDepthRange/DepthDirection`）以本地 jar 为准（已核对一致）。
4. **DH 自身 Vulkan 模式下 Iris 纹理句柄行为**：`getDh*TextureGlId` 在非 OPEN_GL 引擎下按 javadoc 返回 fail —— 若 DH 切 Vulkan，纹理句柄通道失效，自研 RT 需改走数据直读（2.1 节 API）或 Vulkan 侧共享（待证 1）。
5. **`IDhApiDebuggingConfig`** 未逐项盘点（含渲染调试开关），如需深度定制再读 `dh-api-src`。
