# Orion 引擎 G-buffer / 光栅光影路径现状调查（只读取证）

- 日期：2026-11-19
- 范围：D:\fabric-example-mod-26.2\fabric-example-mod-26.2\（cpp\backend、cpp\shaders、src\client、HANDOFF 文档）
- 背景：用户计划把 DH 远场 LOD 改走「G-buffer 的光栅光影」（非原版光栅），需确认引擎有无现成 G-buffer/deferred 光栅基建可挂载。

## 结论摘要

**部分。** 引擎**没有**任何 Vulkan graphics（光栅）管线在跑——全仓无 vkCreateGraphicsPipelines / vkCmdDraw* / VkVertexInput*，唯一 render pass 是水焦散的残留骨架（真实管线是 compute）。引擎所谓「G-buffer」是 **RT rgen 主 pass 写入的 6 张 denoiser guide 图**（NVIDIA RR/NRD 口径），下游 NRD/RT-DI 链完整可复用。因此「把 DH 远场改走 G-buffer 光栅光影」的落点是：**G-buffer 目标图像、描述符与降噪/雾合成链可挂载，但光栅化器本身（graphics pipeline、顶点输入、深度缓冲）必须新建**。

## 逐问发现

### 1) 有没有把几何光栅化到 G-buffer 的管线？——否，全部 compute/RT

- G-buffer 图像定义：`cpp/backend/Sharc.cpp:545` `createRRGBufferImages()` 建 6 张图：diffuseAlbedo R16G16B16A16、specularAlbedo R16G16B16A16、normalRoughness R16G16B16A16、specularHitDistance R32F、stableRadiance R16G16B16A16、transparencyHint R8_UNORM（Sharc.cpp:663-720）。全局尺寸 `g_rrGBufferWidth/Height`（State.cpp:883-884）。
- 写入方：RT rgen 主 pass，非光栅。`cpp/shaders/rgen_main_body.glsl:7994-7997` imageStore rrDiffuseAlbedoImage；8969-8986 注释「GBuffer / depth / MV / normal / roughness / albedo 全分辨率、每帧新写」。消费方：NRD 降噪（Nrd.cpp:4304/4474）、Streamline/RR 链、ScreenSpace.cpp:856-919。
- graphics pipeline 排查（全仓 grep，均 0 命中）：`vkCreateGraphicsPipelines`、`vkCmdDraw*`、`VkVertexInputBindingDescription`。唯一 `VK_PIPELINE_BIND_POINT_GRAPHICS` = Water.cpp:1319 水焦散 render pass 描述；该 pass 的 framebuffer（R16G16B16A16、无深度，Water.cpp:1332-1385）与 vert/frag 源码（cpp/shaders/water_caustic_map.vert/.frag）仍留存，但真实在跑的是 compute 管线：Water.cpp:1441 加载 `water_caustic_map.comp.spv`、1706 绑 `VK_PIPELINE_BIND_POINT_COMPUTE`；Frame.cpp:3295 注释「全部删除：真实在跑的是 recordWaterCausticMap（water_caustic_map.comp）」。
- 「deferred」语义甄别：全仓 `deferred` 命中均为 `VK_KHR_deferred_host_operations`（异步建管线，PipelineDiag.cpp:232-236）与延迟 BLAS 构建（As.cpp:311、OrionInternal.h:1120），**与 deferred rendering 无关**。RT 侧另有 K1/K2 两段式「延迟材质求值」（rt_test_k1/k2*.rgen、rt_test_pomdeferred.rchit.spv），也是 RT 内部分段，非光栅 deferred。
- 做类似事（几何→贴图）的 pass 全是 compute/ray-query：
  - 体素注光 VLI：`Device.cpp:374`「create voxel compute pipeline」；体素占用/albedo 数据由 Java 侧经 `uploadVoxelLightSection` JNI 上传（Vli.cpp:3428-3456），VLI 页面在 compute 里注入方向辐照（Vli.cpp:1486「VLI sourceWords encode voxel occupancy/albedo」）。无光栅体素化。
  - 太阳阴影图：Shadow.cpp **不是**光栅 shadow map，是 ray query 写 1024² R32 图（Shadow.cpp:18-28 注释明言需 ray query 扩展，28-47 做特性探测，不支持则退回纯云影）。
  - 深度预通道：无。backend grep `prepass|pre-pass|depthPrepass` 仅命中 NRD 内部参数（Nrd.cpp:1812-1884，denoiser 配置）；Hi-Z 由 `ss_hzb.comp` 在 compute 中从 RT 主 pass 深度构建（ScreenSpace.cpp:19）。

