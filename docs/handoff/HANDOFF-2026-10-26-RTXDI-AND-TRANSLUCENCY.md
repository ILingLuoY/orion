# 交接：RTXDI 对齐 / 半透明体 / 体积云描述符（2026-10-26）

> 本文档只记**这一轮实际动过的东西**与**验证状态**。未跑过游戏的一律标"未实测"。
> 相关决策记录见 `docs/decisions/`，环境变量总表见 `docs/env-switches.md`。

---

## 一、本轮完成并已编译验证

### 1. 体积云 / 体积光：4 个坏绑定（本轮起因）

日志证据：`[VK GUARD] cloud: binding 2/5/3 NULL sampler`、`binding 8 NULL imageView`、`vol binding 1 NULL sampler`。

| 症状 | 真因 | 修法 |
|---|---|---|
| binding 2/5/3 NULL sampler | `depthInfo.sampler` 取 `g_volDepthSampler`，而它只在**更晚**的 `createVolumeRestirResources()` 里懒建 ⇒ 首帧为 NULL | 新增 `volDepthSampler()`（按需创建、幂等），云的描述符与体积雾 composite 都改走它 |
| binding 8 NULL imageView | `godRayHistoryInfo` 声明在嵌套块里，而 `vkUpdateDescriptorSets` 在块外执行 ⇒ 悬空栈指针被后续临时变量复用 | 提升到函数作用域 |
| `vol binding 1` NULL sampler | 体积雾 composite 的 `sceneDepth` 描述符**从未赋 sampler** | 补 `volDepthSampler()` |
| 另发现 | 体积光 set 仍在写 **binding 5**，但该 set 布局只有 0/1/2/3/4/8/9，`volumetric_light.comp` 也不声明 5 ⇒ 往不存在的 binding 写描述符（当年"调档就崩"的同一类 UB） | 删除该写入 |

采样器生命周期改成**会话级**（`destroyVolumeRestirResources` 不再销毁，改在会话收尾释放），避免 froxel 尺寸变化时云描述符集持有已销毁句柄。

DLL：`FE60594F72F2C478675CEF0FCE286F7F`（5,908,480 B），已部署 `run/`、`src/main/resources/`、`src/main/resources/natives/windows/`、`build/resources/main/`、`build/resources/main/natives/windows/`。

### 2. 修掉一个会让冷编永远报错的问题

`volumetric_clouds_v5.comp` 引用 `cloud_ue_core.glsl`，而**全仓不存在该文件**（`ueRaymarch` 也只剩 `.bak` 里的引用，v5 用的是 `v5Raymarch`）。删除死 include 后 `_deploy_stale_spv.py` 全量重编 **0 失败**。

### 3. RTXDI：先对齐 Remix 调用契约，再对齐 SDK 数学

**关键发现：`submodules/rtxdi` 原本是空目录**，本轮已 `git submodule update --init` 拉到 `8361620`，现在 `rtxdi-sdk/include/rtxdi/` 有真实源码（`ResamplingFunctions.slangh`、`Reservoir.slangh`）。

实施顺序与依据：

| 项 | 实现 | 出处 |
|---|---|---|
| reservoir 状态 | 改成 RTXDI 语义：`samplePos/sampleIdentity ≈ lightIdx+uv`、`weightSum`、`M`（**帧**）、`targetPdf`、`pi/piSum` | `Reservoir.slangh:24-34` |
| 初始采样 | `FinalizeResampling(localState,1.0,M)` 成 W，然后 **M=1 帧** | `ResamplingFunctions.slangh:280-281` |
| 合并 | `risWeight = p̂·W·M`，`M += M_j`，`weightSum += risWeight`，WRS 抽取 | `:66-95` |
| 归一 | `W = weightSum·pi / (p̂_sel·piSum)`，`piSum = p̂_sel·M_cur + Σ p̂@上一帧接收面·M_j` | `:97-106`、`:557-620` |
| 时域钳位 | `historyLimit = maxHistoryLength × state.M`（= 4 帧） | `:485-497` |
| 重投影 | **9 次尝试**：运动矢量 → 8 个随机偏移（radius 5）→ **同像素免相似性判据**兜底 | `:432-467`、`:397-410` |
| 置信度 | 只有运动矢量路径命中才推进（`isGBufferSimilar = i<8`），存历史条目空闲 lane | `:462` |
| RAY_TRACED BC | 选中历史样本时重验可见性，不可见 ⇒ `temporalP = 0` | `:598-611` |
| 空间复用 | 2（收敛）/ 4（新暴露），半径 30px，法线 0.5，深度 0.1，**逐像素随机偏移** | `rtxdi_spatial_reuse.comp.slang:106-131`、`rtx_rtxdi_rayquery.h:71-75` |
| best-light | 每 8×8 格取**锚点像素**的上一帧 reservoir（**近似，非 argmax**） | `:294-344`、`:1330-1412` |

