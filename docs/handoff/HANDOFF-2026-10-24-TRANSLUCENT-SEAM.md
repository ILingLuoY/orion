# HANDOFF 2026-10-24：半透明体贴不透明方块的接缝亮线（机制已确认：面序奇偶错位 ⇒ 下游少乘一次 filter）

> 交接给新会话。本文所有锚点都是实际读过的代码行；没有验证过的东西都标了「未验证」。
>
> **修订记录**
> - 2026-10-24 晚（对 13:52 构建重新核对）：行号、§7 产物指纹、§6 第 4 项入口数全部重测（旧指纹记的是 `run/` 根目录的**孤儿副本**，不是基线）。
> - 2026-10-24 晚（用户确认机制后）：§2 定为**已确认**，§2.1 改为"确认状态 + 残线退路"；新增 **§5.1 两处待改代码逐字原文**、**§5.2 三条硬约束**（`parallaxRayTravel` 双用途 / 绕序不可靠 / 重编范围）；**§9 改成整段可粘贴的开工包**。

## 1. 现象

冰（以及同类实心半透明体）与**不透明方块贴合的那一侧**，接缝处有一条**一像素宽的亮线**。

用户已确认的判据：

- 只在"贴着不透明方块"的那一侧出现；
- 冰**悬空**的边缘没有这条线。

能排除的：

- 不是本轮"二次射线起点偏移方向"那两批改动（那些只动反射/GI/可见性**次级**射线；相机主射线的起点就是相机位置本身 `rt_test.rgen:18202`，`traceRayEXT` 在 `:18258`，偏移只有 `tmin = 0.001`，未动）；
- 不是玻璃 albedo 播种那条（那条的症状是**静止时噪点越积越多**，且移动即消失，与本条形态不同）。

## 2. 根因（**已由用户确认**）

半透明可见性/染色走的是**面序奇偶**模型：沿射线累计"被接受的半透明面"个数，取偶数为"出射面"，只在该面乘一次 filter。

```698:702:cpp/shaders/rt_test.rahit
        bool isLightExitFace =
            (glassFaceOrdinal & 1u) == 0u;

        payload.parallaxRayTravel =
            float(glassFaceOrdinal + 1u);
```

`rt_test_translucent.rahit:873-877` 是完全相同的副本。`glassFaceOrdinal` 来自
`floor(payload.parallaxRayTravel + 0.5)`，赋值语句就在上面几行（`rt_test.rahit` 里该表达式结束于 `:694-696`，
`rt_test_translucent.rahit:865-871` 是完整形态）。

模型的前提是 rahit 注释写的那句「one block adds exactly one filter」，即**一个半透明体必然被射线穿过 2 个面**（进 + 出）。

**Minecraft 会把半透明体与不透明方块之间那一面裁掉。** 于是接缝处的射线路径变成：

```
空气 → [冰的进入面] → 石头        ← 只穿过 1 个半透明面
```

面数由 2 变 1 ⇒ 累计奇偶从这一点起**整体翻转** ⇒ 同一条射线上**下游**的所有玻璃/冰的 `isLightExitFace` 判反 ⇒ 本该乘 filter 的面被跳过。

**为什么表现为「亮」**：filter 是乘法

```743:748:cpp/shaders/rt_test.rahit
            payload.albedo *=
                glassSheetTransmissionScalarF(
                    glassTint,
                    glassCosTheta
                ) *
```

只能让结果变暗。「少乘一次」才会变亮 ⇒ 亮线来自**下游丢失 filter**，不是来自接缝处多乘或错乘。

同类实心半透明体都在同一条管线上：`glass_dielectric_common.glsl:43` 的 `GLASS_ICE_IOR = 1.309`、`:89` 的 `transparentProfile == 2` 就是冰；染色玻璃 / 粘液 / 蜂蜜同理（`glass_tint_common.glsl:348` 的 "dense-body profiles (tinted glass / ice / slime / honey)"）。所以它们理论上都有同一条线，冰最显眼。

