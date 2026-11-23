# HANDOFF 2026-10-24（晚）：清玻璃可见性当空气 + 透明接收面直射降噪

> 接续 `HANDOFF-2026-10-24-TRANSLUCENT-SEAM.md`。用户当晚给出两个新症状并要求"不要测量，要根治"：
> ① 普通清玻璃根本没当空气；② 彩色玻璃块都有噪点，静止时积累。本轮两项都已落地、编译、部署四处。
>
> **修订记录**
> - 第一轮：清玻璃可见性当空气（rahit ×2）+ 透明接收面 RIS 4 样本 + 历史 48→96。
> - 第二轮（用户复测：彩噪"变少了"但仍有，清玻璃静止仍有，截图见会话记录）：**接缝重置**（旧交接 §4，截图里那条横贯窗户的斑点线）+ **火萤钳制**（重尾样本，"孤立亮点"形态的根治）。**旧交接 §4 至此已实施完毕。**
> - 第三轮（用户复测："噪点一点没变"）：部署链已端到端验证无误（日志 `copied=0`、四处理化指纹、启动时间戳三方一致）⇒ 问题出在**改动没命中主导噪点源**。真根因有两个，均已修：
>   1. **RIS 提案数死 lane**：`perfPrimary/perfSecondary` 自 Perf-L10 改造后全仓无写入方 ⇒ `clamp(0,1,32)=1` ⇒ 本地光 RIS 每帧每像素只抽 **1 个**提案（估计值=随机一个发射体的全额贡献，方差最大档）。09-30 用户实测"候选数怎么调都不动"正是因为旋钮断线。修复：`vptRisProposalCount()` 死 lane 时地板 16（提案=纯 buffer 读+ALU，**不新增可见性射线**）。三处消费者：体素 RIS 循环、权重归一、动态光提案（min 16,4）。
>   2. **清玻璃 litScale 的 1/albedo 放大**：`litGlassTintScale = luma(behindRadiance)/luma(receiverAlbedo)` 把 RIS 残差按 1/albedo 放大，深色接收面上饱和到 1，恰好落在玻璃贴图白色边框纹素上 ⇒ 沿玻璃行边界的饱和白点线（用户截图点线）。残差减半只影响"刚饱和"的边缘点，饱和点不动 ⇒ 第二轮钳制"看不出变化"的原因。修复：取光来源改确定性量（evaluateVLIEmitterRadianceAtHit + getSurfaceAmbientRadiance，与 GI 侧清玻璃分支同源），放大路径整个消失。
> - 第四轮（用户复测：**残影滞留**，"前两天没拖尾"）：拖尾 = 长历史 + 宽位置容差在**慢速运动**下以全权重拖旧光照，以前被 1spp 颗粒掩盖、16 提案压净噪底后显形（用户猜"反射"——反射路径无时域状态未动过，实为透射接收面这份 rgen 历史）。修复：**运动自适应历史封顶** `VPT_TRANSPARENT_MOTION_CAP`——玻璃面点前后两帧投影的屏幕滑移（含 ≤1px jitter 差，1.5px 死区吞掉）驱动 `cap = 96/(1+max(slip−1.5,0)×8)`：走动 cap→1~7（拖尾当场消失）、静止满 96（噪点收益不变）。同时修掉火萤钳制"亮区入场爬升慢"（运动中 alpha 变大）。另补齐 `rt_test_gi_half.rgen.spv`（GI_HALFRES 变体）在 resources 两处的漏同步。
> - 第五轮（用户纠正时间线："前天玻璃内一点噪点没有，今天才突然大量噪点"；确认当晚**另有一条并行工作线**在改 VulkanCore.cpp 的 VLI/区块并在 21:27 重编部署过 DLL、"目前修好了"）。新任务：**旗帜图案错乱 / 展示框物品不显示 / 掉落物间歇贴图错乱**。按用户要求逐条读代码核实（不轻信注释，找到一处过时注释"普通实体纹理只用 1..31"已修），三个症状在代码层全部闭合到同一根因：**实体纹理槽池耗尽后的本帧 LRU 抢占**（`EntityRtCapture.slotFor`：抢占本帧已用槽 ⇒ 几何采样被换人的贴图 = 错乱；图集槽被换人后几何 UV 处恰为透明 ⇒ alpha 裁切把几何整片剔掉 = "不显示"。10-13 曾以 32→64 修过同款症状，现 63 个常规槽又满）。修复：**槽池 64 → 128**，同步五处：
>   1. `EntityRtCapture.java` MAX_TEXTURE_SLOTS 64→128（本地玩家代理槽 = MAX_TEXTURE_SLOTS 自动跟随）；
>   2. `VulkanCore.cpp` ENTITY_RT_TEXTURE_SLOT_COUNT 64u→128u（JNI 边界 / 布局 binding 36 / entityInfos 数组全部符号化跟随）；
>   3. 主描述符池 COMBINED_IMAGE_SAMPLER 256→512（布局总量 214→406）；
>   4. ring 描述符池 128→512/集（**顺带修复既有隐患**：64 槽时代 ring 的 128/集已 < 布局需求 214，静默走 legacy idle 兜底）；
>   5. `rt_test.rahit` / `rt_test.rchit` 的 `entityAtlases[64]`→`[128]` + sampleEntityAlphaAtlas case 表 1..128。
>   补丁脚本：`.tmp/patch_entity_slots_128_20261024.py`（C++）、`.tmp/patch_entity_atlas_cases_128_20261024.py`（shader）。
>   产物：rahit.spv 51128B `CE61FF204857`、rchit.spv 155388B `4C37EFA3E81A`、DLL 四处 `10A2BFD0B304C914`（22:14），已验一致；22:14:36 新起的游戏会话无 RT 管线错误。
>
> - 第六轮（用户新任务：**粒子残影**，"老问题，跟 MV 有关，去查官方 RR 怎么处理粒子 MV"）。调研结论：官方 DLSS-RR **不用粒子 MV** 解决粒子拖尾 —— 现行指南的架构是 `kBufferTypeTransparencyLayer`(+Opacity) （粒子画进单独缓冲）+ `kBufferTypeColorBeforeTransparency`（透明叠上去之前的噪声色作 guide），注释原话 "to help with ghosting or disappearing particles"；本仓库 10-23 用的 `kBufferTypeTransparencyHint`(27) 在现行指南中已被这套取代。本仓库粒子的 MV 链路本身是通的（10-09 起 ParticleRtCapture.particlePrevRel 逐粒子相机相对位移 → uploadParticleBatch motion 通道 → rchit VertexMotions），残影的剩余机制是：生灭粒子没有历史语义（MV 修不了）+ BLAS 2 帧重建节奏 + 快速粒子对 MV 精度敏感。本轮落地**旧机制的最小接入**：
>   1. `material_resolve.glsl`：粒子命中置 `RT_STATE_PARTICLE_BIT`(bit 16，makeHitState 之后；
>      rchit/rgen 各声明同名常量)；
>   2. `rt_test.rgen`：主射线读取该位（与 prevOffset 同时刻，之后会被次级射线覆盖）→ 
>      `rrParticlePixel` 并入 TransparencyHint 的 max()；
>   3. `VulkanCore.cpp`：`rrTransparencyHintEnabled()` 默认**启用**（10-23 默认关的理由——值未定义——已失效，
>      rgen 无条件写值；回退 ORION_RR_TRANSPARENCY_HINT=0）。
>   ⚠ **诚实声明**：当前 nvngx_dlssd 是否实际消费 hint 27 未经验证（现行指南已不记载）；不被消费时本改动无副作用、
>   也无效果 —— 那时根治粒子残影的路线就是 TransparencyLayer 架构（rgen 需为粒子像素追一条"背景着色"射线 + 宿主加两个 tag，
>   主要工作量在把主命中着色块抽成可调用）。本轮产物：rgen `CA5B217F…`/rchit `2C1CA286B4C2`/DLL `726153202F119925`。
> - 第七轮（2026-10-25，用户实测否决第六轮的 hint 默认启用："退回以前的半透明处理，现在只要快速转动视角就会堆积大量噪点，非常大的噪点"）。
>   机制复盘：hint=1（玻璃 delta/接收面 guide 像素）+ 运动封顶在**快速转视角**时两级时域同时失效 —— rgen 侧滑移大 ⇒ 历史封顶≈1；
>   RR 侧 hint ⇒ 不信任历史 ⇒ 透过玻璃的内容裸 1spp ⇒ 特大噪点。两项修复：
>   1. **玻璃/透射像素退出 hint**（TransparencyHint 只标记粒子 rrParticlePixel；玻璃恢复 10-23 之前完全按主表面历史平均的处理）；
>   2. **运动封顶加 8 帧下限**（`historyCap = max(cap, 8.0)`）：快转保留 ~1/2.8 的 rgen 侧平滑，拖尾 (7/8)^n 在 8 帧后剩 0.34、
>      16 帧后 0.12 ⇒ 残影滞留不会回来。粒子标记与宿主开关保持不动（hint 值现仅粒子非零）。
>   产物：rgen `75F53151C24E` / ser `1F7243BD6985` / gi_half `750F04EE463A` 四路一致；rchit/DLL 未动。
> **⚠ 并行会话竞态教训（本仓库新增一条坑）**：第五轮部署后指纹复查发现 run/shaders 的 rahit 被并行会话用旧档覆盖、cpp/shaders 源码 22:13 又被改过（128 case 保留）。且 **`_deploy_stale_spv.py` 有设计盲区：只同步"本轮重编过"的产物，cpp 侧新鲜时 run 被外部覆盖它不报错也不修复** —— 部署后必须手动四路指纹比对，不能只看脚本输出。两条工作线同时跑时，重启游戏前先确认四路指纹一致。
>
> **回退档位**（用户要求"要么继续要么回到前天"）：全部行为改动都有独立开关，以下 7 个 define 一起置回旧值即逐位回到 10-22 行为（payload 大 8B 无影响）：
> `GI_CLEAR_GLASS_VISIBILITY_AS_AIR=0 GI_GLASS_SEAM_RESET=0 VPT_TRANSPARENT_RECEIVER_RIS_SAMPLES=1 VPT_TRANSPARENT_HISTORY_FRAMES=48 VPT_TRANSPARENT_FIREFLY_CLAMP=0 VPT_TRANSPARENT_MOTION_CAP=0 VPT_RIS_PROPOSAL_OVERRIDE=1`

