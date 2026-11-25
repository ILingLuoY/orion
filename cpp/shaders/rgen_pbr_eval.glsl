

int decodeF0MetalCode(
    float f0Metal
)
{
    return int(
        round(
            clamp(
                f0Metal,
                0.0,
                1.0
            ) *
            255.0
        )
    );
}


float decodeMetallicFromCode(
    int code
)
{
    return code >= 230
        ? 1.0
        : 0.0;
}


vec3 complexIorToF0(
    vec3 n,
    vec3 k
)
{
    vec3 nMinus1 =
        n - vec3(1.0);

    vec3 nPlus1 =
        n + vec3(1.0);

    return
        (
            nMinus1 * nMinus1 +
            k * k
        ) /
        (
            nPlus1 * nPlus1 +
            k * k
        );
}


vec3 decodeBaseF0(
    int code,
    vec3 albedo
)
{
    if(code <= 229)
    {
        return vec3(
            float(code) / 255.0
        );
    }

    if(code == 230)
    {
        // Iron
        return complexIorToF0(
            vec3(
                2.9114,
                2.9497,
                2.5845
            ),
            vec3(
                3.0893,
                2.9318,
                2.7670
            )
        );
    }

    if(code == 231)
    {
        // Gold
        return complexIorToF0(
            vec3(
                0.18299,
                0.42108,
                1.3734
            ),
            vec3(
                3.4242,
                2.3459,
                1.7704
            )
        );
    }

    if(code == 232)
    {
        // Aluminum
        return complexIorToF0(
            vec3(
                1.3456,
                0.96521,
                0.61722
            ),
            vec3(
                7.4746,
                6.3995,
                5.3031
            )
        );
    }

    if(code == 233)
    {
        // Chrome
        return complexIorToF0(
            vec3(
                3.1071,
                3.1812,
                2.3230
            ),
            vec3(
                3.3314,
                3.3291,
                3.1350
            )
        );
    }

    if(code == 234)
    {
        // Copper
        return complexIorToF0(
            vec3(
                0.27105,
                0.67693,
                1.3164
            ),
            vec3(
                3.6092,
                2.6248,
                2.2921
            )
        );
    }

    if(code == 235)
    {
        // Lead
        return complexIorToF0(
            vec3(
                1.9100,
                1.8300,
                1.4400
            ),
            vec3(
                3.5100,
                3.4000,
                3.1800
            )
        );
    }

    if(code == 236)
    {
        // Platinum
        return complexIorToF0(
            vec3(
                2.3757,
                2.0847,
                1.8453
            ),
            vec3(
                4.2655,
                3.7153,
                3.1365
            )
        );
    }

    if(code == 237)
    {
        // Silver
        return complexIorToF0(
            vec3(
                0.15943,
                0.14512,
                0.13547
            ),
            vec3(
                3.9291,
                3.1900,
                2.3808
            )
        );
    }

    // 255 = generic metal, use albedo as F0.
    // 238..254 先按 generic metal 处理。
    return albedo;
}


// =====================================================
// RR-6.2 - Specular Albedo approximation
// =====================================================

vec3 rrEnvBRDFApprox2(
    vec3 specularColor,
    float alpha,
    float NoV
)
{
    NoV = abs(NoV);

    float Xx = 1.0;
    float Xy = NoV;
    float Xz = NoV * NoV;
    float Xw = NoV * Xz;

    float Yx = 1.0;
    float Yy = alpha;
    float Yw = alpha * alpha * alpha;

    vec2 m1x =
        vec2(
            0.99044 * Xx - 1.28514 * Xy,
            1.29678 * Xx - 0.755907 * Xy
        );

    float biasDen =
        dot(
            vec3(
                1.0 * Xx + 2.92338 * Xy + 59.4188 * Xw,
                20.3225 * Xx - 27.0302 * Xy + 222.592 * Xw,
                121.563 * Xx + 626.13 * Xy + 316.627 * Xw
            ),
            vec3(
                Yx,
                Yy,
                Yw
            )
        );

    float bias =
        dot(
            m1x,
            vec2(
                Yx,
                Yy
            )
        ) /
        max(
            biasDen,
            1.0e-6
        );

    vec2 m3x =
        vec2(
            0.0365463 * Xx + 3.32707 * Xy,
            9.0632 * Xx - 9.04756 * Xy
        );

    float scaleDen =
        dot(
            vec3(
                1.0 * Xx + 3.59685 * Xz - 1.36772 * Xw,
                9.04401 * Xx - 16.3174 * Xz + 9.22949 * Xw,
                5.56589 * Xx + 19.7886 * Xz - 20.2123 * Xw
            ),
            vec3(
                Yx,
                Yy,
                Yw
            )
        );

    float scale =
        dot(
            m3x,
            vec2(
                Yx,
                Yy
            )
        ) /
        max(
            scaleDen,
            1.0e-6
        );

    bias *=
        clamp(
            specularColor.g * 50.0,
            0.0,
            1.0
        );

    return
        specularColor *
        max(
            scale,
            0.0
        ) +
        vec3(
            max(
                bias,
                0.0
            )
        );
}