### 2.1 确认状态与遗留检查

**2026-10-24 用户确认**：机制就是「面序奇偶在贴不透明方块时错位，后果是下游丢失一次 filter」，因此表现为**变亮**的线（`payload.albedo *= surfaceTransmission` 只能变暗，亮线只可能来自"少乘一次"）。§4 按此实现即可，不必再先做判别实验。

保留一条退路（不是前置条件）：若 §4 落地后**仍有残线**，再回来看 `rt_test.rgen:3056-3068` 那段 2026-09-25 的复盘 —— 它用"缝"描述过同一类现象，成因是 `hitPos + N*0.035` 把次级射线起点推进相邻方块。本轮第 5 项已改掉 26 处，但 **VPT 本地光弹射的两个起点仍是 `hitPos + N * 0.035`**（`:3537`、`:6813`，属 §6"有意保留"）。判据：残线**只在火把/萤石照明处**出现 ⇒ 是这条路径，不是面序。

## 3. 为什么不采用"正反面判定"（看似最直接的修法）

直觉修法是把 `isLightExitFace` 换成 `gl_HitTriangleIsFrontFacing`（或带符号的
`dot(geometricNormal, gl_WorldRayDirectionEXT)`）。**在本仓库不安全**，证据是现有代码自己就避开了绕序：

```736:741:cpp/shaders/rt_test.rahit
            float glassCosTheta =
                clamp(
                    abs(dot(geometricNormal, gl_WorldRayDirectionEXT)),
                    0.0,
                    1.0
                );
```

用 `abs()` 而不是带符号的 `dot`，说明**绕序/法线朝向在这个引擎里不可靠**（水面、DH 地形、Orion 边界片各有各的绕序）。一旦朝向判反，可能是**整片玻璃染色直接消失**，比一条亮线严重得多。

## 4. 推荐修法：只在"断开处"重置面序

保留奇偶逻辑，但让**每个连续的半透明区域各自从 0 开始计数**：

1. `cpp/shaders/hit_payload.glsl` 增加一个 float（建议 `float lastGlassFaceT;`），记录上一个被接受半透明面的 `gl_HitTEXT`；
2. 两个 rahit 的 `isLightExitFace` 计算处，先做：

   ```glsl
   // 面距的三种情形（沿着同一根射线量）：
   //   同一个方块体的进/出面            = 1.0 格（薄片更小）
   //   相邻两格玻璃之间那一面被裁掉      = 2.0 格
   //   中间隔 N 格空气的下一个玻璃体     = N 格（N=1 时仍是 1.0）
   // 取 1.5 ⇒ 只把"整格被裁掉"当成新区域，不会切开 1 格空气的两段。
   if(payload.lastGlassFaceT >= 0.0 &&
      gl_HitTEXT - payload.lastGlassFaceT > 1.5)
   {
       glassFaceOrdinal = 0u;
   }

   payload.lastGlassFaceT = gl_HitTEXT;
   ```

   **副作用要预先知道**：面距 2.0 只可能来自"内部面被裁掉"，所以这个阈值同时会给**相邻同种玻璃的每一格各补一次 filter**（原来 2 格只有 1 次）。这既是 A 假设想要的修复，也会**整体压暗多层玻璃/冰**——验证判据第 3 条就是盯这个。

   然后再照原样算 `isLightExitFace = (glassFaceOrdinal & 1u) == 0u;` 并递增
   `payload.parallaxRayTravel`；
3. `payload.lastGlassFaceT` 的初值必须在射线发出前播种成负数（例如 `-1.0`）。

**注意**：`payload.parallaxRayTravel` 同时承担**两个**用途 —— 面序**和**穿层预算
（`rt_test_translucent.rahit:998` 的 `glassFaceOrdinal + 1u >= uint(GI_GLASS_VISIBILITY_MAX_FACES)`）。
重置面序时**不要**顺手把预算计数也清掉，否则穿层上限会失效。建议把两者拆成两个字段，
或预算另用一个独立计数器。

