# HANDOFF 2026-10-25 — 体积云 v5（HPVolumeCloud 移植）重构收尾

参考源码：<https://github.com/AshenOneArt/HPVolumeCloud>（作者的重构未完成版）。
本仓的移植目标是 `cpp/shaders/cloud_v5_core.glsl`（算法本体）+ 三个消费者：

| 文件 | 作用 |
| --- | --- |
| `cpp/shaders/cloud_v5_core.glsl` | HP 模型本体（密度场 / 光照 / 步进 / 烘焙器），**唯一实现** |
| `cpp/shaders/volumetric_clouds_v5.comp` | 游戏端渲染 pass（读 UBO，`mode 8`） |
| `cpp/shaders/cloud_v5_noise.comp` | 游戏端资产烘焙（weather 1024² / detail 64³ / base 128³） |
| `cpp/shaders/cloud_bevy_preview.comp` | 预览器（`mode 8` 渲染 / `mode 10·11·12` 烘焙 / `mode 13` 密度切片诊断） |

预览器：`cpp/build/Release/cloudviewer.exe`（源码 `tools/cloudpreview/viewer.cpp`）。
它和游戏端**共用同一份 core**，所以任何"预览好看、进游戏两回事"都只能是参数映射或资产绑定不一致。

---

## 本轮改了什么（都带证据）

### 1. 距离淡出从"整条射线"改成"逐采样"
- 症状：地平线上方一片扇状条纹；`--view-dist 8000` 时**完全消失**、12 km 内干净 ⇒ 病灶在远场。
- 原因：旧实现在 trace 末尾用"首个命中距离"这一个标量乘 `(1-smoothstep)`，掠射射线（弦长 100~300 km）
  整块保留/整块消失，交界是硬边。
- 新实现：`hpEvaluateCloudProperties` 末尾按采样点到相机的距离淡出（cfg.viewDistance / cfg.fadeStart），
  并且 `hpTraceVolumetricRay` 把 `maxDist` 钳到 `viewDistance`。
  `cfg.cameraAlt` 由 trace 入口从射线原点反解，**调用方不用填**（mode 13 里必须显式给，见 §4）。
- 标定：预览 `--view-dist 12000` 与 60 km 上限下画面一致，扇状条纹消失。

### 2. 覆盖率映射标定（最大的外观问题）
- 症状：云像"半透明薄纱/雪原"，不是积云团块。
- 原因：`covMapScale = 0.6` 让 `coverage = weatherR × 滑块 × 0.6`，均值只有 0.17 ⇒ 阈值
  `1-coverage+0.1` 太高，几乎处处不过阈。用 `mode 13` 密度切片量到的天空被遮比例：

  | covMapScale | 0.6 | 1.0 | 1.4 | 1.8 | 2.2 |
  | --- | --- | --- | --- | --- | --- |
  | 被遮比例 | 2% | 52% | 83% | 91% | 100% |

- 结论：取 **1.0**（滑块 ≈ 天空被遮比例），写进 `hpDefaultConfig()`，游戏端同值。

### 3. base 体积归一化（Perlin-Worley 分布太"满"）
- `--probe 6`：改前 `base3d.R mean 0.733 / min 0.142 / max 1.0` ⇒ 噪声几乎处处高，阈值切不出团块。
- 按同一份统计把窗口从 `(pw-0.30)/0.60` 改成 `(pw-0.40)/0.68`：
  改后 `mean 0.4999 / min 0.0 / max 0.851` ⇒ 团块由噪声自己长出，俯视不再是"雪原"。

### 4. 竖向廓形（profile LUT）重标定
- `mode 13` 切片实测：密度只占层高下 55%，再往上全空 ⇒ 云是"一张 ~500 格的薄毯"。
- 原因：`heightGradient` 是**乘在 eroded 密度上、再被阈值卡的**，旧廓形上端截断在
  0.55/0.75/0.90 太靠下。改成中段平台、靠近层顶才掉（0.70/0.85/0.95 起收）。

### 5. 侵蚀权重标定（"长丝/千层饼"）
- 症状：云底下方成片长丝、远看千层饼。`--erosion 0` 时**完全消失** ⇒ 定位到 detail 侵蚀项。
- 原因：我们烘的 worley 通道均值 0.48，而 `d.b*0.8 + d.a*0.9` 让侵蚀项均值到 0.45，
  把云体内部整片掏空（Schneider 口径只做边缘 ~0.2）。权重降到 **0.45/0.45**。
