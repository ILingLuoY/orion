#ifndef CLOUD_HP_CORE_GLSL
#define CLOUD_HP_CORE_GLSL

// =====================================================
// 2026-11-10 体积云受光两项：
//   CLOUD_GROUND_BOUNCE  地面反弹照亮云底（0 = 关 = 旧档）。
//     物理：阳光打到地面 → 按地面反照率反射 → 上行照亮云底。
//     术语本来就有（groundAlbedo/groundBounce 字段一直在，但零消费者）。
//     参数来自 cloudPhysicsParams.x（地面反照率 0..0.8）/ .y（反弹强度 0..1），
//     这两条 UI 滑条此前也是死的（无着色器读取）。
//   CLOUD_LIGHT_REUSE    光步跨视步复用的步长（出厂 2 = 现有行为）。
//     实现位置在 hpEvaluateCloud 的 view march（lightStride）。
//     调大 = 少做光 march、性能更好、云内明暗过渡更平滑（插值拖尾）。
// =====================================================
#ifndef CLOUD_GROUND_BOUNCE
#define CLOUD_GROUND_BOUNCE 1
#endif
#ifndef CLOUD_LIGHT_REUSE
#define CLOUD_LIGHT_REUSE 2.0
#endif

const float HP_WEATHER_TILE = 96000.0;
const float HP_BASE_TILE    = 8000.0;
const float HP_EROSION_TILE = 1400.0;
#define HP_DENSITY_THRESHOLD     0.1
#define HP_DENSITY_BASE_THRESHOLD 0.41
// 覆盖率→阈值耦合（Schneider 口径 threshold ≈ 1-coverage）：密度阈值不再是常数，
// 而是跟随逐柱 coverage 下移。没有这一步，滑块只调"云的浓度"不调"天空被遮比例"，
// 0.48 的滑块也会得到 ~85% 的全覆盖灰毯（2026-11-01 v5top 实测）。
// 两端按本仓 base 体积侵蚀后的分布标定（eroded 均值 ~0.58）：
// coverage 0 → 阈值 0.85（稀疏），coverage 1 → 0.30（近全覆盖）。
// 天气图 R 均值 ~0.66，滑块高端会饱和（0.9 只盖 52%），映射前乘 GAIN 补偿：
// 实测 0.2 → ~10%，0.48 → ~60%，0.9 → ~85%。
#define HP_COVERAGE_THR_GAIN     1.45
#define HP_COVERAGE_THR_HI       0.85
#define HP_COVERAGE_THR_LO       0.30
#define HP_PRESENCE_SOFT         0.10
#define HP_PHIFWD_OMEGA0         0.999
#define HP_PHIFWD_KAPPA_SCALE    sqrt(3.0 * (1.0 - HP_PHIFWD_OMEGA0))
#define HP_CONE_RATIO            2.0
#define HP_CONE_MIN_STEP         5.0
#define HP_CONE_MAX_DISTANCE     6000.0
struct HPCloudConfig
{
    float bottomHeight;
    float topHeight;
    float planetRadius;
    float coverage;
    float densityMult;
    float noiseScale;
    float detailScale;
    float detailStrength;
    float typeBias;
    int   viewSteps;
    int   lightSteps;
    float shadowDistance;
    int   msOctaves;
    float msAttenuation;
    float msContribution;
    float msEccentricity;
    float luminanceScale;
    float forwardEcc;
    float backwardEcc;
    float phaseMix;
    vec3  sunDir;
    vec3  sunColor;
    vec3  ambientTop;
    vec3  ambientBottom;
    vec3  groundAlbedo;
    float groundBounce;
    float viewDistance;
    float fadeStart;
    float minTransmittance;
    float atmosphereDensity;
    float time;
    float jitter;
    vec3  cameraAlt;

    float detailStrengthCu;
    float detailStrengthTcu;
    float detailStrengthCb;
    float bottomSmoothHeight;
    float wispyEdgeWidth;
    float wispyReach;
    float wispyTopHeight;
    float wispyTopHardness;
    float wispyWeight;
    float billowyWeight;
    float loCoverTopMax;
    float loCoverTopStr;
    float covMapScale;
    float scWorleyStrength;
    float scHeightScale;
    float scCellThickStr;
    float powderIntensity;
    float aoUpwardScale;
    float phiFwdIntensity;
    float phiFwdDepthPow;
    float phiFwdDepthBias;
    float phiFwdCompress;
    float phiFwdBoundary;
    float phiFwdMSBuild;
    float scatterODScale;
    float scatterCurvePow;
    float lightErosion;
    float lightReuse;
    float coneRatio;

    // 2026-11-20 夜间月光氛围：相位地板。
    // >0 时把每个多重散射阶的相位抬到至少这个值（各向同性基底），只在调用方
    // 按夜色权重填（白天 = 0 ⇒ 行为逐位不变）。0 = 关闭。
    float phaseFloor;

    float lightJitter;
    float powderEffect;
    float silverStrength;
    float silverSpread;
    float baseErosion;
    float stormDarkness;
    float sunsetWarmth;
    float weatherCoupling;
    vec3  ambientSunSide;
    vec3  ambientBiasDir;   // 天光"太阳侧"偏置朝向：恒为太阳方向，昼夜交接时不翻
    vec3  lightningPos;
    vec3  lightningColor;

    // 2026-11-21 统一风场：平流方向（世界 XZ 单位向量）。本文件刻意无 cam
    //（cloud_v5_noise.comp / cloud_bevy_preview.comp 没有 cam UBO 也能编译），
    // 所以方向经 cfg 传入：hpDefaultConfig 填出厂 (0.862, 0.507)，有 cam 的宿主
    //（volumetric_clouds_v5.comp）用 windDirXZ() 覆盖。速度（4.9 格/s）不动。
    vec2  windDir;
};

