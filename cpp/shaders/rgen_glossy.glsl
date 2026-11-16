// ============================================================================
// 2026-11-10：glossy 反射链的俄罗斯轮盘 + 层数 LOD
//
// 实测依据（[PERF L10 GPU] + ORION_PERF_ABLATION=alt:0,3,2,1,10）：
//   base trace=16.12ms / noGlossy 9.64ms（-6.48，即反射链占 40%）
//   noGi -1.11ms / noRtao +0.09ms / shadow1of2 -0.01ms
// 而这条路径此前是**确定性跑满**：全文件没有 roulette（GI 路径有），
// 层数只受用户设置 mirrorBounceLimit(1..8) 约束，与表面粗糙度/距离无关。
//
// 两个开关（都在 CameraUBO 里，运行时改，不需重编）：
//   cam.giRayParams.w        > 0.5 ⇒ RR 生效      （ORION_GLOSSY_RR，默认开）
//   cam.pomSecondaryParams.y > 0.5 ⇒ 层数 LOD 生效（ORION_GLOSSY_STEP_LOD，默认开）
// ============================================================================
uint orionGlossyRrSeed()
{
    return (uint(gl_LaunchIDEXT.x) * 1973u +
            uint(gl_LaunchIDEXT.y) * 9277u +
            uint(cam.frameCount) * 26699u) | 1u;
}

// 私有 RNG：不动主采样流 state，避免改变已有噪声模式。
float orionGlossyRandNext(inout uint s)
{
    s = s * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    w = (w >> 22u) ^ w;
    return float(w) * 2.3283064365386963e-10;
}

bool orionGlossyRrEnabled()
{
    return cam.giRayParams.w > 0.5f;
}

bool orionGlossyStepLodEnabled()
{
    return cam.pomSecondaryParams.y > 0.5f;
}

