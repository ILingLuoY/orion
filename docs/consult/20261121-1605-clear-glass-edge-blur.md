# 清玻璃边框模糊 + 噪点：RT 玻璃 alpha-blend 方案诊断

> 你是外部顾问，**无法访问代码仓库、本地文件或这台机器**。本文件是唯一上下文，所有必要代码已贴入。
> 请直接基于本文回答，不需要索要文件。

## 1. 现象

- 平台：Windows 11，RTX 4070 Laptop，驱动 32.0.16.1742，Vulkan 1.2 路径追踪。
- Minecraft 渲染模组：每像素 **1 spp** 路径追踪 + DLSS-RR + NRD 降噪。
- 问题：**透明清玻璃（clear glass）方块的贴图边框**（贴图最外圈 alpha=1 的细线）看起来 **模糊、发虚，且带颗粒噪点**。
- 关键条件：**使用原版 16×16 玻璃贴图时明显**；换高分辨率材质包（32×~128×）后问题明显减轻或消失。
- 中心区域（贴图 alpha≈0.4 的部分）观感正常，问题集中在"边框"这 1 texel 宽的细线。

## 2. 渲染顺序（相关部分）

```
raygen(1spp)：写 rtOutput 颜色 / depth / guide / 玻璃 G-buffer(73,74,77 号图)
        ↓
glass_alpha_blend.comp（compute，全屏）：读玻璃 G-buffer + 背景色，就地合成到 rtOutput
        ↓
glass_alpha_finish.comp（compute，全屏）：最终混合写回 rtOutput
        ↓
NRD / DLSS-RR（时间降噪）→ 后处理
```

注意：**玻璃合成发生在降噪之前**，因此玻璃像素随后会进入 RR 的时间累积与空间滤波。

## 3. 关键代码

### 3.1 玻璃不透明度常量（钉死，不看贴图 alpha）

```glsl
// 玻璃源 alpha 上限。注释写"0.6 = 边框最多 60% 不透明"，
// 但实际值是 0.3（疑似后来为压侧面色块调过、未回退）。
#ifndef VPT_GLASS_ALPHA_BLEND_MAX_ALPHA
#define VPT_GLASS_ALPHA_BLEND_MAX_ALPHA 0.3
#endif

// 玻璃族固定不透明度：贴图 alpha 只贡献颜色，不参与透明度。
#ifndef GLASS_FIXED_ALPHA
#define GLASS_FIXED_ALPHA 0.3
#endif

// 把 a 直接钉成 GLASS_FIXED_ALPHA，完全不读 transmissionWeight
#ifndef VPT_GLASS_ALPHA_BLEND_CONST_ALPHA
#define VPT_GLASS_ALPHA_BLEND_CONST_ALPHA 1
#endif
#if VPT_GLASS_ALPHA_BLEND_CONST_ALPHA
    const float blendAlpha0 = GLASS_FIXED_ALPHA;   // = 0.3
#endif
```

原版 glass 贴图：边框 texel alpha = 1.0，中心 texel alpha ≈ 0.4。

### 3.2 blend pass 的合成项

```glsl
// surface = 玻璃层累积颜色（rgb）+ 累积 alpha（a）
const vec3 alphaBlendColor =
    max(surface.rgb, vec3(0.0)) * surface.a * surfaceLight;

imageStore(alphaBlendRadiance, pixel, vec4(max(alphaBlendColor, vec3(0.0)), 1.0));
```

### 3.3 finish pass 的最终混合

```glsl
vec3 radianceOutput = max(imageLoad(sceneColor, pixel).rgb, vec3(0.0)); // 背景
const vec3 alphaBlendOutput = imageLoad(alphaBlendRadiance, pixel).rgb;

// meta.x 存的是"背景透射率"(= 1 - a)，尽管变量名叫 backgroundAlpha
const float backgroundAlpha = clamp(meta.x, 0.0, 1.0);

imageStore(
    sceneColor,
    pixel,
    vec4(max(radianceOutput * backgroundAlpha + alphaBlendOutput, vec3(0.0)), 1.0));
```

即最终结果是：`background × (1-a) + glassTint × a × surfaceLight`，其中 **a 恒为 0.3**。

### 3.4 原先的 5×5 空间平滑（我已改成双边，尚未实测）