HPCloudConfig hpDefaultConfig()
{
    HPCloudConfig c;
    c.bottomHeight = 1800.0;
    c.topHeight = 3600.0;
    c.planetRadius = 6360000.0;

    c.coverage = 0.5;
    c.densityMult = 0.004;
    c.noiseScale = 1.0;
    c.detailScale = 1.0;
    c.detailStrength = 1.0;
    c.typeBias = 0.0;

    c.viewSteps = 96;
    c.lightSteps = 6;
    c.shadowDistance = 0.0;

    c.msOctaves = 3;
    c.msAttenuation = 0.45;
    c.msContribution = 0.55;
    c.msEccentricity = 0.5;
    c.luminanceScale = 1.0;

    c.forwardEcc = 0.80;
    c.backwardEcc = 0.30;
    c.phaseMix = 0.28;

    c.sunDir = vec3(0.0, 1.0, 0.0);
    c.sunColor = vec3(0.0);
    c.ambientTop = vec3(0.0);
    c.ambientBottom = vec3(0.0);
    c.groundAlbedo = vec3(0.0);
    c.groundBounce = 0.0;

    c.viewDistance = 0.0;
    c.fadeStart = 0.3;
    c.minTransmittance = 0.015;
    c.atmosphereDensity = 0.0;
    c.time = 0.0;
    c.jitter = 0.5;
    c.cameraAlt = vec3(0.0);
    c.detailStrengthCu = 0.55;
    c.detailStrengthTcu = 0.70;
    c.detailStrengthCb = 0.85;
    c.bottomSmoothHeight = 0.08;
    c.wispyEdgeWidth = 0.40;
    c.wispyReach = 0.15;
    c.wispyTopHeight = 0.75;
    c.wispyTopHardness = 2.0;
    c.wispyWeight = 0.45;
    c.billowyWeight = 0.45;
    c.loCoverTopMax = 2.2;
    c.loCoverTopStr = 1.0;
    c.covMapScale = 1.0;
    c.scWorleyStrength = 0.25;
    c.scHeightScale = 0.35;
    c.scCellThickStr = 0.6;

    c.powderIntensity = 1.0;
    c.aoUpwardScale = 1.0;
    c.phiFwdIntensity = 0.0;
    c.phiFwdDepthPow = 1.0;
    c.phiFwdDepthBias = 0.0;
    c.phiFwdCompress = 1.0;
    c.phiFwdBoundary = 0.0;
    c.phiFwdMSBuild = 1.0;
    c.scatterODScale = 0.10;
    c.scatterCurvePow = 1.0;

    c.lightErosion = 0.0;
    c.lightReuse = CLOUD_LIGHT_REUSE;
    c.coneRatio = HP_CONE_RATIO;
    c.phaseFloor = 0.0;

    c.lightJitter = 0.5;
    c.powderEffect = 0.65;
    c.silverStrength = 0.08;
    c.silverSpread = 0.85;
    c.baseErosion = 0.55;
    c.stormDarkness = 0.85;
    c.sunsetWarmth = 1.00;
    c.weatherCoupling = 1.00;
    c.ambientSunSide = vec3(0.0);
    c.ambientBiasDir = vec3(0.0, 1.0, 0.0);
    c.lightningPos = vec3(0.0);
    c.lightningColor = vec3(0.0);
    // 2026-11-21 统一风场：出厂方向 = 原 windUV 硬编码方向（≈30.5°）。
    c.windDir = vec2(0.862, 0.507);
    return c;
}

float hpSaturate(float v) { return clamp(v, 0.0, 1.0); }


// HanPi DensityRemap(x, a, b, c, d)
float hpRemap(float x, float a, float b, float c, float d)
{
    float denom = b - a;
    if(abs(denom) < 1.0e-6)
        return (x < a) ? c : d;
    return (((x - a) / denom) * (d - c)) + c;
}

// HanPi HenyeyGreenstein（归一化，1/4π）
float hpHG(float cosAngle, float g)
{
    float g2 = g * g;
    return (1.0 / (4.0 * 3.14159265359)) * (1.0 - g2) /
        pow(max(1.0 + g2 - 2.0 * g * cosAngle, 1.0e-4), 1.5);
}

// HanPi PowderEffect（smoothstep(0.5, -0.5, cos)——背光侧生效）
float hpPowderEffect(float cloudDensity, float cosAngle, float intensity)
{
    float powder = 1.0 - exp(-cloudDensity * 4.0);
    powder = hpSaturate(powder * 2.0);
    return mix(1.0, mix(1.0, powder, smoothstep(0.5, -0.5, cosAngle)), intensity);
}

// 球壳求交（近交点；数值稳定写法——相机在层上俯视时 b、s 量级 3.6e6 相消）
float hpIntersectShell(float originRadius, float rayDirY, float sphereRadius)
{
    float b = originRadius * rayDirY;
    float c0 = originRadius * originRadius - sphereRadius * sphereRadius;
    float d = b * b - c0;
    if(d < 0.0) return -1.0;
    float s = sqrt(d);
    float q = -(b + (b >= 0.0 ? s : -s));
    if(abs(q) < 1.0e-9) return 0.0;
    return c0 / q;
}

float hpNormalizedHeight(vec3 pos, HPCloudConfig cfg)
{
    float h = max(cfg.topHeight - cfg.bottomHeight, 1.0);
    return (length(pos) - (cfg.planetRadius + cfg.bottomHeight)) / h;
}

float hpSlab(HPCloudConfig cfg)
{
    return max(cfg.topHeight - cfg.bottomHeight, 1.0);
}

// ---------------------------------------------------------------------------
// Profile LUT（程序化）。HanPi 的 _CloudLutTexture：UV=(lutSampleHeight,
// radialDist)，RGB=(Cu, Tcu, Cb) 三型剖面。仓内无 LUT 资产，这里按三型积云
// 的公认形状解析实现；radial 用天气图 B 通道（ScMask）驱动——云团核心
// （Cb 偏向）到边缘（Cu 偏向）剖面渐变。
// ---------------------------------------------------------------------------
// Cu 通道单独成函数：参考的 Sc 混合是 `lerp(heightGradient, profiles.r, scStr)`，
// 即在**同一个 lutSampleHeight** 上取 LUT 的 R(Cu) 通道。我们原来用
// `hpProfileLUT(lutSampleHeight, radial, 0.0)` 再算一遍（等于 lo = mix(cu,cb,radial)），
// 既多一次 6×smoothstep 的完整廓形求值（光步里每步都要付），语义也不对。
float hpProfileCu(float h)
{
    return smoothstep(0.02, 0.10, h) * (1.0 - smoothstep(0.28, 0.60, h));
}

float hpProfileLUT(float h, float radial, float type)
{
    float cu  = hpProfileCu(h);
    float tcu = smoothstep(0.02, 0.09, h) * (1.0 - smoothstep(0.42, 0.72, h));
    float cb  = smoothstep(0.01, 0.07, h) * (1.0 - smoothstep(0.55, 0.85, h));
    float radialCore = 1.0 - clamp(radial, 0.0, 1.0);
    float lo  = mix(cu, cb, radialCore);
    float gradient;
    if(type < 0.5)
        gradient = mix(lo, tcu, clamp(type * 2.0, 0.0, 1.0));
    else
        gradient = mix(tcu, cb, clamp((type - 0.5) * 2.0, 0.0, 1.0));
    return gradient;
}