vec3 tracePrimaryGlossyReflectionRadiance(
    vec3 primarySurfaceOrigin,
    vec3 startOrigin,
    vec3 startDirection,
    vec3 sunDirection,
    inout uint state,
    out float firstSpecularHitDistance
)
{
    firstSpecularHitDistance =
        65504.0;

    // 2026-10-29 消融档 25/26：反射射线的距离上限。
    // 动机见档 24（noLabPbrMath）的同局实测：只把 roughness 换成常量 0.5，
    // trace 就从 16.30 掉到 9.91 ⇒ 钱在"反射射线打到哪里"，不在射线数。
    // 这里只截 tMax，材质 / 方向 / 采样数一概不动；超距按 miss 处理（rmiss 给天空色），
    // 所以观感是"远处反射变空"，属探针专用。
    // 2026-10-30 默认：反射射线距离预算 96 格（原来是 10000 = 不限）。
    //
    // 依据同局 A/B（alt:0,25/26）：tMax=96 -> 1.85 ms，tMax=48 -> 1.95 ms，
    // 两者几乎相同 ⇒ 省下的钱来自「打空的射线不必走完整个 BVH」，
    // 而不是「打到远处几何」。96 格以内的反射完全正确。
    //
    // 回退：消融档 32（glossyBudgetOff）回到 10000；
    //       或把下面的 kGlossyRayBudget 改回 10000.0f。
    // 2026-10-30 回退：96 -> 10000。tMax 截断会让"本该打到远处几何"的反射
    // 回落到天空色，天空明显更亮 ⇒ 室内被整体抬亮（用户实机反馈）。
    // 这条副作用当初评估不足；收益 1.85 ms 先放弃，探针档 25/26/32 保留以便复测。
    const float kGlossyRayBudget = 10000.0;

    float glossyRayMaxDistance =
        (cam.perfAblationStage.x == 32u)
            ? 10000.0
            : kGlossyRayBudget;

    if(cam.perfAblationStage.x == 25u)
    {
        glossyRayMaxDistance = 48.0;
    }
    else if(cam.perfAblationStage.x == 26u)
    {
        glossyRayMaxDistance = 96.0;
    }
    // 2026-11-01 档 35：tMax=192（12 区块）。与 25/26 一起给出远场替换
    // （SSR / SSPT / 天空）能省多少钱的三段上限：
    //   26 vs base = 96 格以外全部的钱；26 vs 35 = 6–12 区块段（96–192 格）；
    //   35 vs base = 12 区块以外。口径同 25/26：只截 tMax，超距按 miss 回
    //   天空色，观感不正确是定义，探针专用。
    else if(cam.perfAblationStage.x == 35u)
    {
        glossyRayMaxDistance = 192.0;
    }

    const float WATER_IOR =
        1.33;

    const float WATER_F0 =
        0.0200593;

    const vec3 WATER_ABSORPTION =
        waterAbsorptionColor();
        const vec3 WATER_SCATTER_COLOR = waterScatterColor();

const float WATER_EXTINCTION_DISTANCE =
    16.0;

    const float RAY_BIAS =
        0.001;

    const int MAX_WATER_INTERFACE_STEPS =
    6;


// A3-4：Primary glossy 命中第一个 opaque 后，
// 最多允许 1 次额外 PBR indirect continuation。
const int MAX_OPAQUE_INDIRECT_CONTINUATIONS =
    1;

// 2026-10-03 镜厅（用户要求）：**纯镜面**命中（LabPBR 金属 + 粗糙度 < 0.05）走
// **确定性镜像续弹**（reflect 方向，无随机采样），层数由 UBO 控制（1..8，出厂
// 1 = 现状）。非镜面命中走原有随机续弹（上限不变）。最坏路径：
// 6 次水界面 + 8 层镜面续弹 + 1 次随机续弹 + 它的终点 ⇒ 16。
const int MIRROR_BOUNCE_HARD_CAP = 8;
const int MAX_PATH_TRACE_STEPS =
    MAX_WATER_INTERFACE_STEPS +
    MIRROR_BOUNCE_HARD_CAP +
    MAX_OPAQUE_INDIRECT_CONTINUATIONS +
    2;


vec3 throughput =
    vec3(1.0);


vec3 accumulatedRadiance =
    vec3(0.0);


vec3 currentOrigin =
    startOrigin;


vec3 currentDirection =
    normalize(
        startDirection
    );


bool insideWater =
    false;


int waterInterfaceStepCount =
    0;


// 2026-11-03：本像素在这条路径上已经走过的**水内光程**（格）。
// 只服务吸收项的浅水软化，与 transmissionInfo.waterPathLength（导给
// SSS/焦散的那份）互不影响。
float waterPathTravelled =
    0.0;

// 浅水区厚度（格）。与 18617 处那条"已确认 0~3 格保持高通透"同值；
// 那份声明在另一个作用域里、主走查取不到，所以这里自带一份。
const float kShallowWaterDistance =
    3.0;


int opaqueContinuationCount =
    0;


// 2026-10-03 镜厅：已完成的确定性镜像弹射层数与上限（UBO 水尾块保留槽 2）
int mirrorBounceCount =
    0;

int mirrorBounceLimit =
    clamp(int(cam.waterFftParams2.y + 0.5), 1, MIRROR_BOUNCE_HARD_CAP);

// 首次命中后按粗糙度/距离收紧的**有效**层数（见下方 pathStep==0 段）。
uint orionGlossyRrState = orionGlossyRrSeed();
int orionGlossyBounceLimit = mirrorBounceLimit;


for(
    int pathStep = 0;
    pathStep < MAX_PATH_TRACE_STEPS;
    ++pathStep
)
    {
        hit.hitDistance =
            1.0;


        hit.state = makeTraceState(RT_PURPOSE_FULL);

        // 2026-11-09：反射路径的视差是"每条 glossy 路径、每一步"都要付的固定税
        // （material_resolve 的 POM 判据里没有射线用途，最多 18 步 x 296 次
        //  texelFetch）。这里沿用 GI 那条同源的 state 位，命中侧走既有早退分支。
        // 关掉：ORION_POM_SECONDARY=0 ⇒ 逐位回到改动前。
        if(secondaryRayPomSkipEnabled())
        {
            hit.state |= RT_STATE_GI_NO_POM_BIT;
        }

        // RR：throughput 已衰减到可忽略时按概率终止，并对存活者除以 q 补偿
        // （无偏：只改方差，不改期望）。第一跳不抽签，保证主反射始终存在。
        if(orionGlossyRrEnabled() && pathStep > 0)
        {
            const float orionQ =
                clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.0, 1.0);

            if(orionQ < 0.02)
            {
                return accumulatedRadiance;
            }

            if(orionQ < 0.30)
            {
                if(orionGlossyRandNext(orionGlossyRrState) > orionQ)
                {
                    return accumulatedRadiance;
                }

                throughput /= orionQ;
            }
        }

#ifdef RT_ENABLE_SER
        // Perf-L10.8: same ray, same flags; SER only reschedules the hit shader.
        if(cam.perfSerAb.z != 0u)
        {
            serTraceRay(
                currentOrigin,
                0.001,
                currentDirection,
                glossyRayMaxDistance,
                secondaryTraverseFlags(),
                0xff,
                pathStep
            );
        }
        else
#endif
        {
            #ifdef ORION_RT_STAGE_COUNTERS
            rtCntRayIssued();
            #endif
            traceRayEXT(
                topLevelAS,
                secondaryTraverseFlags(),
                0xff,
                0,
                0,
                0,
                currentOrigin,
                0.001,
                currentDirection,
                glossyRayMaxDistance,
                0
            );
        }


        if(pathStep == 0)
        {
            if(payload.hitDistance >= 0.0)
            {
                vec3 firstSpecularHitPos =
                    currentOrigin +
                    currentDirection *
                    payload.hitDistance;

                firstSpecularHitDistance =
                    length(
                        firstSpecularHitPos -
                        primarySurfaceOrigin
                    );
            }
            else
            {
                firstSpecularHitDistance =
                    65504.0;
            }
        }

        // 层数 LOD：首次命中后按表面粗糙度与命中距离收紧镜面弹射层数。
        if(pathStep == 0 && orionGlossyStepLodEnabled())
        {
            const float orionGlossyRough =
                clamp(payload.roughness, 0.0, 1.0);

            if(orionGlossyRough > 0.30 || firstSpecularHitDistance > 32.0)
            {
                orionGlossyBounceLimit = min(orionGlossyBounceLimit, 2);
            }
            else if(orionGlossyRough > 0.15 || firstSpecularHitDistance > 16.0)
            {
                orionGlossyBounceLimit = min(orionGlossyBounceLimit, 4);
            }
        }


        // =============================================
        // Sky
        // =============================================

        if(payload.hitDistance < 0.0)
{
    // The in-water segment is already Beer-Lambert attenuated in
    // throughput.  Preserve the environment term instead of producing a
    // black reflection fragment at a fluid-section boundary.
    return
        accumulatedRadiance +
        throughput *
        unpackAlbedoUnorm(payload.albedoPacked);
}


        // 后续 visibility trace 会覆盖 payload，
        // 所以这里必须立即保存。
        float hitDistance =
            payload.hitDistance;


        vec3 hitNormal =
            normalize(
                payload.normal
            );


        vec3 hitAlbedo =
            unpackAlbedoUnorm(payload.albedoPacked);


        float hitMaterialId =
            getLegacyMaterialId(payload.state);


        float hitRoughness =
            payload.roughness;


        float hitF0Metal =
            payload.f0Metal;


        vec3 hitEmission =
            payload.emission;


        float hitPOMSelfShadow =
            clamp(
                payload.pomSelfShadow,
                0.0,
                1.0
            );


        vec3 hitPos =
            currentOrigin +
            currentDirection *
            hitDistance;


        // 当前 ray segment 真正在水中传播。
        if(insideWater)
        {
#ifndef VPT_WATER_SHALLOW_ABSORPTION
// 1 = 前 WATER_SHALLOW_DISTANCE 格几乎不吸收（**当前默认**，与本文件
//     18617 那条注释的"0~3 格保持高通透 / 约 16 格后水底基本不可见"一致）。
// 0 = 逐段满吸收（2026-11-03 之前的行为：1 格就 0.84、5 格 0.41 ⇒ 一片蓝）。
#define VPT_WATER_SHALLOW_ABSORPTION 1
#endif
#if VPT_WATER_SHALLOW_ABSORPTION
            // 吸收只作用在"累计光程超出浅水区"的那部分，按段增量算，
            // 所以海洋那种连续深水仍然会按真实深度衰减（10 格 ≈ 0.28 透过）。
            const float previousWaterPath = waterPathTravelled;
            waterPathTravelled += hitDistance;
            const float absorbedDepth =
                max(waterPathTravelled - kShallowWaterDistance, 0.0) -
                max(previousWaterPath - kShallowWaterDistance, 0.0);
            throughput *=
                exp(-WATER_ABSORPTION * absorbedDepth);
#else
            waterPathTravelled += hitDistance;
            throughput *=
                exp(
                    -WATER_ABSORPTION *
                    hitDistance
                );
#endif
        }


        // =============================================
        // Opaque hit
        // =============================================

if(hitMaterialId <= 0.5)
{
    // =================================================
    // A3-4
    //
    // 当前 opaque 自己的 local radiance：
    //
    // emission
    // +
    // 完整 PBR direct sun
    // =================================================

    vec3 surfaceRadiance =
        evaluateOpaqueSurfaceRadiance(
            hitPos,
            hitNormal,
            -currentDirection,
            hitAlbedo,
            hitRoughness,
            hitF0Metal,
            hitEmission,
            hitPOMSelfShadow,
            sunDirection
        );


    // 2026-10-03 镜厅：**纯镜面**（LabPBR 金属 + 粗糙度 < 0.05）命中：
    //   · 不累加镜面自己的表面辐射（眼睛从镜面只该看到反射像）
    //   · 走确定性镜像续弹（reflect 方向，吞吐 × Fresnel，无随机采样）
    //   · 层数到顶 → 退化为普通墙面观感（拿它自己的 direct/自发光兜底）并返回
    // 非镜面命中走原有随机续弹（上限与行为都不变）。
    const bool hitIsPureMirror =
        decodeMetallicFromCode(decodeF0MetalCode(hitF0Metal)) > 0.5 &&
        hitRoughness < 0.05;

    if(hitIsPureMirror)
    {
        if(mirrorBounceCount >= orionGlossyBounceLimit)
        {
            accumulatedRadiance +=
                throughput *
                surfaceRadiance;

            return
                accumulatedRadiance;
        }

        const float mirrorNdotV =
            max(
                dot(
                    hitNormal,
                    -currentDirection
                ),
                0.0
            );

        if(mirrorNdotV <= 1.0e-5)
        {
            return
                accumulatedRadiance;
        }

        const int mirrorF0Code =
            decodeF0MetalCode(
                hitF0Metal
            );

        const vec3 mirrorF0 =
            decodeBaseF0(
                mirrorF0Code,
                hitAlbedo
            );

        throughput *=
            fresnelSchlick(
                mirrorNdotV,
                mirrorF0
            );

        currentOrigin =
            hitPos +
            hitNormal *
            RAY_BIAS;

        currentDirection =
            reflect(
                currentDirection,
                hitNormal
            );

        mirrorBounceCount +=
            1;

        continue;
    }

    accumulatedRadiance +=
        throughput *
        surfaceRadiance;


    // =================================================
    // 已经完成一次 opaque indirect continuation。
    //
    // 第二个 opaque 就是终点：
    // 取它自己的 emission + direct，然后返回。
    // =================================================

    if(
        opaqueContinuationCount >=
        MAX_OPAQUE_INDIRECT_CONTINUATIONS
    )
    {
        return
            accumulatedRadiance;
    }


    // =================================================
    // 当前 opaque 的 view/outgoing direction
    // =================================================

    vec3 bounceV =
        normalize(
            -currentDirection
        );


    float bounceNdotV =
        max(
            dot(
                hitNormal,
                bounceV
            ),
            0.0
        );


    if(bounceNdotV <= 1.0e-5)
    {
        return
            accumulatedRadiance;
    }


    // =================================================
    // LabPBR material decode
    // =================================================

    int hitF0MetalCode =
        decodeF0MetalCode(
            hitF0Metal
        );


    float hitMetallic =
        decodeMetallicFromCode(
            hitF0MetalCode
        );


    vec3 bounceF0 =
        decodeBaseF0(
            hitF0MetalCode,
            hitAlbedo
        );


    float bounceRoughness =
        max(
            hitRoughness,
            0.05
        );


    // =================================================
    // Fresnel-based lobe selection
    //
    // 和已经确认的 A3-1 使用相同规则。
    // =================================================

    vec3 bounceFForChoice =
        fresnelSchlick(
            bounceNdotV,
            bounceF0
        );


    float bounceFMax =
        max(
            max(
                bounceFForChoice.r,
                bounceFForChoice.g
            ),
            bounceFForChoice.b
        );


    float bounceSpecularProbability =
        hitMetallic > 0.5
            ? 1.0
            : clamp(
                bounceFMax,
                0.05,
                0.95
            );


    bool chooseBounceSpecular =
        rand(state) <
        bounceSpecularProbability;


    vec3 bounceDirection =
        vec3(0.0);


    vec3 bounceWeight =
        vec3(0.0);


    // =================================================
    // GGX Specular continuation
    // =================================================

    if(chooseBounceSpecular)
    {
        vec3 bounceH =
            normalize(
                sampleGGX(
                    hitNormal,
                    bounceRoughness,
                    state
                )
            );


        float bounceVdotH =
            max(
                dot(
                    bounceV,
                    bounceH
                ),
                0.0
            );


        float bounceNdotH =
            max(
                dot(
                    hitNormal,
                    bounceH
                ),
                0.0
            );


        bounceDirection =
            normalize(
                reflect(
                    -bounceV,
                    bounceH
                )
            );


        float bounceNdotL =
            max(
                dot(
                    hitNormal,
                    bounceDirection
                ),
                0.0
            );


        if(
            bounceVdotH <= 1.0e-5 ||
            bounceNdotH <= 1.0e-5 ||
            bounceNdotL <= 1.0e-5
        )
        {
            return
                accumulatedRadiance;
        }


        vec3 bounceF =
            fresnelSchlick(
                bounceVdotH,
                bounceF0
            );


        float bounceD =
            distributionGGX(
                hitNormal,
                bounceH,
                bounceRoughness
            );


        float bounceG =
            geometrySmith(
                hitNormal,
                bounceV,
                bounceDirection,
                bounceRoughness
            );


        vec3 bounceSpecularBRDF =
            (
                bounceD *
                bounceG *
                bounceF
            ) /
            max(
                4.0 *
                bounceNdotV *
                bounceNdotL,
                1.0e-5
            );

        // 2026-10-16 多散射能量补偿：间接镜面延续与直接太阳同口径。
        bounceSpecularBRDF *=
            multiscatterEnergyCompensation(
                bounceF0,
                bounceRoughness
            );


        // sampleGGX:
        //
        // PDF(H) = D * NdotH
        //
        // reflection mapping:
        //
        // PDF(L) =
        // PDF(H) / (4 * VdotH)
        float bounceSpecularPdf =
            max(
                (
                    bounceD *
                    bounceNdotH
                ) /
                max(
                    4.0 *
                    bounceVdotH,
                    1.0e-5
                ),
                1.0e-5
            );


        bounceWeight =
            (
                bounceSpecularBRDF *
                bounceNdotL
            ) /
            (
                bounceSpecularPdf *
                max(
                    bounceSpecularProbability,
                    1.0e-5
                )
            );
    }


    // =================================================
    // Diffuse continuation
    // =================================================

    else
    {
        float bounceDiffuseProbability =
            max(
                1.0 -
                bounceSpecularProbability,
                1.0e-5
            );


        vec3 bounceKD =
            (
                vec3(1.0) -
                bounceFForChoice
            ) *
            (
                1.0 -
                hitMetallic
            );


        bounceDirection =
            sampleHemisphereCosine(
                hitNormal,
                state
            );


        // Cosine hemisphere：
        //
        // BRDF * NdotL / PDF
        // =>
        // kD * albedo
        bounceWeight =
            (
                bounceKD *
                hitAlbedo
            ) /
            bounceDiffuseProbability;
    }


    // =================================================
    // 与已经确认的 A3-1 保持同一 GI transport 尺度。
    // =================================================

    throughput *=
        bounceWeight *
        BSDF_GI_THROUGHPUT_SCALE;


    currentOrigin =
        hitPos +
        hitNormal *
        RAY_BIAS;


    currentDirection =
        bounceDirection;


    opaqueContinuationCount +=
        1;


    continue;
}


        // =============================================
        // Water interface
        //
        // rchit 已确认：
        // 1.0 = Air -> Water
        // 2.0 = Water -> Air
        // =============================================
if(
    waterInterfaceStepCount >=
    MAX_WATER_INTERFACE_STEPS
)
{
    return
        accumulatedRadiance;
}


waterInterfaceStepCount +=
    1;

        hitNormal =
            stabilizeWaterInterfaceNormal(
                hitNormal
            );

        bool exitingWater =
            hitMaterialId > 1.5;


        float cosTheta =
            clamp(
                -dot(
                    currentDirection,
                    hitNormal
                ),
                0.0,
                1.0
            );


        float fresnel =
            WATER_F0 +
            (
                1.0 -
                WATER_F0
            ) *
            pow(
                1.0 -
                cosTheta,
                5.0
            );


        float waterChoice =
            rand(
                state
            );


        // =============================================
        // Reflection
        // =============================================

        if(
            waterChoice <
            fresnel
        )
        {
            // 没跨介质：
            //
            // Air -> Water 的界面反射：
            // 仍在 Air。
            //
            // Water -> Air 的界面反射：
            // 仍在 Water。
            insideWater =
                exitingWater;


            currentOrigin =
                hitPos +
                hitNormal *
                RAY_BIAS;


            currentDirection =
                normalize(
                    reflect(
                        currentDirection,
                        hitNormal
                    )
                );


            continue;
        }


        // =============================================
        // Transmission
        // =============================================

        float eta =
            exitingWater
                ? WATER_IOR
                : (1.0 / WATER_IOR);


        vec3 refractedDirection =
            refract(
                currentDirection,
                hitNormal,
                eta
            );


        // Total internal reflection
        if(
            dot(
                refractedDirection,
                refractedDirection
            ) < 1.0e-6
        )
        {
            insideWater =
                exitingWater;


            currentOrigin =
                hitPos +
                hitNormal *
                RAY_BIAS;


            currentDirection =
                normalize(
                    reflect(
                        currentDirection,
                        hitNormal
                    )
                );


            continue;
        }


        // 真正跨介质
        insideWater =
            !exitingWater;


        currentOrigin =
            hitPos -
            hitNormal *
            RAY_BIAS;


        currentDirection =
            normalize(
                refractedDirection
            );
    }


