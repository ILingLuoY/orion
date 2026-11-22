# 俄罗斯轮盘：生存率曲线为什么长这样，以及为什么不照抄参考实现

> 从 `cpp/shaders/rt_test.rgen:21815-21877` 整理。代码里保留契约
> （三个数值、无偏的充要条件、回退方式），这里保留**与另一个参考实现的曲线对比**
> 和**被否决的移植方案**。

---

## 契约：无偏的充要条件

```glsl
float rrSurvival = max(1.0 - rrTermination, 0.20);
...
if(rrRoll >= rrSurvival) { break; }
currentThroughput /= rrSurvival;
```

掷骰用 `rrSurvival`、除的也是 `rrSurvival` —— **同一个 p**。这就是无偏的全部条件：
E[贡献] = p · (T/p) · rest = T · rest。所以：

- **0.20 的地板不引入偏倚**，它只改方差与终止率；
- 把地板调回 0.05 也不引入偏倚，只是把最大权重从 5× 抬到 20×（2026-10-09 的
  0.05 → 0.20 是**方差**取舍，不是无偏性取舍）。

任何"地板导致有偏"的说法都是把方差当成了偏倚。

## 当前曲线

```
T        = max(r, g, b)                       // 最大分量（变量旧名 rrLuminance 是误名）
rrLow    = clamp(0.85 - sqrt(T), 0, 1)
term     = clamp(rrLow² + max(0, bounce/limit - 0.4), 0, 1)
survival = max(1 - term, 0.20)
```

注意 `rrLow` 在 `T ≥ 0.7225`（=0.85²）时就压到 0 ⇒ 该区间 **RR 完全不终止**（只剩
弹射项）。地板 0.20 实际只在弹射预算末段（`bounce/limit` 接近 1）才起作用。

## 与参考实现（Prime PT `math/transport.slang`）的曲线对比

参考实现是 `survival = clamp(maxComponent(T) × etaScale, 0, 1)`：无弹射项、无地板。

| T | 参考实现 | 本工程 | 本工程的实际后果 |
|---:|---:|---:|---|
| 0.7225 | 0.72 | **1.00** | 我们完全不终止 |
| 0.25 | 0.25 | **0.88** | 我们多追 3.5 倍路径 |
| 0.10 | 0.10 | **0.72** | 我们多追数倍 |
| 0.01 | 0.01 | **0.44** | 最大权重：它 100×，我们 2.3× |
| 0 | 0 | 0.28 | 它直接杀，我们继续 |

**结论：参考实现的曲线比我们激进得多**，暗路径几乎必死、权重可以背到 100× 以上。
把它照抄过来会**放大**萤火虫问题，而不是改善 —— 与该参考实现自己的 FIXME 记录
（"ReSTIR PT 仍存在萤火虫，尚无已确认根因"）方向一致。

所以「照抄参考 RR」被否决。否决理由不是"它的数学不对"（它的式子是干净的无偏形式），
而是**它对应的是另一套重建预算**：它没有我们这条 1spp→à-trous→DLSS-RR 的时域依赖，
可以承受更大的单样本权重。

## η² 为什么在本工程恒等于 1

参考实现在透射事件后做 `etaScale *= relativeEta²`（辐亮度按 η² 缩放），并在 RR 度量里
乘上 `etaScale`。本工程**没有这一项，且这是正确的**：

- 玻璃走的是薄壁（pane）判据，不做体内折射（`material_resolve_glass.glsl` 的薄片模型）；
- 光穿过薄片时进界面与出界面的 η 相消 ⇒ 净 `etaScale ≡ 1`。

**这不是"不需要补偿"，而是"补偿量恒等于 1"** —— 这个区别必须写下来。将来若接入厚介质
（体内折射），η² 会立刻变成 ≠1，而 RR 度量里缺这一项就会让穿透介质的路径能量对不上；
届时要同时补 `etaScale` 的累积与它在 RR 度量里的乘法，两处缺一不可。

## 可以学的三件事（按可落地性排序）