void hpEvaluateCloudProperties(vec3 positionAlt, float slabH, float noiseMipOffset,
    bool simpleMode, HPCloudConfig cfg, sampler2D weatherTex, sampler3D baseTex,
    sampler3D erosionTex, out float outDensity, out float outLocalHeight)
{
    outDensity = 0.0;
    outLocalHeight = 0.0;

    float h = (length(positionAlt + vec3(0.0, cfg.planetRadius, 0.0))
        - (cfg.planetRadius + cfg.bottomHeight)) / max(slabH, 1.0);
    if(h <= 0.0 || h >= 1.0) return;

    vec2 windUV = cfg.windDir * (14.0 * 0.35 * cfg.time);
    vec2 weatherUV = (positionAlt.xz - windUV) / HP_WEATHER_TILE;
    vec4 weather = textureLod(weatherTex, weatherUV, 0.0);
    float cloudType   = clamp(mix(weather.g, cfg.typeBias, 0.35), 0.0, 1.0);
    float scAtlasMask = weather.b;

    float coverageRaw    = weather.r;
#if HP_DIAG_UNIFORM_COVERAGE
    coverageRaw = 0.62;
#endif
    float coverage       = hpSaturate(coverageRaw * cfg.coverage * cfg.covMapScale);
    float coverageHeight = hpSaturate(coverageRaw * cfg.coverage * cfg.covMapScale);

    float presT = hpSaturate((cfg.coverage - 0.10) / 0.80);
    float presCut = pow(1.0 - presT, 5.0);
    float presence = smoothstep(presCut - HP_PRESENCE_SOFT,
        presCut + HP_PRESENCE_SOFT, coverageRaw);
    if(presence <= 0.002)
        return;
    float scStr = cfg.scWorleyStrength * scAtlasMask;
    float scCell = hpSaturate(weather.b * 1.25);

    float scCoverage = hpSaturate(coverageRaw * cfg.coverage * (cfg.covMapScale * 1.1));
    coverage = mix(coverage, scCoverage * scCell, scStr);

    float loCoverTopScale = mix(1.0, max(cfg.loCoverTopMax, 1.0),
        hpSaturate(coverageHeight) * clamp(cfg.loCoverTopStr, 0.0, 1.0));
    float heightForLUT = h / (1.0 + (loCoverTopScale - 1.0) * h);

    float scCompressedHeight = hpSaturate(h / max(cfg.scHeightScale, 0.05));
    float lutSampleHeight = mix(heightForLUT, scCompressedHeight, scStr);
    outLocalHeight = lutSampleHeight;

    // Wrapped per-tile radial profile variation (fixes missing fract() clamp at 1.0).
    float hpRadial = hpSaturate(length(fract(weatherUV * 2.0) - 0.5) * 1.85);
    float heightGradient = hpProfileLUT(lutSampleHeight, hpRadial, cloudType);
    if(scStr > 0.0)
        heightGradient = mix(heightGradient, hpProfileCu(lutSampleHeight), scStr);
    float topCut = mix(0.80, 1.28, hpSaturate(weather.a));
    heightGradient *= 1.0 - smoothstep(topCut - 0.24, topCut, h);
    if(heightGradient <= 0.002)
        return;

    vec3 windOffset = vec3(windUV.x, 0.0, windUV.y);
    float minNoiseScale = HP_BASE_TILE / (6.0 * slabH);
    float noiseScaleClamped = max(cfg.noiseScale, minNoiseScale);
    vec3 baseUVW = positionAlt / (HP_BASE_TILE / max(noiseScaleClamped, 0.02))
        + windOffset / HP_BASE_TILE;
    float baseNoiseR = textureLod(baseTex, baseUVW, max(noiseMipOffset, 0.0)).r;
    float baseShape = pow(abs(baseNoiseR), 0.68);

    // Soft organic bottom taper modulated by baseErosion (replaces flat tabletop cutoff).
    float bottomNoiseFade = smoothstep(0.0, max(cfg.bottomSmoothHeight, 0.04), lutSampleHeight);
    float flatBias = mix(0.45, 0.0, clamp(cfg.baseErosion, 0.0, 1.5));
    baseShape = mix(mix(baseShape, 0.72, flatBias), baseShape, bottomNoiseFade);

    // Lossless early-out before expensive 3D erosionTex fetch: erosion strictly lowers baseShape,
    // so zero density is mathematically guaranteed whenever baseShape <= min(threshold, wispyThreshold).
    float threshold = clamp(mix(HP_COVERAGE_THR_HI, HP_COVERAGE_THR_LO,
        hpSaturate(coverage * HP_COVERAGE_THR_GAIN)), 0.18, 0.95);
    float wispyThreshold = threshold - clamp(cfg.wispyReach, 0.0, 0.5);
    if(baseShape <= min(threshold, wispyThreshold))
        return;

    float billowy = 0.0;
    float wispy = 0.0;
    if(!simpleMode)
    {
        float detailTile = (HP_EROSION_TILE * 0.58) / max(cfg.detailScale, 0.02);
        vec3 detailWindOffset = vec3(windUV.x, 0.0, windUV.y) * 0.9
            + vec3(0.0, cfg.time * 5.0, 0.0);
        vec3 detailUVW = vec3(positionAlt.x, -positionAlt.y, positionAlt.z) / detailTile
            + detailWindOffset / detailTile;
        vec4 d = textureLod(erosionTex, detailUVW, max(noiseMipOffset, 0.0));
        float detailHeightFade = mix(0.28, 1.0, bottomNoiseFade);
        billowy = (d.b * 0.78 + d.a * 0.88) * detailHeightFade * clamp(cfg.billowyWeight, 0.0, 2.0);
        wispy   = (d.r * 0.70 + d.g * 0.80) * detailHeightFade * clamp(cfg.wispyWeight, 0.0, 2.0);
    }
    float base_cloud = 0.0;
    {
        float scCellSafe = max(scCell, 0.001);
        float scCellShaped = scCellSafe * scCellSafe;
        float scCellFactor = mix(1.0, scCellShaped, 0.6);
        float heightClip = mix(1.0, scCellFactor, scStr);

        float detailStrength;
        if(cloudType < 0.5)
            detailStrength = mix(cfg.detailStrengthCu, cfg.detailStrengthTcu, cloudType * 2.0);
        else
            detailStrength = mix(cfg.detailStrengthTcu, cfg.detailStrengthCb,
                (cloudType - 0.5) * 2.0);
        detailStrength = mix(detailStrength, 0.45, scStr);
        detailStrength *= clamp(cfg.detailStrength, 0.0, 2.0);

        float erosionBillowy = min(billowy * detailStrength, 0.95);
        float erosionWispy   = min(wispy   * detailStrength, 0.95);
        float erodedBillowy = hpSaturate(hpRemap(baseShape, erosionBillowy, 1.0, 0.0, 1.0));
        float erodedWispy   = hpSaturate(hpRemap(baseShape, erosionWispy,   1.0, 0.0, 1.0));

        float edgeSoftness = 0.25;
        float densityBillowy = hpSaturate(hpRemap(erodedBillowy, threshold,
            threshold + edgeSoftness, 0.0, 1.0));
        float densityWispy = hpSaturate(hpRemap(erodedWispy, wispyThreshold,
            wispyThreshold + edgeSoftness, 0.0, 1.0));

        densityBillowy *= heightGradient;
        densityWispy   *= heightGradient;

        float wispyT = hpSaturate((h - clamp(cfg.wispyTopHeight, 0.0, 0.99))
            / max(1.0 - clamp(cfg.wispyTopHeight, 0.0, 0.99), 0.001));
        float wispyHeightMask = pow(1.0 - wispyT, max(cfg.wispyTopHardness * 10.0, 0.01));
        densityWispy *= wispyHeightMask;

        float billowyByShape = smoothstep(0.0, max(cfg.wispyEdgeWidth, 0.01), densityBillowy);
        base_cloud = mix(densityWispy, densityBillowy, billowyByShape) * heightClip;

        float typeDensityMult;
        if(cloudType < 0.5)
            typeDensityMult = mix(1.0, 1.25, cloudType * 2.0);
        else
            typeDensityMult = mix(1.25, 1.5, (cloudType - 0.5) * 2.0);
        base_cloud *= typeDensityMult;
    }

    float density = max(0.0, base_cloud);
    density *= presence;
    density *= 1.0 - hpSaturate(0.35 * (1.0 - pow(hpSaturate(coverage), 2.0)));

    density *= cfg.densityMult;

    if(cfg.viewDistance > 1.0)
    {
        float camDist = length(positionAlt - cfg.cameraAlt);
        float fadeT = smoothstep(cfg.viewDistance * clamp(cfg.fadeStart, 0.0, 0.99),
            cfg.viewDistance, camDist);
        density *= 1.0 - fadeT;
    }

    outDensity = max(0.0, density);
}

