# HANDOFF 2026-10-24 — RT 成本归因与"等价优化"穷举

> 一句话结论：`rt` 那 13~15ms 已被逐项拆干净，**在「1 spp / 1 跳 / 707×376」这个形状下没有剩余的等价（不改画质）优化空间**。
> 落地两项净收益共 **−2.9ms/帧**；四条技术路线被实测证伪；四个测量仪器 bug 被修复。

---

## 1. 基线（默认配置，55 个 60 帧窗口，均值）

| scope | ms | 占比 | 说明 |
|---|---|---|---|
| **rt** | **13.26** | 74% | 只有 traceRays + SHaRC resolve |
| dlss | 1.70 | 9% | |
| **snear** | **1.52** | 9% | 近场太阳阴影 256²（65k 根） |
| **height** | **0.66** | 4% | 地形高度场 512²（262k 根） |
| sdr | 0.36 | 2% | AE + 泛光 + 显示变换（全在输出分辨率） |
| cloud | 0.14 | 1% | |
| wsim / caus / sky / sfar / sharcup / rtpipe | ≈0 | — | |
| **frame** | **18.02** | | |

---

## 2. 已落地（都有同局配对实测）

### 2.1 地形探针按需重建 — **−2.1ms**

病灶：近场阴影图与地形高度场的脏判据是 `g_worldGeometryRevision`，而这个修订号**任何 Section 上传都会 +1**（含实体/本地玩家/粒子聚合），逐帧变 ⇒ 两张图**每帧全量重算**，"按需重建"名存实亡。

改法（`cpp/VulkanCore.cpp`）：
- 新增 `g_terrainGeometryRevision`，只由地形类上传递增；
- `markWorldGeometryDirty(bool terrainChanged = true)`，两个动态调用点传 `false`（`ENTITY_RT_SPATIAL_KEY` 那两处）；
- 两个探针改用 `probeGeometryRevision()`；A/B：`ORION_PROBES_TERRAIN_REV=0` 退回旧口径；
- 高度场额外补"量化中心"门（`terrainHeightCenterXZ()` 是**相机为中心**烘焙的，量化步长 = `2*256/512 = 1.0` 格），否则走动时整张图平移。

实测（258 个窗口，base 臂）：`snear 1.52 → 0.04`、`height 0.66 → 0.00`。

### 2.2 无 DH 时远场阴影默认关 — **−0.79ms**

`sunShadowFarEnabled()` 从"一次性读环境变量、默认开"改为按帧自适应：只在 `g_dhFarFieldMode > 0.5` 时派发。
理由：远场图（1024² ≈ 105 万根/帧）的遮挡体只能来自 DH 实例，无 DH 时全部 miss。
实测（101 对配对）：开启后 `rt +0.79`、整帧 `+1.20`。
开关：`ORION_SUNSHADOW_FAR=1/0` 强制；`ORION_SUNSHADOW_FAR_AB=1` 同局交错。

### 2.3 GI 命中处发光项萤火虫钳制（R1，画质项）

GI 命中点的发光体**没有 NEE**，靠 BSDF 偶然命中，单次可达均值数十倍 ⇒ RR 夹不掉又丢能量。
新增 `GI_FIREFLY_EMISSION_MAX`（默认 4.0）钳制 `nextUnifiedEmission`；`=0` 逐位回到旧行为。
**性能中性**（逐像素 `min`）；实测"无萤火虫"。
⚠ 同期评估：轮盘下限 `0.20`（`rrSurvival = max(1-rrTermination, 0.20)`）对 albedo>0.05 的表面**几乎不夹**（0.15 时生存已是 0.79、权重 1.27×），"抬下限"是空操作，未改。

---

## 3. 仪器：新增与修复

| 件 | 说明 |
|---|---|
| **rt 分项 scope**（`passes` 13→20） | 拆出 `snear / height / sfar / sharcup / rtpipe`，此前它们混在 `sky/cloud` 里 |
| **交错 A/B 换臂口径** | 原按 `frameSerial/60` 换臂，与"60 样本窗口"同周期 ⇒ **拍频锁死**（窗口永远差一格），整局零数据。改为按已完成窗口数 `g_perfL10GpuWindowsLogged` 换臂 |
| **边界完整性补齐** | `pollPerfL10GpuTimestampSlot` 规则是"任一边界不可用就丢弃整槽"。**FG 关闭时 index 19（FG 捕获）被跳过 ⇒ 整局只出 1 个窗口**。`endPerfL10GpuFrame` 统一补写"缺失且序号大于已写最大序号"的边界；另加 300 帧一次的自检 `boundaries incomplete, missing index=…` |
| **窗口标签定格** | `abFar= / giMax= / pom= / spp=` 全部改为**首个样本时定格**（打印时刻求值会拿到下一臂） |
| **射线计数接线** | 宿主侧（binding 46 / 12 个 uint / 回读）本就完整，但**着色器一行都没写** ⇒ 恒为 0。补了 9 个埋点，仅在 stage 15 写入；宿主开关改为 `ORION_RAY_STATS=1`（默认关，零成本） |