## 1. 症状 → 根因

### ① 清玻璃没当空气
- 相机主路径（10-07 `GI_CLEAR_GLASS_AS_AIR`）与 GI 走查（10-13）早已当空气；**太阳/AO/本地光的可见性 any-hit 一直漏着**。
- 两个 rahit 的 `RT_PURPOSE_GLASS_VISIBILITY` 玻璃分支对所有 [2.0,2.5) 材质无差别乘
  `glassPaneTransmissionColor(白 sprite × 灰面 shade ⇒ 0.5~1.0) × 薄片 Fresnel(≈0.92)` ⇒
  隔着清玻璃的光被压暗 8%~50%，且深浅随被穿过的面朝向变。

### ② 彩色玻璃静止噪点
- 09-30 已实测排除：不是采样旋钮（不改变结构）、不是 `GI_GLASS_REFLECTION_LEGACY`。
- 10-21/10-24 的 payload 播种现已完备（主射线 + 6 处 GLASS_VISIBILITY 全部
  `albedo=1.0 / parallaxRayTravel=0.0 / glassAlpha=0.0`，本轮逐一复核过），主射线其实不进累乘分支（用途 7 ≠ 5）。
- **真正的结构性根源**：透过玻璃/水看到的接收面写不进 G-buffer，DLSS-RR 拿不到它的表面历史；
  它唯一的时域平均是 `evaluateVPTCandidateDirectTemporal` 的世界空间历史，而那份历史每帧只喂
  **1 个** RIS 样本（1 次发射体选择 + 1 条可见性射线），48 帧封顶后残余 ≈14% 的 1spp σ。
  两级时域平均沉淀成持续闪烁的噪点底，染玻把灰噪染成彩噪。