// [HanPi] HP_EvaluateHpHpPhiFwdTopHeightProxy —— 用低云天气图重建的"柱厚度代理"，
// 只服务于 phi_fwd 的边界受光判断与云底软高度，不参与密度本身。
// 参考用 LoCoverTopMax/LoCoverTopStr 对 coverage 做一次 Möbius 抬升后归一化；
// 我们原来写死 `coverageHeight * 1.2 / 2.0`（只对 max=2.2/str=1 的默认档近似）。
float hpPhiFwdTopProxy(vec2 worldXZ, HPCloudConfig cfg, sampler2D weatherTex)
{
    vec2 windUV = cfg.windDir * (14.0 * 0.35 * cfg.time);
    vec2 uv = (worldXZ - windUV) / HP_WEATHER_TILE;
    float ch = hpSaturate(texture(weatherTex, uv).r * cfg.coverage * cfg.covMapScale);
    float scale = mix(1.0, max(cfg.loCoverTopMax, 1.0),
        ch * clamp(cfg.loCoverTopStr, 0.0, 1.0));
    return hpSaturate(ch * scale / max(cfg.loCoverTopMax, 1.0));
}

vec3 hpEvaluateSunLuminance(vec3 positionAlt, float currentLocalHeight,
    HPCloudConfig cfg, vec3 sunColor, float phaseFunction[3],
    sampler2D weatherTex, sampler3D baseTex, sampler3D erosionTex,
    out float lightExtinctionOD, out float phiFwd)
{
    float totalLightDistance = 0.0;
    vec3 luminance = vec3(0.0);
    lightExtinctionOD = 0.0;
    phiFwd = 0.0;

    float originRadius = length(positionAlt + vec3(0.0, cfg.planetRadius, 0.0));
    {
        float earthB = originRadius * cfg.sunDir.y;
        float earthC0 = originRadius * originRadius
            - cfg.planetRadius * cfg.planetRadius;
        if(earthB < 0.0 && earthB * earthB >= earthC0)
        {
            lightExtinctionOD = 0.0;
            phiFwd = 0.0;
            return vec3(0.0);
        }
    }
    float topRadius = cfg.planetRadius + cfg.topHeight;
    float b = originRadius * cfg.sunDir.y;
    float c0 = originRadius * originRadius - topRadius * topRadius;
    float disc = max(b * b - c0, 0.0);
    totalLightDistance = -b + sqrt(disc);

    {
        float coverDist = clamp(totalLightDistance, 120.0, HP_CONE_MAX_DISTANCE);
        int numSteps = clamp(cfg.lightSteps, 4, 12);
        float r = clamp(cfg.coneRatio, 1.15, 4.0);
        float w0 = coverDist * (r - 1.0) / max(pow(r, float(numSteps)) - 1.0, 1.0e-4);
        w0 = max(w0, HP_CONE_MIN_STEP);

        float extinctionSum = 0.0;
        float curWidth = w0;
        float cumDist = 0.0;
        float slabH = hpSlab(cfg);

        for(int j = 0; j < 12; ++j)
        {
            if(j >= numSteps) break;
            float stepWidth = min(curWidth, coverDist - cumDist);
            if(stepWidth <= 0.0) break;

            // Tight j == 0 light-step jitter prevents near-surface self-shadow speckle.
            float stepSampleOffset = (j == 0)
                ? (0.45 + 0.08 * cfg.lightJitter)
                : (0.38 + 0.24 * cfg.lightJitter);
            float dist = cumDist + stepWidth * stepSampleOffset;
            vec3 samplePosWS = positionAlt + cfg.sunDir * dist;

            float lightDensity = 0.0;
            float lightLocalHeight = 0.0;
            float lightMip = float(j) / max(float(numSteps - 1), 1.0) * 2.5;
            bool useSimpleLight = (j > 0) || (cfg.lightErosion <= 0.5);
            hpEvaluateCloudProperties(samplePosWS, slabH, lightMip,
                useSimpleLight, cfg,
                weatherTex, baseTex, erosionTex, lightDensity, lightLocalHeight);

            extinctionSum += lightDensity * stepWidth;
            if(extinctionSum > 12.0) break;
            if(j > 0 && cfg.sunDir.y >= 0.0)
            {
                float sRadius = length(samplePosWS + vec3(0.0, cfg.planetRadius, 0.0));
                if(sRadius >= topRadius) break;
            }

            cumDist += stepWidth;
            curWidth *= r;
        }
        lightExtinctionOD = extinctionSum;

        for(int o = 0; o < 3; ++o)
        {
            if(o >= cfg.msOctaves) break;
            float attFactor = pow(clamp(cfg.msAttenuation, 0.08, 1.0), float(o));
            float conFactor = pow(clamp(cfg.msContribution, 0.05, 1.0), float(o));
            float trans = exp(-extinctionSum * attFactor);
            luminance += trans * sunColor * phaseFunction[o] * conFactor;
        }
    }

    return luminance * max(cfg.luminanceScale, 0.0);
}

