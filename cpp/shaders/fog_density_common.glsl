#ifndef FOG_DENSITY_COMMON_GLSL
#define FOG_DENSITY_COMMON_GLSL

const float VFOG_DENSITY = 0.01;
const float VFOG_FALLOFF = 40.0;
const float VFOG_SUNLIGHT_ABSORPTION = 1.0;
const float VFOG_SUNLIGHT_STEPLENGTH = 5.0;

// ===========================================================================
// 2026-11-14：带内高度剖面（上疏下密），只在**给了地表基准**时生效。
//
// 病征（用户）："头顶是天空时总是有垂直射下来的光束"，关体积雾即消失；
// 把「雾高度下界」从 -28 抬到 140 后光柱没了，但身周的雾和体积光也没了。
//
// 根因：原来的 dh **只统计带外距离** ⇒ 带内密度恒定。上界 170 / 下界 -28
// 的配置等于 198 格的均匀雾，人站在雾里时竖直方向的整条视线都在雾内、
// 且露天受光 = 1，而水平方向远处落进山体阴影 = 0 ⇒ 两侧对比出一根
// 从头顶直射下来的"光柱"。真实雾/霾是以地表为底向上指数变稀的。
//
// 口径：在 [下界, 上界] 带内，从 **max(下界, 地表)** 起算向上变稀，衰减系数
// 与 VFOG_FALLOFF 无关（2.0 / FOG_INNER_SCALE_HEIGHT），雨强沿用同一 (1 - wet*0.5)：
//   · 下界 > 地表（薄层，如 140..170）→ 基准 = 下界，薄层内几乎不衰减
//   · 下界 < 地表（厚雾包住相机）  → 基准 = 地表，头顶一路变稀 ⇒ 柱消失，
//     身周（地表附近）密度不变 ⇒ 体积光保留
//   · 默认档 [50,65] + 地表 70：带内样本全在基准之下 ⇒ 逐位等价旧档
//
// 回退：-DFOG_INNER_PROFILE=0（重编 v3/v5 两个 SPV 即可）。
// 注意：太阳方向的光学厚度仍走 fogSunOpticalDepth 的带外闭式解（高估一点 τ，
// 表现为厚雾里的太阳项略暗），刻意不改 —— 它只影响整体亮度、不产生结构。
// ===========================================================================
#ifndef FOG_INNER_PROFILE
#define FOG_INNER_PROFILE 1
#endif

const float FOG_INNER_SCALE_HEIGHT = 40.0;

// 共用入口。groundBase = 带内剖面的基准（与 heightMax/heightMin 同一坐标系，
// 即"已经减掉地面锚定偏移"的那个 y）。传 <= -1e8 表示不启用剖面。
float fogDensityAtBase(
    vec3 fogPos,
    float baseDensity,
    float fogTimeFactor,
    float wetness,
    float heightMax,
    float heightMin,
    float groundBase)
{
    float dh =
        max(fogPos.y - heightMax, 0.0) +
        max(heightMin - fogPos.y, 0.0);

    dh *= (2.0 / VFOG_FALLOFF) * (1.0 - wetness * 0.5);

    float density = exp2(-dh);

#if FOG_INNER_PROFILE
    if(groundBase > -1.0e8)
    {
        const float bandTop = max(heightMax, heightMin);
        const float bandBottom = min(heightMax, heightMin);
        const float innerBase = max(bandBottom, groundBase);
        if(fogPos.y > innerBase && fogPos.y < bandTop)
        {
            density *= exp2(-(fogPos.y - innerBase) *
                (2.0 / FOG_INNER_SCALE_HEIGHT) * (1.0 - wetness * 0.5));
        }
    }
#endif

    return (density * (VFOG_DENSITY * 0.15) + baseDensity) * fogTimeFactor;
}

// 旧签名：vol_restir_composite.comp / volumetric_light.comp 用，**不启用**带内剖面。
// 那两处没有地形高度，且它们的介质口径已在 2026-11-01 与解析雾对齐过一次，
// 这里只加体积雾自己的剖面，避免顺带改掉 ReSTIR 体积光 / god ray 的观感。
float fogDensityAt(
    vec3 fogPos,
    float baseDensity,
    float fogTimeFactor,
    float wetness,
    float heightMax,
    float heightMin)
{
    // 函数体与 2026-11-14 之前逐字相同：这两处的 SPV 要求逐字节不变，
    // 所以不复用 fogDensityAtBase（多一层内联会让产物字节变化）。
    float dh =
        max(fogPos.y - heightMax, 0.0) +
        max(heightMin - fogPos.y, 0.0);

    dh *= (2.0 / VFOG_FALLOFF) * (1.0 - wetness * 0.5);

    float density = exp2(-dh);

    return (density * (VFOG_DENSITY * 0.15) + baseDensity) * fogTimeFactor;
}

// 体积雾专用：带地表基准，启用带内剖面。
float fogDensityAtGround(
    vec3 fogPos,
    float baseDensity,
    float fogTimeFactor,
    float wetness,
    float heightMax,
    float heightMin,
    float groundBase)
{
    return fogDensityAtBase(fogPos, baseDensity, fogTimeFactor, wetness,
        heightMax, heightMin, groundBase);
}

#endif
