# MIS-A 实机验证报告

时间：2026-10-22 21:06:40 → 21:26:03（约 20 分钟）
判据对象：`GI_SKIP_SUN_DISK`（bit 14，GI 射线命中天空时不回传太阳日盘）

## 1. 结论

本轮运行**正常**：MIS-A 的 SPV 确实被实际运行，全程无 shader 编译失败、无 VK 设备丢失、无崩溃、正常退出。
日志层面看不到任何异常；画面效果不能由日志判定（见 §5）。

## 2. 证据

**时间线**
- 21:06:40 启动（Fabric 0.19.5 / MC 26.3）
- 21:06:51 `[RTX] shaders: set=shaders ... copied=0`（shader 目录已就位，无需复制）
- 21:23:22 `[RTX CONFIG]`，21:23:23 进入世界
- 21:25:59–21:26:02 `[FRAME BUDGET]` 仍在出帧
- 21:26:03 `Stopping!` — 正常退出

**错误扫描**（`run/logs/latest.log`，688,386 B；`debug.log` 910,810 B）
- 无 shader 编译失败、无 `VK_ERROR`、无 device lost、无 pipeline 创建失败
- 仅有的 ERROR 与渲染无关：登录 401、Realms JWT 解析、世界图标 Bad PNG、`Reflex mode=2` 恢复失败（历史已知）
- `run/crash-reports/` 最新一份为 16:50（本轮之前）；`run/screenshots/` 无本轮新图

**RT 管线确实在跑**
- `[SECTION COMPILE]` 多批 `n=256` 上传（avgMs 0.8–4.4）
- `[RTX-PROXY] proxy upload` 540 次
- `[FRAME BUDGET] frames=60 avgRenderMs=4.5–7.6ms renderPct 31–46%`

**部署核对（4 个 TU × 4 处，MD5 前 8 位）**

| SPV | 哈希 | cpp | run | src/resources | build/resources |
|---|---|---|---|---|---|
| rt_test.rgen | 00C31AA1 | ✓ | ✓ | ✓ | ✓ |
| rt_test.rmiss | 40F4401D | ✓ | ✓ | ✓ | ✓ |
| rt_test_ser.rgen | 652070D1 | ✓ | ✓ | ✓ | ✓ |
| rt_test_ser.rmiss | 40F4401D | ✓ | ✓ | ✓ | ✓ |
| rt_test_sharc_update.rgen | 590C234B | ✓ | ✓ | ✓ | ✓ |

`run/shaders` 写入时间 21:01:10 < 启动 21:06:40 ⇒ **本轮跑的就是含 MIS-A 的版本**。
（`build/resources` 21:06:38 是构建期再复制，哈希相同。）

## 3. 代码自洽复核

- **bit 位不冲突**：purpose 0–2、hitClass 3–4、waterEvent 5–6、glass 7–8、profile 9–11、clearGlass 12；bit 13 空、**bit 14 可用**。`payload.state` 为 uint32，位宽足够。
- **三个 rgen 变体一次覆盖**：`rt_test_ser.rgen`、`rt_test_sharc_update.rgen` 与 `rt_test.rgen` 是同一源文件（`cpp/shaders/rt_test.rgen` 是唯一的 rgen 源）+ 不同宏，`#if GI_SKIP_SUN_DISK` 自动进入三条路径。
- **SER 路径不丢标记**：`traceDiffuseSer` → `serTraceRay` 只封装 `traceRayEXT`，不重写 `payload.state`。
- **rmiss 只压日盘**：`rt_test.rmiss:998-1004` 在 `rainCelestial` / `endEclipse` 调制之后、与 `sunGlow` 合成之前把 `sunDisk` 清零 ⇒ pow64 日冕瓣（±8°，非 delta，NEE 覆盖不到）完整保留。

## 4. 已知缺口（读码发现，尚未修）

`rahit`（透明/染色玻璃）会用 `makeHitState()` 重建 `payload.state`（`material_common.glsl:39-55`，只保留 purpose + 分类位），**bit 14 被丢弃**。
⇒ GI 射线**穿过玻璃后**打中天空时，日盘又被计入。

影响面：仅"透过玻璃看到的 GI 天空"，出现概率低但确实存在。
补法二选一：
1. 在两个 rahit（`rt_test.rahit`、`rt_test_translucent.rahit`）重建 state 处保留该位（`| (payload.state & RT_STATE_NO_SUN_DISK_BIT)`），重编 2 TU × 4 处部署；
2. rgen 侧不复用 `state`，改用一个独立 payload 标志（改动面更大，会变 payload 大小）。

## 5. 需要人眼确认的三条判据

1. 静止时"孤立、夹不掉的亮点"（尤其在地面/墙面阴影里）是否明显减少或消失？
2. 整体亮度有没有下降？重点看室外直照区、室内靠天光的区域——若明显变暗，说明 NEE 对太阳的覆盖不足，需要回看 NEE 的软阴影射线数 / 可见性项。
3. 玻璃后的天空里是否还能看到太阳亮点（对应 §4 的缺口）。

## 6. A/B 回退方法

改 `cpp/shaders/rt_test.rgen:6372` 与 `cpp/shaders/rt_test.rmiss:62`：

```glsl
#define GI_SKIP_SUN_DISK 0
```

（或编译时加 `-DGI_SKIP_SUN_DISK=0`；**两处必须同值**，rmiss 是独立 TU，宏不跨文件传递）

然后重编 `rt_test.rgen` / `rt_test.rmiss` / `rt_test_ser.rgen` / `rt_test_ser.rmiss` / `rt_test_sharc_update.rgen` 五个 SPV，四处部署（`cpp/shaders`、`run/shaders`、`src/main/resources/shaders`、`build/resources/main/shaders`）。
