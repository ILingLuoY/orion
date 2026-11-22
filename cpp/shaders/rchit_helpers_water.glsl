// 2026-11-05 B①：从 rt_test.rchit 225-451 行原样切出的水/天空 helper 段。
// rchit 在原位置 include 回来；K2 的 rgen（ORION_K2_TRANSPORT）也 include 它。
// 段内原直接读的命中内建改为读下面三个全局量（resolveHitMaterial 开头写入
// 真值，与内建同值）。全局量声明也在这里（原先在 rchit 的 include 点之前）。
float materialHitT = 0.0;
vec3 materialRayOrigin = vec3(0.0);
vec3 materialRayDirection = vec3(0.0, 0.0, -1.0);
struct WaterWaveSurface {
    vec3 normal;
};

// Integer-free value hash.  Unlike sin-based shader hashes this is cheap on
// the closest-hit path and remains deterministic for DLSS/RR reprojection.
float rainRippleHash(vec2 p)
{
    p = fract(p * vec2(0.1031, 0.11369));
    p += dot(p, p.yx + 19.19);
    return fract((p.x + p.y) * p.x);
}

// One short-lived impact is assigned to each cell.  Testing the surrounding
// 3x3 cells makes rings continuous at cell boundaries.  This is evaluated
// only for the first visible water interface: no new texture, compute pass,
// particle, BLAS instance, or hardware ray is needed.
vec2 rainImpactRippleGradient(vec2 worldXZ, float cameraDistance)
{
    float rain = abs(cam.weatherParams.z - 1.0) < 0.25
        ? clamp(cam.weatherParams.x, 0.0, 1.0)
        : 0.0;
    if(rain <= 0.01 || getRayPurpose(payload.state) != RT_PURPOSE_PRIMARY_SKY)
    {
        return vec2(0.0);
    }

    float nearFade = 1.0 - smoothstep(28.0, 72.0, cameraDistance);
    if(nearFade <= 0.0)
    {
        return vec2(0.0);
    }

    // Keep every rain-impact dimension 40% smaller than the prior dense
    // field; the 3x3 analytic neighbourhood and ray budget are unchanged.
    // 2026-11-11：UI 旋钮。godRayParams2.w 是唯一空闲车道，两个值打包：
    //   w = size + strength * 8   （size 0.25~4，strength 0~3）
    //   w <= 0 ⇒ 旧 UBO/未设置 ⇒ size=1, strength=1（行为与原档一致）
    // 大小同时缩放格子与扩张速度、并按反比收频率 ⇒ 波的"个数"不变、整体放大。
    const float packedRipple = cam.godRayParams2.w;
    float rippleSize = 1.0;
    float rippleStrength = 1.0;
    if(packedRipple > 0.0)
    {
        rippleStrength = clamp(floor(packedRipple / 8.0 + 1.0e-4), 0.0, 3.0);
        const float rawSize = packedRipple - rippleStrength * 8.0;
        rippleSize = clamp(rawSize <= 0.0 ? 1.0 : rawSize, 0.25, 4.0);
    }
    const float CELL_SIZE = 0.432 * rippleSize;
    const float RING_SPEED = 0.336 * rippleSize;
    const float RING_FREQUENCY = 66.66667 / rippleSize;
    // CameraUBO.padding is the native steady-clock bit pattern.  Keep rain
    // impacts independent of render FPS and Minecraft day-time progression.
    float time = uintBitsToFloat(cam.padding);
    vec2 baseCell = floor(worldXZ / CELL_SIZE);
    vec2 gradient = vec2(0.0);

    for(int y = -1; y <= 1; ++y)
    {
        for(int x = -1; x <= 1; ++x)
        {
            vec2 cell = baseCell + vec2(float(x), float(y));
            float seedX = rainRippleHash(cell + vec2(17.0, 43.0));
            float seedZ = rainRippleHash(cell + vec2(71.0, 29.0));
            float age = fract(time * mix(0.42, 1.08, rain) +
                rainRippleHash(cell + vec2(113.0, 7.0)));
            vec2 impact = (cell + vec2(seedX, seedZ)) * CELL_SIZE;
            vec2 delta = worldXZ - impact;
            float distanceToImpact = length(delta);
            float radius = age * RING_SPEED;
            // 2026-11-21 顾问轮1：环包络 0.204 → 0.30（λ9.4cm 的环在 20cm 包络里
            // 只有 1~2 个波峰，3 圈更接近真实雨滴涟漪观感）。
            float envelope = (1.0 - smoothstep(0.52, 1.0, age)) *
                (1.0 - smoothstep(0.16, 0.30, distanceToImpact));
            if(envelope <= 0.0 || distanceToImpact <= 1.0e-4)
            {
                continue;
            }

            float slope = cos((distanceToImpact - radius) * RING_FREQUENCY);
            gradient += normalize(delta) * slope * envelope;
        }
    }

    // 2026-11-21 顾问轮1：0.060 → 0.170。旧值是"地面水洼已确认可见"的口径，
    // 在 FFT 涌浪斜率（0.5~1+）面前弱 5~10 倍 ⇒ 水面上几乎不可见（用户实测
    // "下雨时几乎没用雨滴涟漪"）。明显度滑条（降雨页·雨波纹明显度 0~3）
    // 继续作为乘钮。
    return gradient * (0.170 * rain * nearFade * rippleStrength);
}