## 2. 本轮改动（全部纯着色器，payload 未动，DLL 未动）

| # | 改动 | 位置 |
|---|---|---|
| 1 | 可见性分支里清玻璃当空气：`materialId < 2.05 && glassVertexFactorIsFaceShade(vertexColor.rgb)` ⇒ 保留 `glassAlpha=1.0`（雨棚位）后直接 `ignoreIntersectionEXT`。不计面序（0 面=偶数，下游染色玻璃奇偶不变）、不消耗 16 面预算。开关 `-DGI_CLEAR_GLASS_VISIBILITY_AS_AIR=0` 回旧档 | `rt_test.rahit`（glassAlpha 之后）、`rt_test_translucent.rahit`（voxy 否决与 glassAlpha 之后） |
| 2 | 透明接收面直射改 **4 样本**：新函数 `evaluateVPTRISDirectLightingMultiSample`（方块光 RIS ×4 平均，动态光源保持单样本），`evaluateVPTCandidateDirectTemporal` 是唯一消费者（另有 21353 处水体直接调用，同族）。rngState 链式推进保证样本独立。开关 `-DVPT_TRANSPARENT_RECEIVER_RIS_SAMPLES=1` 回单样本 | `rt_test.rgen` ~3544-3590、调用点 ~5669 |
| 3 | 透明接收面历史 48 → 96 帧（残余 14%→10%；与 4 样本相乘总残余 ≈ 此前 1/2.9；代价：放/拆火把后玻璃后收敛 ~1.6s）。开关 `-DVPT_TRANSPARENT_HISTORY_FRAMES=48` | `rt_test.rgen` ~5269-5278 |