**明确未逐位对齐、已在代码注释里标注的三处：**

1. `state.M = 1` 的原句没照搬（它依赖 SDK 内部 weightSum 口径，贸然搬会让 W 放大 16 倍）；我们用"每帧 M = 提案数，maxHistoryLength 按帧换算"的等价写法。
2. 空间复用读**上一帧**缓冲（Remix 是独立 pass 读本帧）。**原因：本工程没有 G-buffer**，reservoir 与 shading 在同一个 dispatch 内完成，拆 pass 拿不到接收面。
3. best-light 用锚点而非 argmax（缺 per-tile SSBO）。

### 4. 半透明体：改成 Remix 的 alpha blend 口径

`MaterialInfoTranslucentEXT`（折射材质）在 Remix 里**不是**给 MC 半透明方块用的：
`RenderLayerUtils.java:210` → `RemixEntitySubmitter.java:1541` → `BlockTextureManager.ATLAS_TRANSLUCENT_BLEND`，
即走 `AlphaBlendSurface` + 邻居辐射合成（`alpha_blend_surface.slangh:128-158`、`rtx_composite.h:108-122`）。

改动：

- 共享头 `glass_chain_common.glsl` 新增 `glassAlphaBlendTransmission()`：`T = (1−a) + a·tint`（推导写在头注释里：Remix 的 `C_surface` 取自背景辐射代理，故退化成纯乘数）。
- 致密体不透明度 `a_eff = max(贴图 alpha, 1 − profile 透射率)`（遮光玻璃 0.06 / 冰 0.55 / 黏液 0.45 / 蜂蜜 0.32 换算成 opacity）。
- alpha blend 口径下**停用**多格染料 DDA（否则同一染料乘两遍）。
- **id 空间统一**：生产者 `getLegacyMaterialId()` 给半透明命中 **3.0/4.0**，而三个消费者都写 2.0..2.5 ⇒ 半透明面落到"最近命中"时掉进 `else { behindRadiance = hitAlbedo; }`（无光照 albedo 直出）。判据统一到 `glassChainIsTranslucentMaterial()`（2.0..4.5），rgen 接收链 + 两个 rahit 的 `PRIMARY_TRANSMISSION` / `PASSTHROUGH` 四处全部改用。
- **Java 侧整层兜底标记**：`SectionCompilerMixin.fillUntaggedTranslucentQuads()` —— TRANSLUCENT 层里 `byte27 == 0` 的顶点一律标成 marker 2，水（1）与玻璃族（3..18）不动。`gradlew compileClientJava` 通过。

---

## 二、验证状态

| 项 | 状态 |
|---|---|
| C++ 编译（`rtx_backend`） | 通过，DLL 已部署 |
| SPV 编译（rgen×4 + 2 rahit） | 通过，`_deploy_stale_spv.py` 0 失败 |
| Java 编译（`compileClientJava`） | 通过 |
| **游戏内效果** | **未实测**（本轮没启动过游戏，全部由用户验证） |

当前 SPV 哈希（`run/shaders`，三处目录已同步）：

```
rt_test.rgen.spv             A7A8B54F4F3B632E8F5AF64B13B6F615
rt_test_ser.rgen.spv         B75002A4C2CA5927CB98BDA5E1F87F65
rt_test_sharc_update.spv     516FFC3268C78144B8F789F9ACAF168A
rt_test_gi_half.spv          5D77C932E9B901DB0BCC4A333F2B6F01
rt_test.rahit.spv            0167B729B1F2E02EECC4FC5FCDCEB19F
rt_test_translucent.rahit    4F748BA84CCEAE6916C1A5645A7EC7A5
```

**注意：Java 侧改动需要重启游戏才会生效**（Gradle 会重编 + 重打包 natives），shader 侧改完直接重启即可。

