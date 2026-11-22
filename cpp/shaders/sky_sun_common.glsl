#ifndef SKY_SUN_COMMON_GLSL
#define SKY_SUN_COMMON_GLSL

#ifndef SKY_SUN_TRANSMITTANCE_COLOR
#define SKY_SUN_TRANSMITTANCE_COLOR 1
#endif

#if SKY_SUN_TRANSMITTANCE_COLOR

const vec3 SKY_SUN_OPTICAL_DEPTH = vec3(0.0824, 0.1656, 0.2024);

// 与 rt_test.rgen 的 atmosphereAirMass() 同式（Kasten 近似）。两处必须同步，
// 改一处必须改另一处。
float skySunAirMass(float elevation)
{
    float h = clamp(elevation, -0.065, 1.0);
    return min(24.0, 1.0 / max(0.045, h + 0.135));
}

// 物理基色：elevation 传太阳（或夜间月亮）方向的 y 分量即可。
// 天顶锚定（airmass - 1）⇒ 正午 ≈ vec3(1.0)。
vec3 skySunPhysicalBaseColor(float elevation)
{
    float mass = skySunAirMass(elevation);
    return exp2(-1.44269504089 * SKY_SUN_OPTICAL_DEPTH * (mass - 1.0));
}

#endif
#ifndef SKY_DAY_PHASE
#define SKY_DAY_PHASE 1
#endif
#if SKY_DAY_PHASE

// 黄金时刻权重：黎明/日落两相（太阳贴地平的低角度段）达到峰值，升入
// 早晨/下午后平滑退场。与暮色调色板同带衔接。
float skySunGoldenHourWeight(float elevation)
{
    return smoothstep(-0.035, 0.005, elevation)
        * (1.0 - smoothstep(0.06, 0.22, elevation));
}

// 日照金山：物理透射基色在低角度偏"褐玫瑰"，金山时刻的受光应是高饱和
// 金色（红顶蓝底）。做法 = 亮度锚定的饱和度拉伸 + 金色相偏移，负值钳零
// （黄昏调色板蓝通道被拉伸后可能过零）。输出仍进各消费处的 rain/thunder
// 调制，恶劣天气自动压回灰色。
vec3 skySunGoldenTint(vec3 baseColor, float elevation)
{
    float w = skySunGoldenHourWeight(elevation);
    if(w <= 0.0)
    {
        return baseColor;
    }
    float luma = dot(baseColor, vec3(0.2126, 0.7152, 0.0722));
    vec3 saturated = max(mix(vec3(luma), baseColor, 1.0 + 1.0 * w), vec3(0.0));
    return saturated * mix(vec3(1.0), vec3(1.32, 0.62, 0.30), w);
}

// =============================================================================
// 2026-11-16 午后暖光（"下午 5 点"时段）
//
// 现有 skySunGoldenTint 只覆盖 -2°..+12.6°（黄金时刻）。而一天里**占时最长**
// 的暖光其实是太阳 15°~32° 的午后：色温从正午 ~5500K 降到 ~4500K，阴影开始
// 偏蓝，天空地平线有一层薄暖。这一段此前完全是纯物理（正午白），
// 是"一天里大部分时间没有氛围"的主因。
//
// 窗口：12.6° 淡入、19.5° 满值、31.5°..43° 淡出。
// 本工程的"正午"是 55°（0.819），完全落在窗口外 ⇒ 中午逐位不变。
//
// 只改色温、不改亮度：暖色乘子按 Rec.709 亮度归一到 1.0，所以地面照度、
// 阴影强度、曝光全都不受影响，只是“光变暖了”。
// 回退：SKY_AFTERNOON_WARMTH = 0。
// =============================================================================
#ifndef SKY_AFTERNOON_WARMTH
#define SKY_AFTERNOON_WARMTH 1
#endif
#if SKY_AFTERNOON_WARMTH

float skySunAfternoonWarmthWeight(float elevation)
{
    return smoothstep(0.22, 0.34, elevation) *
        (1.0 - smoothstep(0.55, 0.75, elevation));
}

vec3 skySunAfternoonWarmth(vec3 color, float elevation)
{
    float w = skySunAfternoonWarmthWeight(elevation);
    if(w <= 0.0)
    {
        return color;
    }
    const vec3 warmTint = vec3(1.10, 0.99, 0.86);
    const float warmLuma =
        0.2126 * warmTint.r + 0.7152 * warmTint.g + 0.0722 * warmTint.b;
    const vec3 normalizedTint = warmTint / max(warmLuma, 1.0e-4);
    return color * mix(vec3(1.0), normalizedTint, w);
}

#endif

#endif
#endif
