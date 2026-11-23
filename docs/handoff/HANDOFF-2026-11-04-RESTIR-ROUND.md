# HANDOFF 2026-11-04 — ReSTIR 学习轮：A1 落地 + DI 诊断 + B/C/D 蓝图

> 本轮原始请求：RTX DI 本身有大问题可能要重构，A(B1 GI时域)+B(K1/K2)+C(ReSTIR PT)+D(材质 callable 化)一起做，完成了一起验证。
> 实际交付：**A1 已完整落地可验证**；DI 诊断结论已定案；B/C/D 完成代码级勘察并写成实施蓝图（本文件）。原因见 §5「为什么不堆在一起」。

---

## §1 A1 — GI 首弹时域重投影（已落地，本轮唯一可直接验证项）

### 机制
- 替换 GI 距离阶梯的 interlace+权重补偿（`diffuseGILodWeight ×2/×4/…×32` 纯采样率补偿，已确认 `diffuseGILodWeight == rtLodDiffuseGiWeight`）。
- 时域 α = `1/(1+max(rateWeight,1.0))`：高频像素权重低→历史占比高，低频像素权重高→新样本占比高。
- 被跳过的帧输出重投影历史而不是 0；**时域开启时 rateWeight 置 1.0**（补偿职责移交历史，乘两次=重复计数）。
- 校验：dense MV（UV 单位，`rtDebugPrimaryMotion` 同源）重投影 + 视深相对 10%（0.35m 地板）；miss 像素写 a=0 失效标记（clear 天然给 0，无需哨兵值）。
- ping-pong：读半=`cam.historyReadIndex`，写半=`1-historyReadIndex`（同 vptDirectHistory 约定）；跨帧序依赖信号量内存依赖，不加显式 barrier。

### 代码落点
| 文件 | 内容 |
|---|---|
| `cpp/shaders/rgen_gi_temporal.glsl`（新） | bindings 86/87 rgba16f，`giTemporalEnabled()`（cam.giRayParams.y>0.5），`giTemporalHistoryLoad/Store/Size` 显式分支（GLSL 不允许局部 opaque 句柄），主函数 `giTemporalResolveAndStore` |
| `cpp/shaders/rgen_main_body.glsl` | ①~4718 处捕获 `giTracedThisFrame`；②~6406 处 `indirectLight *= diffuseGILodWeight` 前插入时域分支 |
| `cpp/shaders/rt_test.rgen` | ~7942 include（rgen_vpt_sparse 与 rgen_main_body 之间） |
| `cpp/backend/RtPipeline.cpp:204` | 布局数组 86→88，b[86]/b[87] STORAGE_IMAGE rgen-only |
| `cpp/backend/Vli.cpp:639` | 描述符池 STORAGE_IMAGE 20→22（原配额恰好用满，不运动会 vkAllocateDescriptorSets 失败） |
| `cpp/backend/Sharc.cpp` | `createGiTemporalImages`（createRRGuideImage 后必须重置 layout=UNDEFINED，否则复生时 prepare 跳过 UNDEFINED→GENERAL 转换=validation 错）；`prepareGiTemporalHistoryImages`（needsClear→逐图 UNDEFINED→GENERAL + 清零 + SHADER_WRITE→READ|WRITE barrier） |
| `cpp/backend/State.cpp` | g_giTemporalHistory* 全局（~738） |
| `cpp/backend/OrionInternal.h` | extern + `giTemporalEnabled()`/create/prepare 声明 |
| `cpp/backend/Settings.cpp` | `giTemporalEnabled()`：env `ORION_GI_TEMPORAL`，未设/空→true，'0'→false（逐位回退开关） |
| `cpp/backend/Frame.cpp` | create（~2445）/prepare（~2819）/`ubo->giRayParams[1] = 1.0f`（~370，y 槽原恒 0，UBO 布局未变） |
| `cpp/backend/Cleanup.cpp` | ~170 双图销毁 |

### 部署清单（等「拷」再落盘）
产物均在 `cpp/.tmp/rgen_split/spv_a1/`（spv）与 `cpp/build/Release/`（DLL）：

| 产物 | md5 | 命令要点 |
|---|---|---|
| rt_test.rgen 主变体 | `99aa5f72e20f29408c94a4907b04f1bd` | `-V --target-env vulkan1.2 -S rgen -e main -DRT_BUILD_PRODUCTION_MODE -DSECTION_VERTEX_32B=1` |
| ser 变体 | `31c0c482cf28e344b437f68177100659` | 同上 + `-DRT_ENABLE_SER` |
| sharc 变体 | `30e0d0ae6039cdd3c0b86f96236fcaca` | 同上 + `-DSHARC_UPDATE=1` |
| gihalf 变体 | `2a2e27c0cd4f4fd9e220a2555a145828` | 同上 + `-DGI_HALFRES=1` |
| rtx_backend.dll | `886a3fe9bb4072fc0e57d19351390bd7` | `cmake --build build --config Release`（cpp/ 下） |