---

## 三、未完成 / 下一步

### A. 半透明"第二条链"（Remix 的 alpha blend 完整语义）

现状是 `T = (1−a) + a·tint`，其中 `C_surface` 用 `glassTint × litGlassTintScale` 这个**代理**。Remix 的式子是：

```
out = a · C_surface + (1−a) · C_background
```

- `C_surface` 应该是**玻璃表面自己的着色结果**（albedo × 太阳 NEE + 本地光 RIS + 环境项），而不是代理标量。rgen 的不透明接收面分支里已有这段代码，可抽函数复用。**代价：每个玻璃像素多 1~2 条射线。**
- `C_background` 在 Remix 里取自**邻居不透明像素**（屏幕空间），我们是现算追迹 ⇒ 1spp 噪声。要补齐需要一张"上一帧最终颜色"。

### B. `prevFinalColor`（C 步，已查证到的结论）

- rgen 的 binding 6（`historyColor0`）**声明了但零引用**，是死绑定；宿主侧也没找到它的写入点（`dstBinding = 6`/`6u` 只有天空大气那一处）。
- 真正存在的历史颜色是 `g_diHistoryColorImages[2]`（`VulkanCore.cpp:34119-34122`，ping-pong），但那是 **DI/SVGF 的中间信号**，不是 RR 输出域。
- ⇒ 要用"RR 输出域的背景"必须新增资源：`prevFinalColor`（rgba16f）+ 每帧一次 copy + **binding 75**（当前 `VkDescriptorSetLayoutBinding b[75]` 正好占满 0..74，要扩到 76 并同步描述符池与 4 个变体）。

### C. 真正的 best-light（argmax）

需要 per-tile SSBO（`RtxdiBestLights` 等价物）+ 一个扫描 pass（`RTXDI_FindGradientSampleAndBestLight` 语义）。当前用的是锚点像素近似。

### D. 三 pass 拆分（若要 spatial 读本帧）

必须引入 G-buffer，且 shading 要挪到 reservoir 之后。**reminder：2026-10-21 删掉的 `RT_DEFERRED_MATERIAL` 是"延迟材质求值"（第二遍重新采样图集），实测 17.66ms→28.26ms；Remix 的做法是"材质在首个命中求值 + 光照延迟"，两者不是同一件事，那个 +60% 不能直接引用。**

---

## 四、踩过的坑（下次别再踩）

1. **`VulkanCore.cpp` 只能用补丁脚本改**：`(name, old, new)` 三元组 + `text.count(old)==1` 检查，全部通过才写。文件编辑器会静默截断。
2. **CRLF**：`glass_chain_common.glsl` 是 CRLF，而 `rt_test.rgen` 是 LF。补丁脚本要先归一化成 LF 再匹配，写回时恢复原行尾。
3. **补丁串首行缩进**：`if(` 在文件里是顶格，写成 `    if(` 会 count=0。
4. **补丁的 new 串必须包含被 old 吃掉的上下文**：一次把 `return transmittance;` 吞掉，导致 helper 被插进上一个函数的嵌套作用域（glslang 报 `'inout' : not allowed in nested scope`）。
5. **`_deploy_stale_spv.py` 的变体判定**：源文件已在 stale 列表时，SER 变体曾被跳过（靠反斜杠路径不匹配才侥幸编译），改动共享头后要确认 4 个变体都 DIFF。
6. **id 空间**：半透明是 3.0/4.0，水是 1.0/2.0。任何新写 `materialId` 判断都用 `glassChainIsTranslucentMaterial()`，不要手写区间。

---

## 五、A/B 开关（本轮新增）

| 宏 | 默认 | 作用 |
|---|---|---|
| `VPT_GLASS_ALPHA_BLEND` | 1 | 0 = 回到薄壁折射口径（旧行为） |
| `VPT_DIRECT_WRS_HISTORY_LENGTH` | 4.0 | 时域窗口（帧） |
| `VPT_DIRECT_WRS_TEMPORAL_ATTEMPTS` | 9 | 1 = 只试运动矢量像素 |
| `VPT_DIRECT_WRS_RAY_TRACED_BC` | 1 | 0 = 省掉复用样本的可见性重验（省 1 条射线/像素） |
| `VPT_DIRECT_WRS_SPATIAL_SAMPLES` | 2 | 0 = 关空间复用 |
| `VPT_DIRECT_WRS_SPATIAL_RADIUS` | 30 | 空间采样半径（像素） |
| `VPT_DIRECT_WRS_BEST_LIGHT` | 1 | 0 = 关 best-light 候选 |

