# SER（Shader Execution Reordering）：开销归因与结论

> 从 `cpp/VulkanCore.cpp` 搬出（原文）。结论已写回代码：
> 键内容与融合调用在真实场景里没有可复现收益，生产 pin 回 mode 1。
>
> **2026-11-07 A/B 三套开关已就位**（默认全部等于改动前行为，只有编第二份 SPV 才变）：
> - `SER_PROBE_SECONDARY_FLAGS=1` —— mode 6 探针的遍历改用 `secondaryTraverseFlags()`
>   （与普通 trace 同 flags，含背面剔除）；默认 0 = 裸 `gl_RayFlagsNoneEXT`。
> - `GI_RR_MAX_SURVIVAL=0.9` —— 给 RR 生存率加 Remix 的 `russianRouletteMaxContinueProbability`
>   上限；默认 1.0 = 不加（亮路径完全不终止）。
> - `GI_SER_HINT_SBT_BITS=0` —— mode 1 的键去掉重复的 SBT 2 位（位宽 4→2）；默认 1 = 保留。
> 第二套产物由 `.tmp/build_ab_sets.py` 生成到 `.tmp/ab/{probe,rr,key}/`（11 个 rgen 变体各一份），
> 拷进 `run/shaders` 即触发热重载；档位对比直接用 `ORION_SER_MODE=1|6`，无需重编。
>
> **2026-11-07 核对更正**：现网 pin **不是 mode 1，而是 mode 6**
> （`cpp/backend/Perf.cpp` 的 `updatePerfL10SerPathToggles()` else 分支：
> `g_perfL10SerMode = 6u`）。mode 6 = 各漏斗调用点按 mode 1 处理 + **水面
> 反射 DISTANCE_ONLY 探针改走 rgen 侧 hitObject 查询**。注意后者是本文
> 记录的 "pixel-unequal by construction" 臂（跳过 any-hit 过滤、且探针实际传
> `gl_RayFlagsNoneEXT` 而对照路径带背面剔除），已经随 pin 进入生产。
> 改动前的 pin 语义以代码为准，本文历史测量数据不变。

---

## 路径组轮换与键形状 A/B

原位置：`cpp/VulkanCore.cpp:2266-2329`（64 行）

Perf-L10.9 - SER overhead attribution.

The group rotation above measured all three path groups, and the visibility
group came out clearly worse (+1.99 ms on rt, t = 10.37, n = 28) while the
other two stayed inside the noise.  This rotation isolates WHERE that cost
sits by changing the shape of the SER sequence on the visibility group, the
one with by far the best signal-to-noise:
  0 = plain traceRayEXT, group lane off
  1 = hit object + fused reorder with the custom coherence hint (adopted)
  2 = hit object + fused reorder with the driver-picked key
  3 = hit object + execute with no reordering at all
Comparing 1 against 2 charges the custom hint and the extra SBT query it
needs; comparing 2 against 3 charges the reordering itself.

The shader reads the group lane as the mode, so 1..3 also enable the group.
There is deliberately no separate mode lane: every perf lane is spoken for,
and historyPadding0 is not free either - it carries the upscaler-mode marker
that the ReSTIR/DLSS handoff reads.
Perf-L10.10 - SER pinned to what can pay for it, and the freed tail lane
measures the RTAO dead-ray removal instead.

The mode rotation above settled where the entry fee lives: dropping the
reordering (mode 3) did not make the hit-object path cheaper, it made it the
most expensive of the three (+1.76 ms, t = 3.43), and dropping the custom hint
(mode 2) changed nothing measurable (+1.37 ms against +1.60 ms, well inside
the standard error).  So the cost is the hit-object plumbing itself - the
buffered traversal result plus the deferred shader launch - and a hit object
only pays when the deferred shading is heavy and divergent.

The Khronos SER sample's "When to Use SER" section agrees: multi-bounce path
tracing, material diversity, secondary scattered rays and stochastic
divergence benefit; highly coherent primary rays and low-divergence shaders
do not.

Pinned configuration from that reading:
  .x = 1  diffuse-GI secondary bounce  (the measured win, -20% / -41%)
  .y = 0  visibility group dropped     (measured +1.99 ms, t = 10.37)
  .z = 1  transmission / secondary scattered walks
  .w      reused: 1 skips the dead RTAO trace
2026-10-07 SER key-shape ladder (replaces the retired 1/2/3 rotation).
When enabled, perfSerAb.z -- the radiance group lane that doubles as the
funnel mode in rt_test.rgen's serTraceRay -- rotates 1 -> 4 -> 5 -> 6 per
window so every shape of the hit-object sequence is measured in one session
against the same scene:
  1 the shipped custom-key reorder (baseline of the ladder)
  4 post-trace instance-class coherence key (role table + miss + SBT record)
  5 fully-fused hitObjectTraceReorderExecuteEXT with the driver-picked key
  6 mode 1 everywhere plus the DISTANCE_ONLY water-guide probe switched to
    an rgen-side hit-object query (the one pixel-unequal arm -- perf only)
Modes 4/5 are pixel-equal to mode 1 at every funnel call site (same ray,
same flags, only the scheduling/key differ); arm 6 trades the probe's
any-hit filtering for zero hit-shader dispatch, so it is a perf-only arm.
The window accumulator keys off the same phase, so windows never average
two modes together, and the GPU window log prints serMode= per window --
tools/ser/summarize_ser_ab.py turns a session's rt_debug.log into the
per-mode comparison table.  After the winning mode is chosen, set false
and pin that mode in updatePerfL10SerPathToggles.

2026-10-07 测量结论（三轮，最后一轮为修复流式后的干净样本，每 mode 34 个
窗口）：gpuRt 均值 15.66 / 16.10 / 16.17 / 15.66（mode 1/4/5/6），峰值
19.0~20.0 —— 全部落在 ±1.5% 噪声带内，前两轮被流式 bug 污染的样本符号
互相矛盾也印证了这一点。结论：键内容与融合调用在真实场景里不产生可复现
的收益（RTXPT 的 1-bit 键保守路线再次成立），生产 pin 回 mode 1；三条新
臂保留在 shader 里备用（换驱动/换场景类型时改这个常量即可重测）。