关键断言：sharc/gihalf 两个变体与 A1 之前逐字节一致（宏把特性完全切除），所以 SHaRC 更新 pass 与 GI 半分辨 pass 行为零风险。

### 验证点
1. GI 噪声收敛速度（同帧数下应明显更快，interlace 网纹消失）。
2. 快速转身鬼影（视深校验窗口 ±max(0.35, 10%)，过强/过弱可调该窗口）。
3. 火把放置/移除后 GI 恢复（历史失效→新样本 α 立即拉满，因为 rateWeight=1 时 α=0.5）。
4. 稀疏渲染（sparseParams）共存：稀疏帧 giTracedThisFrame=false 时走纯历史分支。
5. 回退：`ORION_GI_TEMPORAL=0` 逐位回到旧 interlace 行为（同 DLL 内验证，不用换 spv）。

### 已踩过的坑（复现时注意）
- `#if !defined(SHARC_UPDATE)` 在主变体恒假（主变体有 `#define SHARC_UPDATE 0`）→ 必须用取值式 `#if !SHARC_UPDATE`。
- GLSL 不允许局部 opaque image 句柄变量 → 全部走显式分支 helper。
- 描述符池 STORAGE_IMAGE 原配额恰好用满，加 binding 必须同动 pool。

---

## §2 DI 诊断（定案）

### 现状考古结论
1. **现行 "RTXDI" 是自研的**（对齐 Remix 契约；SDK submodule 为空，`_ref/dxvk-remix` 只能从调用点反推）。内联 WRS，无独立 DI 输出图——DI 和 GI 混在同一 litColor。
2. **去噪链全死**：旧 ReSTIR DI + NRD ReBLUR 链（rt_restir_final.rgen / rt_restir_spatial.comp / rt_restir_reconstruct.comp）2026-09-30 整条删除（HANDOFF-2026-09-29-RT-DI-NRD.md）。**退役原因：部署后用户实测「ReSTIR DI 这条链的噪点没有改善」**。
3. NRD C++ 侧 ~90% 存活但被 `kNrdLocalDiEnabled=false` 门控（OrionInternal.h:1060）；atrous 链默认 false 无人开启。
4. 透明/水面接收面走旧 EMA 路径（~14% 噪声），不入 reservoir。
5. 光源整表修订，非逐灯 hash——灯光微调触发全量 reservoir 失效。

### 复活 NRD DI 的缺口（若走这条路）
- 缺 4 个输入生产者：IN_VIEWZ（符号约定：NRD 用负值=命中）、IN_NORMAL_ROUGHNESS（oct-16 编码）、IN_MV（UV 单位）、IN_DIFF_RADIANCE_HITDIST（YCoCg + hitdist 归一化）——这四个全是**静默错**风险（错单位/错符号不报错只画面糊）。
- binding 41 restirDISpatialBuffer（ReSTIRDIReservoir 80B）需重新加回布局+池。
- rt_nrd_composite.comp 是**加法合成**（baseColor.rgb + localDIRadiance），假设主 rgen 已排除本地 DI；现主 rgen 未排除 → 复活必须改减法替换（color − rawLocalDI + denoised）。
- 但注意：这条链当年就是**实测无效才退役的**。**结论：不建议复活 NRD DI**；DI 噪声先走 C 的采样/guide 路线（见 §4 修正版），不够再谈重采样方案。

---

## §3 B — K1/K2 拆分（勘察完成，未实施）

### Prime PT 对照
- K1（primary）：相机一跳 + 全套 guide 输出。落点 ABI：landing_position / normal_direction_x / rgb_header（16B plane ×3）+ direction_yz（8B）+ distance（4B）+ identity（u4）+ prefix_image（bindings 10-15, 26）+ DLSS RR 全套 guide（bindings 19-26：depth/motion/normal/diffuse/specular/distance/specular_motion）。
- K2（transport）：消费 landing 平面，从任意二跳起续路径。

### 本仓勘察结果（证据）
- **便宜阶段输出**（K1 可直接产出）：depth/MV/3 个基础 RR guide/透明提示/玻璃 alpha G-buffer。
- **昂贵传输输出**（K2 才有）：RTAO 8 rays、太阳软阴影 4 rays、glossy、GI 循环、本地光/VPT。
- **7 个传输衍生覆写点**是 K1/K2 拆分的真正难度（K1 写完 K2 要覆写，或移到 K2）：
  - rrSpecularHitDistance（rgen_main_body.glsl:7062 附近）
  - ssIndirect/ssSpecular（:6639-6673）
  - depth 水面覆写（:7004 附近）
  - MV 玻璃覆写（:2219-2239）
  - giHalfImage 写入（:6366）
  - rrStableRadiance（:7879-7886）
  - 半透明 guide 覆写（:1895-2158）