---

## 六、第三轮：半透明从"乘性透射"改成 Remix 的**加性合成**（2026-10-26 续）

参考链由用户指定：**RTX Remix 的 RTXDI 关闭 + RR 降噪**。读的是 `_ref/dxvk-remix`：

| 参考位置 | 内容 |
|---|---|
| `concept/surface/alpha_blend_surface.slangh:128-158` | 逐层 `C' = C_i·a_i + C·(1−a_i)`；`backgroundTransparency *= (1−a)` |
| `pass/composite/composite_alpha_blend.comp.slang:50-84` | `diffuseLight = PrimaryDirectDiffuseRadianceHitDistance[neighbor] · surface.color`（邻居**demodulated**直接光 × 本层 albedo） |
| `pass/composite/composite.comp.slang:812` | `return radianceOutput · backgroundAlpha + alphaBlendOutput`，`backgroundAlpha = Π(1−a_i)` |
| `rtx_render/rtx_rtxdi_rayquery.cpp`（`enableRtxdi*`） | 只控制 ReSTIR；关闭后 direct lighting 退回逐光源 NEE，**composite/RR 的接法不变** |

### 为什么上一轮的口径是错的

上一轮写的是 `T = (1−a) + a·tint`（`glassAlphaBlendTransmission`），等价于假设

```
C_surface == C_background · tint
```

即"玻璃面自己的辐射 = 背景 × 染料"。Remix 的 `C_surface` 是**背景处的辐照度 × 本层 albedo**，不含背景 albedo。
两者在背景是深色面时差得很远：黑石墙后的染色玻璃，旧口径几乎全黑，Remix 口径仍然是被照亮的染色面。

### 现在的实现（确定性展开，不需要随机选层）

```
C_out = C_bg · Π(1−a_i) + Σ_i E_bg · tint_i · a_i · Π_{j<i}(1−a_j)
E_bg  = behindRadiance ÷ 接收面 albedo（逐通道，0.05 下限 / 64 上限）
```

- 第一层（相机直接命中的玻璃）在 `rt_test.rgen` 合成；第 2..N 层由 any-hit 累进
  `payload.glassSurfaceWeightAccum`（新增 vec3 字段），`transmittanceAccum` 同时改成 `Π(1−a)`。
- 天空终止（miss）时 `E_bg = behindRadiance`（天空自己是光源），不做除法。
- `E_bg` 的三项来自既有的 `evaluateDirectSunLighting` + `evaluateVPTRISDirectLighting` +
  环境底光 —— 与"RTXDI 关闭 = 逐光源 NEE"同构，不引入 reservoir。
- depth / normal / roughness / MV **仍由背景不透明面提供**（`ORION_GLASS_RECEIVER_MV`），
  与 Remix 一致：alpha blend 只是一层颜色，guide 属于背景。

### 新开关

| 宏 | 默认 | 作用 |
|---|---|---|
| `VPT_GLASS_ALPHA_BLEND_SURFACE_RADIANCE` | 1 | 0 = 逐位回到上一轮的乘性口径（A/B 用） |
| `VPT_GLASS_ALPHA_BLEND_PAIR_FACES` | 0 | 1 = 进出两个面各算一层（更接近原版逐面混合，玻璃会更不透明） |

### 已知差异 / 未做

1. **清玻璃"当空气"分支没动**（`clearGlassAsAir`，rgen:16355 的 `mix(behindRadiance, tint·litScreen, a)`）。
   它用的是钳到 0..1 的亮度代理，不是 HDR 辐照度 ⇒ 同一块染色玻璃与"清玻璃+染料"观感会有差别。
   要统一需先确认清玻璃的既定观感（用户 10-08 明确要求保留贴图边框），单独 A/B。
2. **所有层共用一个 `E_bg`**（取最终接收面的光照）。Remix 也是"邻居像素近似"，但它每层有自己的屏幕空间邻居。
3. `prevFinalColor`（真正的屏幕空间 C_background）仍未做 —— 见第三节 B，需要新增资源和 binding 75。
4. 游戏内**未实测**。

---