复现射线计数：`ORION_RAY_STATS=1` + `ORION_PERF_ABLATION=pin:15`，日志出 `[PERF L10 RAY]`。

---

## 4. 已证伪清单（勿重复）

### 4.1 rt 内部归因（`alt:` 交错，同局配对）

| 关掉的特性 | Δrt | 说明 |
|---|---|---|
| `noGi` | **−5.47** | GI = rt 的 47%，最大项 |
| `noGlossy` | −1.55 | `glossyFullDistance` 已在下限 16 |
| `noVoxelDda` | −1.15 | 见 §5 |
| `shadow1of2` | −0.02 | **空臂**：`sunShadowSamples` 本来就是 1 |
| `noRtao` | +0.03 | AO 射线数为 0（`aoSkip=1`） |
| `gi1Bounce / rrGi / march1 / tierHalf / rrOff` | ±0.15 | 设置已在最低档：`giBounces=1 / giRoulette=1 / rtSpp=1` |

### 4.2 射线计数（站定，55 窗口稳定）

`instPix = 265,832/帧`；`giPix 97.6%`；`giRayPerPix 0.96`；`b1PerPix 0.00`；**`missPerPix 0.00`**；`zeroTp 7.3%`；`aoRayPerPix 0.00`；`farPix 0.0%`。
⇒ 约 520k 根/帧、`rt ≈ 13ms` ⇒ **≈25ns/射线**，且 **GI 射线 100% 命中**。

### 4.3 四条被证伪的降本路线

| 路线 | 实测 | 判据/原因 |
|---|---|---|
| **SHaRC**（哈希辐射缓存） | `SCALE=2`：Δrt **−0.02**（56 对）；`SCALE=12`：**+0.05**（36 对） | 缓存挂在 GI 第一跳命中处（`:23017` 查询 / `:23126` 更新），1 跳下只能替"该顶点 NEE+材质"；细格不命中、粗格不省；update 成本两臂都付 ⇒ 净负 |
| **GI 距离截断** | 48 格：−0.1~−0.3（8 窗口） | `missPerPix=0` ⇒ 射线本来就立刻命中，`tMax` 无意义 |
| **POM 全关** | −0.4~−0.6（31/40 窗口，4 对 −0.59） | 只对高度图材质生效，非主因 |
| **像素棋盘半分辨率**（`rtSpp=0.5`） | 萤火虫 + **锯齿** | 一半像素的 color/guides/MV 全是上一帧 ⇒ 违反 RR 契约 |

### 4.4 其他结论

- **SER 是命脉**：`ser=0` ⇒ rt **37.04** / frame 48.74；`ser=1 mode5` ⇒ rt **20.80** / frame 28.27。**SER 值 ~16ms**；四档对比 mode5 最优（mode1 21.27 / mode4 21.93 / mode6 20.98）⇒ 已无可调余量。
- **实例剔除**：保留盒（±160，`SUN_SHADOW_EXTENT_XY`）兜住视距内全部实例 ⇒ `culled=0`；ADSC 投射者视锥构造正常（`casterPlanes=8~10`）但无肉。**"剔实例"在 RT 里方向不对**：删掉的是所有射线共享的遮挡体，且结果冻结到下次几何重建 ⇒ 转头漏光。已加 `ORION_TLAS_CULL=5`（只清主射线 mask 位，物理上不可能漏光）作为安全底座。
- **NRC**：与 SHaRC 同结构天花板（1 跳无可截断路径），且需 tensor core/coopmat + 在线训练 + 偏差校正 ⇒ 不做。
- **面缓存探针**（`vptBakedFaceSample`，16³×6 面局部光辐照度）：工程**已试过并因画质关掉** —— `VPT_LOCAL_EXACT_DIRECT_ON_PRIMARY=false`（`:21382`），注释原文"这就是迁移要删掉的逐方块面量化"。
- **DDA 合并**：热路径上只有**一次** DDA（棱缝保守检查）+ 1 根硬件射线，二者互补不可合并；`VPT_VOXEL_PATH_MAX_STEPS=256` 永不触发（实际步数 = `min(段长, 30)`）；角判据 6 次浮点 vs 每步一次体素读 ⇒ 算术不是瓶颈。**1.15ms 是"棱缝不漏光"的价格**。
- **sdr（0.36ms）合并**：可合并"泛光合成→显示变换"（省 ~38MB 全屏读写）≈ **0.1ms**，需一次冷编 + 色彩顺序回归风险 ⇒ 不做。**顺手发现漏判**：`recordBloom` 没有"`postBloomStrength<=0` 就整条跳过"的宿主侧判据，mip 链照跑。

