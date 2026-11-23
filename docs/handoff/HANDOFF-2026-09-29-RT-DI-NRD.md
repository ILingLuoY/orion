# HANDOFF — 2026-09-29 — 本地光 DI 历史被重置（NRD/REBLUR 侧）

写给接手的人（或下一个会话的我）。**请先读完第 0、1、2 节再动手**——这份交接里最有价值的部分不是结论，而是**已被排除的路径**和**哪些仪器是坏的**。我在这一轮里因为仪器问题绕了很多远路，代价是几小时和大量上机次数。

---

## 0. 一句话现状

**症状仍在，根因仍未定；但 2026-09-29 20:22 那一局把范围压到了唯一一个岔路口：`smbFootprintQuality` 的两个因子 —— SMB 四 tap 遮挡位，还是 `sizeQuality` 地板 0.1 —— 哪一个在压历史。**

本轮（20:22 那一局 + 用户截图 `RTX Screenshot 2026.09.29 - 20.22.53.23.png`）已确证三件事，都可复查：

1. **`RTX_NRD_DISOCCLUSION_THRESHOLD=10.0` 确实生效**：`run/rt_debug.log:7400` `20:22:40 [NRD-1 DISOC] threshold=10.000000 alternate=50.000000`；同局无 `missing NRD texture view`、无 Validation Error；MV 为 `motionVectorScale=(1,1,1)`。截图时间 20:22:53，在该局内。
2. **深度容差这条闸门已经开到物理上限**（见新增的 §2.5 `saturate` 天花板），**而棋盘仍在** ⟹ 把锅扣在 `disocclusionThreshold` 上已经不可能，**不要再调这个阈值了**。
3. **本轮 `IN_DIFF_CONFIDENCE ≡ 1`**：部署的 `run/shaders/rt_di_confidence.comp.spv` = `AD3F1AA3AF5C9ABF39D21BEF14F65B6F` / 8464 B，与 §5.2 的 FORCE_ONE 版逐字节相同 ⟹ `c = min(smbFootprintQuality, 1.0) = smbFootprintQuality`。**所以本轮画面上任何"1-2 帧"都不是我方 confidence 造成的，凶手在 NRD 内部。**

4. **★ NRD 的「改 `.hlsl` → 重编 → 运行时」这条链已端到端验证贯通**（2026-09-29 20:58：两枚纯显示 canary——`DIFF` 标签里的空格换成 `'X'`、vp8 tile 里一块纯洋红方块——**同时上屏**）。**以后改 NRD 前不需要再做任何探路实验**；做法、命令、以及"怎么证明改动真嵌进了 DLL"见 §7.1 末尾。
5. **`disocclusionThreshold` 只治"硬重置"那一半，且本项目实际跑的是最不宽容的 `0.01`**（★ 2026-09-29 21:3x 更正：**不是**文档原先写的 `0.1`，见 §5.3 —— 环境变量从 20:57 起就没进过游戏进程）：20:58 两张截图里 vp8 的 `f=0.75` 硬重置棋盘**覆盖左半屏绝大部分**，远重于 20:22 那局（`10.0` ≡ `1.0` 上限）的"最左窄条"。两局姿态不同、**不是严格对照**，但方向明确：`1.0` 远好于 `0.01`。代价是容差大会引入 ghosting（NRD 作者注释说 `0.1` 更"安全"正指此）。**这是缓解，不是根因修复。**

**下一步：仪器已装好并部署（§7.1，2026-09-29 21:1x），只差读数——按 §7.1 的流程，在同一姿态下取 `RTX_NRD_DIAG_DIFF_W=2` 与 `=3` 两张截图，把「SMB 四 tap 遮挡」与「`sizeQuality` 地板 0.1」分开。**

> ⚠️ **21:1x 那两局取到的图作废**：日志证明它们跑的是 `W=0`（仪器关着），见 §5.3 —— 从 IDE 启动时环境变量进不到游戏进程。**已改用 `.idea/runConfigurations/Minecraft_Client.xml` 的 `<envs>` 注入**，取数前先 grep 日志核对档位。

> 怎么判、色阶怎么读、会话怎么核对，全在 §7.1。**当前环境变量已设为 `RTX_NRD_DIAG_DIFF_W=2`**，
> 部署的合成 shader 是 `DEBUG=9`（左半屏色阶 / 右半屏 vp8）。**左半屏整片品红 = 环境变量没生效。**

---

## 1. 症状（用户原话，逐条）

| 场景 | 观察 |
|---|---|
| 静止、不看光源 | 历史色带 **黑** |
| **静止、看光影** | **棋盘**（历史被重置） |
| 转动视角 | **一定棋盘** |

- 「棋盘」= NRD validation 视口 8 的重置换叠，条件是 `data1.x < 1.0`（**帧数 < 1**）。
- 用户最早的表述是「转视角就黑红」「1-2 帧」，与 `c≈0` 的 1.618 帧不动点吻合。
- **用户补充过：上一轮是快速转动，但「即使匀速转动也是这样」。** 这条很重要——它排除了「相位差在匀速下自消」这类解释。

---

## 2. 机制链（代码级，全部已核对过行号）

### 2.1 帧数的递推式

```hlsl
// cpp/deps/NRD/Shaders/REBLUR_Common.hlsli:13-17
uint PackInternalData( float diffAccumSpeed, float specAccumSpeed, float materialID )
{
    // Increment history length for the nxt frame and clamp to the maximum
    diffAccumSpeed = min( diffAccumSpeed + 1.0, gMaxAccumulatedFrameNum );
```

```hlsl
// cpp/deps/NRD/Shaders/REBLUR_TemporalAccumulation.cs.hlsl:897-903
        float diffHistoryConfidence = smbFootprintQuality;
        if( gHasHistoryConfidence != 0 && NRD_SUPPORTS_HISTORY_CONFIDENCE == 1 )
        {
            float confidence = saturate( gIn_DiffConfidence.SampleLevel( gLinearClamp, smbPixelUv, 0 ) );
            diffHistoryConfidence = min( diffHistoryConfidence, confidence );
        }
        diffAccumSpeed *= lerp( diffHistoryConfidence, 1.0, 1.0 / ( 1.0 + diffAccumSpeed ) );
```

```hlsl
// cpp/deps/NRD/Shaders/REBLUR_TemporalAccumulation.cs.hlsl:1008-1009  ← 只有写回，没有 +1
    NRD_SURFACE( gOut_Data1, pixelPos ) = PackData1( diffAccumSpeed, specAccumSpeedCorrected );
```

⟹ 逐帧递推：**`A ← A · lerp(c, 1, 1/(1+A)) + 1`**，其中 `c = min(smbFootprintQuality, IN_DIFF_CONFIDENCE)`。

| `c` | 递推 | 不动点 | 对应实测 |
|---|---|---|---|
| 1 | `A ← A+1` | 涨到 `gMaxAccumulatedFrameNum`(63) | 静止 = 白/青 ✓ |
| ≈0 | `A ← A/(1+A)+1` | **(1+√5)/2 ≈ 1.618** | 转视角 = 黑红 ✓ |

**⟹ 「稳定的 1-2 帧」只有 `c ≈ 0` 这一条路径能产生。** 纯遮挡硬重置会落到 `historyFixFrameNum`(3，橙) 或直接清零。

### 2.2 `smbFootprintQuality` 的两个因子

```
smbFootprintQuality = sqrt01( bilinear(上一帧的四个 smbOcclusion 位) ) × sizeQuality
sizeQuality         = lerp( 0.1, 1.0, saturate( sqr( (NoVprev+1e-3)/(NoV+1e-3) ) ) )   ← 地板 0.1
```

**⚠️ 关键判断点：`sizeQuality` 长期贴在地板 0.1 上，单独就能给出 `A ← A·lerp(0.1,1,1/(1+A))+1` ≈ 1.75 帧的不动点。所以「1-2 帧」并**不**足以证明四个 tap 全被拒——它同样兼容 `sizeQuality` 被压住。这两条必须先分开。**

### 2.3 深度容差 ∝ 深度（这条把「远近差异」解释掉了）

```hlsl
// cpp/deps/NRD/Shaders/Common.hlsli:261-267
float GetFrustumSize( float minRectDimMulUnproject, float orthoMode, float viewZ )
{
    return minRectDimMulUnproject * lerp( viewZ, 1.0, abs( orthoMode ) );
}
```
```hlsl
// cpp/deps/NRD/Shaders/Common.hlsli:585-588
float GetDisocclusionThreshold( float disocclusionThreshold, float frustumSize, float NoV )
{
    return frustumSize * saturate( disocclusionThreshold / max( 0.05, NoV ) ); // don't be very greedy, probably 0.1 is safer ( test 223 )
}
```

- 宿主取 `disocclusionThreshold = 0.01`（日志 `[NRD-1 DISOC] threshold=0.010000 alternate=0.050000`）。
- `frustumSize` ∝ `viewZ` ⟹ **深度容差正比于深度**。3 格处约 **0.04 格**；**1 格处约 0.014 格**。
- **注意 NRD 作者自己的注释就说 `0.1` 更安全。**
- 实测吻合：视口 8 解出来是**随深度递增的梯度**（近处 0 帧 → 中带 ≈16 帧 → 远处 ≈47 帧）；用户的「看光影 = 看近处地面 ⇒ 棋盘」也吻合。

### 2.4 SMB 的视差项（已排除，别再走）

`smbParallaxInPixelsMax` 用 `gCameraDelta`（**相机位置**增量）。**纯转视角时 `gCameraDelta ≈ 0`** ⟹ `smallParallax = 1` ⟹ `cosMaxAngle = cos89° − 0.25 = −0.23`（**法线判据恒真**）、`NoVstrict = NoV`（深度阈值保留「静止斜视」的最多 20 倍宽容）。**⟹ 纯转视角时 SMB 处于三态里最宽松的那一档，它理论上比平移更不该拒史。**

### 2.5 ★ `saturate` 天花板：深度容差已顶格，调阈值到此为止

```hlsl
// cpp/deps/NRD/Shaders/Common.hlsli:585-588
float GetDisocclusionThreshold( float disocclusionThreshold, float frustumSize, float NoV )
{
    return frustumSize * saturate( disocclusionThreshold / max( 0.05, NoV ) );
}
```

`NoV = cos θ` **恒 ≤ 1**。于是有效因子 = `min(1, threshold / NoV)`：

| `threshold` | 因子 = 1（即"最宽容"）的像素 |
|---|---|
| `0.01`（NRD 默认） | 仅 `NoV ≤ 0.01`（几乎完全没有） |
| `0.1` | 仅 `NoV ≤ 0.1` |
| `0.5` | 仅 `NoV ≤ 0.5` |
| **`1.0`** | **全部**（`NoV ≤ 1` 恒成立）⟹ **已经是绝对上限** |
| `10.0`（20:22 那局） | 全部 ⟹ **与 `1.0` 逐像素完全等价，100× 数值 0 效果** |

⟹ **20:22 那一局等价于阈值 `1.0`，即深度容差已经开到顶格**：残余容差恒为
`frustumSize = minRectDimMulUnproject · viewZ` ≈ **一个像素尺度的深度**。
**在"最大宽容"下棋盘仍然存在 ⟹ 深度阈值不是瓶颈（至少不是唯一瓶颈），这条路已到尽头。**

**关于 `alternate`（已核实，不是"没用的打印"）**：宿主把它设成主值的 5×
（`VulkanCore.cpp:18998-18999`），NRD 在
`REBLUR_TemporalAccumulation.cs.hlsl:234`
用 `lerp( gDisocclusionThreshold, gDisocclusionThresholdAlternate, disocclusionThresholdMix )`
按像素混合。**但那个 mix 在我们的配置里恒为 0**，两条路都关：

- `settings.isDisocclusionThresholdMixAvailable = false`（`VulkanCore.cpp:19023`）⟹ 不走 `IN_DISOCLLUSION_THRESHOLD_MIX` 纹理；
- `CommonSettings::strandMaterialID` 用默认值 **999**（宿主从未设置），而 materialID 由 R10G10B10A2 的 alpha 给出，只可能是 `{0, 85, 170, 255}` ⟹ **永远撞不上 999**，`NRD_GetNormalizedStrandThickness` 那条路（`REBLUR_TemporalAccumulation.cs.hlsl:229-240`）也不会触发。

⟹ `disocclusionThreshold ≡ gDisocclusionThreshold`，本轮就是 `10.0`（已顶格）。

---

## 3. ⚠️ 已排除的路径（最重要的部分：不要重走）

| # | 曾以为是根因 | 怎么被排除的 |
|---|---|---|
| 1 | **我们的 `IN_DIFF_CONFIDENCE`**（`rt_di_confidence.comp` 的梯度在移动时饱和到 ≈0） | **判决实验：`FORCE_ONE=1` 后仍 1-2 帧。** 且**那个判决版至今仍在部署**（`rt_di_confidence.comp.spv` = `AD3F1AA3…` / 8464 B，本轮已核 MD5）⟹ 20:22 那一局同样有 `confidence ≡ 1` ⟹ `c = min(smbFootprintQuality, 1.0) = smbFootprintQuality`。**confidence 无罪，且本轮读数是在这条已排除的前提上拿到的，干净。** |
| 2 | **深度容差太小** | **已测到物理天花板，此路已尽**：20:22 那局用 `10.0`，而 `GetDisocclusionThreshold` 的 `saturate( threshold / max(0.05,NoV) )` 在 `threshold ≥ 1.0` 时对**每个像素**都返回 1.0（因 `NoV ≤ 1`），残余容差恒为 `frustumSize` ≈ 一个像素尺度（见 §2.5）。**在这个"最大宽容"下，左缘棋盘与右上 1-2 帧区都依然在 ⟹ `disocclusionThreshold` 不是瓶颈。** |
| 3 | **antilag 封顶历史** | `REBLUR_ANTILAG_MODE = 2`（`REBLUR_Config.hlsli:111`，非 `#ifndef`，不可覆盖），且宿主令 `luminanceSigmaScale=8 / luminanceSensitivity=10000` ⟹ `magic` 巨大 ⟹ `d = 1/(1+d·accumSpeed/magic) ≈ 1` ⟹ antilag ≈ 1，等于关闭 |
| 4 | **`mv.z` 量纲/符号** | 现有实现是**幅度式** `abs(prev) − abs(cur)`，依据 NRD 的 `UnpackViewZ(z) = abs(z·viewZScale)`（NRD 内部视深恒为正）。**已复核为正确，不要再改**。A/B 开关 `NRD_MV_Z_SIGNED_LEGACY`（默认 0） |
| 5 | **`motionVectorBias = 0` 没补抖动** | 我加过 `bias = (jitter_cur − jitter_prev)/size`，**实测无效，且按 README:564-567 对「视口 3」的定义重推可证它本来就不该加** ⟹ 已置 0（`NRD_MV_JITTER_BIAS`，默认 0） |
| 6 | **`previousClip.w` 内层闸门** | 加了旁路开关 `RESTIR_MV_IGNORE_CLIP_W=1` 实测**无任何变化** ⟹ 不是它（默认已回 0） |
| 7 | **相机 UBO 的代际竞态**（CPU 比 GPU 快一帧，单份持久映射） | 加了写 UBO 前 `vkDeviceWaitIdle`（`RTX_CAMERA_UBO_SERIALIZE=1`）**实测无变化** ⟹ 与症状无关（但这条竞态本身是真的，见 §6） |
| 8 | **`historyValid` 闸门恒为 0** | Java 侧 `updateCamera` 挂在 `renderLevel` 的 `@At("HEAD")`、每帧 `frameCount++` ⟹ 连号条件其实一直成立。日志 `(historyValid=1,frameCount=120)`。**这一条我判断错了一次，已改成原生侧快照（见 §6），但它不是根因** |
| 9 | **我们把 `IN_VIEWZ` 写成了超范围哨兵** | **视口 2（线性 viewZ）整屏纯绿**（绿=有效、红=超出 denoisingRange）⟹ **处处在范围内，无哨兵** |
| 10 | **几何来源（水库接收面 vs 主命中）** | 已切到主命中（binding 7 + 15），实测无改善。改动本身有依据（NRD 契约要求几何来自主命中 A），**建议保留** |
| 11 | **DI 链路已死 / `IN_DIFF` 全 0** | **这是我自己的仪器 UB 造出来的假象，见 §4.1。DI 一直活着** |
| 12 | **我们的 MV 差半个到一像素** | **建立在一台坏仪器上，见 §4.2。全部作废** |