vec3 fresnelSchlick(
    float cosTheta,
    vec3 F0
)
{
    // Exact x^5 expansion: same series as pow(x,5), off the SFU pipe and
    // slightly more accurate than the GPU pow approximation.
    vec3 x = vec3(1.0) - vec3(cosTheta);
    vec3 x2 = x * x;
    return
        F0 +
        (
            vec3(1.0) - F0
        ) *
        ((x2 * x2) * x);
}


// =====================================================
// PBR-1B - Burley diffuse angular / roughness response
//
// Reimplemented for this Vulkan RT path from the same
// Disney Burley model used by the reference shader.
//
// IMPORTANT:
// - f90 base = 0.5
// - roughness factor = 2.0
// - Schlick exponent = 5
// - 1/PI is intentionally NOT applied here.
//
// This keeps the project's existing diffuse energy scale
// and only imports the Burley angular/roughness response.
// =====================================================
float evaluateBurleyDiffuseAngular(
    vec3 N,
    vec3 V,
    vec3 L,
    float roughness
)
{
    vec3 H =
        normalize(
            V + L
        );


    float LdotH =
        clamp(
            dot(L, H),
            0.0,
            1.0
        );


    float NdotL =
        clamp(
            dot(N, L),
            0.0,
            1.0
        );


    float NdotV =
        clamp(
            dot(N, V),
            0.0,
            1.0
        );


    float f90 =
        0.5 +
        2.0 *
        roughness *
        LdotH *
        LdotH;


    float nl = 1.0 - NdotL;
    float nl2 = nl * nl;
    float lightScatter =
        1.0 +
        (f90 - 1.0) *
        ((nl2 * nl2) * nl);

    float nv = 1.0 - NdotV;
    float nv2 = nv * nv;
    float viewScatter =
        1.0 +
        (f90 - 1.0) *
        ((nv2 * nv2) * nv);


    return
        NdotL *
        lightScatter *
        viewScatter;
}


// =====================================================
// PBR-2 - Reference-shader rough-specular reflection gate
//
// Active reference settings:
//   ENABLE_ROUGH_SPECULAR
//   TEXTURE_PBR_FORMAT = 1
//   ROUGHNESS_CLAMP = 2
//
// Original material logic, reimplemented for this RT path:
//   x = saturate(perceptualSmoothness * 2.5 - 0.85)
//   reflectionStrength = curveTop(x)
//   curveTop(x) = 1 - (x - 1)^2
//   LabPBR metal code >= 230 forces reflectionStrength to 1.
//
// For opaque specular tracing the reference shader uses this as
// a tracing gate (> 0), not as an extra multiplier on the final
// reflection radiance. We keep the same behavior here.
// =====================================================
float evaluatePBR2ReflectionStrength(
    float encodedRoughness,
    int f0MetalCode
)
{
    // Current payload stores:
    //   roughness = (1 - perceptualSmoothness)^2
    // Recover the original LabPBR smoothness before the later
    // shading roughness clamp is applied.
    float perceptualSmoothness =
        1.0 -
        sqrt(
            clamp(
                encodedRoughness,
                0.0,
                1.0
            )
        );


    float x =
        clamp(
            perceptualSmoothness * 2.5 - 0.85,
            0.0,
            1.0
        );


    float shifted =
        x - 1.0;


    float reflectionStrength =
        1.0 - shifted * shifted;


    if(f0MetalCode >= 230)
    {
        reflectionStrength =
            1.0;
    }


    return
        clamp(
            reflectionStrength,
            0.0,
            1.0
        );
}