```glsl
// 原实现：无边界的高斯 5x5 平均，只跳过"不是玻璃像素"的邻居
for(int dy = -2; dy <= 2; ++dy)
for(int dx = -2; dx <= 2; ++dx)
{
    const ivec2 q = clamp(pixel + ivec2(dx, dy), ivec2(0), extent - ivec2(1));
    if(imageLoad(alphaBlendMeta, q).y <= 0.0) continue;   // .y = 玻璃面到相机距离
    const float w = 1.0 / (1.0 + float(dx * dx + dy * dy));
    albedoSum += imageLoad(alphaBlendGbuffer, q) * w;
    albedoWeight += w;
}
surface = albedoSum / albedoWeight;
```

另有一段同样结构的 5×5，对"体积直接光 + 背景亮度"做平均。

## 4. 我目前的判断（可能有错，请纠正）

1. **边框被稀释**：a=0.3 意味着边框颜色只有 30% 权重，70% 是背景 → 白边对比度天然被压掉，视觉上像"糊"。
2. **5×5 无边界平均**：原版玻璃边框仅 1 texel，屏幕投影常 1~2 px，5×5 把边框与透明中心平均 → 直接糊掉细节。
3. **RR 时间累积**：玻璃像素进入 RR 时，MV/depth 来自背后几何，玻璃层自身的高频（边框）与背景在时间上不稳定 → 残留颗粒。
4. 材质包分辨率高后，边框占的 UV 与像素面积大，抗平均能力变强，所以"装材质就没事"。

## 5. 已做的改动（未测）

把 3.4 的两处 5×5 改成双边加权：
- 深度差 > 2%（相对玻璃面距离）的邻居直接跳过；
- 亮度差按 `exp(-lumaDelta × 12.0)` 衰减；
- Luma 权重调 0 即退化为原版纯高斯。

## 6. 请回答的问题

1. **主因判定**：在上述架构下，半透明物体 1~2 px 细边框模糊，主要来自 (a) alpha 稀释、(b) 空间平滑、(c) RR 时间累积，还是别的地方？请给出**能区分三者的最小实验**（每个实验说清"看什么、说明什么"）。
2. **alpha 策略**：把整个玻璃族的 a 钉死成 0.3（贴图 alpha 只作颜色）是否合理？业界对"同一张贴图里 alpha 从 0.4 到 1.0 不等"的半透明物体，通常怎么处理透明度（逐 texel alpha？固定值？按 albedo 阈值分层？）
3. **RR 引导**：透明/半透明像素应该给 DLSS-RR 什么样的 motion vector 与 depth？用玻璃面本身的还是背后几何的？两者混用是否会导致时间累积抖动？是否应该对透明像素降权或标记为不可累积？
4. **噪点来源**：a=0.3 意味着 70% 的 1spp 背景直接透出。噪点主因是"背景 1spp 透过"，还是"RR 累积不足"？除了"把 a 临时调到 0.9 看噪点是否消失"，还有更快的判别法吗？
5. **双边滤波参数**：joint bilateral / à-trous 用于这种屏幕空间合成时，深度阈值与色差 σ 的常用取值区间是多少？我用的"深度 2%、亮度系数 12"是否在合理范围？
6. **参考实现**：如果 NVIDIA Remix / Portal RTX / Cyberpunk 2077 RT 有公开的透明混合 + RR 处理方式（shader 名称、pass 顺序、alpha 与 guide 的写法），请指名并描述关键做法。

## 7. 期望输出形式

- 先给"最可能的主因"和排序理由；
- 再给可执行实验清单（每条：改什么 → 预期现象 → 结论）；
- 最后给具体改法（伪代码或公式即可，不需要完整 shader）。

## 8. 环境

| 项 | 值 |
|---|---|
| OS | Windows 11 26200 |
| GPU | NVIDIA GeForce RTX 4070 Laptop |
| 驱动 | 32.0.16.1742 |
| API | Vulkan 1.2 + VK_KHR_ray_tracing |
| 采样 | 1 spp / 帧 |
| 降噪 | NRD（局部）+ DLSS-RR |
| 贴图 | 原版 16×16 glass.png（边框 1 texel） |

---