return
    accumulatedRadiance;
}
// =====================================================
// Explicit reflected sunlight from flat water
//
// Path:
//
// Sun
//   -> horizontal water surface
//   -> specular reflection
//   -> opaque receiver surface
//
// This explicitly samples the specular water path instead
// of waiting for a random GI ray to hit the exact sun path.
// =====================================================
float traceWaterReflectedSun(
    vec3 receiverPos,
    vec3 receiverNormal,
    vec3 sunDirection
)
{
    const vec3 WATER_PLANE_NORMAL =
        vec3(0.0, 1.0, 0.0);

    const float WATER_F0 =
        0.0200593;

    // -------------------------------------------------
    // For a flat horizontal mirror:
    //
    // receiver -> virtual sun
    //
    // This ray must hit the water surface if the receiver
    // can receive the reflected sunlight.
    // -------------------------------------------------
    vec3 toWaterDir =
        normalize(
            reflect(
                sunDirection,
                WATER_PLANE_NORMAL
            )
        );

    float receiverNdotL =
        max(
            dot(
                receiverNormal,
                toWaterDir
            ),
            0.0
        );

    // Receiver faces away from reflected light.
    if(receiverNdotL <= 0.0)
    {
        return 0.0;
    }


    // =================================================
    // Receiver -> Water
    // =================================================
    hit.hitDistance =
        1.0;

    hit.state = makeTraceState(RT_PURPOSE_CLASSIFY);

#ifdef RT_ENABLE_SER
    // Perf-L10.8: same ray, same flags; SER only reschedules the hit shader.
    if(cam.perfSerAb.z != 0u)
    {
        serTraceRay(
            // 2026-10-23：偏移改沿射线；沿法线会在方块边缘漏光（复盘见 :3037）
            receiverPos + toWaterDir * 0.001,
            0.02,
            toWaterDir,
            10000.0,
            secondaryTraverseFlags(),
            0xff,
            0u
        );
    }
    else
#endif
    {
        #ifdef ORION_RT_STAGE_COUNTERS
        rtCntRayIssued();
        #endif
        traceRayEXT(
            topLevelAS,
            secondaryTraverseFlags(),
            0xff,
            0,
            0,
            0,
            receiverPos + toWaterDir * 0.001,
            0.02,
            toWaterDir,
            10000.0,
            0
        );
    }


    // No water intersection.
    if(payload.hitDistance < 0.0)
    {
        return 0.0;
    }


    float waterMaterialId =
        getLegacyMaterialId(payload.state);

    // First intersection must actually be water.
    if(waterMaterialId <= 0.5)
    {
        return 0.0;
    }


    // We only want the Air -> Water top-facing surface.
    //
    // materialId:
    //     1 = Air -> Water
    //     2 = Water -> Air
    if(waterMaterialId > 1.5)
    {
        return 0.0;
    }


    vec3 waterHitPos =
        receiverPos +
        toWaterDir * 0.001 +
        toWaterDir *
        payload.hitDistance;

    vec3 waterHitNormal =
        normalize(
            payload.normal
        );


    // Reject vertical water sides.
    // Current explicit solution is for the horizontal
    // water surface only.
    if(
        dot(
            waterHitNormal,
            WATER_PLANE_NORMAL
        ) < 0.5
    )
    {
        return 0.0;
    }


    // =================================================
    // Water -> Sun visibility
    //
    // The reflection only exists if this exact water point
    // is actually exposed to the sun.
    // =================================================
    hit.hitDistance =
        1.0;

    hit.state = makeTraceState(RT_PURPOSE_VISIBILITY_ONLY);

#if RTAO_VISIBILITY_EARLYOUT
    // 2026-11-16：水面太阳可见性的 inline ray query 可选臂（ORION_VIS_QUERY bit1，
    // cam.visQueryAb.y）。默认 0 = 走下方既有 traceRayEXT，逐位旧行为。
    // 同一 TLAS / cullMask 0xff / tmin 0.02 / 同方向同射程；query 旗标只用
    // TerminateOnFirstHit（不带 SkipClosestHitShader，理由见 AO 臂注释）。未命中写
    // -1.0，与 rt_test.rmiss 对 purpose 3 的写入值一致（调用方按 hitDistance >= 0.0
    // 判被遮挡；负值 = 水面暴露给太阳）。
    if(cam.visQueryAb.y != 0u)
    {
        rayQueryEXT glossSunVisQuery;
        #ifdef ORION_RT_STAGE_COUNTERS
        rtCntRayIssued();
        #endif
        rayQueryInitializeEXT(
            glossSunVisQuery,
            topLevelAS,
            gl_RayFlagsTerminateOnFirstHitEXT,
            0xff,
            waterHitPos + sunDirection * 0.001,
            0.02,
            sunDirection,
            10000.0
        );

        int glossSunVisGuard = 0;
        // 防死循环上限（参照 volumetric_fog.glsl 的 proceedGuard）。
        // 2026-11-16 修正：committed 交点必须在查询终止后读，不能在循环体里判
        // （TerminateOnFirstHit 命中即终止，循环体可能一次都不进 ⇒ 恒判未命中 ⇒
        // 水面永远被当成晒着太阳）。
        while(rayQueryProceedEXT(glossSunVisQuery) && ++glossSunVisGuard < 32) {}
        if(rayQueryGetIntersectionTypeEXT(glossSunVisQuery, true) !=
            gl_RayQueryCommittedIntersectionNoneEXT)
        {
            hit.hitDistance = rayQueryGetIntersectionTEXT(glossSunVisQuery, true);
        }
        else
        {
            hit.hitDistance = -1.0;
        }
    }
    else
#endif
#ifdef RT_ENABLE_SER
    // Perf-L10.8: same ray, same flags; SER only reschedules the hit shader.
    if(cam.perfSerAb.y != 0u)
    {
        serTraceRay(
            waterHitPos + sunDirection * 0.001,
            0.02,
            sunDirection,
            10000.0,
            gl_RayFlagsTerminateOnFirstHitEXT |
            gl_RayFlagsSkipClosestHitShaderEXT,
            0xff,
            0u
        );
    }
    else
#endif
    {
        #ifdef ORION_RT_STAGE_COUNTERS
        rtCntRayIssued();
        #endif
        traceRayEXT(
            topLevelAS,
            gl_RayFlagsTerminateOnFirstHitEXT |
            gl_RayFlagsSkipClosestHitShaderEXT,
            0xff,
            0,
            0,
            0,
            waterHitPos + sunDirection * 0.001,
            0.02,
            sunDirection,
            10000.0,
            0
        );
    }


    // Something blocks the sun above the water.
    if(hit.hitDistance >= 0.0)
    {
        return 0.0;
    }


    // =================================================
    // 2026-10-19 reflected caustics (ceiling "wave light")
    //
    // The flat-mirror path above decides **whether** this
    // receiver sees the sun at all; the host's forward-
    // rasterised map (same FFT slopes, reflect(-sun, n))
    // supplies **how much** the wavy surface concentrates
    // it.  The map is keyed by the water source point, so
    // the pattern is world-anchored and camera-independent
    // — the same convention as the refracted map (binding 69).
    //
    // Flat water = 1 (no change).  Uncovered texels stay 0
    // and must read as "unperturbed", hence the > 0 test.
    // =================================================
    float reflectedCausticGain = 1.0;
    if(cam.waterFftParams.y > 0.0 && cam.waterFftParams2.w > 0.5)
    {
        float projScale = float(rtLaunchSize().y) /
            (2.0 * max(tan(cam.cameraParams.z * 0.5), 0.05));
        float causticLod = waterCausticMapLodFromProj(
            receiverPos,
            length(receiverPos - cam.viewInverse[3].xyz),
            projScale);
        // 2026-11-01：宿主改成 compute 生产，反射参考面从 +4 格抬到 +16 格
        // （覆盖到水面以上 32 格）。图的键仍是"源水面坐标"，所以要按接收面的
        // **实际高度**扣掉平坦反射位移 —— 否则 2~8 格（洞穴顶/桥下，最常见）
        // 会过冲成大光斑，30 格处又会偏小。
        const float reflectHeight = max(
            waterHitPos.y - cam.waterSimSurfaceAndOrigin.x, 0.0);
        const vec2 flatReflectOffset = vec2(-sunDirection.x, -sunDirection.z) *
            (reflectHeight / max(abs(sunDirection.y), 0.02));
        float causticDensity = textureLod(
            waterReflectCausticMap,
            (waterHitPos.xz - flatReflectOffset) /
                max(cam.waterFftParams.x, 1.0e-3),
            causticLod).r;
        if(causticDensity > 1.0e-4)
        {
            // 2026-10-20：反射层改用自己的旋钮（waterWaveParams.w，GUI
            // 「反射焦散强度」），不再蹭折射那份「焦散亮度」。两者照的面完全不
            // 同（折射 = 水下地板，反射 = 朝水的天花板），要能分别调才有意义。
            // 本 lane 出厂 4.0，而拆杆之前借的是折射那根的 1.3（随包快照）/ 1.1
            //（用户实机）⇒ 本版默认更亮，就是"不明显"的修法；拖回 1.3 = 旧观感。
            // 配旧 DLL（本 lane 仍写 0）时增益恒为 1 ⇒ 优雅降级成纯镜面太阳光，
            // 不崩不脏值。
            float causticStrength =
                clamp(cam.waterWaveParams.w, 0.0, 8.0);
            reflectedCausticGain = clamp(
                1.0 + (causticDensity - 1.0) * causticStrength,
                0.0,
                6.0);
        }
    }


    // =================================================
    // Fresnel reflection
    // =================================================
    float waterCosTheta =
        clamp(
            dot(
            WATER_PLANE_NORMAL,
            sunDirection
            ),
            0.0,
            1.0
        );

    float waterFresnel =
        WATER_F0 +
        (
            1.0 -
            WATER_F0
        ) *
        pow(
            1.0 -
            waterCosTheta,
            5.0
        );


    return
        receiverNdotL *
        waterFresnel *
        reflectedCausticGain;
}