### 预期效果

| 情形 | 现状 | 修后 |
|---|---|---|
| 单格冰悬空（进/出面距 1.0） | 1 个 filter | **不变** |
| 冰贴不透明方块（穿 1 面） | 该体 1 个 filter，但下游奇偶全反 | 该体 1 个 filter，**下游正常** ⇒ 亮线消失 |
| 中间隔 1 格空气的下一个玻璃体（面距 1.0） | 同一序列 | **不变**（1.5 阈值不会切开） |
| 相邻两格同种玻璃（内部面被裁，面距 **2.0**） | 2 格只乘 1 次 | **每格各乘 1 次** ⇒ 略暗，但更符合「一片一滤」 |

不依赖绕序，不动 `abs(dot(...))` 那套，改动面最小。

> 面距口径再确认一次（这是阈值能不能用对的关键）：**同一个体的进/出面 = 1.0 格**；**相邻两格玻璃之间那一面被裁掉后 = 2.0 格**；**中间隔 N 格空气 = N 格**（N=1 时仍是 1.0）。所以 1.5 切的是"整格被裁掉"，不是"跨空气"。

## 5. 落地清单

| 项 | 内容 |
|---|---|
| 新增字段 | `cpp/shaders/hit_payload.glsl`（当前 `parallaxRayTravel` 在 `:25`） |
| 修改点 | `cpp/shaders/rt_test.rahit:690-702`、`cpp/shaders/rt_test_translucent.rahit:865-877`（原文见 §5.1） |
| 播种点 | **6 处** `makeTraceState(RT_PURPOSE_GLASS_VISIBILITY)`，全部已在 §6 第 4 项补过 `hit.albedo`：`rt_test.rgen:6472`、`:11778`、`:12586`、`:20227`、`:20379`、`:20488`。新字段的播种与这 6 处同一批位置（`parallaxRayTravel = 0.0` 就在旁边） |
| 重编 | payload 结构变了 ⇒ **所有引用 payload 的 TU 都要重编**，不止 3 个 rgen。`python _deploy_stale_spv.py` 会按 mtime 自动找出全部过期产物 |
| 部署 | `cpp/shaders`、`run/shaders`、`src/main/resources/shaders`、`build/resources/main/shaders` 四处（脚本只覆盖 `run/shaders`，其余手 `Copy-Item`）。**平铺的 `run/*.spv` 是旧档孤儿，不要覆盖它**（见 §7）；它是否仍被加载未验证，别拿它当基线 |

### 5.1 两处待改代码的原文（逐字，改前先核对）

`cpp/shaders/rt_test.rahit:690-702`：

```glsl
        uint glassFaceOrdinal =
            uint(
                max(
                    floor(payload.parallaxRayTravel + 0.5),
                    0.0
                )
            );

        bool isLightExitFace =
            (glassFaceOrdinal & 1u) == 0u;

        payload.parallaxRayTravel =
            float(glassFaceOrdinal + 1u);
```

`cpp/shaders/rt_test_translucent.rahit:865-877` 是**结构完全相同**的副本（逐字符一致，仅缩进层级相同、行号不同）。上面的注释块也各有一句 `one block adds exactly one filter`（`rt_test.rahit:688-689`、`rt_test_translucent.rahit:863-864`），那句话就是被裁面打破的前提。

payload 声明是**唯一一处**（`hit_payload.glsl:17-32`，当前 20 dwords / 80 B）；`cpp/VulkanCore.cpp` 与 `cpp/*.h` 里**没有** `HitPayload` 镜像（grep 无命中）⇒ 这是**纯着色器改动**，加一个 `float` 变 84 B，宿主侧不用动、也没有 ABI 风险。加在 `prevOffsetZ` 之后即可（`prevOffset*` 是运动矢量用的，别插中间）。

### 5.2 三条硬约束（新会话务必先读）