---

## 4. ★★★ 仪器清单：哪些可信、哪些是坏的

**这一节是整份交接里最该先读的。我这几轮最大的时间损失就来自没有先验证仪器。**

### 4.1 坏仪器 A — 合成端读 50/51/52（**已修**）

`rt_nrd_composite.comp` 是 **compute**，而 `b[50]/b[51]/b[52]` 原来只声明了
`VK_SHADER_STAGE_RAYGEN_BIT_KHR`。**shader stage 不在 binding 的 stageFlags 里 = 未定义行为，实测稳定读回 0。**

它造出过两个假结论：「`IN_DIFF` 全屏恒为 0」「本地光 DI 整条链已死」。

**反证在同一张表里**：`b[41]`（SSBO，`COMPUTE|RAYGEN`）读得到、`b[53]`（`COMPUTE`）读得到。

**已修**：`cpp/VulkanCore.cpp:12210-12252` 给 50/51/52 补了 `VK_SHADER_STAGE_COMPUTE_BIT`。
**教训**：诊断叠层里每加一个 binding 读取，都要核对该 binding 的 `stageFlags` 是否包含合成端的 stage。

### 4.2 ★ 坏仪器 B — **NRD validation 的视口 3（MV 检查）**（用它得出的结论全部作废）

```cpp
// cpp/deps/NRD/Source/Denoisers/Reblur_Specular.hpp:234-238
        // Outputs
        PushOutput(AsUint(ResourceType::IN_MV));      // ← NRD 自己往 IN_MV 里写它跟踪的运动
```
```cpp
// cpp/deps/NRD/Source/Reblur.cpp:65-78
#define REBLUR_ADD_VALIDATION_DISPATCH(data2, diff, spec) \
    PushPass("Validation"); \
    { \
        PushInput(AsUint(ResourceType::IN_MV)); \
```
```cpp
// cpp/VulkanCore.cpp:17188-17195（宿主早就知道这件事，只是没用它来质疑仪器）
    // IN_MV is the one NRD input the application never writes: ...
    // NRD's TemporalStabilization pass *writes its own tracked motion* into the same
    // image (Reblur_Diffuse.hpp pushes AsUint(ResourceType::IN_MV) as an output),
```

**⟹ `IN_MV` 是 NRD 的**输出**；Validation pass 最后跑、读到的就是 NRD 自己刚写的那份 ⟹ 视口 3 量的是 **NRD 内部跟踪的运动**，不是我们的 MV。**

**决定性证据：三种完全不同的 MV 实现（0 / 中心 uv 真值 / 绕过 clip.w 闸门）画出的视口 3 **逐像素相同**。** 一片场如果真由我们的 MV 驱动，不可能纹丝不动。

**教训**：任何「把我们的输入读回来做对比」的仪器，都要先确认 NRD 没有把同一个资源当输出写。

### 4.3 可信仪器

| 仪器 | 读什么 | 为什么可信 |
|---|---|---|
| **validation 视口 8**（DIFF FRAMES） | NRD 内部 transient `DATA1` | 不是我们的资源，NRD 不覆盖它 |
| **validation 视口 2**（线性 viewZ） | `abs(z·viewZScale)` | 绿=有效 / 红=超出 denoisingRange |
| **validation 视口 0** | 世界网格 1 格 + 两个 mini 视口（抖动样本是否在 `[-0.5,0.5]` 像素内） | NRD 内部 |
| **合成端 binding 53 的 `.w`** | 历史长度（宿主开了 `returnHistoryLengthInsteadOfOcclusion=true`） | `COMPUTE` stage，可读；且 `.w` 的语义由 NRD 保证 |
| **合成端 binding 41** | DI 原始值 | `COMPUTE|RAYGEN`，可读 |

#### 视口 8 的精确口径（`REBLUR_Validation.cs.hlsl`，已逐行核对）

- 视口布局 `0 1 2 3 / 4 5 6 7 / 8 9 10 11 / 12 13 14 15`；**vp8 = row2,col0**。
- 每个 tile 里装的是**整屏缩小 4×**（`viewportPixelPos = viewportUv * gResolutionScale * gResourceSize`，`VIEWPORT_SIZE=0.25`），
  **不是"屏幕的 1/16 裁剪"。所以一个 tile 就是一张全屏缩略图。**
- 棋盘判据在 **validation 图坐标**里：`Sequence::CheckerBoard( pixelPos >> 2, 0 )`，即 **4×4 像素为格**（周期 8）。
- 关键几行：
  ```hlsl
  // :70
  data1 *= REBLUR_MAX_ACCUM_FRAME_NUM;
  // :290-293
  float f = 1.0 - saturate( data1.x / max( gMaxAccumulatedFrameNum, 1.0 ) );
  f = checkerboard && data1.x < 1.0 ? 0.75 : f;
  result.xyz = Color::ColorizeZucconi( viewportUv.y > 0.95 ? 1.0 - viewportUv.x : f * float( !isInf ) );
  ```
  **`f = 0.75` 这条分支只有这一个来源** ⟹ 屏幕上出现 `Zucconi(0.75)` 的颜色，**就等价于**"存在
  `data1.x < 1.0`（硬重置）且落在棋盘类里的像素"。**这是全篇最硬的一条判据，可以排他性地用。**
- 视口顶端 5%（`viewportUv.y > 0.95`）画的是色标 `1 − viewportUv.x`（一根完整光谱条），不是数据。

**`Color::ColorizeZucconi` 全量标定**（`cpp/deps/NRD/build/_deps/mathlib-src/ml.hlsli:1147-1167`，已用源码手算）：

| `f` | 颜色 | 换算到帧数（`f = 1 − N/63`） |
|---|---|---|
| 0.00 | `(0,0,0.026)` 深藏青 | 63 帧；**注意 `isInf` 也输出黑 ⟹ 深蓝有二义** |
| 0.25 | 蓝 | ≈47 帧 |
| 0.40 | `(0.241,0.648,0.451)` 青绿 | ≈38 帧 |
| 0.50 | `(0.057,0.809,0.346)` 绿 | ≈32 帧 |
| 0.60 | `(0.544,0.845,0.157)` 黄绿 | ≈25 帧 |
| 0.66 | `(0.749,0.807,0.110)` 橄榄黄 | ≈21 帧 |
| **0.75** | `(0.935,0.666,0.252)` **浅橙/褐 tan** | **只能来自 `checkerboard && data1.x < 1.0`** |
| 0.80 | `(0.974,0.545,0.255)` | ≈13 帧 |
| 0.90 | `(0.916,0.215,0.090)` | ≈6 帧 |
| 0.95 | `(0.819,0.078,0.372)` | ≈3 帧 |
| **0.975** | `(0.753,0.099,0.295)` **洋红/粉** | **≈1.6 帧 ⟸ 就是"1-2 帧"症状** |
| 1.00 | `(0.677,0.112,0)` 红 | 0 帧 |

**⟹ `c≈0` 的 1.618 帧与 `sizeQuality` 地板 0.1 的 1.75 帧，在这条色阶上不可分辨**（都落进 0.95–0.98 的洋红档）。

### 4.4 ★★ 坏仪器 C — **所有 DEBUG 可视化都穿过 DLSS-RR**

```cpp
// cpp/VulkanCore.cpp:38025-38030
// The colour image is read by the DLSS/RR chain next.
```

合成 pass 的输出 `g_rtOutputImage` **紧接着就被 DLSS/RR 链读走**。所以验证视口、DEBUG==7 分屏、
binding 53/57 探针——**没有一个是"直出屏幕"的**，全都要经过 DLSS-RR 上采样 + 时域累积 + 色调映射。

**后果**：`707×376` 上的高频图案（棋盘格 4×4 vp 像素 → 合成后 ~24×48 屏幕像素）会被**抹平/混淆**；
颜色也会被色调映射改写。**⟹ 这台仪器只能读"大面积、低梯度"的量，不能用来定量读高频图案。**

**这一条解释了本会话最贵的两次误判**：粗粒度自相关探针在含 700 行的区域上做 lag 统计，
曲线单调、没有棋盘的 24/48 尖峰，于是得出"没有棋盘"的结论——**那只是相位混合 + DLSS 抹平造成的假阴性**。
真正读出棋盘的方法只有一条：**先减去标题栏、把显示坐标映射回 vp 像素，再"固定窄行带 + 分 x 单元"比对相位**（见 §4.5）。

### 4.5 一条可复用的判读配方（本轮实测有效）

1. 截图是 **2120×1200**，**客户区顶部有 ~38 px 的窗口标题栏**，底部有任务栏 + hotbar。
   **所有 y 必须先减 ≈38 再换算**（我第一版没减，纵向漂了 ~5 个 vp 行，直接造成假阴性）。
2. 左半屏 `x∈[0,1060]` = vp8 tile（176 texels 宽）被拉 **2×**；`y` 被拉 **4×**。
   换算：`vp_x = display_x / 6.02`、`vp_y = (display_y − 38) / 12.0`。
3. **不要做大范围均值/自相关**（相位混合）。取**一条 16–32 px 高的窄行带**，在带内把 x 切成
   24 px 宽的单元逐单元算平均色，**再看相邻单元是否反相、以及 y 方向（≈96 px）是否换相**。
4. 把读到的 RGB 去查 §4.3 的 Zucconi 表反推帧数；**只要读到 tan(0.75) 就是硬重置，无二义**。

**怎么开仪器**：宿主 `NRD_LOCAL_DI_VALIDATION`（默认 **1**，`VulkanCore.cpp:3329`）已开；合成端用
`-DNRD_COMPOSITE_DEBUG=7 -DNRD_COMPOSITE_DEBUG_VP_LEFT=<视口号> -DNRD_COMPOSITE_DEBUG_VP_RIGHT=<视口号>`
重编即可左右分屏放大两个视口。
**⚠️ 当前部署已换成 `NRD_COMPOSITE_DEBUG=9`**（不再走 DEBUG=7）：左半屏 = P0 诊断值色阶、
右半屏 = vp8，见 §7.1。要回到 20:58 的 `LEFT=8 RIGHT=3` 口径，把
`cpp/shaders/rt_nrd_composite.comp.spv.bak_debug7_20260929`（`E2E10E00F9FB…` / 12360 B）拷回 `run/shaders/` 与 `run/`。

---

## 5. 当前部署状态（截至 2026-09-29 21:18）

### 5.1 DLL

`497FB784965BE6BB83733951ADA07D0E`，5633024 B（2026-09-29 21:1x 已核），**六处齐更**：
`rtx_backend.dll` / `run/` / `src/main/resources/` / `src/main/resources/natives/windows/` /
`build/resources/main/` / `build/resources/main/natives/windows/`。
**这是 P0 仪器版**（`[BUILD] orion-rt tag=2026-09-29-p0-diag`）。上一版（MV 探针版，`DA2C67FA…` / 5620224 B）的备份是
`*.bak_20260929_pre_mvprobe`，可回退。

**生效的编译期开关（源码默认值，行号为本轮实测）：**

| 开关 | 值 | 位置 | 说明 |
|---|---|---|---|
| `NRD_USE_REAL_MOTION_VECTORS` | **1** | `VulkanCore.cpp:86` | **用我们写的真实 MV**（`motionVectorScale=(1,1,1)` + `isMotionVectorInWorldSpace=0`）。日志确认。**本文档 17:55 那版写的是 0，已过时** |
| `NRD_USE_25D_MOTION_VECTORS` | 1 | `:110` | 2.5D |
| `NRD_LOCAL_DI_VALIDATION` | 1 | `:3384` | validation 层开着 |
| `NRD_LOCAL_DI_CONFIDENCE` | 1 | `:19343` | confidence 路径开着（但被 §5.2 的 FORCE_ONE 架空） |
| `NRD_MV_JITTER_BIAS` | 0 | `:18832` | 见 §3 第 5 条 |
| `RTX_CAMERA_UBO_SERIALIZE` | 1 | `:30953` | 写相机 UBO 前 `vkDeviceWaitIdle`（**有约 10% 帧时开销**，见 §6） |

### 5.2 Shader（`run/shaders/` 与 `cpp/shaders/` **两侧已同步**）

| 文件 | MD5 | 字节 | 编译口径 |
|---|---|---|---|
| `rt_restir_final.rgen.spv` | `64A11CFE9B2D57E6161B75F177CAC446` | 47772 | MV 探针版（多写一张 binding 57 私有读回图） |
| `rt_nrd_composite.comp.spv` | `C86E956AE1EA4D2F53E277A85E8A8F39` | 13404 | `-DNRD_COMPOSITE_DEBUG=9`（P0 仪器读数屏：左半屏色阶 / 右半屏 vp8）。DEBUG=7 版已备份为 `cpp/shaders/rt_nrd_composite.comp.spv.bak_debug7_20260929`（`E2E10E00F9FB…` / 12360 B） |
| `rt_di_confidence.comp.spv` | `AD3F1AA3AF5C9ABF39D21BEF14F65B6F` | 8464 | `-DNRD_DI_CONFIDENCE_FORCE_ONE=1`（判决用，**可回退**；本轮的"confidence 无罪"就靠它） |

三个 hash 均在 2026-09-29 21:18 用 `Get-FileHash` 核过，`run/shaders/` 与 `cpp/shaders/` 两侧一致。
**⚠️ 归档里有个同名但不一致的 `variants/rt_nrd_composite.comp.spv.debug7`（`994D91C9…`），已弃用，别拿它回退。**

**源码内剩余开关（`cpp/shaders/rt_restir_final.rgen`）：** `NRD_GEOMETRY_FROM_PRIMARY=1`(:2698)、`NRD_GEOMETRY_CENTER_UV=1`(:2746)、`RESTIR_MV_MATRIX_DIAG=0`(:2940)、`RESTIR_MV_IGNORE_CLIP_W=0`(:3017)。