1. **把生存率写成单一闭式函数，魔数具名**。现在 `0.85` / `0.4` / `0.20` 三个数纠缠在
   一个复合表达式里，无法单独调；改一个都会同时改变终止率与最大权重。参考实现的形式
   （`f(T) · etaScale` 再 clamp）在这一点上更可测。
2. **η² 显式化**：写成常量 1 并附原因（见上），而不是"不存在"。
3. **估计量不该随设置改变**。参考实现的 RR 是积分器的一部分（`PRIME_RUSSIAN_ROULETTE_START = 1`，
   始终开）；本工程的 RR 挂在 `cam.rtSampleParams.y`（用户开关）后面，关掉就换了一个
   估计量 ⇒ 噪声分布变了，而 DLSS RR 的历史是按某一种分布调的。这是**已知的估计量
   不连续性**，暂不改动（关 RR 有 2026-10-07 实测的 −5.9ms 收益），但要登记为取舍。

---

## 2026-11-07 GitHub 调研：RTXPT / Remix 的现行公式与本工程的差异

### RTXPT（NVIDIA-RTX/RTXPT，2025，`Rtxpt/Shaders/PathTracer/PathTracer.hlsli` 的
`HandleRussianRoulette`）——**与本工程同源**

```hlsl
const float rrVal = sqrt(Luminance(path.GetThp()));   // "closer to perceptual"
// a milder version of Falcor's Russian Roulette
float prob = saturate(0.85 - rrVal);
prob = prob * prob;
prob = saturate(prob + max(0, vertexIndex / maxBounceLimit - 0.4));
if (sampleNext1D < prob) terminate;  // 否则 thpRuRuCorrection = 1/(1-prob)
```

即：**本工程的 `(0.85-√T)² + 弹射坡道(0.4)` 就是 RTXPT 的生产公式**（常数逐一相同）。
真正的差异只有两处：

| 项 | RTXPT | 本工程 |
|---|---|---|
| 度量 | BT.709 亮度（再开方） | max(r,g,b)（再开方；2026-10-23 刻意选择，防饱和色失衡） |
| 下限 | 无（prob 可到 1 ⇒ survival→0，权重无上界） | 0.20 地板（最大 5×，2026-10-09 的方差收敛） |

### Remix（dxvk-remix main，2026，`rtx/algorithm/integrator.slangh` 的
`calculateRussianRouletteContinueProbability`）——**新增的东西**

- 双模式 `RussianRouletteMode`：
  - `ThroughputBased`（默认）：`min(BT709Luminance(fullThroughput × continuationThroughput),
    russianRouletteMaxContinueProbability)`；
  - `SpecularBased`：`lerp(russianRouletteDiffuseContinueProbability=0.1,
    russianRouletteSpecularContinueProbability=0.98, specularWeight)`，其中
    `specularWeight = (1-perceptualRoughness) × saturate(russianRouletteDistanceFactor=0.1 /
    hitDistance)`（不透明漫反射 lobe 权重 0、非不透明材质权重 1）。定位是"低粗糙度 +
    短命中距离的镜面细节多追"。
- **`russianRouletteMaxContinueProbability = 0.9`**：survival 永远 ≤0.9 ⇒ 任何路径每跳都有
  ≥10% 概率终止。选项文本明说这是防"镜面房间无限路径"，并注明"RR 太激进会让 warp 里
  线程没活干、伤 occupancy，除非配合 SER"（本工程 GI 恰好全程 SER，该约束成立）。
- **`russianRoulette1stBounceMinContinueProbability = 0.6`**：第一跳的下限，理由是"对
  denoiser 来说一个弱样本好过一个缺失样本"。
- `pathMinBounces=1` / `pathMaxBounces=4`（可配，本工程用 giBounceLimit + 坡道等效）。
- 掷骰用**蓝噪声**（`getNextSampleBlueNoise`）；`<=` 比较保证 p=0 必终止；补偿除法带
  `kThroughputClampedDivisionMaximum=65000` 上限。

### 本工程的可落地清单（按性价比；全部要进跑批 A/B）