float distributionGGX(
    vec3 N,
    vec3 H,
    float roughness
)
{
    float a =
        roughness * roughness;

    float a2 =
        a * a;

    float NdotH =
        max(
            dot(N, H),
            0.0
        );

    float NdotH2 =
        NdotH * NdotH;

    float denom =
        NdotH2 *
        (
            a2 - 1.0
        ) +
        1.0;

    return
        a2 /
        max(
            PI * denom * denom,
            1.0e-6
        );
}


float geometrySchlickGGX(
    float NdotX,
    float roughness
)
{
    float r =
        roughness + 1.0;

    float k =
        (r * r) / 8.0;

    return
        NdotX /
        (
            NdotX * (1.0 - k) + k
        );
}


float geometrySmith(
    vec3 N,
    vec3 V,
    vec3 L,
    float roughness
)
{
    float NdotV =
        max(
            dot(N, V),
            0.0
        );

    float NdotL =
        max(
            dot(N, L),
            0.0
        );

    float ggxV =
        geometrySchlickGGX(
            NdotV,
            roughness
        );

    float ggxL =
        geometrySchlickGGX(
            NdotL,
            roughness
        );

    return
        ggxV * ggxL;
}

float visibilitySmithGGXCorrelated(
    float NdotV,
    float NdotL,
    float roughness
)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float ggxV = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    float ggxL = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return 0.5 / max(ggxV + ggxL, 1.0e-5);
}

// =====================================================
// 2026-10-16 多散射能量补偿（Kulla-Conty 近似，免 LUT）
//
// 单散射 GGX 的遮蔽项把"被微表面挡回来的能量"直接丢掉，粗糙材质的镜面
// 因此偏暗（Furnace 测试不守恒）。补偿系数 = 1 + F0·(1/Eavg - 1)
// （Filament《Physically Based Rendering in Filament》多散射补偿的解析
// 形式，其 dfg.y 即 Eavg）。本仓无 DFG LUT，Eavg 由 MC 积分自拟口径：
// height-correlated exact Smith GGX（与 Filament DFG LUT 同口径）、
// α = 感知粗糙度²、11 个粗糙度点 × (1200 外 × 2000 内) 样本，四次多项式
// 最小二乘拟合，最大残差 0.0017（数据与拟合脚本在会话记录里）。
// 零粗糙度 Eavg=1 ⇒ 光滑材质逐位不受影响；介电质 F0≈0.04 ⇒ 粗糙端
// 最大抬升 ~2%（物理上正确——粗糙介电镜面同样丢能量）。
// 回退：-DGI_METAL_ENERGY_COMP=0 ⇒ Eavg 恒 1 ⇒ 逐位旧档。
// =====================================================
#ifndef GI_METAL_ENERGY_COMP
#define GI_METAL_ENERGY_COMP 1
#endif

#if GI_METAL_ENERGY_COMP
float ggxEavgApprox(
    float perceptualRoughness
)
{
    float r2 =
        perceptualRoughness * perceptualRoughness;

    return
        1.00068
        - 0.16407 * perceptualRoughness
        - 1.64700 * r2
        + 1.88283 * perceptualRoughness * r2
        - 0.66608 * r2 * r2;
}
#else
float ggxEavgApprox(
    float perceptualRoughness
)
{
    return 1.0;
}
#endif

vec3 multiscatterEnergyCompensation(
    vec3 f0,
    float perceptualRoughness
)
{
    return
        vec3(1.0) +
        f0 *
        (1.0 / ggxEavgApprox(perceptualRoughness) - 1.0);
}