**⚠️ 一致性陷阱（方向已变）**：宿主与 rgen **现在都是 `NRD_USE_REAL_MOTION_VECTORS=1`**（一致，无坑）。
但要记住这个坑存在过：宿主曾经是 `0`（NRD 自跟踪）而 **rgen 源码默认就是 `1`** ——
那种组合下只要重编 rgen 时漏掉 `-DNRD_USE_REAL_MOTION_VECTORS=0`，它就会**每帧把 MV 写进 `IN_MV`，
从而每帧覆掉 NRD 自跟踪的运动状态**。切换模式时宿主与 rgen **必须同时**改，
改完用日志里的 `motionVectorScale` / `isMotionVectorInWorldSpace` 核对
（`(1,1,1)` + `0` = 真实 MV 模式；`(0,0,0)` + `1` = 自跟踪模式）。

### 5.3 环境变量

> **★★ 2026-09-29 21:3x 更正：从 IDE 启动时，这些环境变量根本进不到游戏进程。**
> `.idea/runConfigurations/Minecraft_Client.xml` 的 `<envs>` 原本是**空的**，而 IDE 进程早于变量设置就已启动
> ⟹ 游戏继承的是 IDE 的旧环境。证据（`run/rt_debug.log`）：
> - 自 **20:57 起每一局**都是 `[NRD-1 DISOC] threshold=0.010000` —— 即 **DLL 内建默认值**，不是文档多处假定的 `0.1`；
> - `21:19:48` 那一局 `[NRD-1 DIAG] RTX_NRD_DIAG_DIFF_W=0` —— **P0 仪器整局没生效**，该局两张截图作废。
>
> **已把 `RTX_NRD_DIAG_DIFF_W=3` 直接写进该运行配置的 `<envs>`。**
> ⚠ 若 IDE 里已加载该配置，需**重启 IDE**（或在重启配置 UI 的 Environment variables 里确认）才会读到。
> **每局取数前先 `grep "\[NRD-1 DIAG\]" run/rt_debug.log` 核对档位，不要凭设置相信。**

| 变量 | User 作用域 | IDE 启动时的**实际**值 | 说明 |
|---|---|---|---|
| `RTX_NRD_DIAG_DIFF_W` | `3` | 由运行配置的 `<envs>` 注入（此前为**无** ⇒ 0） | P0 仪器档位（§7.1）：`0`=关 / `1`=4tap×sizeQ / `2`=sizeQuality / `3`=4tap / `4`=历史长度÷63。改后**必须重启游戏** |
| `RTX_NRD_DISOCCLUSION_THRESHOLD` | `0.1` | **`0.01`（DLL 默认）** | 本文其余处按 `0.1` / `10.0` 叙述的结论，请以日志里的实际打印为准；由 IDE 启动时一直是 `0.01`（最不宽容的一档） |

- 最近一局（`run/rt_debug.log`）的完整序列：
  `19:43:47 / 20:02:44 / 20:06:41 / 20:14:31 → threshold=0.100000`；
  **`20:22:40 → threshold=10.000000 alternate=50.000000`（当时实验；用户截图 20:22:53 就在这一局内）**；
  随后 `20:57:09 / 20:58:38 / 21:01:42 / 21:20:53 → threshold=0.010000`（**环境变量开始失效**）。
- 删除：`[Environment]::SetEnvironmentVariable('RTX_NRD_DISOCCLUSION_THRESHOLD', $null, 'User')`；
  P0 档位同理：`[Environment]::SetEnvironmentVariable('RTX_NRD_DIAG_DIFF_W', $null, 'User')`（删掉 = 关仪器，回到原行为）。

---

## 6. 构建 / 部署 / 启动（含踩过的坑）

### 6.1 启动游戏（**三个坑**）

```powershell
$env:JAVA_HOME='D:\RTX'                    # ← 坑1：不设会编译失败
$env:GRADLE_USER_HOME='D:\GTX'             # ← 坑2：不设会去重下整套依赖/资源
$env:RTX_NRD_DISOCCLUSION_THRESHOLD='0.1'  # ← 坑3：Gradle 守护进程用的是它自己启动时的环境
cd 'd:/fabric-example-mod-26.2/fabric-example-mod-26.2'
.\gradlew.bat --stop                       # 必须：旧守护进程看不到新环境变量
.\gradlew.bat --no-daemon runClient        # 由本进程 fork 游戏 ⇒ 继承上述环境
```

- **坑 1**：`JAVA_HOME` 为空时 Gradle 自动探测工具链，会误判成
  `D:\GTX\daemon\9.6.0` 并报 `does not provide the required capabilities: [JAVA_COMPILER]`。
  本机 JDK 在 **`D:\RTX`**（有 `javac.exe`）。
- **坑 2**：`GRADLE_USER_HOME` 是**机器级变量** `D:\GTX`；新开的进程可能没继承到，显式设一次。
- **坑 3**：Loom 的 `runClient` 由 **Gradle 守护进程 fork**，守护进程沿用**它启动时**的环境 ⟹ 先 `--stop`。
  或者把变量设成用户级持久变量（本次已这么做），并确保守护进程是之后新起的。

### 6.2 编译单个 shader

```powershell
cd cpp/shaders
& 'C:\VulkanSDK\1.4.309.0\Bin\glslangValidator.exe' -V --target-env vulkan1.2 `
   -DRESTIR_DI_LOCAL_EMA=0 rt_restir_final.rgen -o <out>.spv
```
**编完必须同时覆盖 `run/shaders/`（游戏实际读的）与 `cpp/shaders/`（源侧，构建会回抄）。**
本轮有一次实验就是因为只放了 `cpp/shaders/` 而**从未在游戏里跑过**。

### 6.3 重编 DLL 并部署六处

```powershell
cmake --build cpp/build --config Release --target rtx_backend
# 然后拷到：rtx_backend.dll / run/ / src/main/resources/ / src/main/resources/natives/windows/
#           build/resources/main/ / build/resources/main/natives/windows/
```
游戏若在运行会锁住 `run/rtx_backend.dll`，需重试或先退出游戏。

---

## 7. 下一步（按优先级）

**结论先说：`disocclusionThreshold` 这条路已走完（§2.5 / §3#2），`REBLUR_SHOW` 这条路是死的（§7.0b）。剩下唯一可行且干净的动作，是把 `smbFootprintQuality = sqrt01(四 tap 遮挡) × sizeQuality` 的两半拆开——而它必须打 NRD 补丁。**（✅ 已实现并部署，读数流程见 §7.1。）

### 7.0 本轮否掉的两条"看起来最省事"的路

**（a）继续调 `disocclusionThreshold`** —— 已到物理天花板，见 §2.5。**不要再动它。**

**（b）`REBLUR_SHOW = 4`（一度以为是最省事的路，实际不通）**
`REBLUR_Config.hlsli:55-59` 确实有 `REBLUR_SHOW_SURFACE_HISTORY_CONFIDENCE = 4`，但四条都堵：

1. `REBLUR_TemporalAccumulation.cs.hlsl:869-880` 那段 `#if REBLUR_SHOW == …` **整段套在 `#if( NRD_HAS_SPEC )` 里**，
   而我们是 **REBLUR_DIFFUSE ⟹ `NRD_HAS_SPEC == 0`** ⟹ 那段根本不参与编译，`virtualHistoryAmount` 恒为 0。
2. 我们唯一能看到 `virtualHistoryAmount` 的地方是 **validation 视口 7**（`REBLUR_Validation.cs.hlsl:272` `result.xyz = data2.x`），
   而它在 diffuse 下恒为黑。
3. `REBLUR_SHOW != 0` 会让 `REBLUR_Common.hlsli:316` 把时间累积权重 `w` 置 0 —— **等于把被测对象本身关掉**，
   即便能显示也不可信。
4. `OUT_DATA2` 的 `virtualHistoryAmount` 还是 TemporalStabilization 内部 SMB/VMB 的 lerp 权重
   （`REBLUR_TemporalStabilization.cs.hlsl:270`），写别的东西有副作用。

**（c）只改 `REBLUR_Validation.cs.hlsl`（同样不可行）**
`REBLUR_Validation.resources.hlsli:23-29` 只有 **7 个输入**：Normal_Roughness / ViewZ / Mv / **Data1 / Data2** / Diff / Spec。
**没有任何上一帧输入** ⟹ validation pass **无法**自己重算 `sizeQuality`（要上一帧法线求 `NoVprev`）
或四 tap 遮挡（要上一帧 viewZ/法线）。**必须改 TemporalAccumulation 或 TemporalStabilization。**

### 7.1 P0 — 把诊断值塞进 `OUT_DIFF.w`（唯一可行、且不需要新管道）

**✅ 已实现并部署（2026-09-29 21:xx）。** `diff.w` 是 RGBA16F，能承载 >1 与负值；而
`OUT_DIFF_RADIANCE_HITDIST` 的 `.w` 在本工程里已被宿主用 `gReturnHistoryLengthInsteadOfOcclusion`
征用，**且没有任何消费者读它**（`VulkanCore.cpp:19250` 的注释），所以它是一根现成的示波器线。

⚠️ **不要**去改 `REBLUR_TemporalAccumulation.cs.hlsl` 的 `gOut_Diff.w` —— 那条会被
Blur → Post-blur 一路写成 `DIFF_HISTORY`（**历史本体**），等于**改行为**而不只是改显示。
`REBLUR_TemporalStabilization.cs.hlsl` 才是 binding 53 的**最终写入者**
（`Reblur_Diffuse.hpp:225` + `VulkanCore.cpp:17824`；其后的 Split screen 在 `splitScreen=0` 时直接 return，什么都不写）。

| 环节 | 落点 | 内容 |
|---|---|---|
| 总开关 | `REBLUR_Config.hlsli` | `NRD_DIAG_DIFF_W`（默认 **1** = 启用仪器代码，只多一个 uniform 分支；**该头文件也被 C++ 编译，必须保持纯 ASCII**，否则 C4819 在 /WX 下直接报错） |
| 写值 | `REBLUR_TemporalStabilization.cs.hlsl:181-231` | `.w` = 选中的诊断量；`sizeQuality` 在该 pass 里按 TA 的同一公式重算（只依赖当前帧 N/X/Xprev 与上一帧矩阵，不依赖历史 ⇒ 逐位相同） |
| 选值 | **运行时** `gDebug` | 宿主从环境变量 `RTX_NRD_DIAG_DIFF_W` 灌进 `CommonSettings::debug`（`VulkanCore.cpp` 的 `[NRD-1 DIAG]`）。REBLUR **全仓没有任何 shader 读 `gDebug`**（只有声明与赋值）⇒ 免费的、每帧可改的标量 |
| 显示 | `run/shaders/rt_nrd_composite.comp.spv` | 编成 `-DNRD_COMPOSITE_DEBUG=9`：**左半屏 = 该值色阶，右半屏 = validation 视口 8**，一张截图同时给「凶手是谁」和「这一片几帧」 |

**为什么做成运行时而不是编译期**：NRD 的 SPIR-V 是编进 `rtx_backend.dll` 的，按编译期开关换读数
就得每换一个因子重链宿主 + 重部署六处。现在**改环境变量重启即可，不需要重编**。

**色阶**（值 ∈ [0,1]）：红 `<0.105`（撞 `sizeQuality` 地板 0.1，或 4tap ≈ 0）→ 橙 `0.105~0.2` →
黄 `0.2~0.5` → 绿 `0.5~0.95` → 白 `>0.95`；**品红 = 值不在 [0,1]**（NaN，或仪器关着时 `.w`
是 0~63 帧的历史长度）。⟹ **左半屏整片品红 = `RTX_NRD_DIAG_DIFF_W` 没生效**（环境变量只对新进程生效）。

**读数流程**：`RTX_NRD_DIAG_DIFF_W=2` 看 `sizeQuality` → `=3` 看纯 4-tap → `=1` 看合成值 → `=0` 关。
每次改完都要**重启游戏**；会话与档位的对应关系看 `run/rt_debug.log` 的
`[NRD-1 DIAG] RTX_NRD_DIAG_DIFF_W=N`（另有一行 `[BUILD] orion-rt tag=2026-09-29-p0-diag`）。

**判读（同一姿态，比对左缘棋盘带与右上 1-2 帧区）**：
- **W=2 大片红、W=3 不红** ⟹ 凶手是 `sizeQuality` 地板 ⟹ 进 P1；
- **W=3 大片红、W=2 不红** ⟹ 凶手是四 tap 遮挡拒史 ⟹ 进 P2；
- 两者都红 ⟹ 两条都在压，先修更便宜的 P1；
- 两者都不红而 W=1 红 ⟹ 只有乘性叠加才越线，按主导项修；
- W=1/2/3 都不红但仍 1-2 帧 ⟹ `A ≈ 1.6 帧` 另有来源，§2.1 的模型需要修订。

**附：`W=0` 的语义 与 21:23 两张图的读数（2026-09-29 21:3x 实测修正）**

`NRD_DIAG_DIFF_W` 的 `else` 分支就是原来那行 `diff.w = data1.x`，而
`data1 = UnpackData1( gIn_Data1 )` = `round( p * REBLUR_MAX_ACCUM_FRAME_NUM )`
（`REBLUR_Common.hlsli:55-66`；`REBLUR_DATA1_TYPE = float2`）
⟹ **`W=0` 时 `.w` 是未解包的 0~63 帧**，而 DEBUG==9 的色阶按 [0,1] 上色 ⟹
**红 = 0 帧 / 白 = 1 帧 / 品红 = ≥2 帧**。

21:23 那两张（实际是 `W=0`，见 §5.3）读出：
- `21.23.07.97.png`：左半屏 ≈ **全屏 0 帧**（约 10% 为 1 帧）。这同时解释了右半屏 vp8 为什么是棋盘 ——
  vp8 的棋盘判据正是 `data1.x < 1.0`，见 §2.1；
- `21.23.16.10.png`：**64% ≥ 2 帧、33% 仍为 0 帧**。

**⚠️ 由此暴露一处文档级错误（§2.1 的递推式）**：`A ← A·lerp(c,1,1/(1+A)) + 1` 把两处不同时机的量
混在一起了。按代码重推 —— TA 读的 `diffAccumSpeed`（`TA:310-311`）来自 `PREV_INTERNAL_DATA`，
即上一帧 `PackInternalData` 写的 `min(D+1, 63)`，是**已经 +1 过的量**；它再乘
`lerp(c,1,1/(1+A))`（`TA:903`），最后 `TA:1008` 直接 `PackData1(diffAccumSpeed)`（**无 +1**）。
于是 `c≈0` 的不动点是 `D = (D+1)/(D+2)` ⇒ **D = 0.618**，量化后稳定在 **1 帧**，**不是 1.618 帧**。

⟹ **实测的大面积「0 帧」用这条递推解释不了（它恒 ≥ 1）**。只能是这些像素的 `gOut_Data1`
**压根没被写**（`TA:1003-1005` 的 `#else float diffAccumSpeed = 0;`，或 early-out / 越界后保留了
纹理清零值 0）。**这是目前最有指向性的新线索：症状或许不是"历史被慢慢压到 1 帧"，而是"整片像素的历史没写进去"。**
下一步取 `W=1/2/3` 的读数就是为了一并验证它。