- 顺带修掉 detail 烘焙的**非整数倍频**（2.1/4.1/8.3/0.75/3.1/6.3）：
  `hpWorley3D` 的平铺靠 `mod(cell, tile)`，非整数 tile 会在体积里切出硬边接缝平面。

### 6. `hpDefaultConfig()`：消掉"未初始化 / 写进结构体没人读"
- GLSL struct 没有成员默认值，调用方漏填就是未定义行为。实测：`mode 13` 自己复制了一份映射、
  漏了 `covMapScale` ⇒ coverage 恒 0 ⇒ 整屏黑（还差点被当成"密度场本来就空"）。
- 现在所有构造点都是 `HPCloudConfig hp = hpDefaultConfig();` 再覆盖。游戏端那段 40 多行的
  "再抄一遍默认值"已删除（预览 0.45 / 游戏 1.0 已经开始漂了）。
- `luminanceScale` 原来是死 lane（写了没人读，预览 3.6 / 游戏 3.0）⇒ 现在**乘在太阳项**上，
  否则太阳只有标定值的 1/3，云只能靠环境光撑亮度（整片发平）。
- `ambientTopMult/ambientBottomMult` 两个字段删除：参考里这两件事是在
  `EvaluateEnvironmentLighting` 里乘进 `ambientTermTop/Bottom` 的，本仓由调用方做。
- 顺带：`hp.ambientBottom` 从 `skyColor×0.6` 改成 `skyColor×0.15` —— 参考乘的是
  **SH(向下的地面辐亮度)**，不是天空色；用天空色会让云底和云顶一样亮。

### 7. 预览器与游戏端对齐
- `--game-save` 改成**逐项对齐 native 出厂值**（`g_cloudBottom 1300 / g_cloudThickness 2100 /
  g_cloudCoverage 0.48 / g_cloudDensity 2.40 / g_cloudViewDistance 25000`）。
  以前它抄的是"某一局存档"的数值，存档一变就对不上。
- 新增 CLI：`--hp-cov` / `--hp-fade` / `--hp-erosion` / `--hp-amb-bottom`。
- `--model v5slice`（mode 13）现在也走 v5 资产烘焙（以前只有 `--model v5` 烘，切片读到的是 bevy 资产）。

---

## 追加（同日，第二轮）：云被"拉成条"的根因

用户报"怎么这么多条带状云"。**确认是天气图，而且是三处叠加**，全部量化过。

### 判据（逐项排除）

| 试法 | 结果 | 结论 |
| --- | --- | --- |
| `--steps 200`（56→200） | 条纹形状不变 | 不是步进量化 |
| `--detail-scale 4/8`（侵蚀周期 1400→350/175） | 形状不变 | 不是侵蚀噪声尺度 |
| `--hp-erosion 0` | 条纹还在（只是变光滑） | 不是侵蚀雕出来的 |
| `--pitch 65` | **条纹完全消失** | 掠射视角是放大器 |
| `--yaw 0 / 90 / 169` | yaw 0 条带水平；yaw 169 条带全部汇聚到**同一个灭点** | 条带平行于**世界 X 轴** |

### 新增两个诊断模式（都在 `cloud_bevy_preview.comp`）

- `--model v5top`（mode 14）：密度场**俯视切片**（24 km × 24 km，h=0.45），
  用 FFT 自相关量各向异性 —— "云为什么被拉长"这类问题以后不用再从渲染图反推。
- `--model v5slice`（mode 13）：改成**近场 0.2~10 km**（7.7 m/px）。原来 4~34 km 在
  pitch 25 下整条线跑到云层上方，切片全黑还在被当成"密度场是空的"。

### 三处 bug

1. **`cov` 最低倍频只有 2×2 个格点**：`hpValue2D(q*0.25, tile 2)` ⇒ 整张 24 km 图上
   只有 4 个格点值，双线性插值的等值线是双曲线臂，被阈值一切就是沿轴的条带。
   改成 4/8/16 格胞（6/3/1.5 km）。