// Primary-water direct sun response.  The visibility query is intentionally
// shared with opaque sunlight: water under a roof, cliff, or opaque canopy
// receives no GGX sun glint.  Water-body scattering remains solely on the
// established transmitted-water path, where it has real optical distance.
//
// 2026-10-22: the caller now owns that one visibility trace (sunTransmittance)
// and hands the same result to the water-mirror sky gate, so a shadowed water
// pixel still costs a single sun-transmittance march.
// 2026-11-20 Remix 核心对齐（用户定档："除水面波浪外按 RTX Remix 来，
// 只抄核心不换外观"）。
//
// Remix 的水面**没有粗糙度**：TranslucentSurfaceMaterial 的
// translucentSurfaceMaterialInteractionGetLobeInformation 把
// specularReflectionDirac 写死 true（注释：除非以后给玻璃加 roughness），
// 反射增益就是外表面 Fresnel（brdf.slangh:226
// F = F0 + (1-F0)*(1-v·h)^5，水的 F0≈0.02），反射只吃场景辐亮度、
// 不采任何焦散图。
//
// 我们旧实现这里是 GGX(perceptualRoughness=0.12) 的宽高光瓣：D 峰值
// 1/(π α²) ≈ 1.5e3，掠射时 F→1，而且它还叠在镜面反射 walker 已经带回的
// 日盘/日冕之上（同一份太阳算了两遍）⇒ 近水那片"莫名其妙的反射白光"。
// 拨回 0 可逐位回旧 GGX 档做 A/B。
const int WATER_SURFACE_SUN_MODEL = 1;
const float WATER_SURFACE_SUN_ROUGHNESS = 0.12;
// dirac 瓣本身是 delta，单样本会退化成闪烁，所以给一个有限张角：
// ±12°（cos≈0.978），与 rmiss 里日冕 pow(sunDot,64) 的 ±8° 同量级，
// 保证波面把太阳打散之后仍能连成一条而不是散点。
const float WATER_SURFACE_SUN_APERTURE_COS = 0.978;

vec3 evaluatePrimaryWaterSunLighting(
    vec3 waterSurfacePosition,
    vec3 waterNormal,
    vec3 viewDirection,
    vec3 sunDirection,
    vec3 sunTransmittance
)
{
    const float WATER_F0 = 0.0200593;

    vec3 N = normalize(waterNormal);
    vec3 V = normalize(viewDirection);
    vec3 L = normalize(sunDirection);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);

    // This is an explicit sun term, not moonlight or sky reflection.
    if(L.y <= 0.0 || NdotL <= 0.0 || NdotV <= 0.0)
    {
        return vec3(0.0);
    }

    // Trace result computed by the caller from this same surface point.  It is
    // zero behind opaque blockers and retains any existing glass filtering.
    if(max(sunTransmittance.r, max(sunTransmittance.g, sunTransmittance.b)) <= 0.0)
    {
        return vec3(0.0);
    }

    vec3 sunRadiance =
        getTimeDrivenSkyLightColor() *
        3.2 *
        getTimeDrivenSkyLightRadianceScale() *
        sunTransmittance *
        c3CloudSunTransmittance(waterSurfacePosition + N * 0.035, L);

    vec3 H = normalize(V + L);
    float VdotH = max(dot(V, H), 0.0);
    float F = fresnelSchlickScalar(VdotH, WATER_F0);

    if(WATER_SURFACE_SUN_MODEL == 1)
    {
        // Remix dirac 反射瓣：镜面方向落在太阳（+日冕）张角内才可见，
        // 权重就是 Fresnel，没有 D/G 项。
        vec3 R = normalize(reflect(-V, N));
        float sunAlignment = dot(R, L);
        if(sunAlignment <= WATER_SURFACE_SUN_APERTURE_COS)
        {
            return vec3(0.0);
        }
        float aperture = smoothstep(
            WATER_SURFACE_SUN_APERTURE_COS,
            1.0,
            sunAlignment);
        return sunRadiance * F * aperture;
    }

    float D = distributionGGX(N, H, WATER_SURFACE_SUN_ROUGHNESS);
    float G = geometrySmith(N, V, L, WATER_SURFACE_SUN_ROUGHNESS);
    vec3 sunSpecular =
        sunRadiance *
        ((D * G * F) / max(4.0 * NdotV * NdotL, 1.0e-4)) *
        NdotL;

    return sunSpecular;
}