### 2) 近场地貌怎么被光照？——RT-DI 直接照 TLAS 几何，无光栅 deferred 分支

- 近场几何入 BLAS/TLAS：Java 28B/vertex 上传 → Geometry.cpp `uploadChunk`（Geometry.cpp:2535）→ As.cpp 建 BLAS（含 OMM cutout 分类）→ WorldGeometry TLAS。
- 直接光：太阳光 = rgen 内阴影射线；本地光 = RTXDI 式 WRS reservoir（OrionInternal.h:2123「Lane 3 … RTXDI-style WRS direct-light reservoir」；Settings.cpp:336「0=传统单帧 RIS，1=RTXDI 式时域 reservoir」），降噪链 rt_di_reproject/moments/atrous → rt_nrd_composite（HANDOFF-2026-09-28-RT-DI.md：官方顺序「GBuffer → ReSTIR DI → NRD 一次 → Compositing → Resolve(DLSS-RR)」，此处 GBuffer 即 RT 主命中生成的 guide 图）。
- GI：体素 DDA / SHaRC 缓存（OrionInternal.h:4056/4074 `noVoxelDda`、`voxelDdaGi`；Sharc.cpp）。全部在 RT rgen/compute 内完成，没有 deferred 分支。

### 3) Vulkan graphics pipeline 基建清单——几乎为零

| 组件 | 状态 | 证据 |
|---|---|---|
| RasterPipeline/GraphicsPipeline 类 | **不存在** | backend grep `Raster|rasteriz` 0 命中；`GraphicsPipeline` 仅 Water.cpp:1319 的 bind point 常量 |
| 顶点输入格式（VkVertexInput*） | **不存在** | 全仓 0 命中（几何全走 BLAS 自定义 32B/16B 顶点格式） |
| 渲染目标 | 仅水焦散残留：`g_waterCausticMapRenderPass`/2×Framebuffer，R16G16B16A16 单 attachment、无深度 | Water.cpp:1319-1385、State.cpp:2257-2265、OrionInternal.h:6169 |
| 深度缓冲 | **无 D32/DEPTH image**；「深度」= RT 主 pass 写的线性深度 + Java 侧 DH 深度纹理 | grep `VK_FORMAT_D32|VK_IMAGE_ASPECT_DEPTH` 仅命中 Textures.cpp:129-154 的 DH 外部深度句柄 |
| 可复用 G-buffer 基建 | 6 张 RR guide 图 + `createRRGBufferImages`/`updateRRGBufferDescriptors` | Sharc.cpp:545/401、OrionInternal.h:3325/3346/4821 |
| 场景颜色目标 | binding 24「场景颜色」，fog_screen_apply / volumetric_cloud_composite 写入 | Clouds.cpp:1878-1954、2828、2924 |
| 外部纹理接入通道 | `publishDhTextures` 收 DH 侧 color/depth/atlas 句柄（同设备校验） | Textures.cpp:124-215 |

### 4) LOD 伪节顶点格式与 C++ 展开

