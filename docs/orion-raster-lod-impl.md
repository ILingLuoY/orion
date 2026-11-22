# Orion DH LOD「G-buffer 光栅光影」引擎侧整合 —— 完整实施记录

- 日期：2026-11-19（本轮重派，前次尝试零改动、审计确认后全新开始）
- 工作区：D:\fabric-example-mod-26.2（mod 子目录 fabric-example-mod-26.2\）
- 总旗标：`-Dorion.dh.rasterLod`（默认 false = 完全惰性；开启需搭配 `-Dorion.dh.matrices=true` 与 `-Dorion.dh.farLight.mode=14`）
- 基线：以本轮开工时的磁盘版本为「零差异基线」（工作树含主线未提交修改，Shadow.cpp 当日 18:44 已被主 bug 修复方改过，行号已按当前版本重核）

## 一、做了什么（文件清单，每处一句话）

### C++（cpp\backend\）
| 文件 | 改动 |
|---|---|
| OrionInternal.h | DhLodSection 结构新增 raster 字段（quadCount/origin/width/recordOffset/recordVersion/records）；新增 g_dhRasterLodEnabled 与全部 raster 资源 extern、三个函数声明 |
| State.cpp | 定义上述全局变量（全部零值初始化 = 旗标关闭时零资源） |
| Lod.cpp | ① uploadDhLodSection 在旗标开启时保留原始 64B/quad 记录流 + 原点/宽度（uploadDhLodSection 展开成 Vertex 后原流原本被丢弃——已查清并记录）；② remove/clear/世界卸载三条路径归还记录区间；③ 末尾新增 raster 模块：记录流大 SSBO 分配器（256B 对齐 bump+free-list）、扩容（FGDeviceWaitIdle+重建+全节重分配）、createDhLodRasterResources（输出图/createRRGuideImage + compute 管线仿 Shadow.cpp 的 createSunShadowFarResources + 专用描述符 set0）、recordDhLodRasterPass（每帧清屏+逐节拷贝+派发+三段屏障）、destroyDhLodRasterResources |
| Geometry.cpp | 挂载点②：geometryCaptureSectionLocked 顶部单点门控（旗标开启且 isDhLodSpatialKey → 空 split 返回，三个消费者全覆盖；下游 vertices.empty() 分支 = retire 旧 BLAS 不建新）；挂载点④：sectionRoleOf 加结构性说明注释（DH BLAS 不再创建 ⇒ 角色表无 DH 条目） |
| WorldGeometry.cpp | 挂载点③：TLAS 实例循环在计算 instanceSpatialKey 后、原 0xfe 特判之前插入 skip（旗标开启且 DH 键 → continue，与视锥剔除的 continue 同款）；计数器 g_dhLodRasterTlasSkips |
| Shadow.cpp | 挂载点⑦（当前行号 718-725，非旧档 694）：sunShadowFarEnabled() 自适应分支追加 `&& !g_dhRasterLodEnabled`（DH 键出 TLAS 后 1024² 射线全 miss 纯开销；显式 ORION_SUNSHADOW_FAR=1 仍可强制开）；日志带 rasterLod 位 |
| RtPipeline.cpp | RT 共享描述符布局 b[] 96→98：新增 binding 96（rgba8 storage image）/97（r32f storage image），RAYGEN stage；static_assert 同步 98 |
| Vli.cpp | RT 描述符池 STORAGE_IMAGE 配额 23→25（布局静态绑定数 +2，不补池 vkAllocateDescriptorSets 直接失败——本轮自查抓出的必炸点） |
| Frame.cpp | 资源创建挂点（createSunVisResources 之后，set 已存在）：createDhLodRasterResources(renderWidth, renderHeight)；派发挂点（recordSunShadowFarMap / PERF_L10_GPU_B_SFAR 之后、recordSharcZeroFillIfNeeded 与 traceK1K2 之前）：recordDhLodRasterPass(cmd) |
| Textures.cpp | updateDhFarField 在矩阵校验之前解析 reserved lane values[22] → g_dhRasterLodEnabled；载荷注释更新（[22]=旗标，[23] 仍 reserved） |
| Cleanup.cpp | 设备清理路径追加 destroyDhLodRasterResources() |