// =====================================================
// Glass is a mirror and a filter -- not an emitter (2026-09-25)
//
// An earlier revision gave a lit pane an area-light term of its own:
// sunRadiance * |N.L| / PI, added at every pane hit so a room would pick up
// the daylight arriving through the sheet.  That model is wrong for smooth
// glass, and it is removed here.
//
// A pane is not a Lambertian emitter.  It re-radiates along the MIRROR
// direction and not over a hemisphere: the daylight a room receives through a
// window is the OUTDOOR WORLD seen straight through the sheet -- sunlit
// ground, a wall, the sky -- carried by the regular-transmission lobe.  The
// bounce traversal gets exactly that for free as soon as it continues through
// the pane with the dye applied (see the glass branch in the bounce loop).
// An emitter term would count the same daylight a second time, from a
// direction the glass never sends it.
//
// What the pane does own is the ~4% specular reflection of its two surfaces.
// The same bounce branch now handles that with the same thin-sheet factor
// pair the camera path uses, so reflection and transmission of one pane
// cannot drift between the two paths.
// =====================================================
// =============================================================================
// BSDF 标定表（唯一来源，2026-10-29 合并）
//
// 为什么集中：这些数此前散落在一个太阳求值点、一个动态光源路径、两个本地光
// 循环和两处 GI 吞吐里。改其中一处 BRDF 就要手工回来重标另外几处 —— 历史上
// 已经发生过漏改（PBR-1B 把 Lambert 换成 Burley 时只保留旧的 1.2，注释里写着
// "preserve that exact baseline"）。
//
// 现在**只在这里改**：任何求值点禁止再写裸数字。
//
// ⚠ 它们不是物理量，是美术标定基线。改数值等于改画面亮度，必须做 A/B。
// =============================================================================
const float BSDF_SUN_DIFFUSE_SCALE         = 1.2;   // 太阳直射漫反射（helper 路径）
const float BSDF_LOCAL_DIFFUSE_SCALE       = 0.92;  // 本地光 / 动态光源漫反射
const float BSDF_LOCAL_SPECULAR_SCALE      = 0.28;  // 本地光 / 动态光源镜面
const float BSDF_LOCAL_DIFFUSE_SOFTEN      = 0.45;  // 保留历史值（当前无调用点）
const float BSDF_LOCAL_MIN_SPEC_ROUGHNESS  = 0.35;  // 动态光源镜面波瓣展宽下限
const float BSDF_GI_THROUGHPUT_SCALE       = 0.7;   // GI 每跳吞吐
const float BSDF_GI_RESULT_SCALE           = 1.26;  // 间接光结果倍率

struct BsdfTerms
{
    vec3 F;         // Fresnel(VdotH)
    vec3 kD;        // (1 - F) * (1 - metallic)
    vec3 specular;  // D * G * F / (4 * NdotV * NdotL)，可选多散射补偿
};

// =============================================================================
// 唯一的 GGX 微表面求值点（2026-10-29 合并）
//
// 合并前这段 D/G/F 在 7 处逐字重复：太阳 helper、动态光源、本地光面采样、
// 主命中太阳、远场 LOD、水面/玻璃镜面、GI 弹射。任何一处改了 G 的近似或
// 分母下限，其余六处都会静默分叉。
//
// 返回**未乘**光源、未乘余弦、未乘美术标定的原始项。diffuse 模型由调用点决定：
//   Lambert        : terms.kD * albedo * NdotL
//   Burley         : terms.kD * albedo * evaluateBurleyDiffuseAngular(...)  （已含 NdotL）
// 这三种模型并存是历史标定，统一它们等于改画面，不属于本次重构。
// =============================================================================
BsdfTerms evalBSDFTerms(
    vec3 N,
    vec3 V,
    vec3 L,
    vec3 f0,
    float metallic,
    float roughness,
    float NdotV,
    bool multiscatter
)
{
    // NdotV 必须由调用点传入：合并前各处的下限不同（helper / 本地光 / 远场是
    // 0.0，动态光与主命中太阳是 1.0e-4）。统一成任一个都会让掠射面整块变亮或
    // 变暗，而相邻方块法线不同 ⇒ 方块之间出现分界线。
    float NdotL =
        max(dot(N, L), 0.0);

    vec3 H =
        normalize(V + L);

    float VdotH =
        max(dot(V, H), 0.0);

    vec3 F =
        fresnelSchlick(VdotH, f0);

    float D =
        distributionGGX(N, H, roughness);

#ifndef BSDF_HEIGHT_CORRELATED_SMITH
#define BSDF_HEIGHT_CORRELATED_SMITH 1
#endif
#if BSDF_HEIGHT_CORRELATED_SMITH
    float visTerm =
        visibilitySmithGGXCorrelated(NdotV, NdotL, roughness);

    vec3 specular =
        D * visTerm * F;
#else
    float G =
        geometrySmith(N, V, L, roughness);

    vec3 specular =
        (D * G * F) /
        max(4.0 * NdotV * NdotL, 1.0e-4);
#endif

    if(multiscatter)
    {
        specular *=
            multiscatterEnergyCompensation(f0, roughness);
    }

    BsdfTerms terms;
    terms.F = F;
    terms.kD = (vec3(1.0) - F) * (1.0 - metallic);
    terms.specular = specular;
    return terms;
}