## 结论（顾问回执，2026-11-21）

### 三因叠加，排序

1. **主因：5×5 直接滤了 `alphaBlendGbuffer`**。贴图 albedo/alpha 本身无采样噪声，屏幕空间滤波只应作用于带噪的 `surfaceLight`。量化：5×5 核总权重 6.91，1 px 细线沿线仅得 2.4 ⇒ 自身信号只剩约 35%（孤立边缘约 52%），其余被透明中心稀释。
2. **第二因：合成在 NRD/DLSS-RR 之前，但 Guide 属于背后几何**。RR 在 Guide 里看不到这条 1~2 px 亮线，判定为 firefly 抹平；背景 MV 与玻璃面存在视差，时间重投影错位 ⇒ 历史拒绝、颗粒、闪烁。
3. **放大器：`GLASS_FIXED_ALPHA = 0.3` 钉死**。原版边框 alpha 1.0 被压到 0.3，再乘 5×5 的 35% 留存 ⇒ 有效对比度约为原本 10%；同时 `1-a = 0.7` 让 70% 未降噪 1spp 背景噪点直接叠在边框上。

### 对本文档第 5 节（我实现的深度双边）的否决

边框与玻璃中心**在同一平面**，相对深度差≈0，深度型双边挡不住串扰；仅 `exp(-luma*12)` 略有作用但治标不治本。**已按此结论回退**。

### 根治方案（三步）

1. 删除 `alphaBlendGbuffer` 的 5×5 滤波，改单像素直读（**已落地**，见 A/B 宏 `VPT_GLASS_ALPHA_BLEND_ALBEDO_SMOOTH=0`）。
2. **Alpha-Cutout 分流**：在 raygen/any-hit 里判断 `texAlpha >= 0.85`（边框）→ 按不透明几何终止主光线、写主 G-buffer，天然获得 100% 正确的 Guide 与不透明度；仅 `texAlpha < 0.85`（中心）走穿透与半透明合成。
3. **Post-Denoise Compositing**：把 `glass_alpha_finish` 移到 NRD/DLSS-RR 之后，只对已降噪的 sceneColor 合成低 alpha 玻璃中心。

过渡方案（若暂不能重排管线）：先做第 2 步的 Cutout 分流，剩余低 alpha 中心没有高频细线，在 RR 前合成副作用小一个量级；alpha 可用逐 texel 非线性映射：
`a = mix(clamp(texAlpha*0.55, 0.08, 0.30), clamp(texAlpha, 0.85, 1.0), smoothstep(0.75, 0.90, texAlpha))`

### 双边参数修正（用于 light 滤波，非 albedo）

- 只滤 `surfaceLight`，绝不滤 albedo/alpha；
- 深度用**视角修正**：`relDepthDiff = |qD - D| / (D / max(|dot(N,V)|, 0.15))`，阈值 2%；
- 法线权重 `pow(max(dot(n0,n1),0), 64)`；
- 亮度用 **log 域**：`exp(-|log(1+luma0) - log(1+luma1)| / 0.35)`（σ_L≈0.35，即系数约 2.86；原 12.0 对 HDR 过紧，等效于关闭滤波）；
- 空间权 `exp(-(dx²+dy²)/4.5)`（σ≈1.5）。

### 业界参考

- **RTX Remix / Portal RTX**：`gbuffer → integrate → demodulate → NRD/DLSS-RR → composite`，半透明层在 **composite 阶段**（降噪后）混合；高不透明度区域走 alpha-test 直接进主 G-buffer。
- **Cyberpunk 2077 RT Overdrive**：严格保持送入 RR 的颜色与 DiffuseAlbedo / SpecularAlbedo / NormalRoughness / Depth / MV / SpecularHitDistance 几何一致；没有独立 Guide 的叠层一律延后到 RR 之后合成。

### 待办（按顾问给的实验清单）

- Exp 1（已落地）：关掉 albedo 5×5 → 静止看边框是否瞬间锐利
- Exp 2：对比关 RR / 开 RR、静止 2s / 缓慢平移
- Exp 3：完成时把背景常量化为 0.1，再固定 `surfaceLight=1`，三分噪点归属
- Exp 4：`texAlpha>0.8` 取 a=0.9，验证 alpha 稀释占比