**本次部署哈希**：`rtx_backend.dll` = `497FB784965BE6BB83733951ADA07D0E` / 5633024 B（六处一致，
tag `2026-09-29-p0-diag`）；`run/shaders/rt_nrd_composite.comp.spv` = `C86E956A` / 13404 B（DEBUG=9）；
`REBLUR_TemporalStabilization.cs.spirv.h` = 189232 B blob，已按整段字节在 DLL 里命中（offset 3127200）。
⚠️ 中途那版编译期 `NRD_DIAG_DIFF_W=2`（tag `2026-09-29-p0-w2`，DLL `77CB6A82…`）**已被本版取代**，别再部署。

**回退 P0**：`NRD_DIAG_DIFF_W` 置 0 重编（或只把 `RTX_NRD_DIAG_DIFF_W` 删掉），并把
`cpp/shaders/rt_nrd_composite.comp.spv.bak_debug7_20260929` 拷回 `run/shaders/`（DEBUG=7 版，
`E2E10E00F9FB` / 12360 B），即回到 20:58 那两张截图的观测口径。

**✅ 这条链已经验证过了（2026-09-29 20:58）**——原来那句"开工第一件事"作废，改成下面这套实测流程。

**验证方法（可复用）**：在 `REBLUR_Validation.cs.hlsl` 的 vp8 分支里加**两枚纯显示 canary**
（① 把 `DIFF FRAMES` 中间那个空格换成 `'X'`；② 在 tile 内画一块纯洋红 `float3(1,0,1)` 方块，
选洋红是因为 Zucconi 色阶不产生它；用色块而不是文字，是因为所有 DEBUG 输出都穿过 DLSS-RR，
单个字形会被抹平）。重编后启动一次——**洋红方块与 `DIFFXFRAMES` 同时上屏即通过**。

```powershell
# ★ 关键坑：NRDShaders.vcxproj 的 AdditionalInputs 为空且 VerifyInputsAndOutputsExist=false，
#   MSBuild **不跟踪 .hlsl 输入**；不删这个 stamp 目录，ShaderMake 根本不会被调用。
Remove-Item -Recurse -Force 'cpp\deps\NRD\build\CMakeFiles\NRDShaders'
& 'D:\13136\bin\cmake.exe' --build 'cpp\deps\NRD\build' --config Release --target NRD
& 'D:\13136\bin\cmake.exe' --build 'cpp\build'       --config Release --target rtx_backend
# 全链约 1 分钟（ShaderMake 那步只占 ~0.5 s），然后拷六处
```

**为什么这条链本来该被怀疑、现在不必了**：NRD 是**静态库 + 内嵌 SPIR-V**
（`NRD_EMBEDS_SPIRV_SHADERS=ON` + `--headerBlob`，`cpp/deps/NRD/CMakeLists.txt:22,342`），
SPIR-V 直接编进 `NRD.lib` 的字节数组；而 `run/shaders/` 里**没有任何 `REBLUR*.spv`**，
所以不存在"磁盘上的旧 shader 顶掉内嵌 shader"这条暗路。

**怎么证明"真的嵌进去了"（比时间戳/大小硬，且不用启动游戏）**：把
`cpp/deps/NRD/_Shaders/REBLUR_Validation.cs.spirv.h` 里的十进制字节数组解析出来，
在 `run/rtx_backend.dll` 里**按整段字节搜索**（`.IndexOf` ≥ 0 = 命中）。

⚠️ **两个反直觉的坑，别再用错判据**：
- **不要拿 `OpConstant` 的具体值当指纹**。`Text::Print_ch` 走 `Char_A + ch - 'A'`
  （`ml.hlsli:3124-3138`，`Char_A = 14`），`'X'` 折叠成 37 —— 但**加完第二枚 canary 后 37 就消失了**，
  dxc 的常量编码会随后续改动漂移。同理，**同一份源码重编不保证同 hash**：clean revert 后
  `spirv.h` / `NRD.lib` 的**大小**精确回到 canary 前的 `192011` / `7799340` B，**MD5 却不同**。
  判据请用「整段 blob 逐字节在 DLL 里」+「本轮改动独有的字面量出现在 blob 里」，别用 hash 相等。
- **部署六处之前先退出游戏**。`run/rtx_backend.dll` 会被 JVM 锁住，拷贝直接失败
  （`The process cannot access the file ... being used by another process`），留下**半部署态**。

顺带：DLL 里有 `[BUILD] orion-rt tag=...`（`VulkanCore.cpp:31668`）随二进制走，跑一次看
`run/rt_debug.log` 就能对上版本——**每次重编 DLL 时顺手把 tag 递增**，这是判断"游戏到底加载了哪一份 DLL"最省事的工具。

### 7.2 P1 — 若指向 `sizeQuality`（地板 0.1）

`sizeQuality = lerp(0.1, 1.0, saturate(sqr((NoVprev+1e-3)/(NoV+1e-3))))`，只依赖 `NoVprev/NoV`，
**与 `disocclusionThreshold` 无关**（这正是 §2.5 调阈值无效的原因）。查：
- 我们的**法线图**在那些像素上是否逐帧抖动（法线来自 binding 15 的主命中法线）；
- `IN_VIEWZ` 与法线是否描述**同一个面**；
- `NoVprev` 取自上一帧法线，若上一帧历史已被重置，`NoVprev` 会持续跑偏 ⟹ **自我强化的死锁**（这一条最像）。

### 7.3 P2 — 若指向四 tap 遮挡

那条链是 `IsInScreenBilinear`（屏幕边界）+ 法线 + 深度 三选一。
**屏幕边界这一项已被本轮实测坐实**：左缘 ~4.7% 竖带上的 `f=0.75` 硬重置，
与用户 "向左转左边蓝" 的读数一致 —— 转视角时最左列的上一帧采样点确实落到画面外，
这属于**几何上合理**的拒史（"物理正确但观感差"）。

### 7.4 P3 — 结构性观察

- **宽模糊作用在商上而不是分子分母上**：`rt_di_confidence.comp` 的 mode 1 宽模糊是在除法**之后**做的。
  **对已经饱和到 1 的商做模糊救不回来**（confidence 虽已排除，但这条原则对任何「相对差」型置信度都成立）。
- **`IN_MV` 是共享可写资源**（NRD 会把它自己跟踪的运动写回同一张图）⟹ **视口 3 永远不能用来评价我们的 MV**（§4.2）。
  要评价自己的 MV，只能用合成端 binding 57 那张"NRD 描述符里根本没有"的私有读回图。

---

## 8. 其他重要事实（省得再查）

| 事实 | 值 | 来源 |
|---|---|---|
| 内部渲染分辨率 | **707×376** | `[PERF L10 GPU] dispatch=707x376` |
| 输出分辨率 | **2122×1129** | `[DLSS-4.1] output=2122x1129` |
| **帧生成在开** | `[FG-1 STATE] presented=1`、`numFramesToGenerateMax=5` | 日志 |
| DLSS 在开 | `[DLSS-4.1] Evaluate render=707x376 output=2122x1129` | 日志 |
| NRD tuning | `maxAccumulatedFrameNum=63 maxFastAccumulatedFrameNum=6 historyFixFrameNum=3` | `[NRD-1 TUNING]` |
| 退出时崩溃 | `runClient` 以 `0xC0000005` 结束，发生在 `[FG-3 SWAPCHAIN DESTROY]` 之后 | 很可能是 FG/Streamline 拆除期的问题，**与本问题无关**，但值得单独记一笔 |
| 关键日志字段 | `[NRD-1 DISOC]` / `[NRD-1 TUNING]` / `[NRD-1 MV]` / `[SPV] LOAD` | `run/rt_debug.log` |
| `[SPV] LOAD … size=N` | 可确认**游戏实际加载的那份 spv 是不是你部署的那份** | 省得上机一次白跑 |

---

## 9. 备份索引（`cpp/shaders/variants/`）

本轮相关的备份：

| 文件 | 内容 |
|---|---|
| `rt_di_confidence.comp.spv.pre_confab` / `.pre_force1` | 8412 B，13:56（`FORCE_ONE=0` 的旧口径） |
| `rt_di_confidence.comp.spv.pre_confab_src1411` | 8424 B，14:11（那次编了但没部署的版本） |
| `rt_nrd_composite.comp.spv.pre_viewprobe` | 16008 B，16:20（分屏视口版） |
| `rt_restir_final.rgen.spv.pre_reachprobe` / `.pre_mvzsign` / `.pre_geomfix` / `.pre_matrixdiag` | 各阶段 rgen |

**最常见的回退：**
```powershell
# 撤销 confidence 的判决实验（恢复真公式）
copy cpp\shaders\variants\rt_di_confidence.comp.spv.pre_confab run\shaders\rt_di_confidence.comp.spv -Force
copy cpp\shaders\variants\rt_di_confidence.comp.spv.pre_confab cpp\shaders\rt_di_confidence.comp.spv -Force
# 关闭相机 UBO 同步（去掉约 10% 开销）
#   重编 DLL 时加 -DRTX_CAMERA_UBO_SERIALIZE=0
```

---

## 10. 我这一轮的真实教训（写给下一个我）

1. **每用一台仪器前，先问「它读的是谁的资源、NRD 会不会覆盖它」。** 视口 3 那件事让我白绕了好几轮：它读的是 NRD 写回自己的运动，跟我们的 MV 无关，而三种完全不同的实现画出完全相同的画面——**这个「不变」本身就是最强的反证，我该在第一次看到时就警觉。**
2. **诊断叠层里每加一个 binding 读取，都要核对 `stageFlags`。** 那个 UB 让我两次得出「DI 全 0」的假结论。
3. **「读数没变化」和「读数变了」一样是信息。** 两次完全相同的图像 = 仪器没在量你以为的东西。
4. **共享的可写资源（`IN_MV`）上，应用与 NRD 会互相覆盖。** 任何涉及它的实验都要先说清「谁在这一帧写、谁在下一帧读」。
5. **不要在没有标定调色板、没有确认取样门的情况下读可视化视口。** 视口 8 的棋盘门槛是 `< 1.0`（帧数 0），而 `c≈0` 的衰减路径落到 1.6 帧——**这两者映射到画面上是不同的东西**，混起来会把「衰减」误判成「硬重置」。

---

## 11. ★★★ 第 1 项结论：「0 帧」机制已定死（纯读码，未上机）

> 2026-09-29 续。本节**推翻 §2.1 的一条核心假设**（「0 帧不可能，只能是 1.6 帧」）。所有结论都可在仓库里逐行复核，不需要任何截图或读数。

### 11.1 视口 8 的棋盘是 shader 主动画的，条件是「在范围内 + `data1.x < 1.0`」

`deps/NRD/Shaders/REBLUR_Validation.cs.hlsl:275-295`：

```hlsl
REBLUR_DATA1_TYPE data1 = NRD_SURFACE( gIn_Data1, viewportPixelPos );
data1 *= REBLUR_MAX_ACCUM_FRAME_NUM;              // 换算成"帧"
...
bool isInf = !IsInDenoisingRange( abs( viewZ ) ); // :83
...
float f = 1.0 - saturate( data1.x / max( gMaxAccumulatedFrameNum, 1.0 ) );
f = checkerboard && data1.x < 1.0 ? 0.75 : f;     // :291 ← 棋盘就是这一行画的
result.xyz = Color::ColorizeZucconi( viewportUv.y > 0.95 ? 1.0 - viewportUv.x : f * float( !isInf ) );
```

两条要点：

- 棋盘格子**只**出现在 `checkerboard && data1.x < 1.0` 的像素上（`:291`）——这是 marker，不是"我们看到的时序抖动"。
- 末尾乘了 `float( !isInf )`：**天空/超出 denoisingRange 的像素恒为 0（黑），不会画成棋盘。**
  ⟹ **画面上成套的棋盘区 = 「在渲染范围内、且本帧 `Data1.x == 0`」的像素。** 天空不背这个锅。

### 11.2 `Data1` 与「计数器」是两份资源，别混

| 量 | 谁写 | 有没有 +1 |
|---|---|---|
| `gOut_Data1`（`REBLUR_DATA1_TYPE`，**TRANSIENT**） | `REBLUR_TemporalAccumulation.cs.hlsl:1009` `PackData1` | **没有**（`REBLUR_Common.hlsli:42-54`，直接 `saturate(round(A)/63)`） |
| `gOut_InternalData`（`uint`，**PERMANENT**） | `REBLUR_TemporalStabilization.cs.hlsl:367` `PackInternalData` | **有**（`REBLUR_Common.hlsli:13-17`，`min(A+1, 63)`） |

`PackInternalData` 写的是**下一帧**要读的 `gPrev_InternalData`。所以 §2.1 说的「存储的计数器恒 ≥ 1」是对的——**但那个计数器不是 TA 当帧读到的 `A`。**

### 11.3 ★ 核心更正：`A` 是「四 tap 按遮挡权重归一化」的混合，全拒时**恰好等于 0**

`REBLUR_TemporalAccumulation.cs.hlsl:309-311`：

```hlsl
float4 diffAccumSpeeds = float4( internalData00.x, internalData10.x, internalData01.x, internalData11.x );
float diffAccumSpeed = Filtering::ApplyBilinearCustomWeights( diffAccumSpeeds.x, ..., smbOcclusionWeights );  // :311
```

而 `ml.hlsli:1374-1376`：

```hlsl
// Normalization avoids dividing by small numbers to mitigate potential imprecision problems
#define _ApplyBilinearCustomWeights( s00, s10, s01, s11, w, normalize ) \
    ( ( s00 * w.x + s10 * w.y + s01 * w.z + s11 * w.w ) \
      * ( normalize ? ( dot( w, 1.0f ) < 0.0001f ? 0.0f : rcp( dot( w, 1.0f ) ) ) : 1.0f ) )
```

`ml.hlsli:1364-1371`：`GetBilinearCustomWeights` 把四个遮挡位**直接乘进权重**（`weights.x = occlusion.x * (1-fx)(1-fy)` …）。

**⟹ 四个 tap 全被拒 ⇒ 四个遮挡位全 0 ⇒ 权重全 0 ⇒ `dot(w,1) = 0 < 0.0001` ⇒ 走 `? 0.0f` 分支 ⇒ `A` 不是「小」，而是「恰好 0」。**

### 11.4 于是 `A = 0` 是一个**稳定不动点**，与 `c` 无关

```
:903   A' = A · lerp( c, 1, 1/(1+A) )            // c = min(smbFootprintQuality, IN_DIFF_CONFIDENCE)
:1009  gOut_Data1.x = saturate( round(A') / 63 )
```

代入 `A = 0`：`lerp(c,1,1/(1+0)) = lerp(c,1,1) = 1` ⟹ **`A' = 0 · 1 = 0`** ⟹ `Data1.x = 0` ⟹ 棋盘。

`PackInternalData` 下一帧把存储计数器抬回 1（`min(0+1,63)`），**但只要下一帧四个 tap 仍然全被拒，`:311` 又把它打回 0。** 自持，无需任何"硬重置"。

> **❌ §2.1 错在哪**：它从 `PackInternalData` 的 `+1` 推出「`A ≥ 1` 恒成立」，再代入 `:903` 得到不动点 1.618，从而断言「0 帧不可能」。真正的问题是 **`A` 不是把上一帧计数器直接搬过来**，而是经过一次带 `sum<0.0001 ⇒ 0` 硬分支的归一化混合。**+1 只管存储，管不到这次归一化。**

### 11.5 所以「大面积 0 帧」≡ 大面积像素**连续每帧四个 SMB tap 全被拒**