2. **`type` 通道是常数**：`hpValue2D(p*0.125, tile 1.0)` ⇒ 整张图上只有 **1 个格点**，
   云型永远不变（这也是"云型单一"的直接原因）。改成 2 格胞（12 km 区域渐变）。
3. **高度廓形乘在阈值之前**（照抄参考的写法）：`eroded *= heightGradient` 再
   `remap(eroded, threshold, ...)` ⇒ 过阈与否由 `grad(h)·baseShape` 决定，而 grad 是
   只随高度变化的单峰函数，**剩下的就是它的等值面 = 一层贴着固定高度的水平薄板**。
   改成 HZD/Schneider 的原始顺序：**先阈值、后乘廓形**。

量化效果（`--model v5top` + FFT 自相关半宽，18.8 m/px）：

| | 沿 X | 沿 Z | 比值 |
| --- | --- | --- | --- |
| 修前 | 126 px (2.4 km) | 26 px (0.5 km) | **4.85 : 1** |
| 修后（天气图两处） | 130 px | 57 px | **2.28 : 1** |

### ★ 第三轮：真正的根因 = 预览器的 base 3D 采样器用了 CLAMP_TO_EDGE

**一句话**：`tools/cloudpreview/viewer.cpp` 里 `infoBaseSample` 用的是 `g.base3DSampler`
（`CLAMP_TO_EDGE`，当初为"B 方案"的相机中心有限体积准备的）。v5 的 base 体积是
**世界平铺**的 Perlin-Worley（参考用 `s_trilinear_repeat_sampler`），而
`baseUVW = positionAlt / (8000 / noiseScale)` 在相机周围 ±7.3 km（x）/ ±7.3 km（z）
就出了 [0,1]；越界后被夹到**边界那一层 texel** ⇒ 整个半空间取到同一张平面上的值
⇒ 噪声沿被夹的轴被"切片"，密度变成**巨大的直边平板**。

这就是"板状/条状/块状"的全部来源，也解释了之前所有异常：
- 沿世界 X 拉长（x 先出界 ⇒ x<0 半空间被夹 ⇒ 沿 X 方向被挤出成条）；
- `--erosion 0` / `--steps 200` / `--detail-scale` 都改不掉（不在那两个环节）；
- `--yaw` 一转就全部汇聚到 X 轴灭点（因为条就是沿 X 被夹出来的）。

**决定性证据**：把 mode 14 临时改成直接采样体积纹理原始内容，画面左半边/上边
**整片是平的、右下角才有正常 Perlin-Worley 的圆团**，边界是笔直的轴对齐线
—— 只可能是寻址模式，不可能是噪声配方。

**修复**：`infoBaseSample` 按模型选采样器（`cloudModel >= 3` 用 REPEAT 的
`shape3DSampler`，B 方案仍用 CLAMP）。

修后量化（`--model v5top` + FFT 自相关半宽，18.8 m/px）：

| | 沿 X | 沿 Z | 比值 | 含义 |
| --- | --- | --- | --- | --- |
| 修前 | 126 px (2.4 km) | 26 px (0.5 km) | 4.85 | 平板 |
| 修后 | 23 px (**430 m**) | 13 px (240 m) | **1.77** | 真实积云尺度 |

**注意**：这是**预览器**的 bug —— 游戏端 `VulkanCore.cpp:37189-37203` 的
`g_cloudV4Atlas/Worley/BaseSampler` 一直都是 `REPEAT`，所以游戏里没有这个平板问题。
反过来说：本轮之前所有在预览里做的标定（coverage 表、profile、base 归一化）
都是对着一个被夹坏的场做的，数值需要重新校（现在出厂值在修好后看着是对的，
但那张"0.6→2% / 1.0→52%"的表不可信了）。

### 还没解决的

- 残余 **2.28 : 1** 各向异性。已经确定不是天气图噪声本身（把 `hpValue2D` /
  `hpWorley2D` 逐行搬到 numpy 复算：纯噪声的自相关比 0.95~1.17，各向同性）。
  下一个要看的是 **base 体积的竖直频率 vs 层厚**：`noiseScale 0.55` ⇒ base 格胞
  1212 格，而层厚只有 2100 格 ⇒ 竖直方向只有 **1.7 个格胞**，结构上就做不出"塔"，
  只能做"板"。两个单旋钮验证：`--noise 1.2`（格胞 555）与 `--cloud-top 4000`
  （层厚 3.1 km）。注意 `--noise 1.2` 实测把比值推到 5.11，说明还有第三个因素没找到。