void hpEvaluateCloudStep(float density, float localHeight, float slabH, float h,
    vec3 positionAlt, float stepSize, float relativeRayDistance,
    vec3 totalLuminance, float lightExtinctionOD, float phiFwd,
    HPCloudConfig cfg, vec3 rayDir, float silverPhase,
    inout vec3 inScattering, inout float transmittance)
{
    float sigmaT = max(density, 0.0);
    float stepTransmittance = exp(-sigmaT * stepSize);
    float absFrac = 1.0 - stepTransmittance;

    // Physical Beer-Powder contrast: carves deep billowy crevices while keeping forward-lit rims glowing.
    float cosTheta = dot(rayDir, cfg.sunDir);
    float powderCurve = 1.0 - exp(-lightExtinctionOD * 2.2);
    float powderBackWeight = smoothstep(0.45, -0.45, cosTheta);
    float powderMod = mix(1.0, mix(0.38, 1.28, powderCurve),
        powderBackWeight * clamp(cfg.powderEffect * cfg.powderIntensity, 0.0, 1.5));

    // Dedicated backlit silver-lining rim around forward sun angle.
    float silverAtten = exp(-lightExtinctionOD * 0.42);
    vec3 silverLit = cfg.sunColor * silverAtten * silverPhase
        * clamp(cfg.silverStrength * 3.2, 0.0, 4.0) * 0.25;

    // Sunset-safe height-coupled sky ambient occlusion + storm cloud belly darkening.
    float h01 = hpSaturate(h);
    float heightAO = mix(0.24, 1.0, smoothstep(0.0, 0.72, h01));
    float upwardAO = exp(-lightExtinctionOD * (0.25 + 0.45 * max(cfg.sunDir.y, 0.0))
        * max(cfg.aoUpwardScale, 0.0));
    float bellyDark = mix(1.0, clamp(1.0 - 0.36 * cfg.stormDarkness, 0.22, 1.0),
        smoothstep(0.85, 0.15, h01));
    // 2026-11-21：用恒定的太阳方向而非 cfg.sunDir（夜间会翻成月亮方向，
    // 交接点处 cosTheta 突变 ⇒ 环境光权重跳变）。
    float sunSideBias = pow(max(dot(rayDir, cfg.ambientBiasDir) * 0.5 + 0.5, 0.0), 1.6);
    vec3 topAmbientColor = mix(cfg.ambientTop, cfg.ambientSunSide, 0.35 * sunSideBias);
    vec3 ambientTerm = topAmbientColor * (heightAO * upwardAO * bellyDark)
        + cfg.ambientBottom * ((1.0 - h01) * mix(1.0, 0.45, clamp(cfg.stormDarkness * 0.45, 0.0, 0.85)));

#if CLOUD_GROUND_BOUNCE
    {
        float toGround = 1.0 - h01;
        float sunUp = max(cfg.sunDir.y, 0.0);
        ambientTerm += cfg.groundAlbedo * cfg.groundBounce * cfg.sunColor
            * sunUp * (toGround * toGround) * 0.45;
    }
#endif

    // Volumetric in-cloud sheet lightning inside active storm cores.
    vec3 lightningTerm = vec3(0.0);
    if(dot(cfg.lightningColor, vec3(1.0)) > 1.0e-4)
    {
        vec3 toBolt = positionAlt - cfg.lightningPos;
        float d2 = dot(toBolt, toBolt);
        float falloff = 1.0 / (1.0 + d2 * (1.0 / (520.0 * 520.0)));
        float innerGlow = smoothstep(0.0, 0.65, absFrac * 6.0) * mix(0.45, 1.0, h01);
        lightningTerm = cfg.lightningColor * falloff * innerGlow;
    }

    float albedo = mix(0.96, 0.74, clamp((1.0 - h01) * (cfg.stormDarkness - 0.5) * 0.5, 0.0, 0.65));
    vec3 totalStepLight = totalLuminance * powderMod + silverLit + ambientTerm + lightningTerm;
    vec3 integ = totalStepLight * albedo * absFrac;

    inScattering += integ * transmittance;
    transmittance *= stepTransmittance;
}

void hpEvaluateCloudStep(float density, float localHeight, float slabH, float h,
    vec3 positionAlt, float stepSize, float relativeRayDistance,
    vec3 totalLuminance, float lightExtinctionOD, float phiFwd,
    HPCloudConfig cfg, inout vec3 inScattering, inout float transmittance)
{
    hpEvaluateCloudStep(density, localHeight, slabH, h, positionAlt, stepSize,
        relativeRayDistance, totalLuminance, lightExtinctionOD, phiFwd, cfg,
        vec3(0.0, 1.0, 0.0), 0.1, inScattering, transmittance);
}