1. **`payload.parallaxRayTravel` 是双用途**：面序（本节的奇偶）+ 穿层预算（`GI_GLASS_VISIBILITY_MAX_FACES`，判据在 `rt_test.rahit:777` / `rt_test_translucent.rahit:998`：`glassFaceOrdinal + 1u >= uint(GI_GLASS_VISIBILITY_MAX_FACES)`，16 面 = 8 层）。**重置面序时不要连预算一起清零**，否则穿层上限失效。最稳的写法是给预算单独一个计数器，或让 `parallaxRayTravel` 继续单调递增、只在算 `isLightExitFace` 时用"区域内计数"。
2. **绕序不可靠，不要用正反面判据**：现有代码用 `abs(dot(geometricNormal, gl_WorldRayDirectionEXT))`（`rt_test.rahit:736-741`）而不是带符号点积；换成 `gl_HitTriangleIsFrontFacing` 一旦判反就是**整片玻璃染色消失**。详见 §3。
3. **payload 结构变了 ⇒ 所有引用 payload 的 TU 都要重编**，不止 3 个 rgen（`parallaxRayTravel` 现有 26 个消费者：`rt_test.rgen` 21、每个 rahit 2、`rt_test.rmiss` 1）。`python _deploy_stale_spv.py` 按 mtime 自动覆盖，然后按 §7 同步四处。

### 验证判据

1. 冰 / 染色玻璃贴不透明方块的**接缝亮线消失**；
2. **单格**冰悬空（进/出面距 1.0）的边缘**不应有任何变化**（这部分逻辑没动）；
3. **单层**染色玻璃的透射色**不应整体变亮或变暗**（若变亮 ⇒ 重置把某处 filter 弄丢了；若变暗 ⇒ 阈值在单层上误切了）；
4. **多层**（≥2 格相邻）玻璃/冰按 §4 预期会**略暗**（每格各补一次 filter），这是"一片一滤"的预期结果，不要当成回归；但它必须**均匀**变暗，不能出现新的接缝线；
5. 穿层上限仍生效：`GI_GLASS_VISIBILITY_MAX_FACES` 打满时应有对应行为，不能被重置绕过（§5.2 第 1 条）。

## 6. 本轮已落地、可作为基线的改动（避免重复排查）

顺序即落地顺序，全部已编译/部署并同步五处。

| # | 改动 | 关键锚点 |
|---|---|---|
| 1 | **Reflex 断链根因**：`virtualKey` 由 `0x78u`(VK_F9，非法值) 改 `0u` | `cpp/VulkanCore.cpp` 的 `options.virtualKey`；官方 `sl_reflex.h:55` 只允许 F13/F14/F15。修前 `[REFLEX] SetOptions FAILED result=25`（`eErrorInvalidParameter`）、DLSS-G `status=2`；修后 `status=0 × 158`、`baseLimitHz=82` |
| 2 | **MV 总开关**：`cameraMotionIncluded` 跟随 `g_motionVectorsEnabled`（关掉时 `eFalse`，让 SL 用 depth 自补相机运动）；开关翻转纳入 DLSS reset 触发面；Java 侧恢复路径下发 + 设置界面加控件 | `constants.cameraMotionIncluded`、`RTXSettingsScreen` 的 `pendingMotionVectors` |
| 3 | **天空（miss）MV**：miss 分支按**无穷远点**投影（齐次方向 `w=0`），方向用 `pixel+0.5` **去抖** | `rt_test.rgen` 主命中 `if` 后新增的 `else` 分支 |
| 4 | **玻璃可见性 albedo 播种**：把 `RT_PURPOSE_GLASS_VISIBILITY` 的**全部 6 个**入口都补上 `hit.albedo = vec3(1.0)`（10-21 只补了主射线与 1 处）；现网 6 处均已播种：`rt_test.rgen:6472 / 11778 / 12586 / 20227 / 20379 / 20488` | 见 `HANDOFF-2026-10-21-VERTEX-32B-TERRAIN.md` §8 的「未覆盖」 |
| 5 | **二次射线起点偏移方向**（26 处）：`pos + N*0.035` → `pos + dir*0.001`，`tmin` 全部原样保留 | 依据 `rt_test.rgen:3056-3068` 的复盘「**方向**才是漏光的决定因素」（原话："thin bright lines along block edges"） |
| 6 | **VLI 分帧预算**（`ORION_VLI_BUDGET_MS` / `ORION_VLI_SECTIONS_PER_FRAME`）、gather 扁平 arena（消掉每页 3 次 16KB 堆分配）、`[RT VLI SLOW]`/`[VLI WORKER]` 分项计时 | `recordVLIUpdates`、`uploadVoxelLightSection` |

