# 半透明玻璃块贴地放置时，底边出现天空色亮线

> 你是外部顾问，**无法访问代码仓库或这台机器**。本文档是唯一上下文，所有必要代码已贴入。
> 请直接基于本文回答，不需要索要文件。

## 1. 现象

- Vulkan 路径追踪的 Minecraft 渲染模组（1 spp + DLSS-RR + NRD）。
- 玻璃块（或冰、粘液等半透明体）**放在地面上**时，它与地面接触的那条底边会出现一条**天空色的亮线**。
- 悬空放置（下方是空气）时**没有**这条线。
- 该现象与玻璃边框模糊是不同问题，历史上有过一次修复（见第 3 节），但观察仍在。

## 2. 当前执行的代码路径（已确认）

玻璃走电介质重写档（`VPT_GLASS_DIELECTRIC_REWRITE = 1`），真实遍历档（`VPT_GLASS_REAL_TRAVERSAL = 1`）。

真实遍历档的核心约定是"**不做位移**，只做 2ε 偏移，折射只改方向"：

```glsl
// 入射界面：起点 = 真实命中点沿行进方向推 ε
travelOrigin += travelDirection * 0.002;

hit.hitDistance = 1.0;
traceRayEXT(
    topLevelAS,
    gl_RayFlagsNoneEXT,
    0xff, 0, 0, 0,
    travelOrigin,
    0.01,              // ← tmin
    travelDirection,
    10000.0,
    0
);

// 命中玻璃面时：
const bool enteringGlassFace = dot(faceNormal, travelDirection) < 0.0;
if(dot(faceNormal, travelDirection) > 0.0)
{
    faceNormal = -faceNormal;   // 翻转后：faceNormal 恒逆着行进方向
}

// 共面重复面（相邻玻璃块的内部面）：距离几乎为 0，直接穿过去
if(hit.hitDistance < 0.02)
{
    travelOrigin = hitPosition + travelDirection * 0.002;
    continue;
}

// 真实遍历：出射界面不位移，沿"离开该面"的方向推 ε
// faceNormal 已翻成逆着行进方向 ⇒ 出射侧是 -faceNormal
travelOrigin = hitPosition - faceNormal * 0.002;
continue;
```

## 3. 已排除的原因

| 可疑点 | 状态 | 证据 |
|---|---|---|
| `GI_GLASS_WALK` 折射弯折把透射射线折向面法线 | **已修复** | 弯折块条件已改为 `#if GI_GLASS_WALK && !VPT_GLASS_DIELECTRIC_REWRITE`，重写档下整段不编译 |
| 薄壁位移（`min(slabThickness/cos, 0.15)`）把起点推过薄结构 | **未执行** | 该分支在 `#if !VPT_GLASS_REAL_TRAVERSAL` 内，当前档位编译不进去 |
| 清玻璃"当空气"早退 | **未执行** | `GI_CLEAR_GLASS_AS_AIR = 0`，清玻璃走完整电介质 |

## 4. 我的机制推断（需要你判定或纠正）

玻璃块贴地时，其**底面外法线朝下**，而"玻璃外侧"恰好是**地面实心方块的内部**：

1. 视线从侧面进入玻璃，在玻璃内部向下行进；
2. 命中玻璃底面（从内部看是出射界面）；
3. `faceNormal` 被翻成"逆着行进方向"= 朝上，于是 `-faceNormal` 朝下；
4. `travelOrigin = hitPosition - faceNormal * 0.002` ⇒ 起点落在 **地面内部 y−0.002**；
5. `tmin = 0.01` 让射线先空走 1/100 格才检测，而地面顶面在起点**身后**（共面，t≈0）⇒ 永远打不到；
6. 射线在地面内部继续向下，穿出地面底部 ⇒ 无命中 ⇒ miss shader 返回天空辐射。

悬空玻璃没有这个问题，因为底面外侧是空气，ε 推向空气侧后射线正常打到下方地面。

## 5. 我考虑过的修法及顾虑（请评估或给更好的）

**A. 缩小 tmin / ε**
把 `tmin` 从 0.01 降到 1e-4。但起点已经在实心体内部，射线只会越走越深，降低 tmin 不改变"顶面在身后"这一事实。**判断无效**。

**B. 出射 ε 反向（推回玻璃内侧）**
`travelOrigin = hitPosition + travelDirection * 0.002` 的反向，即起点回到 y+0.002（玻璃内）。但这样会再次命中同一出射面（距离 0.002 < 0.02 触发共面跳过），跳过时又推回外侧 ⇒ 可能来回振荡直到 8 次界面预算耗尽。

**C. 出射点检测外侧介质**
出射时查询"该点外侧是否实心方块"，若实心则**不穿出**，直接把该实心方块的表面作为接收面着色。
顾虑：需要一次体素查询或极短射线，成本与实现复杂度；且要处理"外侧是另一块玻璃/水"的情况。

**D. 允许负距离命中**
把出射起点放在玻璃内侧，并让射线能命中身后共面的面。Vulkan 规范不允许负 tmin。

## 6. 请回答的问题

1. 上述第 4 节的机制推断是否成立？如果成立，说明这就是"玻璃与不透明固体共面贴合"的经典问题，你们（Remix / 参考实现）是怎么处理的？
2. **业界标准做法**：光线从介质内部命中与不透明固体共面的界面时，正确的做法是终止传输并着色该固体表面，还是做某种偏移？有没有可引用的实现（dxvk-remix 的 `trace_transport` / `geometry_resolver` 具体分支）？
3. 如果采用方案 C，判断"外侧是否实心"用哪种方式代价最小：体素 DDA、极短射线（`tmin=0, tmax=ε`）、还是查询已有的 section 数据？
4. 是否应该区分"玻璃底面与实心共面"和"玻璃底面贴另一块玻璃"两种情况？后者应该继续穿（能看到下方玻璃后方的内容）。
5. 有没有办法在不新增查询的前提下，仅靠调整 `tmin`/ε/面朝向判定就彻底解决？如果有，请给出具体数值与判定条件。

## 7. 环境

| 项 | 值 |
|---|---|
| OS | Windows 11 26200 |
| GPU | NVIDIA GeForce RTX 4070 Laptop |
| 驱动 | 32.0.16.1742 |
| API | Vulkan 1.2 + VK_KHR_ray_tracing，`traceRayEXT` tmin/tmax |
| 采样 | 1 spp / 帧 |
| 几何 | Minecraft 方块世界，玻璃块 1×1×1，板厚 1/16 格；贴地时底面与地面顶面共面 |
| 相关宏 | `VPT_GLASS_DIELECTRIC_REWRITE=1`、`VPT_GLASS_REAL_TRAVERSAL=1`、`GI_CLEAR_GLASS_AS_AIR=0` |