void hpEvaluateCloud(float density, float localHeight, float slabH, float h,
    vec3 positionAlt, float stepSize, float relativeRayDistance,
    HPCloudConfig cfg, float phaseFunction[3],
    sampler2D weatherTex, sampler3D baseTex, sampler3D erosionTex,
    inout vec3 inScattering, inout float transmittance)
{
    float lightExtinctionOD;
    float phiFwd;
    vec3 totalLuminance = hpEvaluateSunLuminance(positionAlt, localHeight, cfg,
        cfg.sunColor, phaseFunction, weatherTex, baseTex, erosionTex,
        lightExtinctionOD, phiFwd);
    hpEvaluateCloudStep(density, localHeight, slabH, h, positionAlt, stepSize,
        relativeRayDistance, totalLuminance, lightExtinctionOD, phiFwd, cfg,
        inScattering, transmittance);
}

vec4 gHpDebugInfo = vec4(0.0);

bool hpWeatherEmptyFast(vec3 positionAlt, HPCloudConfig cfg, sampler2D weatherTex)
{
    vec2 windUV = cfg.windDir * (14.0 * 0.35 * cfg.time);
    vec2 weatherUV = (positionAlt.xz - windUV) / HP_WEATHER_TILE;
    float coverageRaw = textureLod(weatherTex, weatherUV, 0.0).r;
#if HP_DIAG_UNIFORM_COVERAGE
    coverageRaw = 0.62;
#endif
    float presT = hpSaturate((cfg.coverage - 0.10) / 0.80);
    float presCut = pow(1.0 - presT, 5.0);
    return coverageRaw <= (presCut - HP_PRESENCE_SOFT);
}

vec4 hpTraceVolumetricRay(vec3 rayOriginPlanet, vec3 rayDir, float maxDist,
    HPCloudConfig cfg, sampler2D weatherTex, sampler3D baseTex, sampler3D erosionTex,
    out float hitDistance, out float meanDistance)
{
    hitDistance = maxDist;
    meanDistance = maxDist;

    cfg.cameraAlt = rayOriginPlanet - vec3(0.0, cfg.planetRadius, 0.0);

    if(cfg.viewDistance > 1.0) maxDist = min(maxDist, cfg.viewDistance);
    hitDistance = maxDist;

    const float slabThickness = hpSlab(cfg);
    const float originRadius = length(rayOriginPlanet);
    const float bottomRadius = cfg.planetRadius + cfg.bottomHeight;
    const float topRadius = cfg.planetRadius + cfg.topHeight;

    const float tBottom = hpIntersectShell(originRadius, rayDir.y, bottomRadius);
    const float tTop = hpIntersectShell(originRadius, rayDir.y, topRadius);

    float marchStart;
    float marchEnd;
    {
        bool insideShell = (tBottom <= 0.0 && tTop > 0.0);
        if(insideShell)
        {
            marchStart = 0.0;
            marchEnd = (rayDir.y >= 0.0) ? tTop : tBottom;
        }
        else
        {
            marchStart = max(min(tBottom, tTop), 0.0);
            marchEnd = max(tBottom, tTop);
        }
    }
    float totalDist = min(marchEnd - marchStart, maxDist - marchStart);
    gHpDebugInfo = vec4(marchStart, totalDist, 0.0, maxDist);
    if(totalDist <= 0.0) return vec4(0.0, 0.0, 0.0, 1.0);

    float stepLargeRaw = totalDist / max(float(cfg.viewSteps), 1.0);
    float stepLargeNearCap = slabThickness * 0.085;
    float stepLargeFarCap = slabThickness * 0.65;
    float farDistRef = 50000.0;
    vec3 inScattering = vec3(0.0);
    float transmittance = 1.0;
    float firstStep = min(stepLargeRaw, stepLargeNearCap);
    float dist = cfg.jitter * firstStep;
    float hit = maxDist;
    float meanDistAccum = 0.0;
    float meanDistWeight = 0.0;

    float cosAngle = dot(rayDir, cfg.sunDir);
    float fwdG = clamp(cfg.forwardEcc, 0.0, 0.86);
    float bwdG = clamp(cfg.backwardEcc, 0.0, 0.65);
    float phaseMix = clamp(cfg.phaseMix, 0.15, 0.45);
    float phaseFunction[3];
    for(int o = 0; o < 3; ++o)
    {
        float ecc = pow(clamp(cfg.msEccentricity, 0.1, 1.0), float(o));
        float phase = mix(hpHG(cosAngle, fwdG * ecc),
            hpHG(cosAngle, -bwdG * ecc), phaseMix);
        phaseFunction[o] = max(phase, cfg.phaseFloor);
    }
    float silverG = clamp(mix(0.84, 0.93, clamp(cfg.silverSpread, 0.0, 1.5) * 0.65), 0.75, 0.94);
    float silverPhase = hpHG(cosAngle, silverG);

    int iterCount = 0;
    int maxIter = clamp(cfg.viewSteps, 8, 256) * 2;

    int lightStride = int(clamp(cfg.lightReuse, 1.0, 8.0));
    vec3 cachedLum = vec3(0.0);
    float cachedLightOD = 0.0;
    float cachedPhiFwd = 0.0;
    int lightTick = 0;

    bool inCloud = false;
    int emptyFineCount = 0;
    float prevCoarseStep = 0.0;

    while(dist < totalDist && iterCount < maxIter)
    {
        iterCount++;

        float distNorm = hpSaturate(dist / farDistRef);
        float absDist = marchStart + dist;
        float viewCap = max(absDist * 0.125, 16.0);
        float slabCap = mix(stepLargeNearCap, stepLargeFarCap, distNorm * distNorm);
        float stepLarge = min(stepLargeRaw, min(viewCap, slabCap));
        float stepSmall = stepLarge * mix(0.42, 0.60, distNorm);

        vec3 pos = rayOriginPlanet + absDist * rayDir;
        vec3 posAlt = pos - vec3(0.0, cfg.planetRadius, 0.0);

        if(!inCloud)
        {
            // Fast 2D weather pre-check skips 3D volume reads across clear blue sky.
            if(hpWeatherEmptyFast(posAlt, cfg, weatherTex))
            {
                prevCoarseStep = stepLarge;
                dist += stepLarge;
                continue;
            }
            float simpleDensity; float simpleLocalHeight;
            hpEvaluateCloudProperties(posAlt, slabThickness, 1.0, true, cfg,
                weatherTex, baseTex, erosionTex, simpleDensity, simpleLocalHeight);
            if(simpleDensity <= 0.001)
            {
                prevCoarseStep = stepLarge;
                dist += stepLarge;
                continue;
            }
            // Backtrack to locate exact cloud surface and prevent coarse-step slicing rings.
            if(prevCoarseStep > 0.0)
            {
                dist = max(0.0, dist - 0.65 * prevCoarseStep);
                absDist = marchStart + dist;
                pos = rayOriginPlanet + absDist * rayDir;
                posAlt = pos - vec3(0.0, cfg.planetRadius, 0.0);
            }
            inCloud = true;
            emptyFineCount = 0;
            prevCoarseStep = 0.0;
            lightTick = 0;
        }

        float detailMip = clamp(log2(max(absDist * 0.00022, 1.0)), 0.0, 2.5);
        float fullDensity; float fullLocalHeight;
        hpEvaluateCloudProperties(posAlt, slabThickness, detailMip, false, cfg,
            weatherTex, baseTex, erosionTex, fullDensity, fullLocalHeight);

        if(fullDensity > 0.001)
        {
            emptyFineCount = 0;
            hit = min(hit, absDist);
            float twd = transmittance * fullDensity;
            meanDistAccum += absDist * twd;
            meanDistWeight += twd;

            float relDist = dist / max(totalDist, 1.0);
            int adaptiveStride = lightStride
                + (absDist > 8000.0 ? 1 : 0)
                + (absDist > 22000.0 ? 1 : 0)
                + (transmittance < 0.15 ? 1 : 0);
            if(lightTick <= 0)
            {
                HPCloudConfig lightCfg = cfg;
                lightCfg.lightSteps = absDist > 22000.0 ? min(cfg.lightSteps, 4)
                    : (absDist > 10000.0 ? min(cfg.lightSteps, 5) : cfg.lightSteps);
                cachedLum = hpEvaluateSunLuminance(posAlt, fullLocalHeight, lightCfg,
                    lightCfg.sunColor, phaseFunction, weatherTex, baseTex, erosionTex,
                    cachedLightOD, cachedPhiFwd);
                lightTick = adaptiveStride;
            }
            --lightTick;
            hpEvaluateCloudStep(fullDensity, fullLocalHeight,
                slabThickness, hpNormalizedHeight(pos, cfg), posAlt,
                stepSmall, relDist, cachedLum, cachedLightOD, cachedPhiFwd,
                cfg, rayDir, silverPhase, inScattering, transmittance);

            if(transmittance < max(cfg.minTransmittance, 0.025))
            {
                transmittance = 0.0;
                break;
            }
        }
        else
        {
            emptyFineCount++;
            if(emptyFineCount >= 2)
            {
                inCloud = false;
                emptyFineCount = 0;
                prevCoarseStep = 0.0;
                dist += stepLarge;
                continue;
            }
        }
        dist += stepSmall;
    }

    hitDistance = hit;
    meanDistance = (meanDistWeight > 0.0) ? meanDistAccum / meanDistWeight : hit;
    gHpDebugInfo.z = float(iterCount);
    gHpDebugInfo.w = hit;
    return vec4(inScattering, transmittance);
}

