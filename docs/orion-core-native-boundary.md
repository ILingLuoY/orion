# Orion 第二原生 DLL（Rust）接入边界调查

- 日期：2026-11-17（只读调查，未改动任何项目文件）
- 工作区：D:\fabric-example-mod-26.2；mod 子目录：fabric-example-mod-26.2\
- 问题：为第二个原生 DLL（Rust，orion_farfield）摸清接入边界，避免与现有 C++ rtx_backend.dll 重复造轮子
- 证据等级：一手代码（文件:行）为主；HANDOFF-2026-11-17-DH-OPT-AND-RUST.md 为项目自述文档，关键结论均与代码对过

## 0. 结论摘要

1. 现有原生面 = 1 个自研 C++ 后端（rtx_backend.dll，208 个 JNI 导出）+ 17 个 NVIDIA 中间件 DLL（Streamline/DLSS/MFG/Reflex）。每帧重活（帧录制、TLAS、GI/DI、去噪、DLSS-FG、水 FFT、云）全部已在 C++/GPU，Rust 不要碰。
2. Java 侧仍在做的计算只有一处重活：DH 远场 LOD 的逐节转换（DhFarFieldUploader.convert）+ 逐节 JNI 派发——这正是项目已定的 Rust 试点（HANDOFF §5），Rust 镜像 `off_convert_dh_section` 已完成并对拍 40/40 一致（M2），尚未接生产（M3）。
3. 接入点：把 orion_farfield.dll 放进 `src/main/resources/natives/windows/` 即自动提取进 gameDir（extractBundledDlls 无名单校验）；加载建议走 FFM 懒加载 + off_abi_version 门（HANDOFF M3 既定方案），不要挂进 NativeVulkan 的 static 块，避免第二个 DLL 的失败连累 RT 主后端。
4. 构建接线不对齐处只有一条：C++ 有部署腿（_deploy_all.ps1 把 cpp/build/Release/rtx_backend.dll 拷进 natives/windows），Rust 只产出到 rust\orion-farfield\dist\，缺部署步骤。

## 1. JNI 导出清单（cpp\backend，行首 JNIEXPORT 严格匹配，已排除 .bak 与注释）

### 1.1 按文件统计（共 208 个）

| 文件 | 导出数 | 用途 |
|---|---|---|
| JniSettings.cpp | 116 | 控制旋钮（set*/get*，纯状态拷贝，无 Vulkan 调用） |
| Streamline.cpp | 46 | Streamline/DLSS-FG/Reflex：初始化+查询+交换链+每帧标记 |
| Lod.cpp | 7 | Voxy/DH 远场 LOD 网格上传/移除 |
| Particles.cpp | 5 | 每帧粒子批 + 纹理句柄 |
| Entities.cpp | 5 | 每帧实体批 + 纹理句柄 |
| Vli.cpp | 3 | 体积光照/光源上传 |
| Water.cpp | 3 | 水波脉冲/船 + FFT 参数 |
| Frame.cpp | 3 | updateCamera / recordFrameCommand / testFrame |
| Geometry.cpp | 3 | setInWorld / setVisibleSections / uploadChunk |
| Sky.cpp | 2 | 星图/末地银河图集纹理 |
| Lights.cpp | 1 | uploadDynamicLights |
| Memory.cpp | 1 | setPostProcessingParams |
| Device.cpp | 1 | initDeviceHandlesNative |
| Sections.cpp | 1 | clearChunkLayers |
| Cleanup.cpp | 1 | cleanup |
| Textures.cpp | 1 | updateAtlas |
| As.cpp | 1 | uploadCutoutOmmSection |

（注：cpp\wrapper 下只有 gradle-9.5.1-bin 发行包缓存，无 JNI 代码。State.cpp:1292 的 setSkyModelSettings、Lod.cpp:534 的 clearWorldGeometry 是注释命中；`clearWorldGeometry` 唯一真定义在 Streamline.cpp:605。）

### 1.2 按用途归类

