# RT 本地光链路交接（2026-09-28 深夜）

## 当前运行态（全部在 run/shaders/，重启即加载）

| 文件 | 哈希 | 状态 |
|---|---|---|
| rt_test.rgen.spv | FD250E4D2C03 | 6 面均匀采样 + PDF×6 标定 |
| rt_restir_final.rgen.spv | A240A4315AB0 | ema_off + 几何距离修正 |
| rt_restir_reconstruct.comp.spv | 3B1C849AB7D9 | 时域 200 帧 + 钳制直通（**注意：此 pass 从未被 dispatch，见下**） |
| rt_di_reproject.comp.spv | 7640B3667C7A4EA8（MD5） | 时域 α=0.05 + **邻域 AABB 钳制已还原**（2026-09-28 深夜：直通实验撤除，理由见 cpp/shaders/rt_di_reproject.comp 内注释；原始实现按 alpha_010_mcvr/strict_090/debug_histlen 三个变体反汇编交叉确证。注意：variants/*.live 归档已被直通版覆盖，不可再当干净版用） |
| rt_di_moments.comp.spv | 3819440C8B2F | 方差图 |
| rt_direct_atrous.comp.spv | F4B1A494AA88 | **尾级探针在源码与 spv 中（左半屏置 0）——开工第一件事：还原**（干净版归档：variants/rt_direct_atrous.comp.spv.live） |
| rt_nrd_composite.comp.spv | 5B52972A1A96 | 干净 |
| DLL | run/rtx_backend.dll | 17:38 版，无需重编 |

回退一键：`copy cpp\shaders\variants\rt_direct_atrous.comp.spv.live run\shaders\rt_direct_atrous.comp.spv -Force`

## 已确证事实（全部实测）

1. **游戏从 run/shaders 现读 spv**（改名即崩验证）；DLL 与日志都在 run/。
2. **RT 本地光项主导屏幕**：mod 内强度归零 ⇒ 灯池消失。
3. **composite 加法上屏**：composite 内部探针（左半屏洋红）可见，且灯池洋红染色 ⇒ DI 项经 composite 到屏。
4. **全链 dispatch 正常**：reproject/moments/atrous/composite 均被调用（日志+代码验证），绑定逐张核对一致，无 NRD 回退（"staying on NRD"/"not ready" 日志 0 次）。
5. **矛盾核心**：中游结构探针（reproject L0 置零、atrous 尾级 OUT 置零）**不可见**，而 composite 内部探针可见。已知其中至少一次"尾级探针不可见"是**部署时序失误**（改了源码忘编译）；22:20:18 已真正部署复测，**结果未回**（用户转入交接）。
6. **用户截图（关键线索）**：夜晚场景，灯簇在屏幕中央附近，**左半地面有灯池、右半没有**，分界恰在屏幕中线、贴屏不动、跨重启存在（当时构建为 21:59:40 的尾级探针版 F4B1A494AA88）——注意这与探针方向（左零）**相反**。
7. 15 格分界线 = 本地光 range（30 直径设置）；多灯才显形的弧缝 = 等贡献线；这些在"composite 探针可见"之后需在真链上重新评估。

## 上一轮的教训（方法论）

- **原版烘焙光会掩罩 DI 量的变化**：探针一律用色相/结构突变（洋红、半屏置零），不要用"亮度变化"判读。
- **部署后必须确认重启时间 > 部署时间戳**，并在回复里打印部署时刻（本轮多次踩坑）。
- 改了源码必须编译部署，二者不可拆（本轮踩坑）。

## 官方基准：NVIDIA 指定的“正确降噪方式”（09-29 读 RTXDI/NRD 源码与文档）

来源：NRD v4.18.0 README（master）、RTXDI FullSample `DenoisingPasses/NrdIntegration.cpp`、`App/SceneRenderer.cpp`、`Doc/Confidence.md`。

1. **只用一个降噪器**。官方 FullSample：`RELAX_DIFFUSE_SPECULAR`（"RELAX 是为 RTXDI 设计的，它的 Sweet Home 就是 RTXDI sample"）或 `REBLUR_DIFFUSE_SPECULAR`，二选一；**DLSS-RR 是默认降噪器且与 NRD 互斥**（选 DLSS-RR 时 denoiserMode 被置 OFF）。官方管线顺序：GBuffer → ReSTIR DI（initial+temporal+spatial）→ DLSSRRInputFormatting/gradient → **NRD 一次** → Compositing → Resolve(DLSS-RR)。**不存在“NRD + 自写 SVGF 链 + DLSS-RR”三层叠加**。
2. **必须喂 history confidence**（`enableGradients` → IN_DIFF/SPEC_CONFIDENCE，官方标注 optional but highly recommended）：由 temporal gradient（复用 reservoir 而非 RNG）→ 4-pass à-trous 空间模糊 → 归一化 → 短时域滤波得到；1.0=稳定、0.0=已变。作用：让历史在**几帧内平滑失效**，避免“开灯/关灯的黑坑”与拖影；有这个才能把 `maxAccumulatedFrameNum` 安全提到 60 帧抑制 boiling。
3. **输入契约**：MV 屏幕空间、**非抖动**、`old = new + MV`，2.5D 推荐（`mv.z = viewZprev − viewZ`）；3D world-space MV 必须**不含相机运动**。viewZ 线性、与矩阵同空间、R16f+。法线世界系、粗糙度线性、用 `NRD_FrontEnd_PackNormalAndRoughness`。hit distance：REBLUR 必须用 `REBLUR_FrontEnd_GetNormHitDist` 归一化并把参数作为 `HitDistanceParameters` 传给 NRD；不含主命中、不得除 PDF/BRDF、跳过 lobe 必须为 0。HDR 输入量级建议 [0;250]（内部 FP16/二阶矩）。
4. **设置**：没法确定就用默认 `{}`；`disocclusionThreshold` 官方示例 0.01（alt 0.05）、`denoisingRange` 1000；`maxAccumulatedFrameNum = 秒 × FPS` 且 `max > maxFast > historyFix`；历史重置用 `accumulationMode=RESTART`（缓冲是垃圾才 `CLEAR_AND_RESTART`）；**NRD 没有 resize**，分辨率变了要销毁重建实例。
5. **本项目的偏离**（与症状一一对应）：① rgen 先做了一遍自写 EMA（`localDITemporalAccumulate`）→ ② NRD 之后又接了自写时域+空间链（reproject/moments/atrous×5，且**写入 NRD 的输出图**，可能覆盖 NRD 结果）→ ③ 最后 DLSS-RR。三层时域叠加 + 无 confidence，正是“移动时光池变化/拖影/历史塌”的结构性来源；NRD 的正确用法是**只吃原始 ReSTIR DI 信号 + confidence**，一次降噪。
6. 已核对无罪的项：宿主 `motionVectorScale={1,1,1}` + UV 单位 MV + 2.5D + `isMotionVectorInWorldSpace=false`（与官方一致）；IN_VIEWZ 已改 R32f（避 FP16 ULP 大于 disocclusionThreshold）。

## ‼ 部署规则（09-29 踩坑，必须先读）

**DLL 不能只拷 `run/`**：Fabric 启动时会把模组的 native 解包覆盖到 `run/rtx_backend.dll`，
所以**必须同时更新模组资源里的那一份**，否则编译出的新 DLL 永远进不了游戏
（09-29 的三步改造因此两次"部署"都无效，日志里仍打印 `[DIRECT ATROUS] dispatch`）。

一次到位（`cpp/build/Release/rtx_backend.dll` → 全部六处）：
1. `<root>/rtx_backend.dll`
2. `<root>/run/rtx_backend.dll`
3. `<root>/src/main/resources/rtx_backend.dll`
4. `<root>/src/main/resources/natives/windows/rtx_backend.dll` ← 启动时解包用的就是它
5. `<root>/build/resources/main/rtx_backend.dll`
6. `<root>/build/resources/main/natives/windows/rtx_backend.dll`

**着色器不需要这样**：`run/shaders/*.spv` 不在同步范围，直接拷 run/shaders 即生效
（这也是为什么"spv 改了生效、DLL 改了不生效"）。

**生效验证（看 `run/rt_debug.log` 本次启动段）**：
- 新版必须出现 `[DI CONFIDENCE] resources created` 与 `[DI CONFIDENCE] pipeline created`；
- 新版**不应**出现 `[DIRECT ATROUS] dispatch ... spatial chain`（链已关）；
- `[NRD-1 TUNING] ... fastHistoryClampingSigmaScale=2.0`（默认值）而非 1.5。

（另有独立发行版 `GuangZhui-Internal-Beta-0.1-Windows-x64/rtx_backend.dll` 未同步，那是分包产物。）

## 三步改造（09-29 起）

**第一步：已完成并部署（13:23）——收敛为「单一降噪器」。**
- 宿主 `cpp/VulkanCore.cpp`：`g_directAtrousEnabled = false`（自写链整体不跑，`recordNRDLocalDenoiser` 接管，合成 pass 不动）；NRD 设置回归官方默认——`fastHistoryClampingSigmaScale` 与 `antilagSettings` 的自定义值已注释掉（理由：官方 README「拿不准就用 `{}` 默认」＋「antilag 初集成应关闭」）。
- shader `rt_restir_final.rgen`：以 `-DRESTIR_DI_LOCAL_EMA=0` 编译（去掉 rgen 侧自写 EMA，原始 1spp 信号直送 NRD）。
- 部署物：`run/rtx_backend.dll`（5607936B, 13:23:13）、`run/shaders/rt_restir_final.rgen.spv`（43668B, 13:22:55）、`run/shaders/rt_nrd_composite.comp.spv`（10812B, 干净版）、`run/shaders/rt_di_reproject.comp.spv`（干净 clamp 版，当前不被 dispatch）。
- 构建命令（已验证可增量）：`cmake --build cpp/build --config Release --target rtx_backend`（VS18 2026, x64）。

**第二步：confidence —— 已实现并部署（13:28，13:42 修正一处致命 bug）。**
- ‼ **bug 与修正（13:42）**：`recordDiConfidence` 沿用了 à-trous 链的写法在每帧开头把 `g_diConfidenceReady=false`，而资源创建函数的早退条件又用了这个标志 ⇒ 被误判成"每帧尺寸变化" ⇒ **每帧销毁重建整条 confidence 链**（ping-pong 历史每帧清零 + 未初始化内存被当历史读）。症状：NRD 收到垃圾 confidence ⇒ 鬼影/闪动/噪点。修正：早退条件改为「资源齐全 + 尺寸一致」，并把描述符刷新收到资源重建之后（`g_diConfidenceDescriptorsDirty`）。
- **生效验证**：`run/rt_debug.log` 里 `[DI CONFIDENCE] resources created 707x376` 应当**每次启动只出现一次**；重复出现即说明又在每帧重建。
- ‼ **梯度定义的噪声缺陷与修正（13:48，第二次实测后）**：13:46 录屏为**像素级高频"沙纸"噪点、对比度近满幅** = 历史几乎没在累积。根因：初版梯度比较"当帧 1spp 亮度 vs 上一帧 1spp 亮度"，而本地光是**一条可见性射线的二值结果**（亮/灭），逐像素相对差天然 ≈1 ⇒ 梯度恒 ≈1 ⇒ `confidence≈exp(-4)≈0.02` ⇒ **NRD 每帧重置历史**。官方原法用"同一光源样本重着色"从根上避免（Confidence.md 明说 gradients 不应捕捉 jitter/噪声类变化），我们无上一帧 reservoir/BVH，故改为：
  * **亮度先做时域 EMA（beta=0.10 ≈10 帧）再比较（EMA vs EMA）**，再乘 `gradientThreshold=0.35` **死区**吸收残留噪声底；
  * 稳定区域 confidence≈1（正常累积到 63 帧），只有真实光照变化才压低。
  可调：`NRD_DI_CONFIDENCE_SENSITIVITY=4.0` / `_THRESHOLD=0.35` / `_EMA_BETA=0.10` / `_TEMPORAL_ALPHA=0.25`。
- **同日恢复官方默认 pre-pass**：`diffusePrepassBlurRadius=30 / specularPrepassBlurRadius=50`（README 的命中距离要求里明确要求 pre-pass 非 0，且它是压制脏输入噪声的空间手段）。旧结论"prepass 产生水平条纹"来自三层叠加时期，若复现则退回 0 并记录。
- 新增 `cpp/shaders/rt_di_confidence.comp`（两个 dispatch：mode 0 算梯度、mode 1 宽模糊+归一化+短时域滤波），宿主 `cpp/VulkanCore.cpp` 新增整块产线（3 张图×ping-pong：梯度 R16F、confidence R8_UNORM×2、亮度 R16F×2；管线/描述符集×2/双 dispatch/屏障/首帧预热 2 帧）。
- 接入点：`nrdResolveResourceView()` 增 `IN_DIFF_CONFIDENCE` → 本帧写出的视图；`nrdBuildCommonSettings()` 置 `isHistoryConfidenceAvailable = (本帧视图非空)`（shader 缺失时自动退回无 confidence，不会让 NRD 因资源未解析整帧不跑）；调用点在降噪器选择之前，帧末翻转 ping-pong。
- **与官方的偏差（重要）**：官方 temporal gradient = 「同一光源样本在前后两帧重着色之差」（需要上一帧 reservoir/BVH）；我们改用「上一帧 DI 信号经 MV 复投影后的亮度差」。宽模糊（单 pass 3×3 步长 4 ≈ ±8px，官方是 4-pass à-trous@1/3 res）与短时域滤波（α=0.25）两道按官方保留。
- 可调常量：`NRD_DI_CONFIDENCE_SENSITIVITY=4.0`、`NRD_DI_CONFIDENCE_TEMPORAL_ALPHA=0.25`、`NRD_DI_CONFIDENCE_MAX_VIEWZ=60000`。关闭：`-DNRD_LOCAL_DI_CONFIDENCE=0`。

**第三步：REBLUR / RELAX 可切换 —— 已实现（默认仍是 REBLUR）。**
- 注意：**必须三处同名宏同时改**，只改一处会用错口径解读（偏色/发黑）：
  1. 宿主 `cpp/VulkanCore.cpp`：`NRD_LOCAL_DI_USE_RELAX`（实例用 `RELAX_DIFFUSE` + `RelaxSettings` 默认 + FPS 窗口）；
  2. `cpp/shaders/rt_restir_final.rgen`：`LOCAL_DI_DENOISER_RELAX`（打包 = 线性 radiance + **原始** hit distance，对应 `RELAX_FrontEnd_PackRadianceAndHitDist`）；
  3. `cpp/shaders/rt_nrd_composite.comp`：`LOCAL_DI_DENOISER_RELAX`（解码 = 直接取 .xyz，不做 YCoCg 逆变换）。
  三者默认 0 = REBLUR。切 RELAX 时 confidence 需重调（README：RELAX 期望更小的取值；且 RELAX 只把 confidence 用在 temporal accumulation，REBLUR 是乘进整条反馈环）。

## ★★ 2026-09-29 14:1x 关键发现：IN_VIEWZ / mv.z 的**符号**与矩阵不同空间（移动即整屏拒史的根因）

**症状**（历史长度探针，13:49 会话）：静止 63 帧（白），相机一动/慢走整屏掉 1-2 帧（红）。

**病根**：`rt_restir_final.rgen` 里
```glsl
float receiverViewZ = clamp(abs(hitPositionView.z), 1e-4, NRD_MAX_VIEW_Z);   // 抹掉符号
float previousViewZ = clamp(abs(previousClip.w), 1e-4, NRD_MAX_VIEW_Z);      // 抹掉符号
nrdMotionViewZDelta = previousViewZ - receiverViewZ;
```
而传给 NRD 的矩阵来自 MC 的 OpenGL 约定（前方 z 为**负**）。NRD README 契约：
*"IN_VIEWZ - view-space Z coordinate … Z values in all pixels must be in the same space,
matching space defined by matrices passed to NRD."* ⇒ 我们违反的正是"同一空间"这一句。
**静止时 prev==cur，NRD 的重投影/深度校验退化成恒等，abs 与否都自洽；一旦移动，
NRD 用矩阵重建的上一帧视深与我们的正值对不上 ⇒ 整屏拒史。** 这与我们源码注释里
早就写下的"任何口径错误都会被静止退化掩盖、只在移动时暴露"完全吻合。

**修法（已部署 13:56:49，仅 shader）**：
1. `receiverViewZ` = 原样保留 `hitPositionView.z` 的符号（只夹幅度、避免 0）；
2. `mv.z` 改用**上一帧矩阵**把同一世界点变到视空间取 `.z` 得到 `previousViewZ`
   （旧写法用 `abs(previousClip.w)`：clip.w 的符号约定与 hitPositionView.z 相反，
   再叠 abs，等于把深度差算成了"|z_prev| − |z_cur|"）；
3. `rt_di_confidence.comp` 的 viewZ 有效性判据改用 `abs(viewZ)`。

**验证方法（沿用同一仪器）**：`NRD_LOCAL_DI_RETURN_HISTORY_LENGTH=1` + 合成端
`NRD_COMPOSITE_DEBUG=4` 已在部署里；慢慢走动，历史长度应从"红(1-2)"变成"青/白(16-63)"。

**复测结果（09-29 14:03 启动）**：**仍是红（1-2 帧）** ⇒ 符号修正不是根因。

## ★★ 2026-09-29 14:1x 官方复核（用户要求"改之前先看官网"）：罪证指向我们自己的 confidence

来源：本地 vendored `cpp/deps/NRD/README.md`（INPUTS / NON-NOISY INPUTS / HISTORY CONFIDENCE /
VALIDATION LAYER 四节）、`cpp/deps/NRD/Shaders/REBLUR_TemporalAccumulation.cs.hlsl`、
官方 NRD-Sample `Shaders/Include/Shared.hlsli`、Streamline `docs/ProgrammingGuideDLSS_RR.md`。

1. **契约复核：我们与官方一致的部分**（可排除）
   - IN_MV：`old = new + MV`、屏幕空间像素单位、**非抖动**、2.5D（`.z = viewZprev − viewZ`，
     官方 Sample `GetMotion` 原文即此公式，取 `gWorldToView`/`gWorldToViewPrev` 的**带符号** z）；
     宿主 `motionVectorScale={1,1,1}`、`isMotionVectorInWorldSpace=false` —— 逐条对得上。
     ★ 这也是"符号修正"的正确依据：官方本来就用带符号 viewZ，`abs()` 是错的。
   - IN_VIEWZ：README 明写"正负都支持，但**所有像素必须与传给 NRD 的矩阵同一空间**" ⇒ 我们已修。
   - DLSS-RR：Streamline 指南要求 MV **不含 jitter**、深度与 MV 同源、矩阵行主序 —— 与 NRD 同向，
     我们这份 MV 是"像素中心 → 上一帧矩阵投影"，本身不含 jitter。
2. **REBLUR 唯一的 confidence 用法**（README 原文 + 源码 `REBLUR_TemporalAccumulation.cs.hlsl:897-903`）：
   ```
   diffHistoryConfidence = min( smbFootprintQuality, IN_DIFF_CONFIDENCE )
   diffAccumSpeed *= lerp( diffHistoryConfidence, 1, 1/(1+diffAccumSpeed) )
   ```
   ⇒ c=0 时逐帧递推 `A ← A/(1+A) + 1` 的**不动点 = 1.618 帧**。
   ⇒ 读数"移动时稳定停在 **1-2 帧**"在 NRD 机制里**只有 c≈0 这一条路径**能产生；
     纯遮挡拒绝会落到 `historyFixFrameNum=3`（橙，日志已确认=3）或重置（validation 视口 8 棋盘红），
     不会是稳定的 1.6。**这与"1-2 帧"的实测（含 viewZ 修完仍不变）完全吻合。**
3. **我们公式的量程标定是错的**（数学上确定，不需要实验）：
   `g = β·|L_raw − L_emaPrev| / max(L_ema, L_emaPrev)`，β=0.10、threshold=0.35、sensitivity=4.0。
   - 静止：重投影落在自己身上，`L_emaPrev` 就是本像素均值 ⇒ g ≈ β·相对偏离 ≈ 0.01~0.05 ≪ 0.35 ⇒ c≈1（=实测"静止白"）；
   - 运动：重投影落到**别的像素**的 EMA 上；在"上一帧近黑、当帧亮"的像素上 `L_ema ≈ β·L_raw`，
     g 直接饱和到 ~1 ⇒ excess≈0.65 ⇒ **c ≈ exp(−2.6) ≈ 0.07** ⇒ 累积速度塌到不动点 1.6 帧。
   - 关键：这条路**与 IN_VIEWZ / MV 口径无关** —— 正是"改了符号仍然红"的解释。
4. **判决实验（14:11:02 已部署，shader-only，未动 DLL）**：`rt_di_confidence.comp` 加编译期开关
   `-DNRD_DI_CONFIDENCE_FORCE_ONE=1`，把 confidence 钉成官方语义的"1 = full confidence / 不加速"
   （不改动 NRD 的遮挡与历史修复判据，只把我们这路输入从方程里摘掉）。
   - 回退：`copy cpp\shaders\variants\rt_di_confidence.comp.spv.pre_force1 run\shaders\rt_di_confidence.comp.spv -Force`
   - **判读（重启 > 14:11:02，缓慢走动）**：
     - 历史长度**变白(≥32)或青(16-31)** ⇒ **定罪 confidence**：我们的梯度是"运动时误判光照变化"的来源。
       后续按官方重构（存上一帧光源样本 + 上一帧 RNG seed 重着色），在那之前先摘掉 confidence 输入
       （NRD README：它是 optional；错的口径比没有更糟），靠 REBLUR 自带 anti-lag + 遮挡判据。
     - 历史长度**仍 1-2 帧（暗红）** ⇒ confidence 无罪，塌陷在 NRD 自己的 SMD 判据里 ⇒ 下一步开
       NRD 自带 validation（`NRD_LOCAL_DI_VALIDATION 1`，**需重编 DLL + 六处部署**），
       看视口 3（MV vs"场景静止时的期望 MV"；洋红=疑似带 jitter、蓝=出屏）与视口 8（漫反累积帧数；
       棋盘红=历史被重置），视口 0 附带的 jitter mini 视口还能查"投影矩阵是否逐帧被扰动"。
   - 顺带确认：越界/天空像素的 confidence 也应给 1（`historyValid=false` 时我们写 `mix` 结果→梯度 0→c 随
     temporalAlpha 收敛到 1），不影响本实验。

## ★★ 2026-09-29 14:13 录屏（confidence≡1 判决）读数 —— confidence 无罪，但探针有盲区

**仪器状态**：`run/shaders/rt_di_confidence.comp.spv` = force-1 版（14:11:02，反汇编已确认
`confidence = 1.0;` 就写在 `imageStore(diConfidenceOut, ...)` 之前），游戏 14:12:40 重启加载
（日志 `[DI CONFIDENCE] resources created 707x376` + `pipeline created`），宿主
`NRD_LOCAL_DI_RETURN_HISTORY_LENGTH=1`、合成端 `NRD_COMPOSITE_DEBUG=4`。探针语义也已源码核对：
`REBLUR_TemporalStabilization.cs.hlsl:181 diff.w = data1.x` → `.w` 确为**历史帧数**（NRDSettings.h:349）。

**逐帧像素统计**（工具：根目录 `_hist_stats.py`，ffmpeg 抽帧 + 分色相统计；沿用"禁止肉眼判读"的教训）：

| 段 | 帧 | 白≥32 | 黑=0 | 红1-2 | 均值 HL |
|---|---|---|---|---|---|
| 静止（开头） | 2-5 | 80% | 2-5% | 2-3% | 34.5 |
| 运动 | 6-8 | 2-4% | 40-62% | 26-30% | 2.4-3.1 |
| 静止（恢复） | 9-11 | 68-79% | 1.5-2% | 0.3-0.8% | 21-34.7 |
| 运动（严重） | 12-17 | 1% | **62-87%** | 7-26% | 0.8-4.0 |
| 混合 | 18-24 | 0.4-10% | 17-61% | 1.5-68% | 2.5-9.2 |
| 静止（尾段） | 26-30 | 68-94% | 0.1-0.5% | 0.1-0.2% | 33-38.6 |
| 静止（结尾） | 31-34 | 70-73% | 23-26% | 0.1% | 29-30 |

1. **confidence 排除**：把 confidence 钉成 1（官方语义"完全采信、不加速"）后，运动段的塌陷形态
   与之前**完全同形** ⇒ 导致运动时历史归零的不是我们这路 confidence 输入。我们的梯度另有毛病
   （量程标定错：`g = β·|Δ|/max(...)` 上限 ≈ β=0.1，死区 0.35 永不触发 → 它实际近乎恒 1，
   即"该失效时不失效"——这是**拖影方向**的病，不是塌陷方向的病），先挂账。
2. **⚠ 探针盲区（本次最重要的发现）**：结尾静止段（同机位）黑仍占 23-26%，而运动段黑涨到
   62-87%。**若相机抬头/转身使天空占比变大，黑完全可以只是"天空"，不是塌陷**——按 rgen
   `receiverValid` 的判据，天空/无效接收面写的是 `NRD_OUT_OF_RANGE_VIEW_Z=65504` 哨兵，
   NRD `IsInDenoisingRange` 失败后**不写该像素**，读回的是残留值，与"写了但 0 帧"在旧编码下
   同为黑。**在减掉天空之前，14:13 这组数字不能用来判定"运动整屏塌陷"。**

**已部署（14:18:17，shader-only）**：`rt_nrd_composite.comp` DEBUG==4 增加**品红 = NRD 没写的
像素（天空/无效接收面）**，判据与 rgen 的 `receiverValid` 逐字一致（读同一份 reservoir 条目；
该 pass 只改写 `sampleAndAreaPdf`，两边判断一致）。
- 新读法：**品红 = 天空/无效（直接扣除）；黑 = NRD 写了但 0 帧（= 真塌陷）；红/橙/绿/青/白 = 1-2/3-6/7-15/16-31/≥32 帧**。
- 回退：`copy cpp\shaders\variants\rt_nrd_composite.comp.spv.pre_skymask run\shaders\rt_nrd_composite.comp.spv -Force`
- 复测要求（重启 > 14:18:17）：① 同机位静止 3 秒（基线）；② 缓慢行走；③ 缓慢转身；④ 抬头看天。
  只有"②③ 时**非品红区域**的黑/红占比"才是塌陷证据。

## ★★ 2026-09-29 14:20 录屏（品红判据）—— 判据失败，改走 NRD 官方 validation

**仪器**：游戏 14:20:19 重启（> 14:18:17 部署），日志有 `[NRD-1] local DI composite pipeline created`
⇒ 品红版 composite 已生效（反汇编也确认 `historyColor = vec3(1.0, 0.0, 1.0)` 在 run 的 spv 里）。

**读数（66 帧 @4fps 像素统计）**：**品红全程 0.0%**，黑在 0~30% 之间随视角变化（0/1 帧与 61~65 帧
黑 24~30% 且**同一屏幕区域**，30~38 帧黑≈0% 白 88~98%，运动段（11-13/23-29/39-42）白 26~80%、
橙绿青（3~31 帧）成片）。直接采样黑区原始 RGB = (24,24,24) 中性灰 ⇒ 不是被冲淡的品红。

**结论：`receiverValid` 这个判据抓不到天空**。主射线未命中几何时，rt_test.rgen 的接收面字段
（`receiverPositionAndViewDepth.w`、`receiverNormalAndWeight.xyz`）**并没有被清零**，所以天空像素
在该谓词下"看起来有效"；它被判 out-of-range 走的是**另一条路**（rgen 把 `hitPositionView.z`
夹到 65504 哨兵）。⇒ 本 pass 无法用 reservoir 自身区分"NRD 没写 / 写了但 0 帧"。
（源码注释已就地记录这个否定结果，品红读法作废。）

**另一个正面结论（重要）**：这段录屏里**有效像素没有整屏塌陷**。静止段白占 88~98%；运动段是
3~31 帧的**部分**回落（新露出表面 + 复投影误差），不是"整屏归零"。真正恒黑的只有那个随视角出现、
屏幕位置固定的区域 —— 形态与"超范围 viewZ（天空/极远）"一致。

**已部署（14:23，本轮唯一需要重编 DLL 的仪器）**：
1. 宿主 `cpp/VulkanCore.cpp`：`NRD_LOCAL_DI_VALIDATION` 0 → **1**（NRD 执行 Validation pass，
   16 宫格写进 OUT_VALIDATION）。DLL 重编成功，**六处全部部署并哈希一致**
   （`9DB1FD98F753A25B4E8B2BD71F3F387F`，14:23:26，5617152B）。
2. 合成端 `run/shaders/rt_nrd_composite.comp.spv` = `-DNRD_COMPOSITE_DEBUG=5`（14:23:32，直显宫格）。
   回退：`variants/rt_nrd_composite.comp.spv.pre_validation`。
3. 其余探针保持：宿主 `NRD_LOCAL_DI_RETURN_HISTORY_LENGTH=1`、`rt_di_confidence` = force-1 版。

**判读表（4x4 宫格，0 1 2 3 / 4 5 6 7 / 8 9 10 11 / 12 13 14 15，每格都是整帧的 1/4 缩略）**：
- **视口 2 = viewZ**：绿=正、蓝=负、**红=超出 denoisingRange**。⇒ 红区形状若与之前观测到的"黑"区
  重合，则"黑=天空/超范围"成立，塌陷问题收窄；几何应为**蓝**（符号修正后前方为负）。
- **视口 3 = IN_MV 与 NRD 期望位置之差**：静止应≈黑；**运动时若整片出彩色 ⇒ 我们的 MV 与 NRD 的
  矩阵口径不一致**（这就是"移动被拒"的直接证据）；洋红=疑似带 cameraJitter；蓝=出屏。
- **视口 8 = 漫反累积帧数**：棋盘红=历史被重置（与历史长度探针互证）。
- **视口 0 + REBLUR 的 mini 视口**：抖动样本应为灰点；**全变红 = 投影矩阵逐帧被扰动**（jitter 检测异常）。

## ★★★ 2026-09-29 14:26 录屏（NRD validation 宫格）——**抓到根因：IN_MV 里混了相机抖动**

**仪器**：游戏 14:25:28 重启（> 14:23:32 部署，新 DLL + `NRD_COMPOSITE_DEBUG=5` 均已生效）。
**读数（26 帧 @2fps，按宫格裁剪后分色相统计）**：

| 视口 | 读数 | 结论 |
|---|---|---|
| 2（viewZ） | **红=超出范围 0.0~0.3%**，绿=正 63~92%，黑 0% | **几乎没有任何像素超范围** ⇒ 之前历史探针里的"黑"**不是**天空/超范围（在无该场景的帧里）。几何为**正** viewZ（前方 +z），与矩阵同空间，符号这一项健康 |
| 3（MV vs 期望 MV） | 帧1 **品红 67.8%**；帧5 黄 16.2%；帧13 黄 64.1% + 蓝 2.3%；帧17 黄 81.9% | **整帧被判"likely jittered MVs"** ⇒ 我们写的 IN_MV 与 NRD 的期望差了一个 **cameraJitter 量级（≤0.5px）** 的偏移 |
| 8（漫反累积帧数） | 帧1 黑 4.6%、帧5-21 黑 0%、帧25 黑 94.9%（该帧整体异常，疑为 reset 帧） | 多数帧历史在正常累积 ⇒ 与"静止 88~98% 白"一致 |

**根因（代码级确证）**：
- 主射线**带抖动**：`rt_test.rgen:15141-15149`
  `pixelCenter = gl_LaunchIDEXT.xy + 0.5 + frameJitter; inUV = pixelCenter / size;`
  （`frameJitter = getDLSSFrameJitter(cam.frameCount)` = Halton(2,3)，`rt_test.rgen:5338-5350`）；
  矩阵本身**不含**抖动（宿主把 jitter 单独喂给 DLSS/NRD）。
- 而 `rt_restir_final.rgen` 算 MV 时当帧 UV 用的是 **`(pixel + 0.5)/renderSize`（无抖动的像素中心）**，
  于是 `mv = uvPrev − uvCur = 真运动 + (jitterPrev − jitterCur)`，**多带 ≤0.5px、逐帧换符号的抖动增量**。
- NRD README 契约："IN_MV … must be **non-jittered**"；validation 视口 3 的洋红判据就是专门抓这个的。
- 后果与本轮主症状一一对应：复投影落点系统性偏 0.25~0.5px ⇒ 方块世界里每条方块边界都是一次深度/法线跳变，
  偏差落在边界上就被判遮挡 ⇒ 历史被拒/被加速；静止时只在边界少量失效（所以静止仍能攒到 63 帧），
  相机一动整屏都在换面 ⇒ 大面积失效、光池跟着屏漂。**同一个 bug 同时污染 DLSS-RR**（Streamline 指南同样要求非抖动 MV）。

**修法（= 官方 NRD-Sample `GetMotion` 的写法，已部署 14:30:08）**：
`rt_restir_final.rgen` 里当帧 UV 改为**用当帧矩阵把 hitPos 投到屏幕**（复用已算好的 `hitPositionView`）：
```glsl
const mat4 currentViewToClip = inverse(cam.projInverse);
const vec4 currentClip = currentViewToClip * hitPositionView;
// currentUv = clip.xy/w 的 NDC → UV（y 翻转与 previousUv 同式）
nrdMotionVector = previousUv - currentUv;
```
抖动自然包含在两侧 UV 里并相互抵消 ⇒ 得到真正的**非抖动 MV**。
- A/B 回退开关：`-DNRD_MV_CURRENT_UV_PIXEL_CENTER=1` 用回旧的像素中心写法。
- spv 回退：`variants/rt_restir_final.rgen.spv.pre_mvfix`（13:56 版）。
- ‼ 编译必须带 **`-DRESTIR_DI_LOCAL_EMA=0`**（13:23"单一降噪器"那一步的开关；忘了会静默把
  rgen 自写 EMA 装回来，体积从 ~45KB 变 ~54KB —— 本次踩到并已修正）。
  命令：`glslangValidator -V --target-env vulkan1.2 -DRESTIR_DI_LOCAL_EMA=0 rt_restir_final.rgen -o rt_restir_final.rgen.spv`
- 当前 `run/shaders/rt_restir_final.rgen.spv` = 14:30:08，44976 B（= 44276 + MVP 修正）。

**验证（重启 > 14:30:08，仍开 validation 宫格）**：
- **视口 3 应几乎全黑**（静止与运动都应是黑的：期望 MV 用的是同一套矩阵，相机运动两侧都有）——
  若运动时仍有成片颜色，说明还有第二个口径问题；
- 视口 8/历史长度探针：运动时的累积帧数应明显高于之前的 1~6 帧。
- 判读后：宿主 `NRD_LOCAL_DI_VALIDATION` 改回 0 重编（六处），composite 回退 `variants/rt_nrd_composite.comp.spv.pre_validation`。

## ★★★ 2026-09-29 14:34 录屏（MV 修正后）——**修正被官方仪器确认**

**仪器**：游戏 14:33:27 重启（> 14:30:08 部署），宫格视图仍开。19 帧 @2fps。

| 帧 | 视口 2 红(超范围) | **视口 3 品红(疑似 jitter)** | 视口 3 黑(一致) | 视口 3 黄/红(残差) | 视口 8 亮(≥16 帧) | 视口 8 黑(0 帧) |
|---|---|---|---|---|---|---|
| 3 | 0.0% | **0.0%** | 55.9% | 31.1%/9.0% | 90.8% | 0.0% |
| 6 | 0.1% | **0.0%** | 55.6% | 28.1%/15.2% | 92.3% | 0.0% |
| 9 | 0.0% | **0.0%** | 84.8% | 0/0 | 90.5% | 0.0% |
| 12 | 0.0% | **0.0%** | 89.3% | 0/0 | 31.9% | 0.0% |
| 15 | 0.1% | **2.2%** | 55.2% | 26.1%/2.4% | 93.5% | 0.0% |
| 18 | 0.0% | 0.0% | 99.8% | 0/0 | 2.1% | 94.9%（末帧异常，两段录屏的末帧都是 94.9%，判为收尾转场，非读数） |

**结论（对照修正前 67.8% 品红）**：
1. **"likely jittered MVs" 的洋红签名 67.8% → 0%** ⇒ IN_MV 里的相机抖动增量确实被消除，NRD 的契约违反已修好；
2. **视口 8 的累积帧数从"运动段 1~6 帧"变成 ≥16 帧占 90%**（且 0 帧恒为 0%）⇒ 时域历史在运动时不再被大面积拒绝；
3. 视口 3 残留 26~31% 的黄/红：NRD 的期望位置是把 X 从 **IN_VIEWZ + 像素中心** 反推出来的（它并不知道主射线被抖动偏了），在斜面/掠射面上这个反推与真实 hitPos 有亚像素差 ⇒ 属于**仪器自身的重建近似**，不再是"抖动混进 MV"。
   （若要进一步压掉，只能等 NRD 侧或改成"抖动烘进矩阵"的方案，收益有限，先不动。）

**当前运行态（14:35:54，交还真实画面）**：
- `run/shaders/rt_nrd_composite.comp.spv` = **干净版**（不带 `-DNRD_COMPOSITE_DEBUG`，10812 B）；
- `run/shaders/rt_di_confidence.comp.spv` = 恢复 `pre_force1`（判决实验结束）；
- `run/shaders/rt_restir_final.rgen.spv` = MV 修正版（14:30:08，44976 B）；
- 宿主仍是 `NRD_LOCAL_DI_VALIDATION=1` + `NRD_LOCAL_DI_RETURN_HISTORY_LENGTH=1`（**故意留着**：两者都不影响观感，
  需要再看宫格时只需把 composite 换成 `-DNRD_COMPOSITE_DEBUG=5`，不必重编 DLL）。
  ⇒ 等确认问题收口后做**最后一次清理重编**：两个开关置 0 → 重编 DLL → 六处齐更。

**下一步的验收标准（重启 > 14:35:54，看真实画面）**：
- 相机移动时**光池不再随屏幕漂/变**，停下即稳定；
- 若仍有漂移，则按顺序复开仪器：composite `-DNRD_COMPOSITE_DEBUG=4`（历史长度+品红）→ 仍不解再 `=5`（宫格）。

## ★★★ 2026-09-29 14:4x 噪点复诊（用户："噪点还在，麻烦再详细看看官方降噪文档"）

### 1) 官方文档逐条对照（本地 vendored NRD v4.18 README + NRDSettings.h + 源码）

| 官方要求（出处） | 我们 | 结论 |
|---|---|---|
| "**the input signal must converge reasonably well for this number of frames**"（README:493） | 1spp、每像素独立光样本、**无时域水库复用** | ⚠ **仅勉强满足**：NRD 的空间滤波是方差驱动的，白噪声下方差≈信号，它故意少模糊 ⇒ 唯一压制噪声的是时域累积 |
| "avoid high-energy fireflies；能量放大不应超 20-30x（≈最小概率 0.05）"（README:494） | RIS 的 sampleAndAreaPdf 未核对最小概率 | ⚠ 未验证 |
| HDR 输入量级 [0;250]（README:872） | 本地光项 O(0.1~10) | ✅ 不是问题 |
| pre-pass 必须非 0（README:506/910） | diffuse 30 / specular 50 | ✅ |
| `maxAccumulatedFrameNum = 秒 × FPS`（README:912） | 1.0s × 65fps = 63 | ✅ |
| REBLUR 命中距离必须归一化且参数同步（README:928） | `nrdNormHitDistance(...,1.0)` + host `hitDistanceParameters` | ✅（注意归一化在 3~10 格外就饱和到 1.0，属"安全但信息量低"） |
| "**Use the validation layer output to ensure … 'diff/spec frames' stays in the violet zone**"（NRDSettings.h:233，写在 `ReblurAntilagSettings` 正上方） | 视口 8 实测**黄/红** | ❌ **这是本轮抓手** |
| "Antilag parameters need to be carefully tuned. **Initial integration should be done with disabled antilag**"（README:930） | 09-28 测过一次"近似关闭无效"，但那次 IN_MV 混着抖动 | ⚠ 需复测 |
| history confidence 是"optional but highly recommended"，有了它才敢把窗口提到 60 帧（README:468/782/590） | **代码里被覆盖掉了，从未被消费** | ❌ 见下 |

### 2) 测量：历史长度根本没按 63 帧跑（用视口 8 自带图例做标定）

`f = 1 - saturate(data1.x / maxAccumulatedFrameNum)`，`ColorizeZucconi(f)`（REBLUR_Validation.cs.hlsl:290）：
f 小=紫=历史长（官方"紫区"），f 大=红=历史短。用**图例行**（`viewportUv.y > 0.95` 那条完整渐变）
逐列采样做标定（工具 `_v8_stats.py`），避开 DLSS-RR/色调映射的色相漂移：

| 录屏 | 各帧平均历史（帧 / 63） |
|---|---|
| 14:34（MV 修正后） | 9.4 / 6.3 / 15.6 / **46.5** / 8.1 / **0.0** |
| 14:26（修正前） | 48.5 / 37.9 / 11.8 / 9.8 / **53.8** |

⇒ 被本地光照亮、信号 1spp 抖动的墙面**只累积 6~16 帧**（1/√10 ≈ 32% 残余噪声 = 用户截图里的噪点），
而太阳照亮、信号平滑处能到 46~54 帧 —— **与 antilag 的触发特征完全一致**（它与局部方差带比较，
平滑区不触发、噪声区持续触发）。

### 3) 两个真问题（本轮已修，需重编 DLL —— 已六处部署 `F7DE32B26A2A60C4917979EA5F317B06`，14:44:20）

1. **confidence 从未被消费（代码 bug）**：`nrdBuildCommonSettings` 里前面按"本帧 confidence 视图
   是否可用"赋了值，**后面又有一句无条件的 `settings.isHistoryConfidenceAvailable = false;` 把它覆盖**
   ——与当年 `enableValidation` 被后面一句覆盖是同一个坑。已删除该行（源码里留了 ★★ 记录）。
2. **antilag 复测**：把 `sigmaScale=8 / sensitivity=1e4`（≈关闭）重新启用。09-28 那次"无效"的结论
   是在 IN_MV 混抖动、复投影落点系统性偏 0.25~0.5px 的状态下得到的，那一轮的 `smbFootprintQuality`
   才是压制者，antilag 无法被判定。现在 MV 已修，这一格必须重测。

### 4) 新仪器：顶部 8 行"历史长度色带"

`rt_nrd_composite.comp` 正常路径加了 `NRD_COMPOSITE_HISTORY_STRIP`（默认 1）：
屏幕顶部 8 行按列显示**该列平均历史长度**（黑 0 / 暗红 1-2 / 橙 3-6 / 绿 7-15 / 青 16-31 / 白 ≥32 帧，
数据源 = NRD 输出的 .w）。这样"画面"和"读数"一次录屏同时拿到，不必再二选一。
关闭：`-DNRD_COMPOSITE_HISTORY_STRIP=0`。回退参考：`variants/rt_nrd_composite.comp.spv.pre_validation`（无叠层版）。

**判读（重启 > 14:44:20）**：
- 顶部色带转**青/白** ⇒ 历史真的按满量程跑起来了（该处画面噪点应显著下降）；
- 色带仍**黄/红** ⇒ 累积计数不是被 antilag 压住的，矛头转向 `smbFootprintQuality` / 每帧 +1 的写回路径；
- 预期副作用：历史活了以后**拖影**会回来（信度输入目前近乎恒 1 = 不加速），这是预期而非新 bug。

### 5) 还没做的官方项（按序）

1. **confidence 梯度重标定**：现在它真的被消费了，而我们的梯度上限 ≈β=0.1 < 死区 0.35 ⇒ 实际恒 1
   ⇒ "该失效时不失效"（拖影方向）。官方做法需要上一帧光源样本 + 上一帧 RNG seed 重着色。
2. **RIS 最小概率/萤火虫**（README:494）：核对 `sampleAndAreaPdf` 的 pdf 下限，避免 >20-30x 能量放大。
3. **估计量侧收敛**（README:493/884-891）：时域水库复用仍是死代码；官方点名的正是 ReSTIR 的
   时空复用（"solid baseline"）。这是"输入信号收敛"的正解，也是换 RR 也躲不开的一步。

## ★★ 2026-09-29 14:5x 用户复测："一转就黑红" —— 已排除清单 + 收窄到 SMB 判据

**现象**：顶部色带（= NRD 的 `.w` 历史长度）静止时正常，**一转动视角就成片黑/红**。
即：confidence 真接上、antilag 关掉之后，历史在运动时仍被压短。

**本轮用代码排除的机制（都读过源码，不是推测）**：
1. **NRD 自己管理"上一帧"数据**：`gPrev_ViewZ / gPrev_Normal_Roughness / gPrev_InternalData`
   在 `REBLUR_TemporalAccumulation.resources.hlsli:25-27` 都是 **`NRD_RESOURCE_PERMANENT`**
   ⇒ 是 NRD 内部维护的历史副本，**不需要应用 ping-pong IN_VIEWZ**（此前"单张 viewZ 图"的
   担心作废）。
2. **没有逐帧历史重置**：`accumulationMode` 只在资源（重）建后第一帧为 `CLEAR_AND_RESTART`，
   录完即 `g_nrdNeedsHistoryReset=false`（VulkanCore.cpp:18690 / 21715）；`g_nrdFrameIndex` 单调递增。
3. **confidence / antilag 都不是压住累积的那只手**（前者"被覆盖"已修，后者≈关闭）。

**收窄后唯一剩下的闸门：SMB（surface motion bilinear）遮挡判据**，
`REBLUR_TemporalAccumulation.cs.hlsl:238-267`（逐行读过）：
```hlsl
float smallParallax = LinearStep(0.25, 0.0, smbParallaxInPixelsMax);
float NoVstrict   = lerp(NoV, 1.0, saturate(smbParallaxInPixelsMax / 30.0));
smbDisocclusionThreshold = (smbNoN2x2 > cosMaxAngle) * IsInScreenBilinear(...)
                         * GetDisocclusionThreshold(disocclusionThreshold, frustumSize, NoVstrict) - eps;
float3 Xvprev    = AffineTransform(gWorldToViewPrev, Xprev);      // Xprev 由 viewZ + **mv.z** 反推
float3 smbPlaneDist = abs(prevViewZ0..3 - Xvprev.z);              // 点采样 NRD 自己的上一帧视深
smbOcclusion0..3 = step(smbPlaneDist, threshold) * IsInDenoisingRange(prevViewZ);
```
两个**只在运动时才出现**的效应：
- 视差一大，`NoVstrict → 1`（`GetDisocclusionThreshold` 内层 `saturate(threshold/max(0.05,NoV))`
  从 NoV<0.05 时的 20 倍宽容退回到**基准 0.01×frustumSize**，3 格处约 0.04 格）
  ⇒ **一移动，深度容差收紧最多 20 倍**；
- 该判据把「我们给出的预期上一帧视深」（= `viewZ + mv.z`，由我们的矩阵/位置反推）与
  NRD 自己存的上一帧视深逐 tap 比大小 ⇒ **我们在 `mv.z`/位置/矩阵上的任何一点不一致，
  都会直接吃掉这份（收紧后的）容差**。

**待查的最具体嫌疑（下一轮）**：`rt_restir_final.rgen:1929` 的接收面世界坐标来自
`reservoir.receiverPositionAndViewDepth.xyz`（reservoir 里的打包值；本 rgen 自己别处就记着
"1/64 格量化"）。若它确实是 snap 过的，则 MV 会带 ~0.5~2px 误差、IN_VIEWZ 带 ~0.008 格误差，
**静止时两端一致（自洽），运动时两端在不同像素上相加** ⇒ 正好是"静止好、一动就掉"。

**已部署（14:49:17）**：合成端换回 `NRD_COMPOSITE_DEBUG=5`（16 宫格），用一次录屏读：
- **视口 3**（我们的 MV vs NRD 期望 MV）：分别看「纯旋转」与「纯平移」两段，看误差是否随运动出现/增长；
- **视口 8**（DIFF FRAMES）：看是**棋盘**（历史重置）还是**细颗粒**（逐像素遮挡）；
- 视口 2（viewZ）与视口 0 的 jitter mini 视口作为健康基线。

## ★★★ 2026-09-29 14:52 录屏（全转动）——MV 定案无罪；容差环境变量已恢复

**仪器**：游戏 14:51:17 重启（> 14:49:17 宫格部署），14 帧 @2fps，全部是转动视角。

| 帧 | 视口 2 红(超范围) | **视口 3**（黑=与我们 MV 一致） | 视口 8 平均历史（图例标定） |
|---|---|---|---|
| 2 | 0.0% | **99.3% 黑** | 60.9 帧（紫 95%） |
| 4 | 0.0% | 76.7% 黑 | 32.2 |
| 6 | 0.0% | 76.8% 黑 | 34.8 |
| 8 | 0.0% | **96.3% 黑** | 23.0 |
| 10 | 0.0% | 47.5% 黑 + 40.1% 黄 | 9.6 |
| 12 | 0.0% | **99.8% 黑** | 46.6（短暂停顿） |
| 14 | 0.0% | **99.8% 黑** | 0.0（末帧，历来如此，非读数） |

1. **IN_MV 彻底定案无罪**：纯转动段视口 3 几乎全黑（99.3/96.3/99.8%）⇒ 我们的非抖动 MV
   在**转动**下与 NRD 用自己矩阵算出的期望位置一致。此前所有"MV 口径"的怀疑到此关闭。
2. **历史仍随转动单调下滑**（60→32→35→23→10），恢复只需一次短暂停顿 ⇒ 压力来自
   **NRD 内部只在运动时才收紧的那一处**（见下）。
3. 视口 2 全程 0% 超范围、几何为正视深 ⇒ viewZ 这一环健康。

**源码定位（REBLUR_TemporalAccumulation.cs.hlsl:238-267，逐行读过）**：
```hlsl
float NoVstrict = lerp( NoV, 1.0, saturate( smbParallaxInPixelsMax / 30.0 ) );   // 一移动就趋近 1
smbDisocclusionThreshold *= GetDisocclusionThreshold( disocclusionThreshold, frustumSize, NoVstrict );
float3 smbPlaneDist0..3 = abs( prevViewZ0..3 - AffineTransform(gWorldToViewPrev, Xprev).z );
```
`GetDisocclusionThreshold` 内层是 `saturate(threshold / max(0.05, NoV))`：
- **静止斜视**（NoV<0.05）：`saturate(0.01/0.05)=0.2` ⇒ 容差 0.2×frustumSize（**最宽容 20 倍**）；
- **一运动**：NoVstrict→1 ⇒ `saturate(0.01/1)=0.01` ⇒ 容差退回 **0.01×frustumSize**（3 格处≈0.04 格）。
而 `smbPlaneDist` 比的是「NRD 自己存的上一帧视深（点采样）」与「我们的 `viewZ + mv.z`」，
即一次**精确一致性检查**，其容差大小由 `disocclusionThreshold` 唯一决定。
⇒ 这就是"静止一切正常、一动就大面积判成遮挡"的机制位置。**>0.25 之后内层 saturate 饱和**
（09-28 那次"放到 0.5 无变化"就是测到天花板），**有效对比区间是 0.01 → 0.05 → 0.1**。

**已部署（14:54）**：
1. 宿主恢复 `RTX_NRD_DISOCCLUSION_THRESHOLD` **环境变量覆盖**（免重编 A/B；默认仍 0.01），
   DLL 六处齐更 `BCD24C884D3A36382B96058C6A411C38`（14:54:06）。
2. 合成端换回**真实画面 + 顶部历史色带**（14:54:00），一次录屏同看"噪点"与"历史长度"。

**A/B 步骤**：默认 0.01 转一圈（基线）→ `set RTX_NRD_DISOCCLUSION_THRESHOLD=0.05` 重启再转 →
必要时 0.1。判读：顶部色带在转动时能否保持青/白、噪点是否随之下降。

## 下一步（按序）

0. **进行中（09-29 14:35 部署）**：MV 修正**已被 NRD validation 确认**（洋红 67.8%→0%，累积帧数 ≥16 帧占 90%），合成端已换回**干净版**、confidence 恢复 `pre_force1` ⇒ **现在是验收观感状态**。重启 > 14:35:54 看真实画面：相机移动时光池是否还漂/变。
   若仍有问题：composite 换 `-DNRD_COMPOSITE_DEBUG=4`（历史长度+品红）→ 再 `=5`（16 宫格），二者都只需重编 composite（宿主两个诊断开关故意留着，免重编 DLL）。
   收口后的清理重编：宿主 `NRD_LOCAL_DI_VALIDATION` 与 `NRD_LOCAL_DI_RETURN_HISTORY_LENGTH` 置 0 → 重编 DLL → 六处齐更。
   **遗留未结案**：① "历史探针里 24~30% 的黑"仍未定性（视口 2 显示几乎无超范围像素）；② 我们的 confidence 梯度量程标定错（上限 ≈β=0.1 < 死区 0.35 ⇒ 实际近乎恒 1，"该失效时不失效"，属拖影方向）；③ 视口 3 残留 26~31% 黄/红（判为 NRD 反推 X 时的亚像素近似）。
   **分析工具（本文件多轮判读都用）**：`_hist_stats.py`（DEBUG==4 历史长度伪彩的像素统计）、`_grid_stats.py`（validation 16 宫格的裁剪与分色相统计）。ffmpeg 抽帧到 `%TEMP%/hf2`、`%TEMP%/hf3`。
   下面这段是上一轮的详细记录与回退物，保留备查：
   **（09-29 13:01 部署）**：**前两条结论已被像素统计推翻（教训：禁止用增强后的画面肉眼判读，必须像素统计）**。逐帧量化（ffmpeg raw + Python 统计，避开 HUD）：
   - 23:25 矩阵差视频：基线 R≈2.4/G≈9.1（=零，视频色调偏移）；转视角段 R→25~59（≈3~7°/帧）；前行段 G→59~62（≈0.1 格/帧）；剧烈晃动 R→128~172（≈17°/帧）。⇒ **prev/cur 矩阵正常相差一帧，宿主更新链健康，时序竞态假设作废**。
   - 23:15 mv 视频（/64 编码反推）：转视角 p99 mv ≈10~28px、剧烈晃动 ≈33~63px（**未饱和**），与 3~7°/帧 / 10°/帧 × f_px≈353 的真值吻合 ⇒ **mv 量级正确、没被放大**。
   ⇒ 三者（矩阵、mv、历史缓冲）都健康，但探针实测相机转动时历史仍塌黑 ⇒ **病在判据本身**（落点是否真在同一表面 / prevDepth·prevNormal 是否与当帧对得上）。
   **13:01 部署拒史原因探针**（`-DRT_DI_REJECT_REASON=1`）：红=落点出界、洋红=重置帧、绿=深度门失败、蓝=法线门失败、黄=接受；合成端已换回干净版（无色码覆盖）。判读（缓慢转动视角）：整屏绿 ⇒ 深度门系统性失败；整屏蓝 ⇒ 法线门；整屏红 ⇒ 尺寸/口径问题；整屏黄 ⇒ 判据健康，塌黑来自下游。
   **拒史原因探针结果（09-29）**：**整屏黄（接受）、转动视角轻微红（边缘合法出界）** ⇒ **reproject 判据健康、DI 时域链这一环无罪**；此前"移动灰阶塌黑"是经 albedo 调制/atrous/DLSS 的显示层现象，不是复投影拒史。
   ⇒ 剩余唯一活着的相机运动相关闸门 = **rgen 内 `localDITemporalAccumulate` 的自有时域判据**（rt_restir_final.rgen，自用 reprojectPreviousPixel + 位置/法线/深度容差；rt_test.rgen 的两个同类实现均为死代码）。
   **13:05 部署 EMA 历史帧数探针**：`rt_restir_final.rgen` 用 `-DRESTIR_DI_DEBUG_M=1`（R=M 分档、**G=上一帧 EMA 历史帧数：红=写了读不回/绿青白=在累积**、B=该条目带 viewDepth），合成端 `-DNRD_COMPOSITE_DEBUG=1`（只透传 rgen 的调试输出，DI 项不参与）。判读（缓慢转动视角）：G 保持绿/青/白 ⇒ EMA 健康；G 塌成红 ⇒ EMA 判据在运动时拒史 = 根因，下一步细分是哪一条（位置/法线/深度/有效位）。
   **当前 run/shaders 状态**：rt_restir_final.rgen.spv = EMA 调试版；composite = DEBUG==1；reproject = 原因探针。**还原生产**：rgen ← `variants/rt_restir_final.rgen.spv.pre_matrixdiag`；reproject ← `variants/rt_di_reproject.comp.spv.clamp_restored`；composite ← 不带宏重编。
   回退：rgen 干净版 = `variants/rt_restir_final.rgen.spv.pre_matrixdiag`；reproject 干净版 = `variants/rt_di_reproject.comp.spv.clamp_restored`；composite 干净版重编（不带 -DNRD_COMPOSITE_DEBUG）。
   **rgen 编译命令**：`glslangValidator -V --target-env vulkan1.2`（plain -V 会报 GL_EXT_ray_tracing 不支持）。
   **已排除**：AABB 钳制直通；部署链路；坐标系（Java viewInverse = T(pos)·R⁻¹ 世界系）；宿主 resetHistory；mv 符号/翻转约定（v1 四变体全黑）；mv≈0（v2 区1 黑反证）；parity/历史缓冲（区3 恒亮）；updateCamera 节奏（每帧一次、frameCount 递增——但注意 prev==cur 提示实际可能有二次调用）；mv=NaN（录屏有梯度结构）。
1. **还原尾级探针**（用 live 归档），重启确认画面正常；
2. **复测 22:20:18 的尾级探针**（当前已部署！重启看：左半灯池应消失）：
   - 左暗 ⇒ 链全通 + 用户截图的"反向分裂"成为唯一未解之谜；
   - 无变化 ⇒ "composite 内部可见、OUT 尾级不可见"的硬矛盾成立 ⇒ 发 GPT 会诊。
3. GPT 会诊材料：本文件 + 两次探针截图（洋红左半屏可见 / 左亮右暗分裂）。
4. 链路确认后：③ `g_directAtrousEnabled` 接设置项 + `GuangZhui-Internal-Beta-0.1-Windows-x64` 分包同步（那份还是老 shader 20F9939680D6）；④ 最后删死代码（reconstruct 是 NRD 降级路径勿删）。

## 给 GPT 的一句话摘要

"Minecraft Vulkan RT：RT 本地光 DI 项确认经 composite 上屏（内部探针可见），但同一链路中游（reproject 输出 L0、atrous 尾级 OUT）的半屏结构探针不可见，且屏幕出现与探针方向相反的左亮右暗半屏分裂（贴屏、跨重启）。全链 dispatch/绑定/日志逐一核实无异常。什么机制能解释？"