#ifdef V5_NOISE_BAKER

vec2 hpHash22(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}
float hpHash13(vec3 p3in)
{
    vec3 p3 = fract(p3in * 0.1031);
    p3 += dot(p3, p3.zyx + 31.32);
    return fract((p3.x + p3.y) * p3.z);
}

float hpWorley2D(vec2 p, float tile)
{
    vec2 cell = floor(p);
    vec2 f = fract(p);
    float best = 8.0;
    for(int y = -1; y <= 1; ++y)
    for(int x = -1; x <= 1; ++x)
    {
        vec2 o = vec2(float(x), float(y));
        vec2 jitter = hpHash22(mod(cell + o, vec2(tile)) + vec2(19.19, 7.7));
        vec2 d = o + jitter - f;
        best = min(best, dot(d, d));
    }
    return 1.0 - sqrt(best);
}

float hpValue2D(vec2 p, float tile)
{
    vec2 cell = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hpHash13(vec3(mod(cell, vec2(tile)) + 19.19, 7.7));
    float b = hpHash13(vec3(mod(cell + vec2(1, 0), vec2(tile)) + 19.19, 7.7));
    float c = hpHash13(vec3(mod(cell + vec2(0, 1), vec2(tile)) + 19.19, 7.7));
    float d = hpHash13(vec3(mod(cell + vec2(1, 1), vec2(tile)) + 19.19, 7.7));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

vec3 hpHash33(vec3 p);

float hpWorley3D(vec3 p, vec3 tile)
{
    vec3 cell = floor(p);
    vec3 f = fract(p);
    float best = 8.0;
    for(int z = -1; z <= 1; ++z)
    for(int y = -1; y <= 1; ++y)
    for(int x = -1; x <= 1; ++x)
    {
        vec3 o = vec3(float(x), float(y), float(z));
        vec3 c = mod(cell + o, tile) + 19.19;
        vec3 jitter = hpHash33(c) * 0.5 + 0.5;
        vec3 d = o + jitter - f;
        best = min(best, dot(d, d));
    }
    return 1.0 - sqrt(best);
}

vec3 hpHash33(vec3 p)
{
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yxx) * p.zyx) * 2.0 - 1.0;
}
float hpPerlin3D(vec3 p, vec3 tile)
{
    vec3 cell = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float n000 = dot(hpHash33(mod(cell + vec3(0, 0, 0), tile) + 19.19), f - vec3(0, 0, 0));
    float n100 = dot(hpHash33(mod(cell + vec3(1, 0, 0), tile) + 19.19), f - vec3(1, 0, 0));
    float n010 = dot(hpHash33(mod(cell + vec3(0, 1, 0), tile) + 19.19), f - vec3(0, 1, 0));
    float n110 = dot(hpHash33(mod(cell + vec3(1, 1, 0), tile) + 19.19), f - vec3(1, 1, 0));
    float n001 = dot(hpHash33(mod(cell + vec3(0, 0, 1), tile) + 19.19), f - vec3(0, 0, 1));
    float n101 = dot(hpHash33(mod(cell + vec3(1, 0, 1), tile) + 19.19), f - vec3(1, 0, 1));
    float n011 = dot(hpHash33(mod(cell + vec3(0, 1, 1), tile) + 19.19), f - vec3(0, 1, 1));
    float n111 = dot(hpHash33(mod(cell + vec3(1, 1, 1), tile) + 19.19), f - vec3(1, 1, 1));
    return mix(mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y),
               mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y), u.z);
}