**一次性初始化 / 生命周期**
- initDeviceHandlesNative（Device.cpp:10）——RT 设备句柄注入、SHaRC/AE/SER 资源创建的时序锚点
- cleanup（Cleanup.cpp:9）
- initStreamlineEarly（Streamline.cpp:4162）、prepareStreamlineVulkan（:2379）、slCreateWin32Surface（:2441）、slCreateSwapchain（:2641）、slGetSwapchainImages（:2822）
- FG 需求探测：isFGRequirementsReady（:4064）、getFGInstanceExtensions（:4076）、getFGDeviceExtensions（:4089）、getFGVulkan12Features（:4102）、getFGVulkan13Features（:4115）、getFGGraphicsQueuesRequired（:4128）、getFGComputeQueuesRequired（:4140）、getFGOpticalFlowQueuesRequired（:4152）
- 低频/一次性：setStreamlineQueuePlan（:453）、probeHDRSurface（:282）、setFGBackbufferFormat（:5450）、clearWorldGeometry（:605）

**每帧（Java 调用落在 C++ 每帧工作里）**
- recordFrameCommand（Frame.cpp:1205）——RT 帧录制主体（~5500 行），Java 调用点：VulkanCommandEncoderMixin.java:139、VulkanGpuSurfaceMixin.java:324
- updateCamera（Frame.cpp:138）——每帧相机/天气/维度状态填 CameraUBO；Java 调用点：VulkanCameraMixin.java:499
- beginStreamlineFrame（Streamline.cpp:3864；Java 调用点 MinecraftReflexMixin.java:92）、markSimulationStart/End（:3876/:3916）、markRenderSubmitStart/End（:3951/:3962）、markPresentStart/End（:3971/:3982）——FG 帧标记
- slAcquireNextImage（:2910）、slQueuePresent（:3016）——交换链获取/呈现
- captureFGFinalAndBuildUIAlpha（:5504）
- 实体批：beginEntityFrame（Entities.cpp:145）、uploadEntityBatch（:175）、endEntityFrame（:379）
- 粒子批：beginParticleFrame（Particles.cpp:10）、uploadParticleBatch（:36）、endParticleFrame（:90）
- uploadDynamicLights（Lights.cpp:234）——每帧 ≤32 条移动光源
- queueWaterImpulse（Water.cpp:3545）、updateWaterBoats（:3563）——30Hz 波场输入
- setInWorld（Geometry.cpp:2305）、setVisibleSections（:2326）——每帧可见集/状态
- testFrame（Frame.cpp:6749）——调试自检

**上传（事件驱动，落在帧间隙）**
- uploadChunk（Geometry.cpp:2535）、clearChunkLayers（Sections.cpp:3895）
- uploadCutoutOmmSection（As.cpp:528）
- uploadVoxelLightSection（Vli.cpp:3428）、uploadLightSection（:3695）、removeLightChunk（:4093）
- Lod.cpp：uploadVoxyLodMesh（:256）、removeVoxyLodMesh（:310）、clearVoxyLodMeshes（:328）、uploadDhLodSection（:460）、removeDhLodSection（:513）、clearDhLodSections（:549）、uploadVoxyLodAtlas（:595）

**纹理句柄 / 贴图上传**
- updateAtlas（Textures.cpp:76）、publishDhTextures（:126）、publishDhBlockRatioAtlas（:208）、updateDhFarField（:281）、uploadPBRAtlases（:1839，sprite 矩形感知 mip）
- updateEntityTexture/updateEntityPbrTexture（Entities.cpp:686/:720）
- updateParticleTexture/updateRainStreakTexture（Particles.cpp:178/:203）
- updateStarMapTexture/updateEndSkyAtlasTexture（Sky.cpp:1149/:1171）

**控制旋钮（纯状态拷贝，写全局原子量/UBO lane）**
- JniSettings.cpp 全部 116 个（:10–2524），含 setPerformanceSettingsNative（:2347）
- setPostProcessingParams（Memory.cpp:254）、setWaterFftParams（Water.cpp:366）
- Reflex/FG 旋钮（Streamline.cpp:3452–3854）