WaterWaveSurface evaluateWaterWaveSurface(vec2 worldXZ, float waterY)
// 2026-11-16 副本收敛说明：本文件是 evaluateWaterWaveSurface /
// evaluateWaterWaveSurfaceClassify / rainImpactRippleGradient 的**唯一活跃
// 副本**（消费者：rt_test.rchit 与 rgen_k2_material_inline）。第三份
// rt_test_translucent.rchit:267 的同名函数是 2026-09-30 时代的 legacy 解析
// 波实现（5 平面波 + 域扭曲 + 自带交互场，不走 FFT）——它是半透明命中组
//（group3）的活跃代码，与这里的 FFT 波面**有意不同**，不是漂移副本；
// 收敛它 = 改半透明水面观感，需要 A/B，不要当清理项做。
// 原 material_common.glsl 里还有一份逐字副本，但全仓无 TU include 它
//（死文件），2026-11-16 归档至 .tmp/dead_code_20261116/。
{
    float cameraDistance;
    vec3 surfacePos = vec3(worldXZ.x, waterY, worldXZ.y);
    float noV = abs(normalize(surfacePos - cam.viewInverse[3].xyz).y);
    float horizonFade = 1.0 - exp(-4.0 * noV);
    // 2026-10-18 方案 A：斜率图 mip 级由屏幕足迹定（仿 voxyLodMip），显式传给采样。
    float projScale = float(gl_LaunchSizeEXT.y)
        / (2.0 * max(tan(cam.cameraParams.z * 0.5), 0.05));
    float waveLod = waterFftSlopeLodFromProj(surfacePos, materialHitT, projScale);
    vec2 naturalSlope = waterNaturalWaveGradient(worldXZ, cameraDistance, horizonFade, waveLod);

    // Persistent Eclipse-style interaction field.  The physical water plane
    // is unchanged; only its optical normal receives the stored height slope.
    // This is gated to the actual water elevation reported for the player so
    // vertically separated ponds cannot share one disturbance.
    float stateAndBorderMask;
    vec2 interactionSlope =
        waterInteractionSlope(worldXZ, stateAndBorderMask);
    float elevationMatch = 1.0 - smoothstep(
        0.65, 1.50, abs(waterY - cam.waterSimSurfaceAndOrigin.x)
    );
    // 2026-10-02 参考重构（HPWater）：自然波与交互场改用参考的"XZ 相加 / Y 相乘"
    // 混合（water_wave_field.glsl 的 waterBlendWaveSlopes，斜率空间的精确等价式）。
    // 混合模式为 0 时逐位回旧档（直接相加）。
    vec2 gradient = waterBlendWaveSlopes(
        naturalSlope,
        interactionSlope * (stateAndBorderMask * elevationMatch),
        waterWaveBlendMode()
    );

    // Rain uses the analytic field above instead of injecting the local
    // player-water simulation.  That makes visible water at any elevation
    // respond while the player is on land, and leaves the 512x512 simulation
    // reserved for actual player entry/exit and movement wakes.
    // （雨滴是高频细节，保持加性、不参与波面混合 —— 参考里没有对应项。）
    gradient += rainImpactRippleGradient(worldXZ, cameraDistance);

    WaterWaveSurface result;
    result.normal = normalize(vec3(-gradient.x, 1.0, -gradient.y));
    return result;
}

// 2026-10-06 近水掉帧降本：CLASSIFY 射线专用的降本波面。classify 只消费界面
// 法线做能量 march（太阳透射 / 水面反射太阳），第 3/4 八度（≈4.4/2.3 格波长）
// 在 Beer-Lambert 积分里不可见 ⇒ Seascape 封顶（只保留基波，折射方向对透射
// 能量的影响是角度级的、远低于噪声底）。交互场保留（玩家尾流对透射/反射太阳
// 仍有贡献，按水面高程门控，与完整路径同一口径）；雨纹本来就被
// rainImpactRippleGradient 的 purpose 检查跳过，这里不再调用。可见水面着色仍走
// evaluateWaterWaveSurface，观感不变。
// 回退：把这个常量改回 4（= 完整波面）。
const int WATER_CLASSIFY_WAVE_OCTAVES = 1;