tap 的接受条件（`:264-267`）：

```hlsl
smbOcclusionN = step( smbPlaneDistN, smbDisocclusionThreshold.N ) * float3( IsInDenoisingRange( prevViewZ_N ) );
```

- **不是阈值太小**：阈值只会让 `smbPlaneDist > threshold` 的那部分失败；而「四个 tap 全不落在有效面上」（例如 `prevViewZ` 全超范围）时，阈值取到物理上限也无济于事。这与 §2.5「阈值已到顶格、棋盘依旧」**完全吻合**——§2.5 当时把这条当成「阈值不是瓶颈」，其实它顺手排除的本来就是错误的嫌疑对象。
- **SMB 落点** = `pixelUv + mv.xy`（`:147`）。**相机静止时 `mv.xy ≈ 0`，四个 tap 就是本像素邻域，本该全部命中。** 所以「静止看光影也出现棋盘」这件事，本身就指向**重投影落点错误**，而不是容差。

### 11.6 这正好把 §3 里唯一「作废后没重做」的那条结论顶回首位

§3 第 12 行「我们的 MV 差半个到一像素」是拿**坏仪器**（视口 3，见 §4.2）测出来的，作废后**从未用可信手段复核**。按 §11.5，MV 若有系统性偏移 ⟹ 重投影落在错误深度 ⟹ 整片 tap 被拒 ⟹ 整片 0 帧。这是目前唯一同时解释「大面积」「静止也发生」「调阈值无效」的机制。

### 11.7 ★ 顺手纠正一处长期误记：我们的 MV 是 **UV 单位 + scale{1,1}**，不是 {1/W,1/H}

`cpp/VulkanCore.cpp:18809-18818`（`NRD_USE_REAL_MOTION_VECTORS` 默认为 1）：

```cpp
// IN_MV 由 rt_restir_final.rgen（binding 55）按 mv.xy = prevUv − curUv 写入，单位为 UV
settings.motionVectorScale[0] = 1.0f;
settings.motionVectorScale[1] = 1.0f;
settings.motionVectorScale[2] = NRD_USE_25D_MOTION_VECTORS ? 1.0f : 0.0f;
```

按 NRD 契约 `pixelUvPrev = pixelUv + (IN_MV*scale + bias).xy`，UV 单位配 `{1,1}` 与「像素单位配 `{1/W,1/H}`」**等价且合法**。Radiance走的是后者，**做第 2/3 项对照时不要把这两条当成差异**（§2 里 `{1/W,1/H,1.0}` 那句描述的是 Radiance，不是本项目）。

### 11.8 由此得到的、可纯读码继续的核查点（不经上机）

1. `cpp/shaders/rt_restir_final.rgen`（binding 55）写 `mv.xy` 时用的 `prevUv`/`curUv` 是否**都不含 TAA 抖动**，以及 `curUv` 是否是**本帧**的 `(pixelPos+0.5)/size`（差半个像素会把整片平移 1 px）。
2. `NRD_USE_25D_MOTION_VECTORS` 当前默认值：`motionVectorScale[2]=0`（2D）时 `:149-158` 会用 `worldToViewPrev` 反推 `mv.z`，此时判据依赖 `{IN_VIEWZ, worldToView, worldToViewPrev}` 三者自洽；相机一停就退化，**恰好掩盖口径错误**。这与「静止正常、一动就崩」同构。
3. 第 2 项要接的 `confidence`：注意 §11.3 已经证明——**只有 `sum<0.0001` 那条硬分支能造出 0**，而 `:898-902` 的 `min(smbFootprintQuality, confidence)` 只会让 `c` 更小、永远造不出 0。**⟹ 外挂 confidence 救不了 0 帧，别指望它。**

### 11.9 与 §11.3 等价的另一句话，以及它推翻了源码注释里的三处同一段推理

`smbFootprintQuality = Math::Sqrt01( ApplyBilinearFilter( o0z, o1y, o2y, o3x, smbBilinearFilter ) ) · sizeQuality`（`:326-328`）——`ApplyBilinearFilter` 的权重由亚像素位置决定、恒和 1。**四个遮挡位全 0 ⇔ `smbFootprintQuality = 0` ⇔ 上面 §11.3 的权重全 0 ⇔ `A = 0`。** 三个说法是同一件事。

⚠️ 但源码里有**三处**注释把 `smbFootprintQuality = 0` 的后果写成了「`diffAccumSpeed` 停在 1.618 帧不动点」：

- `cpp/shaders/rt_restir_final.rgen:3120-3123`
- `cpp/VulkanCore.cpp:9606-9607`
- 本文档 §2.1

**这三处都错在同一个地方**：1.618 是 `A ≥ 1` 时的衰减不动点；而 `smbFootprintQuality = 0` 恰恰意味着 `A = 0`（不是 `A=6`/`A=1` 衰减到 1.6），于是结果是 **0 帧**。这也解释了为什么「大面积 0 帧」一直没被当成这条链的产物——**预测值和观测值对不上（1.6 vs 0），于是被当成了另一件事去查。**

### 11.10 本节顺手核过、**已排除**的几处（别再查）

| 怀疑点 | 结论 |
|---|---|
| `gIn_Tiles` 是宿主可选输入，我们没解析（`nrdResolveResourceView` 无 `TILES` 分支） | **不是问题**：它是 NRD 内部 TRANSIENT，由 `REBLUR_ClassifyTiles.cs.hlsl:54` 自己产出 |
| `isSky` 提前返回导致 Data1 未写 | **不背锅**：validation 里 `isInf` 像素恒黑（`:293`），棋盘只画在 in-range 像素上 |
| MV 单位口径（{1,1} vs {1/W,1/H}） | **两边等价且合法**（§11.7）；`isMotionVectorInWorldSpace=false` 与 2D/2.5D 语义一致 |
| `IN_VIEWZ` 与 MV/几何是否同一张面 | **一致**：`receiverViewZ = nrdGeometryViewPos.z`（rgen:2828），而 MV 也用同一个 `nrdGeometryWorldPos`（:2986） |
| `gCameraDelta` 是否忘了设 | **不是设置项**：本版 `CommonSettings` 无此字段；NRD 从我们给的 `worldToViewMatrix/Prev` 自行导出（`NRDSettings.h:106`） |
| `mv.z` 符号（幅度式 `abs(prev)−abs(cur)`） | 与 `UnpackViewZ(z)=abs(z·viewZScale)` 自洽（§3 第 4 条已核，本节复核通过） |

**⟹ 上面这些全部排除之后，「四个 tap 全被拒」的成因只剩两类：**(a) 重投影落点本身错（MV.xy 的 `prevUv` 用的矩阵/时序与 NRD 不一致）；(b) 我们写进 `IN_VIEWZ` 的深度与 NRD 用 `worldToView` 重建的口径在**运动时**才暴露的失配（静止时 `prev==cur` 会把一切口径错误抵消掉——这正是「静止也出现、转动一定出现」的指纹）。这两条都还需要**一次可信读数**才能分辨，**无法再纯读码定案**。

---


### 12.1 为什么改版本项的范围（纯读码结论）

原计划是「接上 Radiance 的两条 optional 输入：`disocclusionThresholdMix` + `history confidence`」。勘察后的结论是**两条都不该按原义做**：

**（a）`confidence` 这半件早就做完了**，不需要再动：
- `VulkanCore.cpp:18958-18962` 已按"本帧 confidence 视图是否可用"设置 `isHistoryConfidenceAvailable`（不再是硬编码 `false`）；
- `nrdResolveResourceView` 有 `IN_DIFF_CONFIDENCE → g_nrdConfidenceViewThisFrame`（`:19934`）；
- 每帧 `recordDiConfidence` 产出并经 ping-pong 交给 NRD（`:37995`、`:38023`）。

**（b）`disocclusionThresholdMix` 对我们**恒为 0**，按原义实施等于加一张永远全 0 的纹理。**
Radiance 的写入规则是**材质驱动**，不是通用旋钮：
- 直接光（`radiance_ref/.../nrd/prepare.comp:1327-1328`）：
  `(transparentMaterial || directDynamicBlocker || specularMaterial.a > 0.125) ? 1.0 : 0.0`
- 间接光（`:1099-1100`）：`(transparentMaterial || parallaxMaterial) ? 1.0 : 0.0`

我们没有透明材质、没有视差/heightmap 材质，materialID 恒 0（`rt_restir_final.rgen:2870-2877` 把 `IN_NORMAL_ROUGHNESS` 的 alpha 写成 `0.0`）⟹ mix 全 0 ⟹ `disocclusionThreshold ≡ gDisocclusionThreshold`，**与现状逐像素相同**。
加上 §2.5 已证深度容差开到顶格（`threshold ≥ 1.0`）无效，**mix 也不可能治 0 帧**。

⟹ 结论：花一张 r8 纹理 + 改 rgen binding + 改描述符 + 重编部署六处，换来**画面零变化**，不做。

### 12.2 实际做的四件事（`cpp/VulkanCore.cpp`，本次唯一被改的源码文件）


|---|---|---|---|---|---|
| 1 | `diffusePrepassBlurRadius` | `30.0f` | `0.0f` | REBLUR `0.0` / RELAX `0.0` | README:905-910 的"pre-pass 必须非 0"只适用于**主命中做概率性 lobe 拆分**；我们不做拆分（本文件上方 19162 行）。`Reblur.cpp:119` 的 `skipPrePass` 对本实例（只有 diffuse）会因此整条跳过 pre-pass |
| 3 | `antilagSettings` | `8.0f / 10000.0f`（≈关闭） | `4.0f / 3.0f` | REBLUR `4.0 / 3.0` | 09-28 的实测结论已写明"antilag 不是根因"，非根因就没理由留在诊断态 |
| 4 | `minMaterialForDiffuse / Specular` | `4.0f`（NRD 默认） | `0.0f` | 两处都 `0.0f` | `Common.hlsli:248` `CompareMaterials = (max(m0,minm)==max(m,minm))`；对我们 materialID 恒 0 的输入**两者等价**，只为口径一致 |
| 5 | `disocclusionThresholdAlternate` | `×5.0f` | `×2.0f` | `0.01 / 0.02` | **惰性项**：mix 关着、strandMaterialID 也撞不上，改它现在没有画面效果 |

`[NRD-1 TUNING]` 日志已加上 `diffPrepassBlurRadius / specPrepassBlurRadius / antilagSigmaScale / antilagSensitivity / minMaterialForDiffuse / minMaterialForSpecular` 六项，便于从 `run/rt_debug.log` 直接核对。

### 12.3 刻意**没有**对齐的两项（留给用户定，不擅自改）

- **`returnHistoryLengthInsteadOfOcclusion = true`**（`NRD_LOCAL_DI_RETURN_HISTORY_LENGTH`，`:19306-19311`）：这是 09-29 14:0x 留下的**诊断态**，源码注释自己写着"诊断结束后改回 0"。本链没有 `.w` 消费者（`rt_nrd_composite.comp` 明确不读），**两种取值画面完全一致**，所以本次不动它。

### 12.4 部署状态（2026-09-29 本次）

- 只改了 `.cpp`，**未改任何 `.hlsl`** ⟹ 不需要重编 NRD/ShaderMake，只 `--build cpp/build --target rtx_backend`。
- 构建标记已递增：`[BUILD] orion-rt tag=2026-09-29-p2-align-ref`（`:31848`）。跑一次看 `run/rt_debug.log` 即可确认游戏加载的是这一版。
- 六处已同步，MD5 / 大小一致：

```
8848E27E188555DC035137F3B2A5297B  5634560  rtx_backend.dll
8848E27E188555DC035137F3B2A5297B  5634560  run/rtx_backend.dll
8848E27E188555DC035137F3B2A5297B  5634560  src/main/resources/rtx_backend.dll
8848E27E188555DC035137F3B2A5297B  5634560  src/main/resources/natives/windows/rtx_backend.dll
8848E27E188555DC035137F3B2A5297B  5634560  build/resources/main/rtx_backend.dll
8848E27E188555DC035137F3B2A5297B  5634560  build/resources/main/natives/windows/rtx_backend.dll
```

⚠️ 注意：部署前六处**本来就不一致**（`run/` 是 5634048 B、`src/main/resources/` 是 5633024 B），§5.1 里"六处一致"那句话与实测不符。本次已统一。

### 12.5 第 2 项对 0 帧的预期：**零**

这四项里只有 #1（撤掉 30px diffuse pre-pass）会改变画面，而它改的是**空间模糊量**，不是历史长度。**不要指望这一项能治 0 帧**——按 §11，0 帧的成因只剩「重投影落点」与「运动时的深度口径失配」两类，两者都不在这四个参数里。

### 12.6 顺带核过、已排除的一处（别再查）

`CommonSettings::cameraJitter` / `cameraJitterPrev` 与 `frameIndex` 可能不同源（宿主用 `cameraUbo->frameCount` 求抖动、却用 `g_nrdFrameIndex` 当 `frameIndex`）。**不是问题**：`gJitter` 在 NRD 里只被 `REBLUR_Validation.cs.hlsl:154/187` 与 `RELAX_Validation.cs.hlsl:136` 使用，**没有任何功能路径消费它**。


---


**用户决策（verbatim）**：`C. 翻开关启用 RELAX_DIFFUSE` ——「三处同步翻 0→1，加 makeDefaultRelaxSettings 对齐，并按 README:802 重调 confidence。明确接受 09-27 那次变暗/回归的风险，且需要你跑一次游戏才能判定。」

> **先说诚实话**：这一项**不是**「RELAX 更优」的证据，而是一次**口径修正后的复测**。
> 09-26/09-27 那两次 RELAX 实验（衰减到 ~0.06 后回滚，见 `VulkanCore.cpp:18548-18567`）当时
> 的输入走的是「MV=0 + NRD 内部跟踪」，且 `viewZ` 口径当晚才从径向距离改成 clip.w。
> 现在 MV / IN_VIEWZ 已按**主命中**口径统一，这是**首次在口径正确的条件下**跑 RELAX。
> 结果未定，**判定必须靠跑一次游戏**（用户已接受这一点）。

### 13.1 改动清单（4 处源码，必须同步；少一处就会静默错口径）

| # | 文件 | 位置 | 改动 |
|---|---|---|---|
| 1 | `cpp/VulkanCore.cpp` | `:18391-18392` | `NRD_LOCAL_DI_USE_RELAX` 0→**1** |
| 3 | `cpp/VulkanCore.cpp` | `:21858-21861` | RELAX 分支改为调用新函数（不再用「NRD 官方默认 `{}`」） |
| 4 | `cpp/VulkanCore.cpp` | `:31956` | 构建 tag `2026-09-29-p2-align-ref` → `2026-09-29-p3-relax-di` |
| 5 | `cpp/shaders/rt_restir_final.rgen` | `:2917-2918` | `LOCAL_DI_DENOISER_RELAX` 0→**1**（打包口径：线性辐照度 + 原始 hitDist） |
| 6 | `cpp/shaders/rt_nrd_composite.comp` | `:215-216` | `LOCAL_DI_DENOISER_RELAX` 0→**1**（解码：`max(packed.xyz,0)`，不做 YCoCg 逆变换） |
| 7 | `cpp/shaders/rt_di_confidence.comp` | `:73-74` + `:162-166` | **第 4 处同步点**：`LOCAL_DI_DENOISER_RELAX` 默认 1，亮度取样由 `packedTerm.x`（YCoCg 的 Y）改为 `dot(packedTerm.xyz, luma)`（RELAX 的 .x 只是 R 分量） |