**C++ 每帧对上传的消化速率**：SECTION_REBUILD_MAX_SECTIONS_PER_BATCH = 24（OrionInternal.h:302）——HANDOFF P2 指出 Java 一笔 dispatch 全部变更节 vs C++ 每帧只消化 24 节是卡顿点。

## 2. NativeVulkan.java 的 DLL 提取 + System.load 模式

文件：fabric-example-mod-26.2\src\client\java\com\example\vulkan\NativeVulkan.java

- :1029 `private static boolean loaded = false;`
- :1035–1037 `NATIVE_RESOURCE_DIR = "natives/windows"`（mod 根路径下）
- :1135–1250 `extractBundledDlls(gameDir)`：遍历 `mod.getRootPaths()/natives/windows`，walk 全部 `*.dll`，`Files.copy(REPLACE_EXISTING)` **平铺到 gameDir 根**（`gameDir.resolve(文件名)`）；size+Files.mismatch 判同跳过；一个 DLL 都没有则抛 IOException
- :1049–1117 `extractBundledShaders`：SPIR-V 落盘 gameDir\shaders（C++ 按文件路径加载；失败仅告警不阻断）
- :1303–1382 static 块：extractBundledDlls → extractBundledShaders → 校验 gameDir\rtx_backend.dll 存在 → **System.load(backendPath)（:1360）** → loaded=true（:1370）；任何 Throwable → loaded=false（:1380），全部 native 入口按 rtActive/loaded 早退，宿主回原版 Vulkan 渲染
- dev 运行时的提取源是 build/resources/main/natives/windows（_deploy_all.ps1:28 同步拷贝这一份）；生产是 jar 内 natives/windows

### 第二 DLL 接入点评估

1. **提取（零代码改动）**：orion_farfield.dll 放进 `src/main/resources/natives/windows/` 后，extractBundledDlls 自动提取进 gameDir 根；该函数没有 DLL 名单/数量校验。
2. **gradle（零改动）**：verifyBundledNatives（build.gradle:219–250）只校验目录存在、非空、rtx_backend.dll 在——无名单硬编码（:249 的"18 个"只是 println 计数与注释意图）；jar 任务只 exclude `**/*.bak_*` 和根目录重复的 rtx_backend.dll（:259–264）。第 19 个 DLL 自动进 jar。
3. **加载形态**：
   - 不建议挂进 NativeVulkan static 块再 System.load 一次：static 块是"一损俱损"语义，第二个 DLL 缺失/被占用（前一实例未退导致 REPLACE_EXISTING 失败）会把 loaded 拉成 false，连累 RT 主后端。
   - 推荐（= HANDOFF §7 M3 既定方案）：DhBridge 首次使用时 FFM 懒加载 `SymbolLookup.libraryLookup(Path.of(gameDir, "orion_farfield.dll"))`，随后 `off_abi_version()!=2` 拒载（lib.rs:13–15 现返回 2）；配 `-Dorion.dh.rustConvert` A/B 开关与首帧对拍回退。bench 已验证该 FFM 模式（OrionFarFieldBench.java:106–107）。
   - FFM 需要 `--enable-native-access=ALL-UNNAMED`（JDK 26；build_rust.bat 尾部 bench 命令已示范）；若不想动 launch 参数，可改传统 JNI 形态（生成头文件/注册表），但要承担符号名严格匹配纪律（NativeVulkan.java 里 setWaterWaveAflParams 的 javadoc 明确警示：只换 jar 不换 DLL 会因符号解析失败崩模组）。