批次 5 的 26 处分组：玻璃可见性 2、玻璃反射 4、玻璃反射命中点重建 1、本地光二级弹射可见性 2、玻璃 GI 4、
水面反射 4、水面反射命中点重建 1、水面→太阳 2、通用太阳可见性 2、水体太阳透射 1、RTAO 1、水下 GI walker 2。

**有意保留 `0.035` 的位置**（不要"顺手统一"）：

| 位置 | 原因 |
|---|---|
| `rt_test.rgen:3537`、`6813` | 体素 DDA 起点（`:3553` 有 VPT-P3 的"起点嵌入检测"说明；`:6813` 是 `vptLoadedPathToCandidateEmitter` 的调用方），偏移目的是**离开当前 voxel**，沿射线偏起不到这个作用 |
| `rt_test.rgen:8320`、`14964`、`14971` | 云透射率（`c3CloudSunTransmittance`）的**采样点**，不是射线起点 |
| `rt_test.rgen:24162` 那段注释 | **已过时**（还写着"RTAO 的射线从 0.035 格偏移起步"，实际已被第 5 项改成 `dir*0.001`），下次改该文件时顺手更新；注释不影响 SPV |

> 行号口径：以上都是**当前 24,670 行**的 `cpp/shaders/rt_test.rgen`。13:51 的 GI 距离截断补丁（`historyPadding0`）在本文件增删过行，所以旧会话记录的 `3517 / 6812 / 8325 / 15009 / 15016 / 24417` 已整体漂移，**引用前按符号名重新定位**（`vptLoadedPathToCandidateEmitter`、`c3CloudSunTransmittance`），不要按行号硬跳。

## 7. 当前产物指纹（基线，2026-10-24 13:52 构建）

`cpp/shaders` / `run/shaders` / `src/main/resources/shaders` / `build/resources/main/shaders` **四处逐字节一致**：

| 产物 | 字节数 | SHA256 前 12 |
|---|---|---|
| `rt_test.rgen.spv` | 561060 | `DBD29E886675` |
| `rt_test_ser.rgen.spv` | 573284 | `F9D002768688` |
| `rt_test_sharc_update.rgen.spv` | 576496 | `D9F53533361A` |
| `rtx_backend.dll` | 5826048 | `C3D9F71C0729D937`（四处一致：`run/`、`cpp/build/Release/`、`src/main/resources/natives/windows/`、`build/resources/main/natives/windows/`） |

**⚠ 陷阱：`run/` 根目录还平铺着一份旧 SPV**（`run/rt_test.rgen.spv` 560260 / `E66B30240614`、`run/rt_test_ser.rgen.spv` 572420 / `F981DBC94BFB`），比 `run/shaders/` 里的旧一档；旧会话记录的指纹就是这份**孤儿副本**，不是基线。取指纹一律以 `run/shaders/` 为准。

rahit 仍是 10-23 23:08 的构建（源 `rt_test.rahit` / `rt_test_translucent.rahit` = 10-23 21:59、`hit_payload.glsl` = 10-23 22:00 ⇒ 相对源码是新鲜的）：`rt_test.rahit.spv` 42520 / `08C9C8560132`、`rt_test_translucent.rahit.spv` 36664 / `A1F5A335DA51`。**本轮改 §4 会直接改变这两个值。**