---

## 5. 死旋钮（设置界面上存在但几乎/完全不生效）

| 旋钮 | 实情 |
|---|---|
| 「太阳阴影采样」`sunShadowSamples` | `shadowSamples` **硬编码** `rtLodDistance<16 ? 2 : 1`（`:20232`），设置只能 `min()` 往下压 ⇒ 16 格外完全无效 |
| 「每像素采样数」`rtSpp=0.5` | 产生萤火虫 + 锯齿，不可用 |
| `rrMode=4` | = **ULTRA_PERFORMANCE**（707×376 线性 1/3），已是最低档，无更低 |
| `RR sharpness` | **RR 忽略锐化**（`rrOptions.sharpness=0`），没有内建补偿 |
| `rt_di_reproject.comp` / moments 双缓冲 | 建好但**从未 bind/dispatch**，ReSTIR DI 删除后的死件 |
| 射线计数（修复前） | 宿主全齐、着色器零写入 |

---

## 6. 尚未解决

1. **"糊"**（移动时更明显）：最可能是 1/3 内部分辨率本身（RR 不做锐化），零成本验证＝把 RR 档位从"超级性能"切"质量"；若质量档仍糊，才查时域对齐（MV/depth）。
2. **驱动侧崩溃**：`nvoglv64.dll` / `nvgpucomp64.dll`，都在**进世界那一刻**，我们的日志零错误、零 device-lost，捕获层没触发。今天出现 3 次，与改动无关（老 DLL 也复现过）。
3. `depthInverted` 硬编码 `eTrue`（`:62267`）未验证是否与实际投影一致（`primaryDepth = clipPos.z/clipPos.w`）。若将来排查 RR 重投影问题，先把这里做成可切换。

---

## 7. 复现脚本与环境开关

脚本（`.tmp/`）：
`launchPerf.bat`（裸基线）、`launchAblate*.bat`（`alt:` 阶梯）、`launchGiDist.bat`、`launchRayStats.bat`、`launchSharc.bat`、`launchSerLadder.bat`、`launchShadowCost.bat`。

开关：

```
ORION_PERF_TIMESTAMPS=1            分项计时（默认开）
ORION_PERF_ABLATION=alt:3,10,16    同局交错关特性；pin:N 钉住某档
ORION_RAY_STATS=1                  射线计数（需配 pin:15）
ORION_TLAS_CULL=-1|0..5            剔除档位（5 = 仅清主射线 mask）
ORION_TLAS_KEEPBOX=0|1             关/开 ±160 保留盒
ORION_TLAS_NEAR_GUARD=N            近场保底球半径（默认 24）
ORION_SUNSHADOW_FAR=1|0 / _AB=1    远场阴影强制 / 同局 A/B
ORION_PROBES_TERRAIN_REV=0         地形探针退回旧口径
ORION_GI_MAX_DISTANCE=NN [_AB=1]   GI 射线 tMax（**实测无效，且车道与 pomDepth 复用，建议删除**）
ORION_SHARC=1 [_AB/_SCALE/_ENTRIES/_UPDATE/_QUERY]   SHaRC（**实测无效**）
ORION_SER_LADDER=1                 SER 五档轮换
ORION_TEST_CRASH=1                 崩溃捕获层自检
```

所有改动均为 `.tmp/patch_*.py`（校验-备份-写入三段式），备份为 `*.bak_pre_<topic>_<时间戳>`。

---

## 8. 如果继续，建议顺序

1. **先解决"糊"**（零成本）：切 RR 档位对比；这决定当前画面是否"本来就这样"。
2. **`sdr` 的两处**：`recordBloom` 的宿主侧早退（零风险）+ 泛光合成并入显示（0.1ms，需冷编）。
3. 想要真实收益只能动画质或架构：
   - 次级命中材质降级（POM 已被否决）；
   - 真正的半分辨率 GI + 时空重建（需要历史缓冲 + 重投影 + 邻域钳紧，工程量大，且 RR 不替代这一步）；
   - 体素 DDA 改粗格/缩范围（漏光）。
4. **不要**再回头：SHaRC、NRC、面缓存直射、实例剔除、距离截断、棋盘半分辨率 —— 四条已证伪，两条已被工程自己否决。