// 2D **梯度**噪声（Perlin）。天气图专用。
// 为什么不能用 hpValue2D：value noise 的格胞内部是"平顶平台"，被 coverage 阈值
// 一切出来，等值线就是**轴对齐的方块** —— 云于是挤在一格一格的矩形区域里，
// 而且怎么调参数都改不掉形状（方块是晶格决定的，不是参数决定的）。
// 梯度噪声每个格胞内是线性斜坡、格点处为 0，等值线是曲线的，阈值化后没有平台。
// 平铺仍然成立：格点索引用 mod(cell, tile)。
float hpGradient2D(vec2 p, float tile)
{
    vec2 cell = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    vec2 g00 = hpHash22(mod(cell + vec2(0.0, 0.0), vec2(tile)) + 19.19) * 2.0 - 1.0;
    vec2 g10 = hpHash22(mod(cell + vec2(1.0, 0.0), vec2(tile)) + 19.19) * 2.0 - 1.0;
    vec2 g01 = hpHash22(mod(cell + vec2(0.0, 1.0), vec2(tile)) + 19.19) * 2.0 - 1.0;
    vec2 g11 = hpHash22(mod(cell + vec2(1.0, 1.0), vec2(tile)) + 19.19) * 2.0 - 1.0;
    float n00 = dot(g00, f - vec2(0.0, 0.0));
    float n10 = dot(g10, f - vec2(1.0, 0.0));
    float n01 = dot(g01, f - vec2(0.0, 1.0));
    float n11 = dot(g11, f - vec2(1.0, 1.0));
    return mix(mix(n00, n10, u.x), mix(n01, n11, u.x), u.y);
}

// ---- mode 0: weather 2D (512^2) ----
// Rich domain-warped cumulus fronts, clear blue-sky gaps, storm cell distribution, and height variation.
vec4 hpRenderWeather(vec2 uv)
{
    const float tile = 4.0;
    vec2 p = uv * tile;
    vec2 warp = vec2(
        hpGradient2D(p * 1.0 + vec2(3.7, 11.1), 4.0),
        hpGradient2D(p * 1.0 + vec2(17.3, 5.9), 4.0)) * 0.85;
    vec2 q = p + warp;
    float c1 = hpGradient2D(q * 0.5 + vec2(3.1, 7.2), 2.0) * 0.5 + 0.5;
    float c2 = hpGradient2D(q * 1.0 + vec2(17.9, 2.3), 4.0) * 0.5 + 0.5;
    float c3 = hpGradient2D(q * 2.0 + vec2(41.3, 19.7), 8.0) * 0.5 + 0.5;
    float c4 = hpGradient2D(q * 4.0 + vec2(71.3, 53.1), 16.0) * 0.5 + 0.5;
    float cov = 0.34 * c1 + 0.32 * c2 + 0.22 * c3 + 0.12 * c4;
    cov = clamp((cov - 0.5) * 2.5 + 0.60, 0.0, 1.0);

    float typeRaw = 0.65 * c1 + 0.35 * c2;
    float type = clamp((typeRaw - 0.32) * 1.65, 0.0, 1.0);
    float blob = 0.62 * hpWorley2D(q * 1.0, 4.0)
               + 0.38 * hpWorley2D(q * 2.0 + vec2(7.31, 4.19), 8.0);
    float scMask = smoothstep(0.45, 0.82, blob);
    float topVar = clamp(
        0.45 * (hpGradient2D(p * 1.0 + vec2(51.7, 12.3), 4.0) * 0.5 + 0.5) +
        0.35 * cov +
        0.20 * (hpGradient2D(p * 2.0 + vec2(133.7, 29.1), 8.0) * 0.5 + 0.5),
        0.0, 1.0);
    return vec4(cov, type, scMask, topVar);
}

// ---- mode 1: base 3D (128^3, max tile 24 = 5.3 voxels/cell -> Nyquist-safe billows) ----
vec4 hpRenderBase(vec3 coord)
{
    const vec3 tile = vec3(6.0, 6.0, 6.0);
    vec3 p = coord * tile;
    float perlin = 0.50 * hpPerlin3D(p, tile)
                 + 0.32 * hpPerlin3D((p * 2.0 + 11.7).zyx, tile * 2.0)
                 + 0.18 * hpPerlin3D((p * 4.0 + 23.1).yxz, tile * 4.0);
    perlin = perlin * 0.5 + 0.5;
    float wf = 0.55 * hpWorley3D(p, tile)
             + 0.30 * hpWorley3D((p * 2.0 + 5.7).zyx, tile * 2.0)
             + 0.15 * hpWorley3D((p * 4.0 + 13.3).yxz, tile * 4.0);
    float pw = wf + perlin * (1.0 - wf);
    pw = clamp((pw - 0.34) / 0.66, 0.0, 1.0);
    return vec4(pw, wf, perlin, 1.0);
}

// ---- mode 2: erosion 3D (64^3, integer tiles 4/8/16 >= 4 voxels/cell -> zero sand aliasing) ----
vec4 hpRenderDetail(vec3 coord)
{
    const vec3 tile = vec3(4.0, 4.0, 4.0);
    vec3 p4 = coord * tile;
    float pLo  = hpPerlin3D(p4, tile) * 0.5 + 0.5;
    float pMid = hpPerlin3D((p4 * 2.0 + 7.3).zyx, tile * 2.0) * 0.5 + 0.5;
    float pHi  = hpPerlin3D((p4 * 4.0 + 17.9).yxz, tile * 4.0) * 0.5 + 0.5;
    float wispyLo = clamp(0.65 * pLo + 0.35 * pMid, 0.0, 1.0);
    float wispyHi = clamp(0.60 * pMid + 0.40 * pHi, 0.0, 1.0);

    // Inverted distance (1 - Worley) carves crisp crevices while preserving bulbous puffs.
    float wLo  = clamp(1.0 - hpWorley3D(p4 + vec3(1.1, 2.3, 0.7), tile), 0.0, 1.0);
    float wMid = clamp(1.0 - hpWorley3D((p4 * 2.0 + 9.1).zyx, tile * 2.0), 0.0, 1.0);
    float wHi  = clamp(1.0 - hpWorley3D((p4 * 4.0 + 13.9).yxz, tile * 4.0), 0.0, 1.0);
    float billowyLo = clamp(0.62 * wLo + 0.38 * wMid, 0.0, 1.0);
    float billowyHi = clamp(0.62 * wMid + 0.38 * wHi, 0.0, 1.0);
    return vec4(wispyLo, wispyHi, billowyLo, billowyHi);
}
#endif
#endif