1. **蓝噪声掷骰**：把 `pcg_hash(seed ^ 0x9e3779b9)` 换成现成的
   `glassBlueNoiseSample(pixel, frameCount, index)`（binding 76 + R2 偏移 + min 钳制，
   `rt_test.rgen:404-424`，与 Remix `getNextSampleBlueNoise` 同口径）。终止/存活的空间
   分布变蓝噪声 ⇒ DLSS-RR 时域累积里的"孤立亮点"减少，直接对着 2026-10-09 那次
   "静止噪点"的根因。均值不变，只改分布。
   ✅ **已落地 2026-11-07**（`rgen_main_body.glsl` GI 回路 RR 块，宏
   `GI_RR_BLUE_NOISE` 默认 1，回退 = 编 SPV 时 `-DGI_RR_BLUE_NOISE=0`；
   seedIndex = `diffuseBounceCount + 37`，每弹射一档 R2 相位、与玻璃选层链错开）。
   14 个 rgen 变体已重编并四处同步（run / src / build / cpp），审计 0 漂移。
   **画面对比（静止时噪点是否变细）仍属跑批验证项**。
2. **survival 上限 0.9**（Remix 同名参数）：现在 T ≥ 0.7225 的亮路径完全不终止；加上限
   后平均路径长度下降、代价是亮路径每跳多背 ≤1.11× 权重。需 A/B。
3. **第一跳地板 0.6**：本工程常跑 giBounces=1 ⇒ 第一跳就是唯一跳，RR 本来就不触发；
   只有把 GI 弹射数调回 2+ 时才有意义。暂不动。
4. **SpecularBased 模式**：本工程 GI 回路是余弦漫反射 + 薄壁玻璃，没有"镜面 lobe 继续
   追踪"的分支，暂不移植；将来若把 glossy 并进 GI 路径再评估。

---

## 2026-11-16：glossy 反射链的 RR（第二个实现）

GI 之外还有一条独立的镜面链 `rgen_glossy.glsl`（trace 段 40% 的成本）。它此前有 RR，
但写成了"固定阈值筛选"：

```glsl
q = max(throughput)                 // 每跳只乘一个 Fresnel
if (q < 0.02) return;               // 萤火虫闸
if (q < 0.30) { survive with p = q; throughput /= q; }
```

**问题**：金属链的 Fresnel 是 0.9+，`q` 永远 >0.3 ⇒ 这段代码一次也不执行。镜面深度
实际由整数 `mirrorBounceLimit`(1..8) 决定，于是"镜子里映镜子少一层"是硬切档，
且空房间与堆满镜面的房间付一样的射线钱。

改成与 GI 同形的深度坡道（`GLOSSY_RR_DEPTH_RAMP=0.5`、`GLOSSY_RR_SURVIVAL_FLOOR=0.35`）：

```
term      = (1 - q) + 0.5 * max(0, pathStep / cap - 0.4)
survival  = clamp(1 - term, 0.35, 1)
存活 ⇒ throughput /= survival
```

契约不变（掷骰 p 与除法 p 是同一个数 ⇒ 无偏），地板只把最大权重抬到 1/0.35 ≈ 2.86×。
前 40% 的 cap 深度不参与抽签 ⇒ 近处镜面保持满质量；`mirrorBounceLimit` 仍是硬上限。
语义上这是"按内容付费"而非"按档位付费"：亮的镜面链自然多弹，暗的自动早停。

**默认值对齐**：`Settings.cpp` 的 `glossyRrEnabled()` 之前要求 `ORION_GLOSSY_RR=1`，
而 shader 头注释写的是"默认开" ⇒ 这份实现从未生效。现改为默认开、`=0` 关闭。

回退阶梯（都不需要改 shader 代码）：
`ORION_GLOSSY_RR=0` → 整块不执行（回到确定性跑满）；`-DGLOSSY_RR_DEPTH_RAMP=0` →
只留亮度项；`-DGLOSSY_RR_SURVIVAL_FLOOR=1.0` → 只剩抽签、不减预算。

**已验证**：14 个 rgen 变体重编并四处同步，`tools/audit_spv_freshness.py` 80 个产物
0 漂移 / 0 未部署。**帧时间与画面（镜面链噪声、映镜子层数观感）仍属跑批验证项**——
出数前不要把 `GLOSSY_RR_DEPTH_RAMP` 再往上抬。