- **没有 raytracing→raytracing 全屏 barrier 先例**（最近的是 sharcCacheBarrier）；K1→K2 需要 image barrier + 同描述符集（跨 dispatch 不需要跨队列）。
- K1 dispatch 可插在 recordGiHalfPass 位置（Frame.cpp:2964 附近）。
- 管线复用模板：Sharc.cpp SHaRC/GI-half 双管线 = 同 TU + 宏 + 同 g_rayTracingDescriptorSet + 各自 SBT 区（Sharc.cpp:1740-1926, 2050-2230）。K1 管线照抄此模式。

### 实施要点（下轮开工时）
1. K1 = 主变体宏开关（如 `-DK1_PRIMARY=1`），只保留 primary trace + guide 输出 + landing 平面写入；其余 (含 GI 循环/VPT/DI) 全部 `#if !K1_PRIMARY` 保留在 K2。
2. landing 平面新增 bindings（仿 A1 的 86/87 手法，注意池配额同步）。
3. K1→K2 之间插 vkCmdPipelineBarrier（SHADER_WRITE→SHADER_READ，stage RAY_TRACING_SHADER→RAY_TRACING_SHADER）。
4. 七个覆写点逐个判定归属：覆写依赖传输结果的一律移 K2；纯几何的留 K1。

---

## §4 C/D — 采样/重建路线 + 材质 callable 化（蓝图；2026-11-05 修正）

> **重大修正（2026-11-05，用户向原作者求证）**：Prime PT 的 `engine/restir_pt/`（reservoir/shift/paired 等 13 文件）是**未接线的死代码**——全树 import 全部发生在该目录内部，`prime/` 接入层零引用。作者原话确认链路压根不走 ReSTIR PT。之前把"模块存在"当成"链路使用"，与 voxel/dag.slang 是同一类死代码陷阱。下方 C 已按真实链路重写。

### C（修正版）：学它的真实抗噪配方，不移植 ReSTIR PT
Prime PT 实际链路 = **普通 1spp PT + NEE（direct_sample）+ RR（roulette）+ binary Sobol 低差异采样 + DLSS RR 全套 guide + 单帧空间重建**（engine/pt/integrator.slang 只 import pt.scene/path_sampler/transport/roulette；path_sampler.slang 用 binary_sobol；prime/rr.slang 用 spatial_denoise）。
对 Orion 可移植的三件事：
1. **binary Sobol 采样器**（engine/pt/binary_sobol.slang）替换/叠加现有随机流——采样质量是 1spp 观感的半壁江山。
2. **RR guide 质量**：Prime PT 的 reconstruction.slang（GUIDE_SKY/SURFACE_PROXY/MOTION_PROXY 等 6 类 guide 失效分类 + PSR 反射运动参考）比本仓 guide 更精细，对照补缺。
3. **单帧空间重建**（reconstruct/spatial_denoise.slang：几何/材质权重、valid=false 不入邻域）在 DLSS RR 之前加一层轻量空间滤波。
- DI 的结构性修法不再是"ReSTIR PT 吸收"（前提已废）；回退到 §2 的结论：先评估 Sobol+guide 路线对直接光噪声的改善，不够再谈重采样类方案（此时才值得评估把死代码 restir_pt 接活，但那是自研决策不是对标移植）。

### D：材质 callable 化 + Flow 去重
- 目标：把 rgen 内联的材质分支改 callable shader，缩小主 TU 体积/编译时间（本轮 rgen 拆分已把 29202→7942+19 文件，callable 化是下一层）。
- 依赖 D 的前提：材质调用点已随 B/C 重排，先 B/C 后 D 避免重排两次。

---

## §5 为什么不堆在一起交付

1. **回归归因**（记忆规则：视觉回归先定时间线）：A1 是可单独验证的观感改动；若和一条未经实测的 DI 重写叠加，画面出问题时无法归因。
2. **旧 DI 链的退役理由是实测无效**——在同一验证轮里复活它，大概率把 A1 的正确性也污染掉。
3. B/C/D 是结构性手术（7 个覆写点、跨 dispatch barrier、采样器替换），需要 A1 先证明时域地基可用。

**下一轮开工顺序建议：B → C（Sobol/guide/空间重建路线，DI 视改善程度再定）→ D。本文件 §3/§4 已含全部代码级坐标，可直接开工。**