## 3. 产物指纹（**第三轮后**基线；四处逐字节一致已校验）

| 产物 | 字节数 | SHA256 前 12 |
|---|---|---|
| `rt_test.rgen.spv` | 571048 | `207B6C452297` |
| `rt_test_ser.rgen.spv` | 583272 | `334FD53D4064` |
| `rt_test_sharc_update.rgen.spv` | 586484 | `456BE3CB2710` |

（第三轮只动 rgen；rahit/rchit/rmiss 维持第二轮指纹：`rt_test.rahit.spv` 43396/`D93CF0269A30`、`rt_test.rchit.spv` 147688/`06548330602A`、`rt_test.rmiss.spv` 139648/`6256673D3153`、`rt_test_translucent.rahit.spv` 37480/`AE2C661BC8E7`、`rt_test_translucent.rchit.spv` 108104/`B198E4D7FAF8`，ser.rmiss= rmiss 同哈希、pomdeferred= rchit 同哈希。）DLL 未动（`C3D9F71C0729D937`）。早期各表全部作废。

## 3.1 第二轮改动明细

| # | 改动 | 位置 | 开关 |
|---|---|---|---|
| 4 | **接缝重置**（旧交接 §4 落地）：面距 > 1.5 格 ⇒ `glassFaceOrdinal` 重置为 0，`lastGlassFaceT` 记录 `gl_HitTEXT`。**面序与穿层预算拆成两个字段**（`parallaxRayTravel` 只剩面序；预算走新 `glassFaceBudget`，不随重置清零） | `hit_payload.glsl`（+2 float）、两个 rahit 玻璃分支、rgen 6 处播种点（脚本 `.tmp/patch_glass_seam_20261024.py`） | `-DGI_GLASS_SEAM_RESET=0` 回旧档 |
| 5 | **火萤钳制**：当帧样本亮度 > 历史均值 ×8 时按比例压缩（只压标量不动色度）。护栏：仅位置判据通过、历史计数 ≥4、历史亮度 >1e-3 时生效 | `rt_test.rgen` `evaluateVPTCandidateDirectTemporal` 内（多样本调用之后） | `-DVPT_TRANSPARENT_FIREFLY_CLAMP=0` 回无钳制 |

预算判据口径：`payload.glassFaceBudget >= float(GI_GLASS_VISIBILITY_MAX_FACES)`，其中 budget 在判据处已含当前面 ⇒ 与旧档 `glassFaceOrdinal + 1u >= MAX` 逐值同义（第一稿 `+1.0 >=` 是差一错误，已改）。

## 4. 验证判据（下一会话/用户侧）

1. **接缝亮线消失**（截图里那条横贯窗户中部的斑点线）：冰/染色玻璃贴不透明方块的一侧；
2. **单格冰悬空**（进/出面距 1.0）边缘**不应有任何变化**；
3. **单层染色玻璃透射色**不应整体变亮/变暗；**多层（≥2 格相邻）玻璃/冰会均匀略暗**（每格各补一次 filter，"一片一滤"预期，不是回归）；
4. **穿层上限仍生效**（预算已与面序拆开，重置不得绕过 16 面上限）；
5. 清玻璃后：阳光光斑、灯光色池、AO 与不隔玻璃时**逐位一致**；清玻璃仍挡雨（`glassAlpha` 位保留）；
6. 静止看彩玻：孤立亮点（火萤）应基本消失，残余均匀颗粒应明显变细（4 样本 × 96 帧 × 钳制三层叠加）；
7. 若仍有残粒：下一个已知未启用环节是 DLSS-RR 的 TransparencyHint（rgen 已写值，宿主被 `ORION_RR_TRANSPARENCY_HINT` 关着）——它是"降历史信任"，对静止噪点方向**不一定**有利，开之前先想清楚。

## 5. 未竟事项

- ~~旧交接 §4（面序断开重置）~~ **本轮已实施**（见 §3.1 第 4 项）。
- `run/` 根目录平铺的旧 SPV 孤儿仍未清理，是否被加载仍未验证（旧交接 §7 陷阱照旧）。
- 不透明表面（窗框/墙面）在截图上也有零星白点：09-30 记载该路径"本地光经 RR 后是干净的"，rgen 侧时域（LOCAL_RIS_PRIMARY_TEMPORAL）也实测过"开启反而更噪"，两者都别再动；若用户仍报不透明面噪点，先用它自己是否随 RR 开关变化来定位（那是 RR 的输入侧问题，不是 RIS）。
