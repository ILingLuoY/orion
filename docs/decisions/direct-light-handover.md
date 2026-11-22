# 直射光「太阳 → 月亮」交接连续化（2026-11-19）

原文件：`cpp/shaders/sky_model_common.glsl`、`volumetric_clouds_v5.comp`、
`volumetric_clouds_v3.comp`、`volumetric_light.comp`、`fog_screen_march.comp`、
`fog_froxel_composite.comp`、`rt_test.rmiss`。

## 1. 症状

用户：「云在夕阳到夜晚的某一段时间光照会突然跳变」。

表现为日落之后约十几秒内，整片云（连同没入地平线那一侧的暖光、前向散射的银边、
云影的方位）一次性换到天的另一边，并同时变暗。

## 2. 根因

2026-11-16 把"夜间光源"从 `-sunDirection()`（永远满月、永远与太阳正对）改成
**按月相沿日轨移动的真实月亮**之后，四个消费者（云 v5/v3、体积光、体积雾）都写成：

```glsl
vec3 lightDirection = sunElevation >= 0.0 ? sunDir : moonDir;   // 方向硬翻
float dayAmount     = smoothstep(-0.03, 0.03, sunElevation);    // 颜色/能量混色
```

方向在 `sunElevation == 0` 那一刻**整束**换掉，而混色权重 `dayAmount` 在那一刻只有
**0.5** —— 即直射能量还剩一半（云满档 1.5，此处 0.75）时就换了方向 + 颜色。

按 20 分钟一天（`sunElevation = 0.819·sin(2π·phase)`，`d/dphase ≈ 5.15`）算，
方向跳变在 1 帧内完成，量级：

| 月相 | 方向差 | 交接帧直射能量 |
|---|---|---|
| 满月（z=0） | 176.4° | 0.750 / 1.5 |
| 上弦（z=0.25） | 86.4° | 0.781 / 1.5 |

`dayAmount` 所属的 ±0.03 带在时间上只有约 7 秒，所以观感是"突然"而不是"过渡"。

同一处的第二个病灶：颜色用了 `skyModelSunColor()`，而它在 `sunElevation < 0` 处
**硬返回月蓝**（2026-11-13 的修正，见该函数注释）。交接带里哪怕只借它取一次颜色，
也会在能量尚未归零时提前变成月蓝。

## 3. 定案

交接点下移到太阳沉到地平线下 **0.05**（≈2.9°），并让两侧能量**在该点同时归零**：

```glsl
const float SKY_DIRECT_HANDOVER_ELEVATION = -0.05;
float skyDirectSunAmount (float s) { return smoothstep(SKY_DIRECT_HANDOVER_ELEVATION, 0.06, s); }
float skyDirectMoonAmount(float s) { return 1.0 - smoothstep(-0.11, SKY_DIRECT_HANDOVER_ELEVATION, s); }
bool  skyDirectIsSun     (float s) { return s >= SKY_DIRECT_HANDOVER_ELEVATION; }
```

- 方向/颜色仍在一点硬翻，但那一帧两侧权重都是 0 ⇒ 画面上不可见；
- 交接带之外两侧各自连续，`s > 0.06` 太阳权重严格 1.0、`s < -0.11` 月亮权重严格 1.0
  ⇒ **白昼与深夜两端与改动前逐位一致**；
- 太阳侧颜色改用 `skyModelSunColorContinuous()`（不判夜、不早退，只含天气缩放），
  月蓝早退只留给真正的夜间；
- 月光专属的 `moonStrength` / `moonPhase` / `skyModelMoonPhaseDim` 改按
  `moonDirectAmount` 施加。旧式按 `1 - dayAmount` 会在 `s ∈ (-0.03, 0)` 就满额生效
  —— 那一刻太阳项还占大头，等于用「月光强度/月相」误伤夕阳余晖。

逐帧复算（Python 复现同一组公式，步长 1/24000 相位 ≈ 1 游戏刻）：交接带内
`|Δ(能量 × 方向)|` 的最大单帧变化是 **0.0044**（满档 1.5 的 0.3%），而旧口径是
**0.75 单帧**（50%）。

## 4. 消费者清单（必须共用同一组权重函数）