### Shader（cpp\shaders\ + mod 根）
| 文件 | 改动 |
|---|---|
| dh_lod_raster.comp | countsAndDims.w 由 pad 复用为「本节记录流在大 SSBO 内的字节偏移」；base = quad×64 + offset；头注释记录接线语义。glslang 重编通过（vulkan1.3，-Icpp/shaders） |
| cpp\shaders\rgen_dh_far.glsl | 新增 binding 96/97 readonly storage image 声明 + dhLodRasterModeActive/DepthIsGeometry/DepthAt/ColorAt 助手（texel 映射与 DH 版同一套：dhFarTextureUv 翻转+缩放，含 dhFarTextureUv 前向声明）；dhFarDepthAt 顶部加 mode==14 分支（深度源换本项目 R32F 图，其余 mode 不执行） |
| cpp\shaders\rgen_main_body.glsl | K1 合成块外门放宽 `(params.x>0.5 \|\| modeRequested==14)`；颜色源切换 `(mode==14) ? dhLodRasterColorAt(pixelCoord) : texture(dhFarColor, dhFarUv)`（gamma 2.2 解码/强度/增益/雾机制原样复用）；mode 4 的大气透视条件扩为 `==4 \|\| ==14` |
| cpp\shaders\material_resolve.glsl | 挂载点⑤：DH 分支头加结构性说明注释（rasterLod 下 DH 键不在 BLAS/TLAS/角色表 ⇒ isDhLod 恒 false ⇒ 分支不可达；本 TU 未声明相机 UBO，旗标无法低风险到达） |
| cpp\shaders\lod_coverage.glsl | 挂载点⑥：lodHitIsRedundant 头加同类说明注释（近场让位逻辑由 K1 mode=14 的 dhFarFieldOurs 门承担） |

### Java（src\client\java\）
| 文件 | 改动 |
|---|---|
| compat\dh\DhFlags.java | 新增 `RASTER_LOD = flag("orion.dh.rasterLod", false)`（JVM 属性优先、环境变量 ORION_DH_RASTER_LOD 兜底） |
| mixin\VulkanCameraMixin.java | NativeVulkan.updateCamera 之后调 EntityRtCapture.setEngineCameraMatrices(viewInvArray, projInvArray)（唯一权威相机入口，同帧一致） |
| client\EntityRtCapture.java | 新增公开静态桥：setEngineCameraMatrices + getEngineCameraViewInverse/getEngineCameraProjInverse（列主序 float[16]，与 setCameraOrigin/setHandRotation 同模式；compat.dh 包内类全部包私有，无法被 mixin 直接访问——编译期发现后改走此桥） |
| compat\dh\DhFarFieldGeometry.java | push() 新增 mode==14 && RASTER_LOD 分支：载荷 [0..15] = 本引擎 inverse(worldToClip) = viewInv×projInv（新增列主序 multiplyColumnMajor 助手），[20/21]=0（我们的深度非 reverse-Z ⇒ rgen lanes 判无效 ⇒ 走矩阵路径+变体自选），[22]=1（旗标下发），[24..39]=0（mode 14 不用正向矩阵）；既有路径 PAYLOAD[22] 改为旗标值。DhApiImpl.java 无需改动（push 经 DhApiBeforeRenderEvent 已接通） |

## 二、新绑定 slot 与数据流记录

- **rgen set0/binding 96**：`rgba8 readonly image2D dhLodRasterColorImage`（dh_lod_raster.comp 的 RGBA8 color 输出；顶点色，display 编码，与 DH 图同色彩约定）。选 96/97 依据：全 RT 闭包占用扫描（0-95 几乎占满，89/93/95 为最近新增簇），96/97 空闲且远离既有簇。
- **rgen set0/binding 97**：`r32f readonly image2D dhLodRasterDepthImage`（clip.z/w，Vulkan [0,1]，0.0 = 无几何）。
- **compute pass set0**：0 = cam UBO（g_cameraUboBuffer，sizeof(CameraUBO)）、1 = 记录流大 SSBO（整段 range）、2 = color out、3 = depth out；push constants = ivec4×2（32B），w lane = 记录字节偏移。
- **RT 共享布局**：b[96]/b[97] = STORAGE_IMAGE ×2（RAYGEN），池配额 STORAGE_IMAGE 23→25。
- **记录数据在哪（调查结论）**：64B/quad 记录流来自 DhFarFieldUploader.convert 的 JNI 字节数组，uploadDhLodSection 展开成 Vertex 后**原流被丢弃**——本次在 g_dhLodSections（CPU，旗标开启才保留）+ 单一大 device-local SSBO（GPU，bump+free-list 分配器，256B 对齐）+ 常驻 host-visible staging（渲染线程当帧 memcpy + vkCmdCopyBuffer）重建保留链。上传线程零 Vulkan 调用。派发枚举源 = g_dhLodSections（与 g_chunkMeshes 的 DH 键一一对应，且只有它持有 push constants 所需的原点/宽度/记录）。
- **mode=14 重建路径**：Java 推本引擎逆 MVP 进载荷 [0..15]（rgen dhFarWorldPoint 直接消费）→ 变体自选（y 翻转位）落在主射线上 → 距离走矩阵路（near/far lane 置零杀掉解析 reverse-Z）→ 位置锚定射线 → 法线/护栏/倾角压制/GI 射线/guide 重写/strength+gain 混合/mode=4 同款大气透视全部复用既有机器。