WaterWaveSurface evaluateWaterWaveSurfaceClassify(vec2 worldXZ, float waterY)
{
    float cameraDistance;
    vec3 surfacePos = vec3(worldXZ.x, waterY, worldXZ.y);
    float noV = abs(normalize(surfacePos - cam.viewInverse[3].xyz).y);
    float horizonFade = 1.0 - exp(-4.0 * noV);
    float projScale = float(gl_LaunchSizeEXT.y)
        / (2.0 * max(tan(cam.cameraParams.z * 0.5), 0.05));
    float waveLod = waterFftSlopeLodFromProj(surfacePos, materialHitT, projScale);
    vec2 naturalSlope =
        waterNaturalWaveGradientCapped(
            worldXZ,
            cameraDistance,
            WATER_CLASSIFY_WAVE_OCTAVES,
            horizonFade,
            waveLod
        );

    float stateAndBorderMask;
    vec2 interactionSlope =
        waterInteractionSlope(worldXZ, stateAndBorderMask);
    float elevationMatch = 1.0 - smoothstep(
        0.65, 1.50, abs(waterY - cam.waterSimSurfaceAndOrigin.x)
    );
    vec2 gradient = waterBlendWaveSlopes(
        naturalSlope,
        interactionSlope * (stateAndBorderMask * elevationMatch),
        waterWaveBlendMode()
    );

    WaterWaveSurface result;
    result.normal = normalize(vec3(-gradient.x, 1.0, -gradient.y));
    return result;
}

// Must remain bit-for-bit equivalent to rgen's continuous celestial-light
// functions so POM self-shadowing and direct lighting cannot diverge.
// 2026-10-07 末地光源球开关（定义见 sky_model_common.glsl，本文件不 include
// 它，提前兜底；重复定义同值合法）。
#ifndef END_ORB_ENABLE
#define END_ORB_ENABLE 1
#endif

vec3 getTimeDrivenSunDirection()
{
    const float TWO_PI = 6.28318530718;
    vec3 horizontalAzimuth =
        normalize(
            vec3(0.5, 0.0, 0.3)
        );
    float dayAngle =
        fract(cam.historyParams.w) * TWO_PI;

#if END_ORB_ENABLE
    // 末地光源球：固定天顶（只自转、不公转）（endOrbDirection，见
    // sky_model_common.glsl；本文件不 include 那个头，字面量内联）。
    // ⚠ 六个方向副本必须同一向量，否则可见球与直射光方向分家。
    if(abs(cam.weatherParams.z - 3.0) < 0.25)
    {
        // 2026-10-18：光源球固定在末地天顶（只自转、不公转）。
        return vec3(0.0, 1.0, 0.0);
    }
#endif

    return normalize(
        horizontalAzimuth * cos(dayAngle) +
        vec3(0.0, sin(dayAngle), 0.0)
    );
}

const float TIME_DRIVEN_SUN_NOON_SCALE = 1.0;
const float TIME_DRIVEN_MOON_NOON_SCALE = 0.05;

float getTimeDrivenSunElevation()
{
    const float TWO_PI = 6.28318530718;
#if END_ORB_ENABLE
    // 末地恒"白天"（光源球固定天顶），与 rgen/rmiss 同一份判据。
    if(abs(cam.weatherParams.z - 3.0) < 0.25)
    {
        return 1.0;
    }
#endif
    return sin(fract(cam.historyParams.w) * TWO_PI);
}

float getTimeDrivenSkyLightRadianceScale()
{
    float elevation = getTimeDrivenSunElevation();
    if(elevation >= 0.0)
    {
        return elevation * TIME_DRIVEN_SUN_NOON_SCALE;
    }
    // 2026-11-16 自查修正：夜间项必须用**月亮真实高度**。
    // 旧式 -elevation（太阳下沉深度）成立的前提是"月亮恒在太阳正对面"；
    // 月亮按月相移动后该前提没了 —— 月亮已经落山，水面反射 / 焦散仍会按
    // "月亮在天顶"给月光。这里与 rt_test.rgen 的
    // getClearTimeDrivenSkyLightRadianceScale 同口径（本文件是方位锥公式，
    // 高度就是 sin(moonAngle)，与 getTimeDrivenSunElevation 同一量纲）。
    const float TWO_PI = 6.28318530718;
    float moonAngle = fract(
        fract(cam.historyParams.w) + cam.sssParams.z + 0.5) * TWO_PI;
    float moonHeight = clamp(sin(moonAngle), 0.0, 1.0);
    float moonIllumination = 0.5 + 0.5 * cos(
        TWO_PI * clamp(cam.sssParams.z, 0.0, 1.0));
    return moonHeight * moonIllumination * TIME_DRIVEN_MOON_NOON_SCALE;
}

vec3 getTimeDrivenSkyLightDirection()
{
    if(getTimeDrivenSunElevation() >= 0.0)
    {
        return getTimeDrivenSunDirection();
    }
    // 2026-11-16：夜间光源 = 真实月亮位置（按月相沿日轨移动）。
    // 满月（月相 0）时与本文件旧的 -sunDirection 逐位等价。
    // 本文件不 include sky_model_common.glsl，故按**本文件自己的**方位锥公式
    // 内联（必须与上面 getTimeDrivenSunDirection 同族，否则日月不同轨）。
    const float TWO_PI = 6.28318530718;
    vec3 horizontalAzimuth = normalize(vec3(0.5, 0.0, 0.3));
    float moonAngle = fract(
        fract(cam.historyParams.w) + cam.sssParams.z + 0.5) * TWO_PI;
    return normalize(
        horizontalAzimuth * cos(moonAngle) + vec3(0.0, sin(moonAngle), 0.0));
}