**为什么 confidence 也要改（容易漏）**：该 pass 的 binding 0 直接读 NRD 的
`IN_DIFF_RADIANCE_HITDIST`（宿主 `recordDiConfidence` 绑的就是 `g_nrdDiffRadianceHitDistImage`），
所以「亮度怎么从打包里取」必须跟随降噪器族；只改宿主会得到「红色通道梯度」而不是亮度梯度。

**`nrdBuildRelaxSettings` 与 REBLUR 分支的刻意差异**：REBLUR 侧用
`scaleFrameCount(configured, max) = clamp(GetMaxAccumulatedFrameNum(configured/60.0f, fps), 1, max)`
（把 30/12 当作 **60fps 基准帧数**随本机 FPS 缩放），且 `historyFixFrameNum` 按
`min(diffMaxFast, specMaxFast) - 1` 推导。specular* 项在 `RELAX_DIFFUSE`（无 spec 输入）下惰性，

### 13.2 编译口径（**已用对照编译逐字节确证**）

`cpp/shaders/*.spv` **不由 CMake 编译**（`cpp/CMakeLists.txt` 只引用 `ShaderMakeBlob.lib` 做查找），
必须手工用 `C:\VulkanSDK\1.4.309.0\Bin\glslangValidator.exe` 编译，并**同时覆盖 `cpp/shaders/` 与 `run/shaders/`**。

**证据（对照编译 = 对当前源码传「旧宏值」重建线上二进制）**：

| 文件 | 线上(旧) hash / 字节 | 复现命令的核心 flag | 结果 |
|---|---|---|---|
| rgen | `64A11CFE…` / 47772 | `--target-env vulkan1.2 -DRESTIR_DI_LOCAL_EMA=0` | **逐字节一致** |
| composite | `287041A1…` / 11308 | `--target-env vulkan1.3 -DNRD_COMPOSITE_HISTORY_STRIP=0` | **逐字节一致** |
| confidence | `AD3F1AA3…` / 8464 | 无法复现（见下） | spirv=1.5 ⟹ **vulkan1.2** |

> ⚠️ **两处过时信息已更正**：
> - §5.2/§0 说「部署的 composite 是 `DEBUG=9` / 13404 B」——**错**。磁盘实测
>   `cpp/shaders/rt_nrd_composite.comp.spv` = `287041A1…` / 11308 B / 22:09:08，
>   对应 `-DNRD_COMPOSITE_HISTORY_STRIP=0`（**debug 色带关、正常输出**）。
> - confidence 归档 `pre_force1` = `D020ECCE…` / 8412 B / **spirv1.0**，与当前源码
>   （RELAX=0、默认 env）产出的 8492 B 差 80 B ⟹ 线上 confidence 的**源码已漂移，无法从现源码复现**。
>   故「线上 confidence = `FORCE_ONE=1`」这一条**只能靠 §5.2 的记录**，本次未能用哈希二次确认。
>   （不影响本项：方案 C 本就要**去掉** `FORCE_ONE` 重编。）

**第 3 项的重编命令（`RELAX=1`，已执行，`spirv-val` 全部 `val=ok`）**：

```powershell
$gl='C:\VulkanSDK\1.4.309.0\Bin\glslangValidator.exe'; $sh='cpp\shaders'
# rgen：保留线上口径 RESTIR_DI_LOCAL_EMA=0，翻 RELAX=1
& $gl -V --target-env vulkan1.2 -DRESTIR_DI_LOCAL_EMA=0 -DLOCAL_DI_DENOISER_RELAX=1 "$sh\rt_restir_final.rgen"      -o "$sh\rt_restir_final.rgen.spv"
# composite：保留 HISTORY_STRIP=0（正常输出），翻 RELAX=1
& $gl -V --target-env vulkan1.3 -DNRD_COMPOSITE_HISTORY_STRIP=0 -DLOCAL_DI_DENOISER_RELAX=1 "$sh\rt_nrd_composite.comp" -o "$sh\rt_nrd_composite.comp.spv"
# confidence：翻 RELAX=1，且**不带** -DNRD_DI_CONFIDENCE_FORCE_ONE（恢复真公式）
& $gl -V --target-env vulkan1.2 -DLOCAL_DI_DENOISER_RELAX=1 "$sh\rt_di_confidence.comp" -o "$sh\rt_di_confidence.comp.spv"
# 然后把三个 spv 一并拷到 run\shaders\
```

> `--target-env` 的取值不是猜的：由 on-disk spv 的 SPIR-V 版本字反推——
> rgen/confidence = **spirv1.5 = vulkan1.2**，composite = **spirv1.6 = vulkan1.3**。
> `confidence` 不带 `target-env` 会退化到 spirv1.0，与线上不一致。

### 13.3 本轮产物哈希

**Shader（`cpp/shaders/` 与 `run/shaders/` 两侧已同步，MD5 一致）**：

| 文件 | MD5 | 字节 | 备注 |
|---|---|---|---|
| `rt_restir_final.rgen.spv` | `79A9A11FC3CDA12E67054593F476CFBD` | 44868 | RELAX 打包（比旧版小 ~2.9KB：内联 vec4 取代 `nrdPackRadianceAndNormHitDist` 函数体） |
| `rt_nrd_composite.comp.spv` | `09FB1A07D560F0B682F2D8DF953BFB28` | 10604 | RELAX 解码（比旧版小 ~0.7KB：省掉 YCoCg 逆变换） |
| `rt_di_confidence.comp.spv` | `722649EFEE087ECBCC2BBEF88D92CDC2` | 8652 | **已去 FORCE_ONE**，亮度取样跟随 RELAX 口径 |

**DLL**：`F356AB9592D720734202469B0AEA23E9` / 5634560 B（二进制内已含 tag `2026-09-29-p3-relax-di`，已用 `ASCII.GetString` 搜过）。

**部署六处状态**（截至本轮）：

| 位置 | 状态 |
|---|---|
| `rtx_backend.dll`（根） | ✅ `F356AB95…` |
| `run\rtx_backend.dll` | ✅ `F356AB95…`（先被运行中的游戏进程 PID 33540 锁定，用户退出游戏后补拷成功） |
| `src\main\resources\rtx_backend.dll` | ✅ `F356AB95…` |
| `src\main\resources\natives\windows\rtx_backend.dll` | ✅ `F356AB95…` |
| `build\resources\main\rtx_backend.dll` | ✅ `F356AB95…` |
| `build\resources\main\natives\windows\rtx_backend.dll` | ✅ `F356AB95…` |

补拷命令（游戏关闭后）：
```powershell
Copy-Item cpp\build\Release\rtx_backend.dll run\rtx_backend.dll -Force
Get-FileHash run\rtx_backend.dll -Algorithm MD5   # 期望 F356AB9592D720734202469B0AEA23E9
```

### 13.4 跑游戏时怎么核对（不要凭设置相信）

1. **确认加载的是新 spv**：日志里搜 `[SPV] LOAD`，看三个文件的 `size=`
   是否为 `44868 / 10604 / 8652`（§8 的「`[SPV] LOAD … size=N`」用法）。
2. **确认走的是 RELAX 分支**：日志搜 `[NRD-1 TUNING] relax`，应出现
   `diffMaxFrames=… specMaxFrames=… historyFixFrameNum=… smoothedFps=…`
   （宿主 `nrdBuildRelaxSettings` 的 `g_nrdSettingsLogged` 一次性打印）。
   若出现的是 `[NRD-1 TUNING] reblur …`，说明宿主没翻开关。
3. **确认 FORCE_ONE 已摘除**：这没有直接日志，靠 §13.3 的 confidence hash 间接确认。
4. `run/rtx_backend.dll` 被锁 = 游戏在跑 ⟹ **部署 run/ 前必须先退游戏**。

### 13.5 判定与回退

**判定**（需跑一次游戏，用户已接受）：本地光是否仍「变暗/回归」到 09-27 的观感。
- 若亮度正常、噪点收敛 ⟹ 09-27 的衰减确由当时的输入口径错误造成，RELAX 可留。
- 若仍变暗 ⟹ 回到「RELAX 不适合本项目 1spp 直光信号」的结论（README:932 明确 RELAX 的
  Sweet Home 是 RTXDI / 极干净高 RPP 信号），应考虑换 `RELAX_DIFFUSE_SPECULAR` 并同时喂
  specular 路，或退回 `REBLUR_DIFFUSE`。

**回退（三处 + confidence 共四处一起改回，缺一即错口径）**：
```powershell
# 1) 宿主：cpp\VulkanCore.cpp:18392  NRD_LOCAL_DI_USE_RELAX 1 -> 0，重编 DLL 并部署六处
# 2) 三个 shader 的 LOCAL_DI_DENOISER_RELAX 改回 0 重编（含 confidence）
# 3) 或用 §13.3 之前的旧二进制直接回退：
#    run\shaders\rt_restir_final.rgen.spv  <- cpp\shaders\variants\rt_restir_final.rgen.spv.mvprobe  (64A11CFE…/47772)
#    （composite/confidence 的旧版见 §5.2；注意 §13.2 已指出 §5.2 的 composite 条目过时）
```

**遗留可选项**：`NRD_LOCAL_DI_RETURN_HISTORY_LENGTH`（REBLUR 专用诊断态）在切 RELAX 后自然失效，
本轮未处理；若后续要读 RELAX 的历史长度（`OUT_DIFF_RADIANCE_HITDIST.w` 本身就是帧数，见
`NRDDescs.h:113`），需另写解码。

---

## 14. 第 3 项回退（2026-09-29 22:5x）：RELAX → REBLUR

**触发**：部署后用户实测 —— **ReSTIR DI 这条链的噪点没有改善**。因此 §13 的假设（"09-29 是首次
在口径正确的前提下跑 RELAX，可能不再衰减"）被实测否掉，按 §13.5 的既定回退路径执行。

**为什么换降噪器本来就治不了这个噪点（读码结论，非猜测）**：

1. 换降噪器改的是"时域累积行为"，不改 1spp 估计量的方差。
2. 本链的时域累积**整条甩给 NRD 一层**：线上口径是 `RESTIR_DI_LOCAL_EMA=0`
   （`rt_restir_final.rgen:1450-1465` 的 64 帧 EMA 关掉了），NRD 一旦逐帧拒史就掉回接近 1spp。
3. RELAX 对 `IN_DIFF_CONFIDENCE` 的用法是历史帧数的**硬上限**
   （`min(historyLength, maxAccumulatedFrameNum * c)`，NRD README:802），而 REBLUR 是
   `historyLength *= lerp(c, 1, 1/(1+A))` 乘进反馈环 —— 同样低估 confidence，RELAX 会直接把
   窗口砍成噪点，REBLUR 会被稀释。我们的 confidence 由"EMA-vs-EMA 梯度 + 自家算的 MV"产出
   （`VulkanCore.cpp` 的 `NRD_DI_CONFIDENCE_*`），MV 一有偏差就直击这条硬上限。
4. 与 §1/§2 记录的 09-27 结论一致：RELAX 的 Sweet Home 是 RTXDI / 极干净高 RPP 信号
   （NRD README:932）。

**改动（四处开关，缺一即错口径）**：

| # | 文件 | 位置 | 改动 |
|---|---|---|---|
| 1 | `cpp/VulkanCore.cpp` | `NRD_LOCAL_DI_USE_RELAX` | 1 → 0 |
| 2 | `cpp/VulkanCore.cpp` | `nrdBuildRelaxSettings()` | 整段包进 `#if NRD_LOCAL_DI_USE_RELAX`（否则 /WX 下 C4505 未引用函数） |
| 3 | `cpp/VulkanCore.cpp` | `[BUILD]` tag | `2026-09-29-p3-relax-di` → `2026-09-29-p3-revert-reblur` |
| 4 | `rt_restir_final.rgen` | `LOCAL_DI_DENOISER_RELAX` | 1 → 0 |
| 5 | `rt_nrd_composite.comp` | `LOCAL_DI_DENOISER_RELAX` | 1 → 0 |
| 6 | `rt_di_confidence.comp` | `LOCAL_DI_DENOISER_RELAX` | 1 → 0 |

`nrdBuildRelaxSettings` 与 `NRD_DI_CONFIDENCE_*` 常量**原地保留**（前者编掉、后者两族共用），
重新启用只需把四处改回 1 并重编。

**产物与部署（2026-09-29 22:57，六处 + 两侧已核对一致）**：

| 产物 | MD5 / 字节 |
|---|---|
| `rt_restir_final.rgen.spv`（两侧） | `64A11CFE9B2D57E6161B75F177CAC446` / 47772 |
| `rt_nrd_composite.comp.spv`（两侧） | `287041A15DEFB92A9ACF479F4F37E859` / 11308 |
| `rt_di_confidence.comp.spv`（两侧） | `C32A15737E56B7DA0683BE2F9B2E3A8D` / 8532 |
| `rtx_backend.dll`（六处一致） | `7AAE6AFB29DE464F1E184BABECAA1581` / 5634560 |

**✅ 逐字节回退验证**：前两个 spv 的哈希与 §13 之前记录在案的线上旧值**完全相同**
（`64A11CFE…/47772`、`287041A1…/11308`）⟹ shader 侧确已逐字节回到 RELAX 之前的状态。
`rt_di_confidence.comp.spv` 无此待遇 —— §13.2 已指出线上那份 confidence 源码早已漂移、
无法复现（旧归档 `D020ECCE…`/8412 B/spirv1.0），所以这里只保证"回退后的源码 + 线上口径
（`--target-env vulkan1.2`）"，不保证与某个历史二进制相等。

**DLL 二进制核对**：`ASCII` 搜 `2026-09-29-p3-revert-reblur` = 命中；
`2026-09-29-p3-relax-di` 与 `[NRD-1 TUNING] relax` 均**未命中** ⟹ RELAX 分支确已编掉。

**跑游戏核对**（沿用 §8 的日志字段）：
1. `[BUILD]` 行应出现 `tag=2026-09-29-p3-revert-reblur`。
2. `[SPV] LOAD` 三个 `size=` 应为 `47772 / 11308 / 8532`。
3. `[NRD-1 TUNING]` 应为 `reblur …`（而不是 `relax …`）。

**注意**：本次回退只把降噪器族退回 REBLUR，**没有**动"时域累积整条依赖 NRD"这个结构
（`RESTIR_DI_LOCAL_EMA=0` 维持原样）。若噪点仍不可接受，下一个杠杆是按 §13 之外的方向
提高有效样本数（重新启用 rgen 内 EMA、或加大 `RESTIR_DI_SPATIAL_MAX_M` / 可见性样本），
而不是继续在降噪器族之间来回切。

---

## 15. L1 实验（2026-09-29 23:0x）：打开 rgen 内 64 帧 EMA

**背景**：§14 回退后用户实测 **REBLUR 同样很噪**，且噪点集中在光斑/阴影过渡带（粗颗粒 + 地面长条纹）。
按 §14 末段给出的方向，改打"有效样本数"这个杠杆。