- 口径澄清：28B 是**每顶点**（Minecraft 地形 ABI），4 顶点/quad = 112B/quad；DH 远场伪节走 **Voxy 通路 4×16B=64B/quad**。
- 地形：`uploadChunk` 严格校验 `vertexStride == 28`（Geometry.cpp:2560），逐顶点解码：pos f32×3（+0..11，加 section*16 偏移）、rgba8（+12..15）、uv f32×2（+16..23）、light u32（+24..27）、materialMarker@27（Geometry.cpp:2690-2776）。
- quad→三角形展开：非索引每 quad 写 6 顶点 `(0,1,2)(0,2,3)`（Geometry.cpp:2902-2907）；opaque 索引化路径每 quad 4 顶点+索引（Geometry.cpp:2895-2898、2684 `triVertexCount = indexed ? quad*4 : quad*6`）。2026-10-21 起另有 48B→32B SectionVertex 重打包（Geometry.cpp:1039-1104）。
- DH/Voxy 伪节：`DhFarFieldUploader.java:36`「照 voxy 通路的形态改成每节一个 pseudo-section」，映射进 Y=32766 保留带（OrionInternal.h:1721、Geometry.cpp:3048）；C++ 侧在 Lod.cpp:8-16 解码「four 16-byte vertices per quad: u16 x,y,z,pad, rgba8, material8, light/face bytes」（`voxyQuadFaceOf` Lod.cpp:75-95 算面法向）。
- Java 侧 `emitQuad`（ParticleRtCapture.java:410）是**粒子批**的 quad 展开，与地形 quad 无关，勿混淆。

### 5) 远场雾/大气合成点

- 天空 LUT：`sky_atmosphere.comp`（Sky.cpp:921，256×128 LUT，每帧重写）。
- 雾链（录制集中在 Clouds.cpp）：`fog_froxel_density/light` → `fog_froxel_composite`（Clouds.cpp:2924）→ `fog_screen_apply`（Clouds.cpp:2828；云关时直接混入场景颜色 binding 24，Clouds.cpp:1954、4195、4488）；云开时 `volumetric_cloud_composite`（Clouds.cpp:2621/3110）。
- DH 远场当前合成点：**RT 主 pass K1 内**的合成块（rgen_main_body.glsl:8114-8612）：对 miss/水/玻璃像素把 DH color 混入主 radiance（「图像定色+几何定光」，rgen_dh_far.glsl:1-14 头注），并同步把 rrDiffuseAlbedo 等 guide 图写成 DH 色（rgen_main_body.glsl:8619-8642）——即 DH 远场像素**已经进 RT G-buffer**，走 NRD/RR 降噪。
- 因此远场若改走光栅 G-buffer：合成点天然不变（K1 合成块替换为读光栅 G-buffer），雾的接入点在 `fog_froxel_composite`/`fog_screen_apply` 之前（需保证光栅深度参与 froxel 雾）或在 fog_screen_apply 之后、cloud_composite 之前。

## 待证项

- `run/rtx_backend.dll` 与 cpp 源码是否完全同步（多个 bak_*.dll 备份并存，报告以 cpp 源码为准）。
- 水焦散 render pass 残留是否还有别处引用（未逐一枚举调用点，仅确认管线侧是 compute）。
- Java 侧（src/client）是否存在自建 VK graphics pipeline（本次只查了 C++ backend 的引擎侧）。

## 建议的下一步

1. 若实施「DH 远场走 G-buffer 光栅」：新建一条 VK graphics pipeline（可参照 Water.cpp 水焦散 render pass 骨架 + `water_caustic_map.vert` 的顶点展开方式——它就是 gl_VertexIndex 逐 quad 展开，无顶点缓冲），目标写入现有 6 张 RR guide 图（或新增一张 far-field 专用图），深度用 DH 深度纹理（Textures.cpp 已有句柄）。
2. 伪节 quad 数据已在 C++ 侧（Lod.cpp），光栅化可直接消费 64B/quad 记录，展开函数可仿照 Geometry.cpp:2902-2907 的 (0,1,2)(0,2,3)。
3. 合成侧改动集中在 rgen_main_body.glsl:8114-8612 合成块与 fog_froxel/fog_screen_apply 的深度输入。

## 证据等级说明

全部结论基于一手代码（当前仓库 cpp/backend、cpp/shaders、src/client）与仓库内 HANDOFF 文档（官方 NRD 契约部分引用 RTXDI/NRD 上游，属二手转述、已标注）。未使用任何网络来源。