- 近场切片量到：同一柱子的密度竖向只占 h∈[0.18, 0.77]，且**云底/云顶高度在整条
  10 km 剖面上基本一致**（层状感的主要来源）。

---

## 怎么复现

```powershell
# 编译预览着色器（+ 预览器）
& "C:\VulkanSDK\1.4.309.0\Bin\glslangValidator.exe" -V --target-env vulkan1.2 -S comp -e main `
    -Icpp/shaders -o cpp/shaders/cloud_bevy_preview.comp.spv cpp/shaders/cloud_bevy_preview.comp
cmake --build cpp/build --config Release --target cloudviewer

# 玩家实际看到的默认世界（云底 1300 / 厚 2100 / 云量 0.48 / 密度 2.4 / 云距 25 km）
cpp\build\Release\cloudviewer.exe --model v5 --shot out/a.ppm --frames 4 --game-save
# 云顶俯视
cpp\build\Release\cloudviewer.exe --model v5 --shot out/b.ppm --frames 4 --game-save --cam-y 4200 --pitch -20

# 密度竖向切片（诊断"云为什么是扁的/有空层"）
cpp\build\Release\cloudviewer.exe --model v5slice --shot out/s.ppm --frames 2 --game-save
#   r = 归一化密度（1 = 满密度）, g = profile 局部高度；注意图是上下翻转的（y=0 是地面）