**关键事实**：`RESTIR_DI_LOCAL_EMA` 的**源码默认值本来就是 1**
（`rt_restir_final.rgen:1452-1455`），部署口径却一直显式传 `-DRESTIR_DI_LOCAL_EMA=0`
（§6.2 / §13.3 记录）。也就是说 09-28 写好的那层"NRD 之前的 64 帧时域平滑"
**从未进过任何部署二进制**。本次就是把这个显式覆盖去掉。

**为什么这是唯一对路的杠杆**：换降噪器族改的是"历史拒得多快"，不是有效样本数；
而本链的有效样本数 = 空间复用（`RESTIR_DI_SPATIAL_MAX_M=64`）× 时域窗口，
时域窗口那一层此前完全由 NRD 承担（NRD 一拒史就掉回 1spp）。EMA 在 NRD 之前
先攒一层，静景下窗口 64 帧（σ ≈ 1/8）。

**基础设施核对（确认不是半成品）**：EMA 依赖 binding 26/27 的 `VPTDirectHistory`
缓冲，宿主侧完整在线 —— `createVPTDirectHistoryBuffers`（每帧确保尺寸/清零）、
`prepareVPTDirectHistoryBuffers(cmd)`（`VulkanCore.cpp:37689` 每帧调用）、
`ubo->historyReadIndex`（`:31503`）。所以只需重编 shader，**不动 DLL**。

**产物**：

| 产物 | 口径 | MD5 / 字节 |
|---|---|---|
| `rt_restir_final.rgen.spv`（两侧） | `--target-env vulkan1.2 -DRESTIR_DI_LOCAL_EMA=1`（其余宏取源码默认，RELAX=0） | `643E1349D17B2D1FF85178E041C96F1A` / 57044 |

对照：EMA=0 版为 `64A11CFE9B2D57E6161B75F177CAC446` / 47772（§14）。

**回退**：改回 `-DRESTIR_DI_LOCAL_EMA=0` 重编同一文件即可，**逐字节可复现**
（§14 已证）。不需要保留二进制备份。

**待观察（跑一局留意）**：
1. 静景下光斑/半影的粗颗粒是否明显收敛（预期 σ 降到约 1/8，但不保证归零）。
2. **移动时的拖影**：EMA 在重投影位移大的帧把窗口压到 `LOCAL_DI_EMA_MOVING_ALPHA_FLOOR=0.20`
   （≈5 帧），设计上宁可噪也不拖影，但 EMA + NRD 两层时域串联本身有滞后风险 ——
   若出现拖影，下一步应把 NRD 的窗口调浅（而不是关掉 EMA）。
3. 日志里 `[BUILD] tag` 仍是 `2026-09-29-p3-revert-reblur`（DLL 没变），
   只有 rgen spv 变了，别按 tag 判断 spv 版本。

**停止条件（预注册）**：若半影颗粒没有质变 ⟹ 转查 §14 提到的 L2（日志中
`mvZeroWhileRotating=1` 出现在 `rotDeg=4.2029` 的真实旋转帧上，怀疑旋转时 MV 为 0，
那会让 EMA 与 NRD 同时拿错世界点的历史）；L2 修完仍噪 ⟹ 停止在 ReSTIR DI 上投入，
把本地光方案默认切到经典模式（mode 1）。

**实测结果（2026-09-29 23:1x）**：用户反馈 **「静止噪，移动更噪」** ⟹ 停止条件命中。
也就是说 α=1/64 的 64 帧窗口在静景下**根本没有收敛** ⟹ EMA 没在累积（不是"层数不够"）。

### 15.1 L2 已撤销：MV 没问题，是探针本身坏了

对 `run/rt_debug.log`（8.3 MB，含 19:43–23:12 多局）全量重跑统计，结论**推翻 L2 假设**：

| 统计口径 | 帧数 | 其中 `prevEqCurView=1` |
|---|---|---|
| 全部 CAMPROBE 行 | 4929 | 2519 |
| **真实旋转帧 `rotDeg>0.5`（最大 29.345°）** | **889** | **0** |

即：**所有真实转动过的帧，`prevViewInverse` 都逐帧正确更新，没有一帧陈旧。**
`[NRD-1 MV]` 也显示 `motionVectorScale=(1,1,1)`、`historyValid=1,frameCount=120`，外闸通过。

**`mvZeroWhileRotating` 是假警报，这个字段有两处缺陷（我的仪器错误，不是 MV 的错误）**：

1. 定义是 `rotating && (prevEqCurView || prevEqCurProj)`
   （`VulkanCore.cpp:31473`）—— **把 `prevEqCurProj` 也 OR 进来了**，而转动视角本来
   就不改投影矩阵，所以它在旋转时几乎恒为 1，测不到 MV 的任何东西。
2. `rotating = rotDeg > 0.02f`（`:31443`）**低于 `acos` 的精度地板**：实测 466 个
   "rotating 但矩阵逐字节相同"的帧，`rotDeg` 全部落在 **[0.028, 0.0485]** 这个窄带里
   （0.028 出现 274 次），就是 `acos(dot≈1)` 的数值噪声被 57.2958 放大出来的假角度。

**结论**：L2 从"待查的次级疑点"改为**已排除**。MV 的两个真实历史 bug（当帧 UV 混入
jitter、`mv.z` 的 abs/符号）在当前部署源码里都已经是"修好的"那一支默认值
（`NRD_MV_CURRENT_UV_PIXEL_CENTER=0`、`NRD_MV_Z_SIGNED_LEGACY=0`），而画面仍然噪
⟹ 即便 MV 还有残渣，也不是这轮噪点的主因。

**探针的处置建议**：`mvZeroWhileRotating` 这个字段应当**废弃或重定义**
（至少把 `prevEqCurProj` 摘掉、阈值提到 0.1° 以上），否则它还会再骗一次人。

---

## §16 收口第一步：只走经典 + 锁死 NRD 链（2026-09-30）

### 16.1 裁定与依据

用户裁定：**放弃 ReSTIR DI 路线，本地光只走经典 0.31 RIS 链**。
依据是 §13–§15 的四轮实验全封死（换降噪器 REBLUR/RELAX 无效、64 帧 EMA 无效、
MV 已被实测排除）。用户当前指令是"分两步"：**先让只走经典生效，删代码留下一步**。

### 16.2 为什么 NRD 必须一起停（关键事实）

用户原本的设想是"NRD 可以暂时留着，看看有没有其他用处"。**这个设想需要修正**：

NRD 现在**唯一**接的一路信号是 `g_nrdDiffRadianceHitDistImage`（binding 50–52，
IN_DIFF / IN_MV / IN_VIEWZ 三张图），而它的**唯一生产者就是 `rt_restir_final.rgen`**。
经典模式下该 pass 整链被跳过 ⟹ 三张图没人写 ⟹ NRD 若继续跑，读到的就是上一帧
ReSTIR 留下的陈旧数据，再经 `rt_nrd_composite.comp` 叠回画面。这就是 §14 记着的
"经典模式陈旧光污染"。

所以 "留着 NRD" 的正确含义是 **保留代码、但必须关停**，不是"它还能继续干活"。

### 16.3 本次改动（4 处，全部已落地）

| 位置 | 改动 |
|---|---|
| `VulkanCore.cpp:38161-38182` `recordDiConfidence` | 包进 `if(enableReSTIRLocalDI)`；**else 分支显式 `g_nrdConfidenceViewThisFrame = VK_NULL_HANDLE`** |
| `VulkanCore.cpp:38198` `recordNRDLocalDenoiser` | `if(enableReSTIRLocalDI && !g_directAtrousReady && g_nrdReady)` |
| `VulkanCore.cpp:38210` ping-pong 翻转 | `if(enableReSTIRLocalDI && g_nrdConfidenceViewThisFrame != VK_NULL_HANDLE)` |
| `VulkanCore.cpp:38221` composite | `if(enableReSTIRLocalDI && (g_nrdReady \|\| g_directAtrousReady))` |
| `RTXSettingsConfig.java:137-138` | `localLightMode` 默认值 `0` → `1` |

`enableReSTIRLocalDI` 就是 `g_localLightMode.load() == 0`（`VulkanCore.cpp:37799`），
与 shader 端同一原子量，与 §14 的"锁步"约定一致 —— 第 4 处 `recordDiConfidence`
是被顺带发现漏锁的：它此前只受 `#if NRD_LOCAL_DI_CONFIDENCE` 门控。

**两处坑，都已在代码注释里写死缘由**：

1. `g_nrdConfidenceViewThisFrame` 是 **static**。跳过赋值不会让它变 NULL，而是保留
   上一帧（甚至上一次 ReSTIR 会话）的句柄 ⟹ 下面 ping-pong 会误判"本帧有产物"而
   一直翻转。所以 else 分支**必须显式置 NULL**。
2. composite 的 `|| g_directAtrousReady` 那一支必须**一起锁**。它当前恒 false 纯属
   侥幸：`g_directAtrousEnabled` 恒为 false（`:20202`），而 `recordDirectAtrous` 的
   early-return（`:21222`）**发生在** `g_directAtrousReady = false`（`:21229`）**之前**
   —— 这个赋值根本执行不到。一旦有人把 à-trous 开来又关掉，ready 会卡在 `true`
   让 composite 复活。

### 16.4 产物

- build tag：`2026-09-30-classic-only-nrd-gated`
  （`dh=local-light-default-mode1+nrd-chain-gated-by-enableReSTIRLocalDI`）
- `cpp/build/Release/rtx_backend.dll` = **`3615148D9EB3AE35F430841C27444632` / 5635072 B / 09-30 12:27:57**
- 部署六处，**哈希逐处核对一致**：`<root>`、`run/`、`src/main/resources/`、
  `src/main/resources/natives/windows/`、`build/resources/main/`、
  `build/resources/main/natives/windows/`
- 已用二进制字符串检索确认新 tag 写入、旧 tag `2026-09-29-p3-revert-reblur` 消失

### 16.5 验证方法（下一步）

跑游戏，查 `run/rt_debug.log`：
1. `[BUILD]` 末尾应出现 `2026-09-30-classic-only-nrd-gated`；
2. 经典模式下**不应**再出现 NRD dispatch / `[NRD-1 TUNING]` 行；
3. 画面里那层"多出来的光"应当消失。

**注意**：Java 那边改的是"新装 / 重置后的初值"，已存在的配置文件里的 `localLightMode`
仍然生效（`loadConfig` 的 `:1343-1361` 分支）。若日志显示仍是 ReSTIR，需在游戏内
把"本地光方案"手动切到经典。

### 16.6 明确未做（留给第二步）

本次**一行 ReSTIR 代码都没删**，这是刻意的：
- `/WX` 下删一半最危险（未被引用的 static 函数触发 C4505，`nrdBuildRelaxSettings`
  已经踩过一次，只能整段 `#if`）；
- `rt_test.rgen`（607 KB）里经典链与 ReSTIR 链**交织在同一个 `if(classicLocalLight)`
  分支**（`:17456-17485`），必须逐点摘，不能整段砍；
- `restirDiSwitch` / `localLightModeParams` 是主机与 shader 共用的 UBO 字段，
  删一个不改另一个就是静默错位。

真实体量：C++ 至少 9 个专用函数 + 125 处符号引用；shader 3 个源文件
（`rt_restir_final.rgen` 143 KB / `rt_restir_spatial.comp` / `rt_restir_reconstruct.comp`）
外加 15 个 `.spv/.bak` 残留；Java 侧 `restirDiVisibilityVariant` + `localLightMode`
共 39 处引用。

### 16.7 NRD 的"其他用处"（若将来要重启用）

NRD 目前只连了 **diffuse 一路**（资源解析表 `:20101-20115` 只有 IN_VIEWZ /
IN_NORMAL_ROUGHNESS / IN_MV / IN_DIFF_* ，**没有任何 SPEC 通道**），实例只创建了
一个 `REBLUR_DIFFUSE`（`:18403-18406`）。官方 README:10 明确 **NRD 不为透明/体积设计**，
玻璃的官方解法是 *denoising-free*（SHARC + 重投影已降噪帧 + 抖动 + TAA）。

真要有"其他用处"，得**新接线 + 新引导**，属于新功能而非剩饭。候选：
- `SIGMA`：太阳 / 本地光**阴影**降噪（含 translucency）—— 与现有那套太阳阴影链相关；
- `REBLUR_SPECULAR`：镜面噪声 —— 但官方明说不为透明设计，玻璃收益存疑。

---

## 17. 收口第二步：彻底删除 ReSTIR 链（施工清单）

### 17.0 用户裁定与前置

用户三项裁定：**① 基线备份走 `git init + 提交`；② ReSTIR 链彻底删除；③ NRD 保留
代码但永久关停。**

前置已done：新建 `.gitignore` 排除 ~3.4 GB 缓存（`.gradle-user-home/` `.tmp/`
`cpp/build/` `cpp/caches/` `cpp/deps/` 等，`cpp/deps/NRD` 带独立 `.git` 必须整目录排除），
然后基线提交 **`a4f528d`**（424 文件）。回退手段自此具备：`git checkout a4f528d -- <path>`。

### 17.1 删除前的两个关键事实

**（a）第二步不改变画面。** `localLightMode=1` 已使 `enableReSTIRLocalDI=false`，
主机三个 `executeReSTIR*` 全 false、shader 走 `classicLocalLight` 分支，ReSTIR 链
**当前已完全不执行**。删除是纯代码清理。

**（b）玻璃两处 ReSTIR 调用是死代码**（上一轮误判为"最大风险点"，已纠正）：

| 位置 | 包裹宏 | 默认值 | 是否参与编译 |
|---|---|---|---|
| `rt_test.rgen:12702` | `GI_GLASS_REFLECTION_INDIRECT`（:164-166） | 0 | 否 |
| `rt_test.rgen:16061` | `GI_GLASS_RECEIVER_RESTIR_DI`（:11392-11394） | 0 | 否 |

全仓库无 `-D` 覆盖。故 shader 侧**唯一活着的 ReSTIR 调用点是 `:17477`**（主不透明
路径 else 分支）。

### 17.2 shader 编译/校验配方（已逐字节复现）

```powershell
$gl='C:\VulkanSDK\1.4.309.0\Bin\glslangValidator.exe'
& $gl -V --target-env vulkan1.4 -S rgen -e main -o cpp\shaders\rt_test.rgen.spv      cpp\shaders\rt_test.rgen
& $gl -V --target-env vulkan1.4 -S rgen -e main -DRT_ENABLE_SER -o cpp\shaders\rt_test_ser.rgen.spv cpp\shaders\rt_test.rgen
```
复现验证：未改源码时产出 **439344 B / MD5 `008C6FBFB56CE5E3004EE46E75B7CC8D`**，
与 `cpp/shaders/` 和 `run/shaders/` 现存 spv 逐字节一致。**改完必须同时覆盖两处目录。**

### 17.3 `rt_test.rgen` 删除清单（行号=基线 a4f528d 状态，必须从下往上执行）

**纯删除（range）**