4. **加载顺序**：orion_farfield 是纯计算 cdylib（Cargo.toml：crate-type=["cdylib"]、lto、不设 panic=abort→导出统一 catch_unwind 返回错误码），**不依赖 rtx_backend/Vulkan**，与 System.load(rtx_backend)（:1360）无先后约束。建议保持零依赖（不链接 rtx_backend），避免 LoadLibrary 依赖解析把两个 DLL 耦合起来。
5. **杀软/签名**：与现网 18 个未签名 DLL 同一风险类别，无新增类别（未签名 DLL 落盘 gameDir 后被 AV 启发式扫描的先例已存在）；jar 内提取不传播 MOTW。占位锁是比杀软更常见的实际故障（上一点）。

## 3. 构建接线对照

**C++ 腿**
- build_dll.bat（工作区根）：MSBuild VS18 Community amd64 编 `cpp\build\rtx_backend.vcxproj`（Release x64）
- vcxproj：OutDir=`cpp\build\Release\`、TargetName=rtx_backend（:62–:82）；:330–:377 的 Command 是 CMake ZERO_CHECK 重生成规则（cmake.exe D:\13136），**不是**拷贝步骤
- 分发腿：_deploy_all.ps1（mod 根）:43 `dllSource='cpp/build/Release/rtx_backend.dll'` → :27–28 目标 `src/main/resources/natives/windows` 与 `build/resources/main/natives/windows` → :52 Copy-Item -Force；:67–68 对各部署点与 Release 输出做备份/校验
- build.gradle:352 注释：rtx_backend.dll 相对自己的目录解析 SPIR-V（shaders/ 由 extractBundledShaders 落盘 gameDir）

**Rust 腿**
- rust\build_rust.bat：设 RUSTUP_HOME/CARGO_HOME=D:\fabric-example-mod-26.2\.toolchains（C 盘仅剩 ~0.3GB，工具链装在工作区）→ `cargo build --release --target x86_64-pc-windows-msvc` → copy 到 `rust\orion-farfield\dist\orion_farfield.dll`
- 注释自述「与 build_dll.bat 并列，不接 gradle」；Rust 1.100.0 stable-x86_64-pc-windows-msvc，MSVC 链接器直接命中 VS18、无需 vcvars（HANDOFF §7 M0）

**对齐结论**：工具链（Release x64 MSVC）对齐；产物落点不同（cpp\build\Release vs rust\...\dist）；**Rust 缺部署腿**（dist → src/main/resources/natives/windows 无自动拷贝，需补 build_rust.bat 或并入 _deploy_all.ps1）；两侧均无签名/时间戳步骤、均无 CI 接线。vcxproj 由 CMake 4.4 生成（cpp\CMakeLists.txt 为源），Rust 是独立 cargo 工程——构建系统不统一是既定现状（试点阶段刻意不接 gradle）。

## 4. Java FFM 使用情况

- mod 生产源码（fabric-example-mod-26.2\src 全部 *.java）：`java.lang.foreign` / `MemorySegment` / `Linker` **零命中**——生产通路全部是传统 JNI。
- 唯一 FFM：rust\orion-farfield\bench\OrionFarFieldBench.java（:1–6 导入 java.lang.foreign.*；:106 SymbolLookup.libraryLookup；:107 Linker.nativeLinker()），JDK 26 单文件运行。

## 5. natives\windows 现状清单（src/main/resources/natives/windows/）

18 个正式 DLL（与 build.gradle:260 注释一致）：
- 自研：rtx_backend.dll
- Streamline（11）：sl.common、sl.deepdvc、sl.directsr、sl.dlss、sl.dlss_d、sl.dlss_g、sl.interposer、sl.nis、sl.nvperf、sl.pcl、sl.reflex
- DLSS（3）：nvngx_dlss、nvngx_dlssd、nvngx_dlssg
- 其他 NVIDIA（3）：nvngx_deepdvc（DeepDVC）、RTXMFG（MFG）、NvLowLatencyVk（Reflex 低延迟）
- 另有 9 个 rtx_backend.dll.bak_*（jar 任务排除）
- 分发包 GuangZhui-Internal-Beta-0.1-Windows-x64\：同一套 DLL 平铺在包根 + mods\modid-1.0.0.jar + shaders\

## 6. 已原生化清单（不要碰，Rust 不重复实现）

- 帧管线/RT 全链：帧录制 recordFrameCommand（Frame.cpp:1205）、相机 UBO（Frame.cpp:138）、TLAS/BLAS 与几何缓存（Geometry/As/Sections/Vli/Lod 各入口）
- GI/DI/去噪/ReSTIR/SHaRC、天空/云/雾/水/粒子/实体渲染（shaders + C++）
- DLSS/Streamline/Reflex/MFG 中间件接入（Streamline.cpp 46 导出）
- 控制/设置面（JniSettings.cpp 116 导出，纯状态拷贝）
- 上传通路（chunk/OMM/体素光/光源/Voxy/DH LOD 网格字节已原生接收——注意：**接收**是原生的，**编码成 16B quad** 仍在 Java，见 §7）

## 7. Java 侧仍承担的计算（候选）

1. **DhFarFieldUploader.convert**（DhFarFieldUploader.java:282）——DH 列数据 → 16B packed quad/顶点缓冲 + 材质/发光标记，逐节一次 JNI+锁。HANDOFF §5 定策的 Rust 目标；Rust 镜像 `off_convert_dh_section`（rust\orion-farfield\src\lib.rs:67–68）已完成：对拍 40/40 逐位一致，A/B ≈1.15–1.43×（HANDOFF §7 M2），**未接生产**。
2. **派发形态改造**（DhFarFieldUploader.java:202 dispatch 一次提交全部变更节，:214 循环逐节 convert+upload）→ 与 P2「8–16 节/帧分帧派发」合流为 M3 批量导出 `off_convert_sections`（一次锁内提交一批）。
3. **DhLodBufferHookMixin 字节拷贝暂存**（M3 可选 off_pack_* 直写，HANDOFF §5 P6）。
4. **DhLodMeshSink.makeRoom O(N²)**（HANDOFF §3 P3）——HANDOFF 明确：Java 小修解决，不值得为它起 Rust。

其余每帧重活均已在 C++/GPU，无第二个 Rust DLL 的合理落点。

## 8. 第二 DLL 接入建议（M3 对齐）

1. 产物接线：rust\orion-farfield\dist\orion_farfield.dll → 复制进 `src/main/resources/natives/windows\orion_farfield.dll`（第 19 个 DLL）。补 build_rust.bat 部署腿（对齐 _deploy_all.ps1:43–52 的两条路径），或并入 _deploy_all.ps1。
2. 加载：FFM 懒加载（DhBridge 首用，libraryLookup gameDir 下 DLL）+ `off_abi_version()!=2` 拒载 + `-Dorion.dh.rustConvert` A/B 开关 + 首帧对拍不一致回退 Java 路径（HANDOFF §7 M3 第 2/3 条原案）。不要进 NativeVulkan static 块。
3. 依赖纪律：orion_farfield 保持零依赖（不链 rtx_backend/Vulkan；只允许 CRT），与 System.load(rtx_backend) 无顺序约束。
4. 性能形态：批量导出一次锁内提交 N 节，替代每节 JNI+锁（与 P2 合流）；SIMD 留后手（HANDOFF §7 诚实结论：转换核心收益 1.15–1.43×，生产收益主要来自 M3 调用形态 + P2）。
5. 若走 JNI 而非 FFM：需要 --enable-native-access 的顾虑消失，但符号名必须与 Java native 声明严格一致，且版本门/回退纪律不变。

## 9. 待证项

1. orion_farfield.dll 进 natives/windows 后，dev（build/resources/main）与生产 jar 两条提取路径未实测（本次只读未构建）。
2. FFM 生产化的 `--enable-native-access=ALL-UNNAMED` 注入点（gradle run 配置 vs jar manifest vs 启动器参数）未查。
3. cpp\CMakeLists.txt 的完整编译选项（LTO/优化档）未逐项比对 cargo release 档——两者产物性能口径只在 HANDOFF 的 bench 里可比。