| 文件 | 直射光 | 满档 | 备注 |
|---|---|---|---|
| `volumetric_clouds_v5.comp` | 云 | 1.5 | 主路径 |
| `volumetric_clouds_v3.comp` | 云 | 6.0 | 仅 `cloud_v3.flag` 时存在；同口径防漂移 |
| `volumetric_light.comp` | 光柱 | `GODRAY_SUN_ENERGY` | 月光项乘 `GODRAY_MOON_FRACTION` |
| `fog_screen_march.comp` | 体积雾 | 1.5 | 同时把夜间方向从 `-lightDir0` 改为**真实月亮**（与其余三处一致） |
| `fog_froxel_composite.comp` | 体积雾（froxel 路径） | 1.5 | 与上一行同式；两边必须同步 |
| `rt_test.rmiss` | 反射里的 Tyndall 云 | 6.0 | 次线反射路径，主 `main()` 的方向选择同步改用 `skyDirectIsSun` |

## 5. 有意不改的地方

- **地形 `rt_test.rgen`**：`getTimeDrivenSkyLightDirection()` 仍在地平线处硬翻。
  那里直射能量本来就随 `|e|` 归零（`normalizedCycleElevation`），硬翻发生在零能量点；
  而且"太阳落山后云顶仍被照亮、地面已经不受光"是物理正确的，不该抹平。
- **阴影方向**（`volumetric_fog.glsl:1125`、`fog_froxel_composite.comp` 的
  `cloudGapTransmittance` 探针）：必须与阴影生产者 `sunShadowLightDirection()` /
  `rt_cloud_shadow.comp`（恒用 `sunDirection()`）保持同一向量，不能跟着交接带走。
- **`volumetric_light.comp` 的空气分支 / `fog_froxel_light.comp` 的可见性射线**：
  只做阴影查表，跟随阴影生产者。

## 6. 排除的方案

1. **只把 `dayAmount` 的 ±0.03 加宽**（例如 ±0.10）。方向仍在 `s = 0` 硬翻，只是
   那一帧权重从 0.5 变成别的值 —— 只要不为 0 就还是"半亮时换方向"，治不了根。
2. **交接点放到 -0.10 以保留"日落后云还有暖光"**。会让暖色直射光延续到地平线下
   5.7°（≈37 秒），与 2026-11-13 的定案（"夜间直射光完全不含暮色，日落之后的暖色
   全部由天空 LUT 承担"）冲突。而且旧口径本来就**没有**暖色余晖：`s < 0` 那一侧
   给的是**蓝月光**从月亮方向打过来 —— 用户看到的就是这个错向项。所以本方案只是
   去掉错向项，并保留 `s ∈ (-0.05, 0)` 一小段真实的太阳余晖。
3. **方向插值**（`normalize(mix(sunDir, moonDir, t))`）。满月时两向量夹角 176°，
   插值中点几乎对消，`normalize` 会退化出任意方向，比硬翻更糟。
4. **在环境 in-scatter 里也消除方向跳**。`clearSkyInscatter()` 的三个方向瓣
   （天顶 / 太阳侧 / 反日侧）确实仍随 `lightDirection` 翻转，但它们乘的
   `skyScale = daylight · |e|/cos35` 在交接点已经很小（≈0.008），实测方向跳的幅度
   ~0.01 量级、低于云自身噪声，不值得为它引入第三套方向。

## 7. 回退

把各区段的 `skyDirectSunAmount/skyDirectMoonAmount/skyDirectIsSun` 换回
`dayAmount` / `1 - dayAmount` / `sunElevation >= 0.0`，并把
`skyModelSunColorContinuous()` 换回 `skyModelSunColor()`。交接带常量
`SKY_DIRECT_HANDOVER_ELEVATION` 无需删除（回退后无读者）。

## 8. 复测要点

夕阳 → 夜晚整段（`phase 0.46 → 0.56`）：云的亮度/颜色/银边/云影方位连续；水面与
玻璃里的云倒影同步；雾里那团暖光不跳到反方向；光柱不换边；夕阳余晖保留到地平线下
约 3°。白昼与深夜两端观感应与改动前**逐位相同**。