| # | 行范围 | 内容 |
|---|---|---|
| R1 | 358-371 | UBO 字段 `restirDiSwitch` + `localLightModeParams`（含注释） |
| R2 | 573-596 | `struct ReSTIRDIReservoir` + binding 19/20 |
| R3 | 1198-1265 | `evaluateEmitterSelfGlowAtHit` + 上方 2026-09-27 文档注释 |
| R4 | 1741-2491 | `RESTIR_DI_*` 常量 + 全部 `restirDI*` helper + `evaluateReSTIRDIDirectLighting` |
| R5 | 4195-4518 | `evaluateReSTIRDITemporalStabilization` 及其 6 个私有 helper（**全为死代码，无调用点**） |
| R6 | 1012-1018 | `classicLocalLightActive()` |
| R7 | 15623-15627 | sky 分支 `restirDIWriteCurrent(empty)` |
| R8 | 15676-15681 | water 分支 `restirDIWriteCurrent(empty)` |
| R9 | 15929-15934 | 玻璃透射分支 `restirDIWriteCurrent(empty)` |
| R10 | 16043-16070 | `#if GI_GLASS_RECEIVER_RESTIR_DI` 整块（含 `:16061` 调用） |
| R11 | 11358-11394 | `GI_GLASS_RECEIVER_RESTIR_DI` 文档注释 + 宏定义 |
| R12 | 12289-12316 | `GI_GLASS_REFLECTION_RIS` 文档注释 + 宏定义 |
| R13 | 12681-12712 | `#if GI_GLASS_REFLECTION_INDIRECT` 内 RIS 写水库块（`:12702` 调用）。⚠️ **不要**连带删 12664-12671 那段注释——它描述的是被保留的 `surfaceSceneReflection`（12672-12679） |
| R14 | 10396-10432 | 反射面 `#if GI_GLASS_REFLECTION_INDIRECT` 块（GI 弹射 + receiver 导出） |
| R15 | 178-187 | `glassReflectionReceiver*` 导出变量（`#if GI_GLASS_REFLECTION_INDIRECT`） |
| R16 | 164-166 | `GI_GLASS_REFLECTION_INDIRECT` 宏定义 |

**替换（range → 折叠为存活分支）**

| # | 行范围 | 处理 |
|---|---|---|
| S1 | 17357-17400 | 删 `RESTIR_DI_KILL_EMITTER_SELF_GLOW` + 三元 → 直接 `vec3 localEmitterEmission = evaluateVLIEmitterRadianceAtHit(hitPos, normal, albedo);` |
| S2 | 17455-17485 | `if(classicLocalLight){…}else{ReSTIR}` → 只剩 `#if LOCAL_RIS_PRIMARY_TEMPORAL` 的经典两分支（保留该宏，它属经典链 A/B） |
| S3 | 17494-17507 | `runLocalSecondary` 三处 `classicLocalLight &&` 前缀删除 |
| S4 | 12200-12214 | `(GI_GLASS_RECEIVER_RESTIR_DI != 0 ? 0 : evaluateVLIEmitterRadianceAtHit(...))` → 直接调用 |
| S5 | 12318-12335 | `#if GI_GLASS_RECEIVER_RESTIR_DI` 空分支 + `#else` → 只留 `evaluateTransparentReceiverDirectLighting(...)` |

**保留（不要误删）**

- binding 26/27 `VPTDirectHistory` —— 经典 `evaluateVPTPrimaryTemporal` / `evaluateVPTCandidateDirectTemporal` 在用；
- `LOCAL_RIS_PRIMARY_TEMPORAL`（`:17452`）—— 经典链 A/B 开关，默认 0；
- `vptReprojectPreviousPixel` 前向声明（`:1735`）；
- `evaluateVPTLocalSecondaryBounce` / `evaluateVPTRISDirectLighting` 等全部经典链。

**残留注释提及（可选清理）**：`:146` `:161` `:170` `:180` `:670` `:754-767` `:1651-1652`
`:2705` `:5057` `:5682` `:10399` `:16490` `:18445` `:18556`。这些多数在解释存活代码的
历史成因，删掉反而丢信息，建议只改会误导的（如 `:762` "MUST stay in lockstep with
enableReSTIRLocalDI"）。

### 17.4 待办（后续阶段）

- **阶段2 C++**：删 9 个专用函数（`populateReSTIRDiSwitchParams` `destroyReSTIRDIReservoirBuffers`
  `updateReSTIRDIReservoirDescriptors` `createReSTIRDIReservoirBuffers` `prepareReSTIRDIReservoirBuffers`
  `createReSTIRDISpatialPipeline` `destroyReSTIRDISpatialPipeline` `createReSTIRDIReconstructPipeline`
  `destroyReSTIRDIReconstructPipeline`）+ 107 处 `ReSTIR` 引用 + descriptor binding 19/20
  + `CameraUBO.restirDiSwitch`/`localLightModeParams` + `g_localLightMode`；
  NRD 三处 gate 的判据由 `enableReSTIRLocalDI` 换成编译期常量 `false`（永久关停）。
  ⚠️ `/WX` 下被删函数若变 static 未引用触发 C4505，须整段 `#if` 或彻底删干净。
  ⚠️ binding 19/20 从 descriptor set layout 移除前先确认无其他 shader 引用（已确认：
  仅 `rt_test.rgen` 与 `rt_restir_spatial.comp`）。
- **阶段3**：删 `rt_restir_final.rgen` / `rt_restir_spatial.comp` / `rt_restir_reconstruct.comp`
  及 `cpp/shaders/variants/` 下 50 余个 `rt_restir_*` 变体与 `.bak`。
- **阶段4 Java**：`localLightMode` + `restirDiVisibilityVariant` 共 39 处（含 UI、持久化、ConfigData）。
- **阶段5**：重编 shader（§17.2）+ DLL（§6.3 六处部署）+ 核对哈希与日志。
- **阶段6**：本节补记结果。

### 17.5 阶段1 已完成记录（2026-09-30）

对 `cpp/shaders/rt_test.rgen`（基线 19990 行）执行 R1–R16 + S1–S5，另加：

- **额外删除 R12**（12289-12316）：首次脚本漏掉该段，第二次补删（28 行）。
- **额外删除 10259-10271**：`#if GI_GLASS_REFLECTION_INDIRECT` 内的
  `tracePrimaryWaterReceiverIndirectReworked` 前向声明，仅服务于被删的 R14
  GI 弹射；其真实定义在更下方且调用点都在定义之后，删除安全。
- **R14 实际范围 10396-10433**（表里写 10432，少算了外层 `#endif`）。
- **额外修补**：`classicLocalLight` 在表外还有一处使用（GI 顶点
  `nextUnifiedEmission`，原 18456-18464）→ 折叠为无条件
  `evaluateVLIEmitterRadianceAtHit(...)`；否则删除 `classicLocalLight` 后编译失败。
- 同步润色会误导的残留注释（`:146` 原 130-163 段、原 698-717 本地光架构段、
  原 618-619 binding 19/20 段、原 12666-12770 镜面注释）。

编译验证（§17.2 配方）：
```
rt_test.rgen.spv     MD5 98727F03B235607C2B53D40E037A947A
rt_test_ser.rgen.spv MD5 026FC2AF39990BE54F20459318CDE024
```
两变体 glslangValidator 退出码 0；已同时覆盖 `cpp/shaders/` 与 `run/shaders/`。
净删 19990 → 18482 行（RT_ENABLE_SER 与主变体共用同一份源码）。

### 17.6 阶段2–5 已完成记录（2026-09-30）

**阶段2 — C++（`cpp/VulkanCore.cpp`，唯一编入库的源文件）**

- JNI 三个入口删除：`setLocalLightMode` / `getLocalLightMode` / `setRestirDiVisibilityVariant`
  （含注释块）；`updateCamera` 内 `restirHistoryValid` → `cameraHistoryValid`。
- 9 个 ReSTIR 专用函数删除（`populateReSTIRDiSwitchParams` `createReSTIRDIReservoirBuffers`
  `prepareReSTIRDIReservoirBuffers` `updateReSTIRDIReservoirDescriptors`
  `destroyReSTIRDIReservoirBuffers` `createReSTIRDISpatialPipeline`
  `destroyReSTIRDISpatialPipeline` `createReSTIRDIReconstructPipeline`
  `destroyReSTIRDIReconstructPipeline`）及其全部调用点（约 107 处符号引用）；
  `populateLocalLightModeParams(ubo->localLightModeParams);` 一并删。
- SBT：`SBT_GROUP_RESTIR_FINAL_RAYGEN=5u` 删除，`SBT_PIPELINE_GROUP_COUNT` 6u→5u，
  `g_restirFinalRaygenOffset` / `g_restirFinalRaygenRegion` 与 group[5] memcpy 删除。
- 管线创建：`rt_restir_final.rgen.spv` 的 loadSpv 删除，`mods/stages` 7→6、`groups` 6→5，
  `pci.stageCount` 7→6、`pci.groupCount` 6→5；两处 `createReSTIRDI*Pipeline()` 调用删除。
- 帧图内整块「ReSTIR DI same-frame spatial reuse」删除（`enableReSTIRLocalDI` 定义 +
  `executeReSTIRDISpatial` / `executeReSTIRDFinalRaygen` / `executeReSTIRDIReconstruct`
  + spatial/finalRaygen/reconstruct 三段取证）。
- **NRD 判据换编译期常量**：新增 `constexpr bool kNrdLocalDiEnabled = false;`，四处 gate
  （confidence / ready / confidenceView / composite）由运行期 `enableReSTIRLocalDI` 改为该
  常量（永久关停，代码保留）。composite 内引用 `g_restirDISpatialBuffer` /
  `g_restirDIReservoirBytes` 的 barrier 块删除。
- descriptor set layout：binding **19/20 移除**（生产者与消费者皆已删，仅存 `rt_restir_*`
  及 `.bak*` 陈旧副本）。**binding 41 保留**——判据：生产者 raygen 已删，但保留的
  `rt_nrd_composite.comp` 仍声明 `layout(std430, binding = 41) readonly buffer
  ReSTIRDISpatialBuffer`；NRD 代码保留（仅编译期关停），其 shader 接口须在 set 0 中找到 41，
  故追加 `b[55].binding = 41`，数组 56 项、`static_assert == 56u`。
  **布局实测**：`entryCount=56`，下标 0..55 唯一，binding 号 0..57 唯一，**缺失 = 19,20**。
- BUILD tag → `2026-09-30-classic-only-nrd-gated`，`dh=local-light-classic-031-only+nrd-chain-disabled`。
- 修复删除残留：`updateLocalLightGridDescriptor()` 内 `vkUpdateDescriptorSets(...)` 多出
  `3, barriers, 0, nullptr`（编译器 C2065）→ 恢复 5 参调用。

**阶段3 — shader 源与产物删除（共 100 个）**

- `git rm` 追踪文件 **66 个**（`git diff --cached --diff-filter=D` 实测计数，全部 `rt_restir_*`）；
  另有 gitignore 覆盖的调试副本 34 个直接 `Remove-Item`。
- 覆盖：`rt_restir_final.rgen` / `rt_restir_spatial.comp` / `rt_restir_reconstruct.comp`
  三源文件 + 各自 `.spv`，以及 `.variants/` 下 50 余个 `.bak*` / `.probe*` / `.new` /
  `.ablate_*` / `.ema_*` / `.uniform_*` / `.temporal_*` 变体；分布目录：`cpp/shaders/`、
  `cpp/shaders/variants/`、`run/`、`run/shaders/`、`.tmp/`、`cpp/.tmp/`、
  `GuangZhui-Internal-Beta-0.1-Windows-x64/shaders/`。
- `build.gradle`：`rtxRuntimeShaderFiles` 删除 `rt_restir_final.rgen.spv` /
  `rt_restir_spatial.comp.spv`，注释「以下 5 个」→「以下 3 个」。
- 残留检查：全仓库已无 `rt_restir_*` 文件。

**阶段4 — Java（3 文件，子代理执行、已独立核验）**

- `NativeVulkan.java`：删 native 声明 `setLocalLightMode`/`getLocalLightMode`/
  `setRestirDiVisibilityVariant`、init 内 `savedLocalLightMode` 与两个下发分支。
- `RTXSettingsConfig.java`：删常量 `RESTIR_DI_VARIANT_MIN/MAX/DEFAULT`、字段
  `restirDiVisibilityVariant`/`localLightMode`、getter/setter、DTO 字段、load/save 分支。
- `RTXSettingsScreen.java`：删 `pendingRestirDiVisibilityVariant`/`pendingLocalLightMode`、
  两个 CycleButton 段、apply 逻辑、死代码 `reSTIRDiVariantLabel`。
- 核验：`git diff --numstat` 15/138、10/70、36/152；**新增行全部属于 `cloudSunForwardScattering`
  （更早未提交特性，与本任务无关）**，无 ReSTIR 相关新增；`build/classes` 最新
  `2026-09-30 13:51:48`。

**阶段5 — 产物重编与部署**

- 重建 DLL：`cpp/build/Release/rtx_backend.dll` = **`3B8D8A80EC0C1F04E2A478CB73ACCADE` /
  5571072 B / 09-30 13:54:34**；**六处部署逐处哈希一致**：`<root>`、`run/`、
  `src/main/resources/`、`src/main/resources/natives/windows/`、`build/resources/main/`、
  `build/resources/main/natives/windows/`。
- DLL 二进制字符串审计：
  `setLocalLightMode` / `getLocalLightMode` / `setRestirDiVisibilityVariant` /
  `rt_restir_final.rgen.spv` / `rt_restir_spatial.comp.spv` / `rt_restir_reconstruct.comp.spv`
  **absent**；`2026-09-30-classic-only-nrd-gated`、`rt_test_ser.rgen.spv` **PRESENT**。
- shader：`rt_test.rgen.spv` = `62A8E3A99CF6C3E84FA97EF80A6B4606` / 401152 B
  （`cpp/shaders/` 与 `run/shaders/` 一致）；`rt_test_ser.rgen.spv` =
  `E34534ACA1E0501CB60888C24B6A4CB6` / 411272 B。

**遗留与对账**

- `rt_test.rgen` 行数：基线（a4f528d）**19990** → 阶段1 结束（§17.5）18482 → **当前 18502**。
  当前与 §17.5 差 **+20 行**，两个 spv 实测哈希（`rt_test.rgen.spv` = `62A8E3A9…`、
  `rt_test_ser.rgen.spv` = `E34534AC…`）与 §17.5 记录的 `98727F03…` / `026FC2AF…` 均不同
  ——阶段1 完成之后、本阶段之前该文件又经过其它改动（工作区同时存在
  `volumetric_clouds.comp` / `volumetric_light.comp(.spv)` 等未提交特性），重编后产物哈希随之变化。
  全量核对：`git diff --numstat a4f528d -- cpp/shaders/rt_test.rgen` = 96 / 1584（净 −1488，与
  19990−18502 相符）。工作区文件为 LF 行尾（CR=0），与仓库其余 shader 一致，非异常。
- `rt_test.rgen` 内 `ReSTIR` 字样残留 **20 处，全部位于注释**（无活代码）。
- **未删除、留待用户裁定**：`cpp/shaders/rt_local_visibility.rahit` / `.rmiss` /
  `rt_local_visibility_translucent.rahit` 及其 `.spv` —— 经确认为死文件（未出现在任何管线
  加载列表，其自身注释亦说明 `RT_PIPELINE_SPV_FILES` 不含它们），为控制本次改动范围暂留。

---