## 七、实测第一反馈：玻璃块只有正对面透明（2026-10-26 续）

截图现象：一个浅蓝染色玻璃块，正对相机的面透（能看见门内的房间），**四个侧面是纯蓝实心**。

### 两个成因，都在"alpha blend 表面被当成电介质薄片"这条残留上

**1. `glassReflection = vec3(viewFresnel)`（rgen:16533 改前）**

`viewFresnel = schlick(abs(dot(N,V)), 0.04)`。侧面法线几乎垂直于视线 ⇒ `N·V → 0` ⇒ **F → 1.0**
⇒ `surfaceSceneReflection` 拿到 100% 权重 ⇒ 整面被镜面反射盖住，透射的 `(1−a)` 看不见。

Remix 的 `AlphaBlendSurface::getAlphaBlendSurface()`（`alpha_blend_surface.slangh:110-126`）只返回
`vec4(albedo/opacity, opacity)`，**没有反射项**；合成式里 `(1−a)` 已经是全部透射能量，
再叠一层 Fresnel 反射是**重复计能量**。

**2. `surfaceSpecular`（太阳 GGX 高光）同类问题**

它只在 `N·L>0` 的面上出现 ⇒ 与 alpha blend 的逐面均匀观感冲突，同样是重复能量。

**3. `C_surface` 的光照来源选错**

第三节 A 原本就写明"C_surface 应该是玻璃表面自己的着色（太阳 NEE + 本地光 RIS + 环境项）"，
第三轮图省事用了代理：`E_bg = behindRadiance ÷ 接收面 albedo`。问题是那个量描述的是
**透过玻璃看到的那面墙**，不是照在玻璃上的光 —— 侧面斜视背后正好是天空，
`E_bg` 直接等于天空辐射除以一个很小的 albedo ⇒ 过曝成纯色。

### 修正

- `glassReflection = vec3(0.0)`（alpha blend 口径），`surfaceSpecular = vec3(0.0)`。
- `C_surface` 改用玻璃**自身位置**的受光：
  `evaluateDirectSunLighting(glassPos, N, V, vec3(1), …) + getSurfaceAmbientRadiance() + evaluateVLIEmitterRadianceAtHit(glassPos, N, vec3(1))`
  —— 与 `clearGlassAsAir` 分支同源；代价是每个玻璃像素**多 1 条**带遮挡的太阳射线（本地光 RIS 仍未接入）。

### 新增 A/B

| 宏 | 默认 | 作用 |
|---|---|---|
| `VPT_GLASS_ALPHA_BLEND_PANE_SPECULAR` | 0 | 1 = 回到叠加裸 Fresnel + 太阳 GGX（旧行为，侧面会重新变实心） |
| `VPT_GLASS_ALPHA_BLEND_PANE_ILLUMINATION` | 1 | 0 = 回到"背景辐照度"代理 |

SPV：4 个 rgen 变体重编并同步三处；两个 rahit 逐字节未变（它们不引用这两个宏）。
**仍未在游戏内实测**（已交用户验证）。

---

## 八、根因（三通道读数确认）：alpha blend 的 `a` 取错了量

`DBG_GLASS_ALPHA_BLEND=1` 实测一块 2 格高染色玻璃柱：

```
左面  紫  R≈1.0  G≈0    B≈0.6
右面  红  R≈1.0  G≈0    B≈0.05
```

`G≈0` 排除多层叠加；`R≈1` 说明 `blendAlpha0 = 1` ⇒ 合成式退化成 `C_out = C_surface`，
背景被完全覆盖 ⇒ 看起来"实心染色面"。**与反射、面朝向都无关**（第七节那两处是独立的能量重复，已修）。

### 为什么 `a` 是 1

`blendAlpha0 = max(payload.glassAlpha, 1 − transmission_weight)`：

- `transmission_weight`（profile 0）= 1.0 ⇒ 第二项为 0；
- `payload.glassAlpha` 由 `material_resolve_glass.glsl:1526` 取 `clamp(atlasSpriteSample.a)`，
  **对染色玻璃实测恒为 1.0**。这与本仓库既定口径一致 ——
  `rt_test.rahit:661-674` 的 `GI_GLASS_FULL_COVERAGE` 原文：
  "Glass blocks and panes use opaque coverage … Base texture alpha neither removes
   surface samples nor scales transmission."