**rgen 的哈希已经会被 GI 距离截断那条改动影响**（`historyPadding0` 车道 + `perfGiTraceMaxDistance()`，见 `.tmp/patch_gi_distance_shader.py`）。新会话改完 §4 后看到 rgen 哈希与上表不同是必然的，不要误判为"产物没更新"。

编译口径（必须与 `tools/audit_spv_freshness.py` 的 `BASE_FLAGS` 逐项一致，`_deploy_stale_spv.py:25` 同值）：

```
-DRT_BUILD_PRODUCTION_MODE -DSECTION_VERTEX_32B=1
```

重编 + 部署：

```powershell
python _deploy_stale_spv.py          # 只覆盖 run/shaders，其余三处要手 Copy-Item
```

## 8. 操作约束（本仓库踩过的坑，务必遵守）

1. **`cpp/VulkanCore.cpp` 是 ~2.6MB / 6.8 万行的单 TU，禁止用文件编辑器改** —— 已发生 3 次静默损坏（截断、整段函数消失、C1075）。一律写 Python 补丁脚本：每条 `(name, old, new)` 先 `count == 1`，全部通过再一次性 `io.open(..., newline='')` 写回，脚本放 `.tmp/`。
2. 改完必须编译验证 + grep 关键标识符确认插入存在，再部署四处（DLL）/ 五处（shader）。
3. 游戏运行时 DLL 被占用，`Copy-Item` 会失败；等进程退出再拷。
4. 诊断顺序：**先读源码**，禁止用"可能是驱动/世界/环境"代替阅读。本仓库已有两次因此浪费数轮启动（RT payload 未播种、Reflex 非法参数）。
5. 本仓库反复出现的模式是**「问题诊断过、修法验证过，但只覆盖了一部分」**（玻璃 albedo 播种、偏移方向、Reflex、VLI 都属此类）。改一处时务必把所有同类入口列全。

## 9. 新会话开场（整段可直接粘贴）

> 读 `HANDOFF-2026-10-24-TRANSLUCENT-SEAM.md`，按 §4/§5 实现"面序在断开处重置"。背景一句话：半透明体贴不透明方块时 Minecraft 裁掉了那一面 ⇒ 该体只贡献 1 个面（奇数）⇒ 整条射线从这点起奇偶翻转 ⇒ 下游丢一次 filter ⇒ 一像素亮的线（`payload.albedo *= surfaceTransmission` 只可能变暗，所以亮线只能是"少乘"）。机制已由用户确认，不用再做判别实验。
>
> 两处待改代码（原文见 §5.1，逐字核对后再动手）：
> - `cpp/shaders/rt_test.rahit:690-702`
> - `cpp/shaders/rt_test_translucent.rahit:865-877`（结构完全相同的副本）
>
> 三条硬约束（§5.2）：
> 1. `payload.parallaxRayTravel` 同时是**面序**和**穿层预算**（`GI_GLASS_VISIBILITY_MAX_FACES`，`rahit:777` / `translucent.rahit:998`，16 面 = 8 层）⇒ 重置面序不能连预算一起清零；
> 2. **绕序不可靠**，禁止用 `gl_HitTriangleIsFrontFacing` 代替奇偶（现有代码用 `abs(dot(...))` 就是证据，见 §3）；
> 3. payload 结构变了 ⇒ 所有引用 payload 的 TU 都要重编（`parallaxRayTravel` 有 26 个消费者），然后按 §7 同步四处 + 核对指纹。
>
> 动手顺序：先读 `hit_payload.glsl` 全文与两个 rahit 的可见性分支（`rt_test.rahit:675-790`、`rt_test_translucent.rahit:855-1010`）→ 确认 `lastGlassFaceT` 的播种点（6 处 `makeTraceState(RT_PURPOSE_GLASS_VISIBILITY)`，§5 表）→ 改 → 重编 → 部署 → 按 §5 的 5 条判据验收。