## 三、验证了什么（全部实际跑过）

1. **glslang**：`C:\VulkanSDK\1.4.309.0\Bin\glslangValidator.exe -V --target-env vulkan1.3 -S comp -Icpp/shaders -o dh_lod_raster.comp.spv dh_lod_raster.comp` → exit=0（19660B）。
2. **全量 shader 编译部署**：`python _deploy_stale_spv.py` → 汇总「目标 96 / 重编 24 / 有变化 17 / 已部署 17 / 未变 7 / **失败 0**」，反汇编校验错误 0，三处部署目录（run/shaders、src/main/resources/shaders、build/resources/main/shaders）逐文件哈希一致。7 个 spirv-dis SKIP 为脚本既有 1.4 风味跳过逻辑（改前同样存在，非本轮引入）。
3. **MSBuild 冷编译**：`build_dll.bat` → 全 36 个 cpp 冷编 + 链接，BATEXIT=0，rtx_backend.dll 生成；池配额修复后二次构建 BATEXIT=0。新 DLL 已部署 run\rtx_backend.dll（旧 DLL 备份 .bak_20261119_rasterlod）。
4. **gradle**：`GRADLE_USER_HOME=D:\GTX gradlew compileClientJava --offline` → BUILD SUCCESSFUL（第一轮 2 个包可见性错误，改走 EntityRtCapture 桥后通过）。
5. **检视层**：全部 rasterLod=false 门控点复核（见上方 grep 结果）：旗标关闭时 updateDhFarField 载荷 [22]=0、几何捕获/TLAS/阴影/资源创建/每帧派发全部首行短路，行为与基线逐位一致（编译期无宏改动，无路径拆分）。

## 四、没验证什么（待证项）

1. **游戏内实机验证未做**（硬性约束⑤不起游戏）：mode=14 端到端成像、变体自选收敛到 y 翻转位、水 alpha、DH 图尺寸跟随、[TLAS] 计数、帧耗时——全部待实机。建议实机开启组合：`-Dorion.dh.rasterLod=true -Dorion.dh.matrices=true -Dorion.dh.farLight.mode=14 -Dorion.dh.lodMesh=true`（disableRendering 保持 true）。
2. **旗标开启前的上传节**：记录保留自第一帧 push 起；DH 若在首次 push 前上传，节缺记录被跳过（有限日志）并在下次重传恢复——是否实际出现待实机确认。
3. **记录缓冲 64MB 初始容量**在大世界 DH 满载下的扩容频率与单帧重分配成本未实测（扩容走 DeviceWaitIdle，为已知取舍）。
4. **mode=14 的水像素**（alpha=128）按不透明路径重打光，水面专属质感待实机定夺（v2 可升级）。
5. **逐 quad last-writer-wins 重叠伪影**（shader v1 语义）待实机。
6. **⑤⑥挂载点无旗标位**：结构性摘除是否在所有边缘时序（如旗标下发晚于首批上传）下充分，待实机日志确认 g_dhLodRasterTlasSkips 与 [DH-RASTER] 输出。

## 五、回退清单

全部 20 个改动文件均有同目录 `.bak_20261119_rasterlod` 副本（EntityRtCapture.java 的备份由当前文件剔除新增块重建，因该文件改前已含主线未提交修改、HEAD 不等于改前状态）；run\rtx_backend.dll.bak_20261119_rasterlod 为旧 DLL。逐文件还原即可回到基线；shader 侧还需按指纹重编部署。