也就是说：**玻璃贴图 alpha 是覆盖率，从来不是透明度**。第三轮把它当 alpha blend 的源 alpha 是口径错误。

### 修法

`a` 改由材质常量给出（与 MC 的 `SRC_ALPHA` 语义对齐）：

```
profile 0（染色玻璃）    a = VPT_STAINED_GLASS_SOURCE_ALPHA = 0.4（vanilla translucent 常量）
profile 1..4（致密体）   a = 1 − transmission_weight（剖面表，不变）
```

main 与两个 any-hit 同一处口径。清玻璃（`clearGlassAsAir`）在 `tracePrimaryGenericTransparentRadiance`
开头就 return，不消费链透射率，因此不受影响。

### 新开关

| 宏 | 默认 | 作用 |
|---|---|---|
| `VPT_STAINED_GLASS_SOURCE_ALPHA` | 0.4 | 染色玻璃的 alpha blend 源 alpha；偏实调小、偏透调大 |
| `DBG_GLASS_ALPHA_BLEND` | 0 | 1 = 把 R=alpha / G=链权重 / B=受光 画成颜色（定位用） |

SPV：6 个产物重编并同步三处（4 个 rgen + 2 个 rahit）。**未实测**。

---

## 九、C_surface 取光改回 Remix 的邻居语义（2026-10-26 实测第二反馈）

三个现象同源：**C_surface 用了"玻璃表面自身 NEE"**（第七节那版，属于自创，Remix 没有）。

| 现象 | 机制 |
|---|---|
| 侧面看不见后面 | 斜视背后是天空，兜底写成了 `E = 天空辐射` ⇒ 面亮到发白，盖住背景 |
| 光源紧贴玻璃变白 | 传 `albedo=1` 求辐照度，紧贴点光源时无上限 ⇒ 截断成白 |
| 彩色光源+玻璃出现萤火虫 | 同上 + 1spp 太阳阴影射线在玻璃边缘跳变 |

### 官方口径（`composite_alpha_blend.comp.slang:50-84, 174-187`）

```slang
diffuseLight = PrimaryDirectDiffuseRadianceHitDistance[neighbor] * surface.color;
// 邻居搜索：if (!isMiss && (isCoplanar || isDepthSimilar) && isNormalSimilar && isSameGeometry)
// 找不到邻居 => stochasticAlphaBlendUseRadianceVolume（体积辐照度）
```

C_surface 只有一个来源：**不透明邻居的 demodulated 直接光 × 本层 albedo**。没有"表面自身 NEE"。

### 改动

- `VPT_GLASS_ALPHA_BLEND_PANE_ILLUMINATION` 默认 **0**（背景接收面的 demodulated 直接光）。
- miss 兜底改成 `getSurfaceAmbientRadiance()`（对应 radiance volume），不再用天空辐射。

### 仍未对齐（按官方参数逐条）

| 官方 | 本工程 | 影响 |
|---|---|---|
| `UseNeighborSearch` + 6 次迭代 / 半径 10 / ×1.6 扩张 / 法线 0.9 / 深度 0.1 / 共面 0.2 | 用"透射射线命中的背景面"代替 | 取光点可能远在 50 格外，与屏幕邻域亮度不一致 |
| `UseRadianceVolume` + `Multiplier 1.0` | 环境底光近似 | 室内无窗处偏暗 |
| `EnableFilter`（5×5、transparencyBandwidth 0.05、distanceWeight 0.5、depthBandwidth 0.05） | 无 | **Remix 抑制玻璃噪点的主要手段，缺这一环萤火虫更明显** |
| `OpacityThreshold 0.95` | 无（遮光玻璃 0.94 恰好临界） | 近不透明体仍走混合 |
| `AlphaBlendSurface::update` 随机选层 + `backgroundTransparency` | 确定性逐层求期望 | 更干净，但少了与 RR 的随机一致性 |
| `backgroundTransparency` 参与体积/雾（composite:581-588） | 无 | 玻璃后的雾/体积光不按玻璃衰减 |
| `hasEmissive` ⇒ `+= sharedRadiance` | 无 | 自发光玻璃面不亮 |
| 独立 composite pass（读**已降噪**的邻居光照） | rgen 内就地取**本帧 1spp** 背景 | 噪声基线与 Remix 不同 |

SPV：4 个 rgen 重编同步（rahit 未变）。**未实测**。