# / 资产分布
cpp\build\Release\cloudviewer.exe --model v5 --shot out/_p.ppm --frames 2 --probe 6
```

部署（**必须带生产档编译开关**，否则 SPV 会悄悄换档）：

```powershell
python _deploy_stale_spv.py
```

⚠ 手工 `glslangValidator` 编出来的 `.spv` 如果没带 `-DRT_BUILD_PRODUCTION_MODE -DSECTION_VERTEX_32B=1`，
文件变成"比源码新"⇒ 部署脚本会认定它不需要重编，于是 run/shaders 里留着旧档。
本轮就踩了一次，最后是手工按 BASE_FLAGS 重编 + `Copy-Item` 补齐的。

当前已部署校验（run/shaders 与 cpp/shaders 同哈希）：

| shader | md5 前 8 位 |
| --- | --- |
| `volumetric_clouds_v5.comp.spv` | `A7ADA1C4` |
| `cloud_v5_noise.comp.spv` | `142D2903` |

（v5 的两个 comp 是**运行时从 `shaders/` 目录加载**的，不进 DLL；改它们不需要重编 rtx_backend。）

---

## 还没做完（按优先级）

1. **受光层次**：`--probe` 显示 `cloud` 通道 mean ≈ 0.99/1.17/1.39、max ≈ 4.6~6.0，
   tonemap 后 p50 已经压到 0.86 ⇒ 大量截断，云看起来仍偏平。
   下一步应该在 `hpEvaluateCloud` 里核对 `scatterSourceAmount` 与 powder/MS octave 的口径，
   而不是继续调 `luminanceScale`。
2. **detail 侵蚀仍是程序化噪声**：参考用的是 32³/64³ 的 DDS（均值/分布未知），我们烘的
   worley 通道分布偏"热"（均值 0.48、min/max 跨 0~0.94）。目前靠权重 0.45 压住，
   正经做法是把 `hpRenderDetail` 的输出分布也标定到参考口径。
3. **ScMask / CloudType 两个通道基本没参与塑形**（`weather.b` 只影响 sc 压缩高度，
   `weather.g` 的云型插值在 `loCoverTopMax` 上很弱）⇒ 天空里云型仍单一。
4. **性能**：60 km 上限 + 逐采样淡出后，远场步进浪费已消失；但 `viewSteps` 仍是 56（游戏出厂），
   云顶俯视时步长 37 格 vs detail 周期 233 格，远处仍会糊。若要继续优化，
   先量 `gHpDebugInfo.z`（迭代数）再动。
5. **游戏内实机验证**：本轮只做到"预览器里成立 + 两端参数逐项对齐 + SPV 已部署"，
   还没有进世界看。进游戏后先确认日志里有
   `[CLOUD V5] assets baked: weather 1024^2 + detail 64^3 + base 128^3`，
   否则读到的是旧资产（表现会是"形状对不上预览"）。

---

## 第四轮：正式接管游戏端（v5 成为唯一云模型，全参数接进设置）

### 1. 只剩一条云路径

`cloudModelRequested()` 恒返回 0（v5）。`ORION_CLOUD_MODEL=ue` 开关**已退役**——
旧 bevy/v4/UE 实现的着色器与烘焙器仍留在仓库里（随时可回退），但没有任何代码路径会选中它们。

### 2. CameraUBO 尾追加 28 个 HP 参数（偏移 1600..1712）

`camera_ubo.glsl` 与 `CameraUBO`（C++）同步追加七块：

| 块 | 分量 |
| --- | --- |
| hpShapeA | detailStrengthCu / Tcu / Cb · bottomSmoothHeight |
| hpShapeB | wispyEdgeWidth / wispyReach / wispyTopHeight / wispyTopHardness |
| hpShapeC | wispyWeight / billowyWeight / loCoverTopMax / loCoverTopStr |
| hpShapeD | covMapScale / scWorleyStrength / scHeightScale / scCellThickStr |
| hpLightA | forwardEcc / backwardEcc / phiFwdIntensity / phiFwdDepthPow |
| hpLightB | phiFwdDepthBias / phiFwdCompress / scatterODScale / scatterCurvePow |
| hpMiscA  | msEccentricity / aoUpwardScale / fadeStart / powderIntensity（保留） |

- 哨兵：**负值 = 未写入**，着色器回落 `hpDefaultConfig()`；唯一例外 `phiFwdDepthBias`
  （合法值就是负数，所以永远采用 UBO 值）。
- `static_assert(sizeof(CameraUBO) == 1712)`（1600 → 1712）。
- 因为 `camera_ubo.glsl` 变了，**所有**声明该块的 SPV 都重编了一次（30 个）。

### 3. JNI

`setCloudHpParams(float[28])` / `getCloudHpParams(float[28])`（VulkanCore.cpp，
写 28 个 `g_hp*` 原子量并清一次云时域历史）。非有限值直接拒；关键项做范围钳制。

### 4. Java

- `RTXSettingsConfig.CloudHpSettings`：数组式记录（28 项），内部 clone + 按
  `CLOUD_HP_RANGES` 逐项钳制；`sanitized()` 处理旧存档（字段缺失 ⇒ 出厂值）。
  配套 `DEFAULT_CLOUD_HP` / `CLOUD_HP_LABELS` / `CLOUD_HP_RANGES` 三张表。
- 落盘：数据类新增 `float[] cloudHp`（Gson 直接序列化）。
- `NativeVulkan`：两个 native 声明 + 启动时恢复一次。
- `RTXSettingsScreen`：新增「体积云 · HP 全参数（v5）」段落，**表格驱动 28 条滑条**，
  每条改动立刻 `setCloudHpParams` + 落盘；`apply()` 整体应用路径也推一次。

### 5. 部署状态（本轮收尾）

| 产物 | 状态 |
| --- | --- |
| volumetric_clouds_v5.comp.spv | `05DFAC88`，cpp/ 与 run/ 一致 |
| cloud_v5_noise.comp.spv | `CD762107`，cpp/ 与 run/ 一致 |
| 其余 28 个 SPV | 全部因 camera_ubo.glsl 变更而重编并部署 |
| rtx_backend.dll | `26BF1BA4`：resources 两处已更新；**run/ 被运行中的游戏占用，未替换** |

⚠ **DLL 与 SPV 版本必须一致**：新 v5 SPV 读 CameraUBO 偏移 1600..1712，旧 DLL 的块只有
1600 字节，混用时那 28 个 lane 读到越界值。**关掉游戏后跑一次**
`python .tmp/deploy_dll.py` 补齐三处（该脚本会做 sha 校验）。


---

## 第五轮（10-26）：实心平板 = 廓形被我写成了矩形（两处自造偏差）

用户报"全遮时整片天空变成一块刀切一样的实心板"。查下来是**廓形链路上的两个自造偏差**叠加，
已用 .tmp/analyze_profile.py 把 heightGradient(h) 逐覆盖率算出来定案。

### bug 1：hpProfileLUT 的平台占了 84~87% 的层厚

高度坐标不是直接进廓形的，先被 Coverage 的 Möbius 压缩：

`
loCoverTopScale = 1 + 1.2 * coverageHeight      // coverage=1 时 2.2
lut = h / (1 + (scale-1)*h)                     // h=1 时 lut 只有 0.45
`

上一版我把三档廓形的上端截断推到 0.70/0.85/0.95（为了消"薄毯"），结果
h ∈ [0.13, 1.0]（84~87% 的层厚）全落在平台上，廓形恒等于 **1.0**
⇒ 密度与高度无关 ⇒ 云填满整个 slab，顶面 = slab 的几何顶面（一个平面）= 实心板。

修法：三档改成**钟形**（真实 CloudLUT 的形状）

| 类型 | 起 | 收 | coverage=0.78 时 h=1 处的梯度 |
| --- | --- | --- | --- |
| Cu | 0.02→0.10 | 0.28→0.60 | 0.17 |
| Tcu | 0.02→0.09 | 0.42→0.72 | 0.76 |
| Cb | 0.01→0.07 | 0.55→0.85 | 1.00 |

### bug 2：radial 被我接成了 1 - coverage（覆盖率耦合进廓形）

参考的 radial 是**空间**量（天气图中心→边缘），我因为天气图是平铺的改成 1 - coverage，
于是 
adialCore = 1 - radial = coverage ⇒ **覆盖率越高越选 Cb（占满层高）**
⇒ 全遮必然实心。改成中性常数 **0.5**，云型仍由天气图 G 通道决定。

### 附带：顶部软边（自创，有注释）

heightGradient *= 1 - smoothstep(0.82, 1.0, h)（用**未压缩**的 h）。
理由：Möbius 把 lut 最高只推到 0.45，而解析廓形都在 0.5 以上才衰减 ⇒ 高覆盖下
Cb 仍可能整层恒 1。等以后换成真 LUT 资产，这句可以删。

### 部署

olumetric_clouds_v5.comp.spv = D7A5E212、cloud_bevy_preview.comp.spv = 1E8FED1C，
cpp/ 与 run/ 一致。DLL 不需要重编（纯着色器改动）。

---

## 第六轮（10-28）：与参考对齐的一轮「性能优先」重审

完整记录见 `docs/decisions/cloud-lightstep-perf.md`。结论摘要：

### 1. 成本结构（为什么先动光步）

每命中采样点的体积采样次数：视线步 2~3 次，**光步 8 步 × 3 次 = 24 次**
（weather 2D + base 3D + erosion 3D）。参考靠 3D 噪声的 **mip 链**把远光步换粗层
（`mipOffset = j/(N-1)*3`），而本仓两张体积 `mipLevels=1` ⇒ 那个 mip 参数一直空转。

### 2. 新增两个性能开关（出厂即开）

| 字段 | 出厂 | 语义 |
| --- | --- | --- |
| `lightErosion` | 0 | 光步 `simpleMode=true`（跳过 erosion 采样）；参考为 false 全算 |
| `lightReuse` | 2 | 每 2 个命中步重算一次光步，中间复用太阳项 / OD / phi_fwd |

实现上把 `hpEvaluateCloud` 拆成 `hpEvaluateCloudStep`（积分，公式不变）+
保留的 `hpEvaluateCloud`（参考 1:1 入口）。trace 只走 step 版 + 缓存。

### 3. 实测（预览器，1280×720，**关校验层**后 200 帧 × 3）

| 变体 | 均值 | 相对 |
| --- | --- | --- |
| 改前 | 6737 ms | 1.00× |
| 仅 lightErosion=0 | 5095 ms | 1.39× |
| 仅 lightReuse=2 | 6381 ms | 1.11× |
| 两者同开（出厂） | 5076 ms | 1.33×（扣 1 s 启动 ≈1.4×） |

⚠ 校验层开着时同一变体两次能差 2 倍，任何云性能对比都必须先
`="VK_LAYER_KHRONOS_validation"`。

外观代价：`lightReuse=2` 几乎为零（p99 1.3/255）；`lightErosion=0` 让云略暗
（平视 mean 1.96 / 仰视 3.84，Δ亮度 −1.9 ~ −3.8）——simpleMode 里腐蚀项归零 ⇒
光步看到的密度更大。要参考语义就把 `lightErosion` 设 1。

### 4. 部署

volumetric_clouds_v5.comp.spv = `BEA9207A`、cloud_bevy_preview.comp.spv = `F946BCFB`，
cpp/ + run/ + 两处 resources 四处一致。纯着色器改动，DLL 不需要重编。

### 5. 下一步

给 base/erosion 体积补 mip 链（DLL 侧）才是上面两个开关的"正解"；
受光层次 / detail 分布标定等纯质量项放到性能预算确定之后。

---

## 第七轮（10-28 晚）：云量/云大小解耦 + 天气图 512² + 3D 噪声 mip 链

细节见 `docs/decisions/cloud-coverage-size-split.md`。要点：

### 1. 云量不再改云的大小

- 旧：`threshold = mix(0.62, 0.14, pow(coverage, 0.8))` ⇒ 滑条一动，每朵云被削一圈。
- 新：云量 → `presence` 遮罩（`presCut = mix(0.95, 0.02, sqrt((云量-0.1)/0.8))`，
  按天气图 R 的真实分布标定），阈值改成**常数 0.41**。
- 实测（`--model v5top`，18.8 m/px）：出厂 covered 97.8%→95.1%（持平）；
  云量 0.28 时 covered 91.8%→83.3%（面积真的少了），而自相关半宽 14px→18px
  （旧版是"面积不掉、云被削小"，新版是"面积掉、云不变小"）。

### 2. 云的大小滑条方向原来反的 —— **但反转只能放 UI，不能放着色器**

第一版把着色器改成 `noiseScale = 0.55²/size`，**当天就出事故**：存量存档
`cloudSize = 1.45` 的格胞从 460 格（层厚 2080 的 4.5 个）变成 3196 格（>1.5× 层厚），
层内不到一个格胞 ⇒ 密度沿 y 恒定 ⇒ **云被竖向拉成柱子**（用户实机反馈）。
现在：着色器恢复 `noiseScale = max(cloudParams3.z, 0.05)`，方向反转改在 UI
（滑条显示 `0.55/存档值`），核心再加 `noiseScale ≥ HP_BASE_TILE/(6·slabH)` 的
"格胞 ≤ 层厚/2"钳制兜底。

### 3. 天气图 1024² → 512²

`CLOUD_V4_ATLAS_SIZE = 512u`（−6 MB、烘焙 1/4 快），同时砍掉烘焙器最细一档 f=16
（512² 下 4 texel/格胞必出方块），权重分给 f=2/4/8；实测 R 均值仍是 0.661。
预览器图集也从 **1920×1080（非正方形！）** 改成 512²，两边终于在看同一张天气图。

### 4. 3D 噪声 mip 链

`CLOUD_V4_BASE_MIPS = CLOUD_V4_WORLEY_MIPS = 4`，blit 逐级下采，采样器 maxLod=3，
光步的 `textureLod(..., j/(N-1)*3)` 从此真的生效。顺带补了一次
SHADER_READ_ONLY → GENERAL（v5 是 storage 写）。
**本机实测收益 ~0**（固定 LOD3 vs 渐变：3908 vs 3840 ms，1.7% 在噪声内）——
8 MB 体积本来就在 L2 里；保留是为了对齐参考 + 弱卡余量。外观差 mean 0.60/255。

### 5. 部署状态

| 产物 | 状态 |
| --- | --- |
| volumetric_clouds_v5.comp.spv | `D5C8542A`，四处一致 |
| cloud_bevy_preview.comp.spv | `CA6B4883`，四处一致 |
| cloud_v5_noise.comp.spv | `FFF4DD74`，四处一致 |
| rtx_backend.dll | `03322108`：resources 两处已更新；**run/ 被运行中的游戏占用，未替换** |

⚠ 新 DLL 才带 512² 天气图 + mip 链。**关掉游戏后跑** `python .tmp/deploy_dll.py`，
否则 run/ 里还是旧 DLL（旧 DLL 烘 1024²、无 mip，画面仍正确，只是拿不到这两项的收益/内存）。
