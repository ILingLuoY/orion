
vec3 decodeLocalLightColor(
    uint typeCode
)
{
    // 2026-10-31：原来的 7 档硬编码调色板并入 local_light_profile.glsl 的
    // 32 档档案表 —— 那里每档不只有颜色，还带满级亮度与基础传播距离。
    return localLightProfileColor(typeCode);
}
// =====================================================
// E1-1 - Unified Local Emitter Radiance
//
// Same Minecraft emitter definition is now shared by:
// - Primary
// - Glossy reflection
// - Diffuse GI
//
// incomingRayDirection points TOWARD the hit surface.
// Probe 0.02 block past the surface so a boundary hit is
// classified inside the block that was actually intersected.
// =====================================================

vec3 evaluateLocalEmitterCandidateRadiance(
    vec3 hitPos,
    vec3 incomingRayDirection,
    vec3 hitAlbedo,
    LocalLightGPU candidateLight
)
{
    vec3 safeIncomingDir =
        normalize(
            incomingRayDirection
        );


    vec3 emitterProbePos =
        hitPos +
        safeIncomingDir *
        0.02;


    vec4 worldPosEmission =
        candidateLight
            .worldPosEmission;


    vec3 emitterBlockMin =
        worldPosEmission.xyz -
        vec3(0.001);


    vec3 emitterBlockMax =
        worldPosEmission.xyz +
        vec3(1.001);


    bool hitThisEmitter =
        all(
            greaterThanEqual(
                emitterProbePos,
                emitterBlockMin
            )
        ) &&
        all(
            lessThanEqual(
                emitterProbePos,
                emitterBlockMax
            )
        );


    if(!hitThisEmitter)
    {
        return vec3(0.0);
    }


    float emitterLevel =
        clamp(
            worldPosEmission.w,
            1.0,
            15.0
        );


    // 2026-10-31：亮度改按光源档案（每档自己的满级亮度），不再统一 emission / 15。
    float emitterLevel01 =
        localLightProfileBrightness(
            candidateLight.meta.x,
            emitterLevel
        );


    vec3 emitterColor =
        decodeLocalLightColor(
            candidateLight.meta.x
        );


    vec3 safeAlbedo =
        clamp(
            hitAlbedo,
            vec3(0.0),
            vec3(1.0)
        );


    float albedoRadiance =
        dot(
            safeAlbedo,
            vec3(0.333333333)
        );


    // E1-2A:
    // 保留原来的纹理，而不是把整个方块刷成恒定发光色。
    return
        emitterColor *
        3.2 *
        LOCAL_LIGHT_ENERGY_SCALE * localLightIntensity() *
        emitterLevel01 *
        albedoRadiance *
        safeAlbedo;
}


vec3 evaluateLocalEmitterRadianceAtHit(
    vec3 hitPos,
    vec3 incomingRayDirection,
    vec3 hitAlbedo
)
{
    // =====================================================================
    // HARD-KILL (2026-09-21): local direct light is disabled unconditionally
    // at the shader source level, bypassing the user-facing toggle entirely.
    // This guarantees every local emitter (torch/lamp/glowstone/etc) stops
    // contributing DIRECT light; only the secondary bounce (G2B guide) and
    // sun/sky/GI remain.  Revert this block to restore the toggle.
    // =====================================================================
    {
        return
            vec3(0.0);
    }


    uint lightCount =
        localLightBuffer.header.x;


    if(lightCount == 0u)
    {
        return
            vec3(0.0);
    }


    // The inward probe uses the same convention as the candidate test below.
    // Search the 3x3x3 neighborhood to preserve the original inclusive box
    // test at a 32-block grid boundary (an emitter on either side can own an
    // exactly-on-boundary probe).
    vec3 emitterProbePos =
        hitPos -
        normalize(
            incomingRayDirection
        ) *
        0.02;


    ivec3 probeCell =
        localLightGridCellCoord(
            emitterProbePos
        );


    uint packedIndexBase =
        localLightGridPackedIndexBase();


    int minCellX =
        localLightGridNeighborMinOffset(
            emitterProbePos.x
        );

    int maxCellX =
        localLightGridNeighborMaxOffset(
            emitterProbePos.x
        );

    int minCellY =
        localLightGridNeighborMinOffset(
            emitterProbePos.y
        );

    int maxCellY =
        localLightGridNeighborMaxOffset(
            emitterProbePos.y
        );

    int minCellZ =
        localLightGridNeighborMinOffset(
            emitterProbePos.z
        );

    int maxCellZ =
        localLightGridNeighborMaxOffset(
            emitterProbePos.z
        );


    vec3 emitterRadiance =
        vec3(0.0);


    for(int cellZ = minCellZ; cellZ <= maxCellZ; ++cellZ)
    {
        for(int cellY = minCellY; cellY <= maxCellY; ++cellY)
        {
            for(int cellX = minCellX; cellX <= maxCellX; ++cellX)
            {
                uint firstLightIndex =
                    0u;

                uint cellLightCount =
                    0u;


                if(
                    !localLightGridLookupCell(
                        probeCell +
                        ivec3(
                            cellX,
                            cellY,
                            cellZ
                        ),
                        firstLightIndex,
                        cellLightCount
                    )
                )
                {
                    continue;
                }


                for(
                    uint candidateOffset = 0u;
                    candidateOffset < cellLightCount;
                    ++candidateOffset
                )
                {
                    uint lightIndex =
                        localLightGridBuffer.words[
                            packedIndexBase +
                            firstLightIndex +
                            candidateOffset
                        ];


                    if(lightIndex >= lightCount)
                    {
                        continue;
                    }


                    vec3 candidateRadiance =
                        evaluateLocalEmitterCandidateRadiance(
                            hitPos,
                            incomingRayDirection,
                            hitAlbedo,
                            localLightBuffer.lights[
                                lightIndex
                            ]
                        );


                    emitterRadiance =
                        max(
                            emitterRadiance,
                            candidateRadiance
                        );
                }
            }
        }
    }


    return
        emitterRadiance;
}
vec3 evaluateLocalEmitterSurfaceRadiance(
    vec3 hitPos,
    vec3 incomingRayDirection,
    vec3 hitAlbedo
)
{
    uint lightCount =
        localLightBuffer.header.x;


    vec3 emitterRadiance =
        vec3(0.0);


    vec3 safeAlbedo =
        clamp(
            hitAlbedo,
            vec3(0.0),
            vec3(1.0)
        );


    // 发光表面不是恒定颜色覆盖，
    // 而是让 emitter lighting 继续经过原表面 albedo。
    //
    // Radiance(albedo) 在原 shader 中就是 RGB 平均值。
    float albedoRadiance =
        dot(
            safeAlbedo,
            vec3(
                0.333333333
            )
        );


    for(
        uint lightIndex = 0u;
        lightIndex < lightCount;
        ++lightIndex
    )
    {
        LocalLightGPU candidateLight =
            localLightBuffer
                .lights[lightIndex];


        vec4 worldPosEmission =
            candidateLight
                .worldPosEmission;


        vec3 emitterProbePos =
            hitPos +
            normalize(
                incomingRayDirection
            ) *
            0.02;


        vec3 emitterBlockMin =
            worldPosEmission.xyz -
            vec3(0.001);


        vec3 emitterBlockMax =
            worldPosEmission.xyz +
            vec3(1.001);


        bool hitThisEmitter =
            all(
                greaterThanEqual(
                    emitterProbePos,
                    emitterBlockMin
                )
            ) &&
            all(
                lessThanEqual(
                    emitterProbePos,
                    emitterBlockMax
                )
            );


        if(!hitThisEmitter)
        {
            continue;
        }


        float emitterLevel =
            clamp(
                worldPosEmission.w,
                1.0,
                15.0
            );


        // 2026-10-31：亮度改按光源档案（每档自己的满级亮度）。
        float emitterLevel01 =
            localLightProfileBrightness(
                candidateLight.meta.x,
                emitterLevel
            );


        vec3 emitterColor =
            decodeLocalLightColor(
                candidateLight.meta.x
            );


        vec3 texturedEmitterRadiance =
            emitterColor *
            3.2 *
            LOCAL_LIGHT_ENERGY_SCALE * localLightIntensity() *
            emitterLevel01 *
            albedoRadiance *
            safeAlbedo;


        emitterRadiance =
            max(
                emitterRadiance,
                texturedEmitterRadiance
            );
    }


    return
        emitterRadiance;
}
// L4/L7 旧路径（主可见性 / 次级漫反射 GI）的距离淡出。
// 2026-10-31：射程改按光源档案 —— 每个方块自己的基础距离 × 用户系数，封顶 30 格。
// 淡出起点仍是射程的 80%（17 格光 → 24 格开始淡、30 格归零，与旧值一致）。
float localLightRangeFade(
    float distanceToLight,
    uint typeCode,
    float emissionLevel
)
{
    float range =
        clamp(
            localLightProfileBaseRange(typeCode, emissionLevel) *
                vptLocalLightRangeScale(),
            1.5,
            VPT_LOCAL_MAX_DISTANCE
        );


    float fadeStart =
        range *
        0.80;


    return
        1.0 -
        smoothstep(
            fadeStart,
            range,
            distanceToLight
        );
}


// =====================================================
// C-2 - Local Light distance attenuation
//
// Replaces the old hard 1 / d^2 falloff.
//
// Confirmed curve:
//     0.55 / (1 + 0.18*d + 0.025*d^2)
//
// Goals:
// - reduce the near-field hotspot,
// - preserve much more medium-range energy,
// - let indoor lighting spread naturally.
// =====================================================

float localLightDistanceAttenuation(
    float distanceToLight
)
{
    float d =
        max(
            distanceToLight,
            0.0
        );


    return
        0.55 /
        (
            1.0 +
            0.18 * d +
            0.025 * d * d
        );
}



// =====================================================
// L7.1 - True voxel cube area light
//
// B quality mode:
//   2 real emitter-surface samples / selected light / pixel
//
// The old projected disk approximation is removed.
// Each sample lies on an actual visible face of the emitting
// 1x1x1 voxel.  The shadow ray therefore ends on the emitter
// surface itself, which makes penumbra width depend on the real
// source/blocker/receiver geometry.
//
// Up to 4 selected local lights -> at most 8 local-light
// visibility rays / pixel.
//
// Sampling is spatially + temporally decorrelated.  RR / DLSS
// can integrate the remaining 2-spp stochastic noise.
// =====================================================

const int LOCAL_LIGHT_CUBE_AREA_SAMPLES =
    2;


// =====================================================
// L7.2
// Keep 2 rays / light, but enlarge the effective emitting
// face from 1x1 to 2.0x2.0 blocks.
// =====================================================
const float LOCAL_LIGHT_CUBE_EMITTER_SIZE =
    2.0;

const float LOCAL_LIGHT_CUBE_EMITTER_HALF_SIZE =
    LOCAL_LIGHT_CUBE_EMITTER_SIZE *
    0.5;


// Keep samples slightly away from the virtual emitter border.
// This still avoids unstable edge hits while allowing a much
// wider penumbra than the old 1x1 source.
const float LOCAL_LIGHT_CUBE_SURFACE_INSET =
    0.03;


// The ray target is already on the emitter surface, so the old
// 0.70 block center-exclusion distance is no longer appropriate.
// Stop only a tiny distance before the sampled surface.
const float LOCAL_LIGHT_CUBE_RAY_END_EPSILON =
    0.03;


// =====================================================
// P1-1 - Hybrid Local Light emitter shapes
//
// type 1..3: point-like fire family, represented by the
//             diffuse light sphere (radius 0.5)
// type 4..5: full-block emitters, use the confirmed effective
//             2.0x2.0 cube-face area
// =====================================================

const float LOCAL_LIGHT_POINT_SPHERE_RADIUS =
    0.5;


const float LOCAL_LIGHT_POINT_SPHERE_PROJECTED_AREA =
    3.14159265359 *
    LOCAL_LIGHT_POINT_SPHERE_RADIUS *
    LOCAL_LIGHT_POINT_SPHERE_RADIUS;


bool isPointLikeLocalLightType(
    uint typeCode
)
{
    // 2026-10-31：几何形状改由光源档案给出（火把/蜡烛/末地烛 = 点状，
    // 荧石/岩浆/海晶灯 = 整块）。旧的"1..3 号都是点状"是按 7 档分类写的，
    // 换到 32 档后已不再成立。
    return localLightProfilePointLike(typeCode);
}


// Existing L7 energy shaping is intentionally retained.
// It only controls the game-tuned local-light response; the
// penumbra itself is now produced geometrically by the cube area.
const float LOCAL_LIGHT_L7_NEARFIELD_MIN_D2 =
    1.75;

const float LOCAL_LIGHT_L7_RADIANCE_SCALE =
    LOCAL_LIGHT_ENERGY_SCALE;

// 2026-10-29：原先这里的 0.92 / 0.45 / 0.28 / 0.35 四个 BSDF 标定已搬到文件
// 前部的 BSDF 标定表（搜索 BSDF_LOCAL_）。这里只保留光源能量侧的两个常量。

// =====================================================
// 2026-10-07：移动光源（动态光源）的材质 BRDF
// =====================================================
// 结构照抄上面那份零调用点的点光实现（evaluatePrimaryLocalLights 的
// diffuseTerm + specularTerm），标定共用同一组 LOCAL_LIGHT_L7_*，于是"手持的灯"与
// "方块射出的光"在同一材质上观感同源。原型在动态光源采样端那一段（约 2955 行）。
//
// f0.x < 0 ⇒ 调用方没有材质（GI 顶点、透明接收、时域缓存、旧 4 参重载）⇒ 走纯 Lambert
// 分支，kD 恒 BSDF_LOCAL_DIFFUSE_SCALE(=0.92)，与改动前 `* hitAlbedo * 0.92`
// **逐位等价**。
//
// 粗糙度按 BSDF_LOCAL_MIN_SPEC_ROUGHNESS 抬高：动态光源是"几条点光源 + 均匀
// 提案"的估计器（见 evaluateDynamicLightRISDirect），窄镜面波瓣在这个提案分布下方差
// 极大；抬高下限把波瓣展宽，把噪点换成可控的柔和高光。
// =====================================================
vec3 dynamicLightBRDF(
    vec3 N,
    vec3 V,
    vec3 L,
    vec3 albedo,
    vec3 f0,
    float metallic,
    float roughness
)
{
    float NdotL = max(dot(N, L), 0.0);
    if(NdotL <= 0.0)
    {
        return vec3(0.0);
    }
    if(f0.x < 0.0)
    {
        // 无材质：与改动前逐位相同的 Lambert（0.92 就是 BSDF_LOCAL_DIFFUSE_SCALE）。
        return albedo * (BSDF_LOCAL_DIFFUSE_SCALE * NdotL);
    }

    float specularRoughness =
        max(roughness, BSDF_LOCAL_MIN_SPEC_ROUGHNESS);

    float NdotV = max(dot(N, V), 1.0e-4);

    BsdfTerms bsdf =
        evalBSDFTerms(
            N,
            V,
            L,
            f0,
            metallic,
            specularRoughness,
            NdotV,
            true
        );

    vec3 diffuseTerm =
        bsdf.kD * albedo * (NdotL * BSDF_LOCAL_DIFFUSE_SCALE);
    vec3 specularTerm =
        bsdf.specular * (NdotL * BSDF_LOCAL_SPECULAR_SCALE);
    return diffuseTerm + specularTerm;
}


// =====================================================
// Sample the visible projected surface of the point-like sphere.
//
// The sphere is centered at BlockPos + 0.5 and uses the confirmed
// diffuse-light radius 0.5.  Sampling the receiver-facing
// projected disk avoids wasting rays on the hidden half of the sphere.
// It is an isotropic point-like emitter, so callers must NOT apply the
// cube-face emitterFacing term to this sample.
// =====================================================

vec3 sampleLocalLightPointSphereSurface(
    vec3 emitterBlockPos,
    vec3 receiverPos,
    int selectedSlot,
    int sampleIndex
)
{
    vec3 emitterCenter =
        emitterBlockPos +
        vec3(0.5);


    vec3 centerToReceiver =
        receiverPos -
        emitterCenter;


    float centerToReceiverLengthSquared =
        dot(
            centerToReceiver,
            centerToReceiver
        );


    vec3 frontDirection =
        centerToReceiverLengthSquared > 1.0e-8
            ? centerToReceiver *
                inversesqrt(
                    centerToReceiverLengthSquared
                )
            : vec3(
                0.0,
                1.0,
                0.0
            );


    vec3 referenceAxis =
        abs(frontDirection.y) < 0.999
            ? vec3(
                0.0,
                1.0,
                0.0
            )
            : vec3(
                1.0,
                0.0,
                0.0
            );


    vec3 tangent =
        normalize(
            cross(
                referenceAxis,
                frontDirection
            )
        );


    vec3 bitangent =
        cross(
            frontDirection,
            tangent
        );


    uvec3 emitterBits =
        floatBitsToUint(
            emitterBlockPos
        );


    uint framePhase =
        cam.frameCount &
        15u;


    uint state =
        gl_LaunchIDEXT.x * 1877u ^
        gl_LaunchIDEXT.y * 9241u ^
        framePhase * 26711u ^
        emitterBits.x * 31847u ^
        emitterBits.y * 42131u ^
        emitterBits.z * 61169u ^
        uint(selectedSlot + 1) * 1031u ^
        uint(sampleIndex + 1) * 2017u;


    float projectedRadius =
        LOCAL_LIGHT_POINT_SPHERE_RADIUS *
        sqrt(
            rand(state)
        );


    float projectedAngle =
        6.28318530718 *
        rand(state);


    float diskX =
        projectedRadius *
        cos(projectedAngle);


    float diskY =
        projectedRadius *
        sin(projectedAngle);


    float frontOffset =
        sqrt(
            max(
                LOCAL_LIGHT_POINT_SPHERE_RADIUS *
                    LOCAL_LIGHT_POINT_SPHERE_RADIUS -
                    projectedRadius *
                    projectedRadius,
                0.0
            )
        );


    return
        emitterCenter +
        tangent * diskX +
        bitangent * diskY +
        frontDirection * frontOffset;
}


// =====================================================
// Sample one of the three cube faces visible from receiverPos.
//
// Face-selection probability is proportional to the projected
// axis weight abs(receiver-center).  This avoids wasting either
// of the two rays on a back-facing cube face.
//
// The returned point stays on the selected voxel-face plane and uses
// the confirmed effective 2.0x2.0 tangential extent.
// =====================================================

vec3 sampleLocalLightCubeSurface(
    vec3 emitterBlockPos,
    vec3 receiverPos,
    int selectedSlot,
    int sampleIndex,
    out vec3 emitterSurfaceNormal,
    out float faceSelectionProbability
)
{
    vec3 emitterCenter =
        emitterBlockPos +
        vec3(0.5);


    vec3 centerToReceiver =
        receiverPos -
        emitterCenter;


    vec3 faceWeights =
        abs(
            centerToReceiver
        ) +
        vec3(1.0e-4);


    float faceWeightSum =
        faceWeights.x +
        faceWeights.y +
        faceWeights.z;


    uvec3 emitterBits =
        floatBitsToUint(
            emitterBlockPos
        );


    // 16 temporal phases.  Two samples per light use different
    // seed branches while adjacent pixels and emitters are also
    // decorrelated.
    uint framePhase =
        cam.frameCount &
        15u;


    uint state =
        gl_LaunchIDEXT.x * 1973u +
        gl_LaunchIDEXT.y * 9277u ^
        framePhase * 26699u ^
        emitterBits.x * 31847u ^
        emitterBits.y * 42131u ^
        emitterBits.z * 61169u ^
        uint(selectedSlot + 1) * 1013u ^
        uint(sampleIndex + 1) * 1999u;


    float faceSelector =
        rand(state) *
        faceWeightSum;


    float tangentExtent =
        max(
            LOCAL_LIGHT_CUBE_EMITTER_HALF_SIZE -
                LOCAL_LIGHT_CUBE_SURFACE_INSET,
            0.01
        );


    float u =
        mix(
            -tangentExtent,
            tangentExtent,
            rand(state)
        );


    float v =
        mix(
            -tangentExtent,
            tangentExtent,
            rand(state)
        );


    vec3 samplePos =
        emitterCenter;


    if(faceSelector < faceWeights.x)
    {
        faceSelectionProbability =
            faceWeights.x /
            faceWeightSum;


        bool positiveFace =
            centerToReceiver.x >=
            0.0;


        emitterSurfaceNormal =
            vec3(
                positiveFace
                    ? 1.0
                    : -1.0,
                0.0,
                0.0
            );


                samplePos.x =
            emitterBlockPos.x +
            (
                positiveFace
                    ? 1.0
                    : 0.0
            );

        samplePos.y =
            emitterCenter.y +
            u;

        samplePos.z =
            emitterCenter.z +
            v;
    }
    else if(
        faceSelector <
        faceWeights.x +
        faceWeights.y
    )
    {
        faceSelectionProbability =
            faceWeights.y /
            faceWeightSum;


        bool positiveFace =
            centerToReceiver.y >=
            0.0;


        emitterSurfaceNormal =
            vec3(
                0.0,
                positiveFace
                    ? 1.0
                    : -1.0,
                0.0
            );


                samplePos.x =
            emitterCenter.x +
            u;

        samplePos.y =
            emitterBlockPos.y +
            (
                positiveFace
                    ? 1.0
                    : 0.0
            );

        samplePos.z =
            emitterCenter.z +
            v;
    }
    else
    {
        faceSelectionProbability =
            faceWeights.z /
            faceWeightSum;


        bool positiveFace =
            centerToReceiver.z >=
            0.0;


        emitterSurfaceNormal =
            vec3(
                0.0,
                0.0,
                positiveFace
                    ? 1.0
                    : -1.0
            );


        samplePos.x =
            emitterCenter.x +
            u;

        samplePos.y =
            emitterCenter.y +
            v;

        samplePos.z =
            emitterBlockPos.z +
            (
                positiveFace
                    ? 1.0
                    : 0.0
            );
    }


    return
        samplePos;
}


float localLightApproxScore(
    vec3 surfacePos,
    vec3 N,
    vec4 worldPosEmission,
    uint typeCode
)
{
    float emissionLevel =
        clamp(
            worldPosEmission.w,
            1.0,
            15.0
        );


    // L2 stores integer BlockPos coordinates. The first L4 light
    // position is the geometric center of that emitting block.
    vec3 lightPos =
        worldPosEmission.xyz +
        vec3(0.5);


    vec3 toLight =
        lightPos -
        surfacePos;


    float distanceSquared =
        dot(
            toLight,
            toLight
        );


    if(distanceSquared <= 1.0e-6)
    {
        return
            0.0;
    }


    float distanceToLight =
        sqrt(
            distanceSquared
        );


    float rangeFade =
        localLightRangeFade(
            distanceToLight,
            typeCode,
            emissionLevel
        );


    if(rangeFade <= 0.0)
    {
        return
            0.0;
    }


    vec3 L =
        toLight /
        distanceToLight;


    float NdotL =
        max(
            dot(
                N,
                L
            ),
            0.0
        );


    if(NdotL <= 0.0)
    {
        return
            0.0;
    }


    // Stable inverse-square ranking. Clamp the near field to
    // avoid a singular score when a surface is very close to
    // the emitting block center.
float distanceAttenuation =
    localLightDistanceAttenuation(
        distanceToLight
    );


    // 2026-10-31：打分也用档案亮度（×15 保持旧的 0..15 量级，排序口径不变）。
    float emissionScore =
        localLightProfileBrightness(typeCode, emissionLevel) * 15.0;


return
    emissionScore *
    distanceAttenuation *
    rangeFade *
    NdotL;
}


// =====================================================
// G2B-2 Q1 + E1 - Diffuse-path Local-Light guiding
//
// Q1:
//   Keep exactly one diffuse GI path.  For every diffuse
//   direction, choose with equal probability between:
//     1) the confirmed cosine-weighted BSDF proposal, and
//     2) a Local-Light directional proposal.
//
//   The path throughput is corrected with the full mixture PDF:
//
//     pMix = 0.5 * pBSDF + 0.5 * pLight
//
//   No Local-Light radiance is added here.  The selected direction
//   is still traced as a normal full scene ray; blockers therefore
//   remain ordinary path hits rather than binary shadow tests.
//
// E1 guide geometry:
//   type 1..3 = confirmed R2 sphere, radius 0.5
//   type 4..5 = actual 1x1x1 emitter cube
//
// The guide geometry defines only the sampling PDF.  Emission is
// still accepted exclusively by the confirmed emitter-hit path.
// =====================================================

const float G2B_LOCAL_GUIDE_PROBABILITY =
    0.5;

const float G2B_LOCAL_GUIDE_EPSILON =
    1.0e-6;

const float G2B_CUBE_GUIDE_INSET =
    0.001;


bool g2bSurfaceBelongsToEmitter(
    vec3 surfacePos,
    vec3 surfaceNormal,
    vec3 emitterBlockPos
)
{
    vec3 insideProbe =
        surfacePos -
        normalize(surfaceNormal) *
        0.02;


    return
        all(
            greaterThanEqual(
                insideProbe,
                emitterBlockPos -
                    vec3(0.001)
            )
        ) &&
        all(
            lessThanEqual(
                insideProbe,
                emitterBlockPos +
                    vec3(1.001)
            )
        );
}


float g2bLocalGuideSelectionWeight(
    vec3 surfacePos,
    vec3 surfaceNormal,
    LocalLightGPU light
)
{
    if(
        g2bSurfaceBelongsToEmitter(
            surfacePos,
            surfaceNormal,
            light.worldPosEmission.xyz
        )
    )
    {
        return
            0.0;
    }


    return
        max(
            localLightApproxScore(
                surfacePos,
                surfaceNormal,
                light.worldPosEmission,
                light.meta.x
            ),
            0.0
        );
}


float g2bLocalGuideWeightSum(
    vec3 surfacePos,
    vec3 surfaceNormal
)
{
    uint lightCount =
        localLightBuffer.header.x;


    float weightSum =
        0.0;


    ivec3 centerCell =
        localLightGridCellCoord(
            surfacePos
        );

    uint packedIndexBase =
        localLightGridPackedIndexBase();


    for(int cellZ = -1; cellZ <= 1; ++cellZ)
    {
        for(int cellY = -1; cellY <= 1; ++cellY)
        {
            for(int cellX = -1; cellX <= 1; ++cellX)
            {
                uint firstLightIndex =
                    0u;

                uint cellLightCount =
                    0u;


                if(
                    !localLightGridLookupCell(
                        centerCell +
                        ivec3(
                            cellX,
                            cellY,
                            cellZ
                        ),
                        firstLightIndex,
                        cellLightCount
                    )
                )
                {
                    continue;
                }


                for(
                    uint candidateOffset = 0u;
                    candidateOffset < cellLightCount;
                    ++candidateOffset
                )
                {
                    uint lightIndex =
                        localLightGridBuffer.words[
                            packedIndexBase +
                            firstLightIndex +
                            candidateOffset
                        ];


                    if(lightIndex >= lightCount)
                    {
                        continue;
                    }


                    weightSum +=
                        g2bLocalGuideSelectionWeight(
                            surfacePos,
                            surfaceNormal,
                            localLightBuffer.lights[
                                lightIndex
                            ]
                        );
                }
            }
        }
    }


    return
        weightSum;
}


bool g2bSampleSphereGuideDirection(
    vec3 surfacePos,
    vec3 emitterBlockPos,
    inout uint rngState,
    out vec3 sampledDirection,
    out float directionalPdf
)
{
    vec3 sphereCenter =
        emitterBlockPos +
        vec3(0.5);


    vec3 toCenter =
        sphereCenter -
        surfacePos;


    float centerDistanceSquared =
        dot(
            toCenter,
            toCenter
        );


    float radiusSquared =
        LOCAL_LIGHT_POINT_SPHERE_RADIUS *
        LOCAL_LIGHT_POINT_SPHERE_RADIUS;


    if(
        centerDistanceSquared <=
        radiusSquared +
            G2B_LOCAL_GUIDE_EPSILON
    )
    {
        sampledDirection =
            vec3(0.0, 1.0, 0.0);

        directionalPdf =
            0.0;

        return
            false;
    }


    float centerDistance =
        sqrt(
            centerDistanceSquared
        );


    vec3 coneAxis =
        toCenter /
        centerDistance;


    float cosThetaMax =
        sqrt(
            max(
                1.0 -
                    radiusSquared /
                    centerDistanceSquared,
                0.0
            )
        );


    float cosTheta =
        mix(
            1.0,
            cosThetaMax,
            rand(rngState)
        );


    float sinTheta =
        sqrt(
            max(
                1.0 -
                    cosTheta *
                    cosTheta,
                0.0
            )
        );


    float phi =
        2.0 *
        PI *
        rand(rngState);


    vec3 referenceAxis =
        abs(coneAxis.z) < 0.999
            ? vec3(0.0, 0.0, 1.0)
            : vec3(1.0, 0.0, 0.0);


    vec3 tangent =
        normalize(
            cross(
                referenceAxis,
                coneAxis
            )
        );


    vec3 bitangent =
        cross(
            coneAxis,
            tangent
        );


    sampledDirection =
        normalize(
            tangent *
                (
                    cos(phi) *
                    sinTheta
                ) +
            bitangent *
                (
                    sin(phi) *
                    sinTheta
                ) +
            coneAxis *
                cosTheta
        );


    directionalPdf =
        1.0 /
        max(
            2.0 *
                PI *
                (
                    1.0 -
                    cosThetaMax
                ),
            G2B_LOCAL_GUIDE_EPSILON
        );


    return
        true;
}


float g2bSphereGuideDirectionalPdf(
    vec3 surfacePos,
    vec3 rayDirection,
    vec3 emitterBlockPos
)
{
    vec3 sphereCenter =
        emitterBlockPos +
        vec3(0.5);


    vec3 toCenter =
        sphereCenter -
        surfacePos;


    float centerDistanceSquared =
        dot(
            toCenter,
            toCenter
        );


    float radiusSquared =
        LOCAL_LIGHT_POINT_SPHERE_RADIUS *
        LOCAL_LIGHT_POINT_SPHERE_RADIUS;


    if(
        centerDistanceSquared <=
        radiusSquared +
            G2B_LOCAL_GUIDE_EPSILON
    )
    {
        return
            0.0;
    }


    vec3 originFromCenter =
        surfacePos -
        sphereCenter;


    float b =
        dot(
            originFromCenter,
            rayDirection
        );


    float c =
        dot(
            originFromCenter,
            originFromCenter
        ) -
        radiusSquared;


    float discriminant =
        b * b -
        c;


    if(discriminant < 0.0)
    {
        return
            0.0;
    }


    float root =
        sqrt(
            discriminant
        );


    float farIntersection =
        -b +
        root;


    if(farIntersection <= 0.0)
    {
        return
            0.0;
    }


    float cosThetaMax =
        sqrt(
            max(
                1.0 -
                    radiusSquared /
                    centerDistanceSquared,
                0.0
            )
        );


    return
        1.0 /
        max(
            2.0 *
                PI *
                (
                    1.0 -
                    cosThetaMax
                ),
            G2B_LOCAL_GUIDE_EPSILON
        );
}


bool g2bSampleCubeGuideDirection(
    vec3 surfacePos,
    vec3 emitterBlockPos,
    inout uint rngState,
    out vec3 sampledDirection,
    out float directionalPdf
)
{
    vec3 emitterCenter =
        emitterBlockPos +
        vec3(0.5);


    vec3 receiverOffset =
        surfacePos -
        emitterCenter;


    vec3 faceWeights =
        abs(receiverOffset) +
        vec3(1.0e-4);


    float faceWeightSum =
        faceWeights.x +
        faceWeights.y +
        faceWeights.z;


    float faceSelector =
        rand(rngState) *
        faceWeightSum;


    float usableSide =
        1.0 -
        2.0 *
        G2B_CUBE_GUIDE_INSET;


    float u =
        G2B_CUBE_GUIDE_INSET +
        rand(rngState) *
        usableSide;


    float v =
        G2B_CUBE_GUIDE_INSET +
        rand(rngState) *
        usableSide;


    vec3 samplePos =
        emitterCenter;


    vec3 emitterNormal =
        vec3(0.0);


    float faceProbability =
        1.0;


    if(faceSelector < faceWeights.x)
    {
        bool positiveFace =
            receiverOffset.x >= 0.0;


        faceProbability =
            faceWeights.x /
            faceWeightSum;


        samplePos =
            vec3(
                emitterBlockPos.x +
                    (
                        positiveFace
                            ? 1.0
                            : 0.0
                    ),
                emitterBlockPos.y + u,
                emitterBlockPos.z + v
            );


        emitterNormal =
            vec3(
                positiveFace
                    ? 1.0
                    : -1.0,
                0.0,
                0.0
            );
    }
    else if(
        faceSelector <
        faceWeights.x +
            faceWeights.y
    )
    {
        bool positiveFace =
            receiverOffset.y >= 0.0;


        faceProbability =
            faceWeights.y /
            faceWeightSum;


        samplePos =
            vec3(
                emitterBlockPos.x + u,
                emitterBlockPos.y +
                    (
                        positiveFace
                            ? 1.0
                            : 0.0
                    ),
                emitterBlockPos.z + v
            );


        emitterNormal =
            vec3(
                0.0,
                positiveFace
                    ? 1.0
                    : -1.0,
                0.0
            );
    }
    else
    {
        bool positiveFace =
            receiverOffset.z >= 0.0;


        faceProbability =
            faceWeights.z /
            faceWeightSum;


        samplePos =
            vec3(
                emitterBlockPos.x + u,
                emitterBlockPos.y + v,
                emitterBlockPos.z +
                    (
                        positiveFace
                            ? 1.0
                            : 0.0
                    )
            );


        emitterNormal =
            vec3(
                0.0,
                0.0,
                positiveFace
                    ? 1.0
                    : -1.0
            );
    }


    vec3 toSample =
        samplePos -
        surfacePos;


    float sampleDistanceSquared =
        dot(
            toSample,
            toSample
        );


    if(
        sampleDistanceSquared <=
        G2B_LOCAL_GUIDE_EPSILON
    )
    {
        sampledDirection =
            vec3(0.0, 1.0, 0.0);

        directionalPdf =
            0.0;

        return
            false;
    }


    float sampleDistance =
        sqrt(
            sampleDistanceSquared
        );


    sampledDirection =
        toSample /
        sampleDistance;


    float emitterFacing =
        max(
            dot(
                emitterNormal,
                -sampledDirection
            ),
            0.0
        );


    if(
        emitterFacing <=
        G2B_LOCAL_GUIDE_EPSILON
    )
    {
        directionalPdf =
            0.0;

        return
            false;
    }


    float faceArea =
        usableSide *
        usableSide;


    directionalPdf =
        faceProbability *
        sampleDistanceSquared /
        max(
            emitterFacing *
                faceArea,
            G2B_LOCAL_GUIDE_EPSILON
        );


    return
        true;
}


float g2bCubeGuideDirectionalPdf(
    vec3 surfacePos,
    vec3 rayDirection,
    vec3 emitterBlockPos
)
{
    vec3 boxMin =
        emitterBlockPos;


    vec3 boxMax =
        emitterBlockPos +
        vec3(1.0);


    vec3 safeDirection =
        vec3(
            abs(rayDirection.x) > 1.0e-8
                ? rayDirection.x
                : (
                    rayDirection.x >= 0.0
                        ? 1.0e-8
                        : -1.0e-8
                ),
            abs(rayDirection.y) > 1.0e-8
                ? rayDirection.y
                : (
                    rayDirection.y >= 0.0
                        ? 1.0e-8
                        : -1.0e-8
                ),
            abs(rayDirection.z) > 1.0e-8
                ? rayDirection.z
                : (
                    rayDirection.z >= 0.0
                        ? 1.0e-8
                        : -1.0e-8
                )
        );


    vec3 t0 =
        (
            boxMin -
            surfacePos
        ) /
        safeDirection;


    vec3 t1 =
        (
            boxMax -
            surfacePos
        ) /
        safeDirection;


    vec3 nearByAxis =
        min(
            t0,
            t1
        );


    vec3 farByAxis =
        max(
            t0,
            t1
        );


    float nearDistance =
        max(
            max(
                nearByAxis.x,
                nearByAxis.y
            ),
            nearByAxis.z
        );


    float farDistance =
        min(
            min(
                farByAxis.x,
                farByAxis.y
            ),
            farByAxis.z
        );


    if(
        farDistance <
            max(
                nearDistance,
                0.0
            ) ||
        nearDistance <= 0.0
    )
    {
        return
            0.0;
    }


    int hitAxis =
        0;


    if(nearByAxis.y > nearByAxis.x)
    {
        hitAxis =
            1;
    }


    if(
        nearByAxis.z >
        nearByAxis[hitAxis]
    )
    {
        hitAxis =
            2;
    }


    vec3 hitPoint =
        surfacePos +
        rayDirection *
        nearDistance;


    float inset =
        G2B_CUBE_GUIDE_INSET;


    bool insideSampledFace =
        true;


    if(hitAxis == 0)
    {
        insideSampledFace =
            hitPoint.y >= boxMin.y + inset &&
            hitPoint.y <= boxMax.y - inset &&
            hitPoint.z >= boxMin.z + inset &&
            hitPoint.z <= boxMax.z - inset;
    }
    else if(hitAxis == 1)
    {
        insideSampledFace =
            hitPoint.x >= boxMin.x + inset &&
            hitPoint.x <= boxMax.x - inset &&
            hitPoint.z >= boxMin.z + inset &&
            hitPoint.z <= boxMax.z - inset;
    }
    else
    {
        insideSampledFace =
            hitPoint.x >= boxMin.x + inset &&
            hitPoint.x <= boxMax.x - inset &&
            hitPoint.y >= boxMin.y + inset &&
            hitPoint.y <= boxMax.y - inset;
    }


    if(!insideSampledFace)
    {
        return
            0.0;
    }


    vec3 receiverOffset =
        surfacePos -
        (
            emitterBlockPos +
            vec3(0.5)
        );


    vec3 faceWeights =
        abs(receiverOffset) +
        vec3(1.0e-4);


    float faceWeightSum =
        faceWeights.x +
        faceWeights.y +
        faceWeights.z;


    float faceProbability =
        faceWeights[hitAxis] /
        faceWeightSum;


    float emitterFacing =
        abs(
            rayDirection[hitAxis]
        );


    float usableSide =
        1.0 -
        2.0 *
        inset;


    float faceArea =
        usableSide *
        usableSide;


    return
        faceProbability *
        nearDistance *
        nearDistance /
        max(
            emitterFacing *
                faceArea,
            G2B_LOCAL_GUIDE_EPSILON
        );
}


float g2bLocalGuideDirectionalPdf(
    vec3 surfacePos,
    vec3 surfaceNormal,
    vec3 rayDirection,
    float guideWeightSum
)
{
    if(
        guideWeightSum <=
        G2B_LOCAL_GUIDE_EPSILON
    )
    {
        return
            0.0;
    }


    float mixturePdf =
        0.0;


    uint lightCount =
        localLightBuffer.header.x;


    ivec3 centerCell =
        localLightGridCellCoord(
            surfacePos
        );

    uint packedIndexBase =
        localLightGridPackedIndexBase();


    for(int cellZ = -1; cellZ <= 1; ++cellZ)
    {
        for(int cellY = -1; cellY <= 1; ++cellY)
        {
            for(int cellX = -1; cellX <= 1; ++cellX)
            {
                uint firstLightIndex =
                    0u;

                uint cellLightCount =
                    0u;


                if(
                    !localLightGridLookupCell(
                        centerCell +
                        ivec3(
                            cellX,
                            cellY,
                            cellZ
                        ),
                        firstLightIndex,
                        cellLightCount
                    )
                )
                {
                    continue;
                }


                for(
                    uint candidateOffset = 0u;
                    candidateOffset < cellLightCount;
                    ++candidateOffset
                )
                {
                    uint lightIndex =
                        localLightGridBuffer.words[
                            packedIndexBase +
                            firstLightIndex +
                            candidateOffset
                        ];


                    if(lightIndex >= lightCount)
                    {
                        continue;
                    }


                    LocalLightGPU light =
                        localLightBuffer.lights[
                            lightIndex
                        ];


                    float selectionWeight =
                        g2bLocalGuideSelectionWeight(
                            surfacePos,
                            surfaceNormal,
                            light
                        );


                    if(selectionWeight <= 0.0)
                    {
                        continue;
                    }


                    float shapePdf =
                        isPointLikeLocalLightType(
                            light.meta.x
                        )
                            ? g2bSphereGuideDirectionalPdf(
                                surfacePos,
                                rayDirection,
                                light.worldPosEmission.xyz
                            )
                            : g2bCubeGuideDirectionalPdf(
                                surfacePos,
                                rayDirection,
                                light.worldPosEmission.xyz
                            );


                    mixturePdf +=
                        (
                            selectionWeight /
                            guideWeightSum
                        ) *
                        shapePdf;
                }
            }
        }
    }


    return
        mixturePdf;
}


bool g2bSampleLocalGuideDirection(
    vec3 surfacePos,
    vec3 surfaceNormal,
    float guideWeightSum,
    inout uint rngState,
    out vec3 sampledDirection,
    out float directionalPdf
)
{
    uint lightCount =
        localLightBuffer.header.x;


    if(
        lightCount == 0u ||
        guideWeightSum <=
            G2B_LOCAL_GUIDE_EPSILON
    )
    {
        sampledDirection =
            vec3(0.0, 1.0, 0.0);

        directionalPdf =
            0.0;

        return
            false;
    }


    float selectionTarget =
        rand(rngState) *
        guideWeightSum;


    float accumulatedWeight =
        0.0;


    uint selectedIndex =
        0u;


    bool hasEligibleLight =
        false;


    bool selected =
        false;


    ivec3 centerCell =
        localLightGridCellCoord(
            surfacePos
        );

    uint packedIndexBase =
        localLightGridPackedIndexBase();


    for(int cellZ = -1; cellZ <= 1; ++cellZ)
    {
        for(int cellY = -1; cellY <= 1; ++cellY)
        {
            for(int cellX = -1; cellX <= 1; ++cellX)
            {
                uint firstLightIndex =
                    0u;

                uint cellLightCount =
                    0u;


                if(
                    !localLightGridLookupCell(
                        centerCell +
                        ivec3(
                            cellX,
                            cellY,
                            cellZ
                        ),
                        firstLightIndex,
                        cellLightCount
                    )
                )
                {
                    continue;
                }


                for(
                    uint candidateOffset = 0u;
                    candidateOffset < cellLightCount;
                    ++candidateOffset
                )
                {
                    uint lightIndex =
                        localLightGridBuffer.words[
                            packedIndexBase +
                            firstLightIndex +
                            candidateOffset
                        ];


                    if(lightIndex >= lightCount)
                    {
                        continue;
                    }


                    float selectionWeight =
                        g2bLocalGuideSelectionWeight(
                            surfacePos,
                            surfaceNormal,
                            localLightBuffer.lights[
                                lightIndex
                            ]
                        );


                    if(selectionWeight <= 0.0)
                    {
                        continue;
                    }


                    hasEligibleLight =
                        true;


                    selectedIndex =
                        lightIndex;


                    accumulatedWeight +=
                        selectionWeight;


                    if(selectionTarget <= accumulatedWeight)
                    {
                        selected =
                            true;

                        break;
                    }
                }


                if(selected)
                {
                    break;
                }
            }


            if(selected)
            {
                break;
            }
        }


        if(selected)
        {
            break;
        }
    }


    if(!hasEligibleLight)
    {
        sampledDirection =
            vec3(0.0, 1.0, 0.0);

        directionalPdf =
            0.0;

        return
            false;
    }


    // Floating-point accumulation can place the target a few ULPs
    // above the final sum.  selectedIndex already stores the final
    // eligible light, which is the correct stable fallback.
    if(!selected)
    {
        selected =
            true;
    }


    LocalLightGPU selectedLight =
        localLightBuffer.lights[selectedIndex];


    float conditionalPdf =
        0.0;


    bool sampleValid =
        false;


    if(
        isPointLikeLocalLightType(
            selectedLight.meta.x
        )
    )
    {
        sampleValid =
            g2bSampleSphereGuideDirection(
                surfacePos,
                selectedLight.worldPosEmission.xyz,
                rngState,
                sampledDirection,
                conditionalPdf
            );
    }
    else
    {
        sampleValid =
            g2bSampleCubeGuideDirection(
                surfacePos,
                selectedLight.worldPosEmission.xyz,
                rngState,
                sampledDirection,
                conditionalPdf
            );
    }


    if(!sampleValid)
    {
        directionalPdf =
            0.0;

        return
            false;
    }


    directionalPdf =
        g2bLocalGuideDirectionalPdf(
            surfacePos,
            surfaceNormal,
            sampledDirection,
            guideWeightSum
        );


    return
        directionalPdf >
        G2B_LOCAL_GUIDE_EPSILON;
}


void g2bSampleDiffuseDirectionMIS(
    vec3 surfacePos,
    vec3 surfaceNormal,
    inout uint rngState,
    out vec3 sampledDirection,
    out float bsdfToMixtureWeight,
    bool useLocalGuide
)
{
    vec3 N =
        normalize(
            surfaceNormal
        );


    if(
        !useLocalGuide ||
        !LL_P3_ENABLE_G2B_LOCAL_GUIDE ||
        perfL10DisableG2bLocalGuide()
    )
    {
        sampledDirection =
            sampleHemisphereCosine(
                N,
                rngState
            );

        bsdfToMixtureWeight =
            1.0;

        return;
    }


    float guideWeightSum =
        g2bLocalGuideWeightSum(
            surfacePos,
            N
        );


    float guideProbability =
        guideWeightSum >
            G2B_LOCAL_GUIDE_EPSILON
                ? G2B_LOCAL_GUIDE_PROBABILITY
                : 0.0;


    bool chooseGuide =
        guideProbability > 0.0 &&
        rand(rngState) <
            guideProbability;


    float sampledGuidePdf =
        0.0;


    if(chooseGuide)
    {
        bool guideSampleValid =
            g2bSampleLocalGuideDirection(
                surfacePos,
                N,
                guideWeightSum,
                rngState,
                sampledDirection,
                sampledGuidePdf
            );


        if(!guideSampleValid)
        {
            guideProbability =
                0.0;

            sampledDirection =
                sampleHemisphereCosine(
                    N,
                    rngState
                );
        }
    }
    else
    {
        sampledDirection =
            sampleHemisphereCosine(
                N,
                rngState
            );
    }


    float NdotDirection =
        max(
            dot(
                N,
                sampledDirection
            ),
            0.0
        );


    float bsdfPdf =
        NdotDirection /
        PI;


    float lightPdf =
        0.0;


    if(guideProbability > 0.0)
    {
        lightPdf =
            chooseGuide
                ? sampledGuidePdf
                : g2bLocalGuideDirectionalPdf(
                    surfacePos,
                    N,
                    sampledDirection,
                    guideWeightSum
                );
    }


    float mixturePdf =
        (
            1.0 -
            guideProbability
        ) *
        bsdfPdf +
        guideProbability *
        lightPdf;


    bsdfToMixtureWeight =
        mixturePdf >
            G2B_LOCAL_GUIDE_EPSILON
                ? bsdfPdf /
                    mixturePdf
                : 0.0;
}


vec3 evaluatePrimaryLocalLights(
    vec3 hitPos,
    vec3 hitNormal,
    vec3 viewDir,
    vec3 hitAlbedo,
    float hitRoughness,
    int hitF0MetalCode,
    float primaryDistance,
    out vec3 emitterRadiance
)
{
    emitterRadiance =
        vec3(0.0);


    int activeLightCount =
        1;


    int areaSampleCount =
        clamp(int(cam.perfSecondary.y), 1, 2); // user "局部光区域采样"


    // LL-P3.1 primary budget:
    // - exactly one highest-scored local emitter;
    // - preserve the confirmed two L7 area samples in the near field;
    // - use one area sample from 16 blocks onward.
    if(primaryDistance >= 16.0)
    {
        areaSampleCount =
            1;
    }

    uint lightCount =
        localLightBuffer.header.x;


    if(lightCount == 0u)
    {
        return
            vec3(0.0);
    }


    vec3 N =
        normalize(
            hitNormal
        );


    vec3 V =
        normalize(
            viewDir
        );


    float NdotV =
        max(
            dot(
                N,
                V
            ),
            0.0
        );


    if(NdotV <= 0.0)
    {
        return
            vec3(0.0);
    }


    // -------------------------------------------------
    // Keep the strongest nearby contributor. No shadow ray is fired
    // during this candidate pass; Binding 18 restricts it to the only
    // grid cells that can contain an emitter within its 30-block range.
    // -------------------------------------------------

    uint selectedIndex[MAX_PRIMARY_LOCAL_LIGHTS];

    float selectedScore[MAX_PRIMARY_LOCAL_LIGHTS];
    for(
    int slot = 0;
    slot < MAX_PRIMARY_LOCAL_LIGHTS;
    ++slot
)
{
    selectedIndex[slot] =
        0u;

    selectedScore[slot] =
        -1.0;
}


    ivec3 centerCell =
        localLightGridCellCoord(
            hitPos
        );

    int minCellX =
        localLightGridNeighborMinOffset(
            hitPos.x
        );

    int maxCellX =
        localLightGridNeighborMaxOffset(
            hitPos.x
        );

    int minCellY =
        localLightGridNeighborMinOffset(
            hitPos.y
        );

    int maxCellY =
        localLightGridNeighborMaxOffset(
            hitPos.y
        );

    int minCellZ =
        localLightGridNeighborMinOffset(
            hitPos.z
        );

    int maxCellZ =
        localLightGridNeighborMaxOffset(
            hitPos.z
        );

    uint packedIndexBase =
        localLightGridPackedIndexBase();


    for(int cellZ = minCellZ; cellZ <= maxCellZ; ++cellZ)
    {
        for(int cellY = minCellY; cellY <= maxCellY; ++cellY)
        {
            for(int cellX = minCellX; cellX <= maxCellX; ++cellX)
            {
                uint firstLightIndex =
                    0u;

                uint cellLightCount =
                    0u;


                if(
                    !localLightGridLookupCell(
                        centerCell +
                        ivec3(
                            cellX,
                            cellY,
                            cellZ
                        ),
                        firstLightIndex,
                        cellLightCount
                    )
                )
                {
                    continue;
                }


                for(
                    uint candidateOffset = 0u;
                    candidateOffset < cellLightCount;
                    ++candidateOffset
                )
                {
                    uint lightIndex =
                        localLightGridBuffer.words[
                            packedIndexBase +
                            firstLightIndex +
                            candidateOffset
                        ];


                    if(lightIndex >= lightCount)
                    {
                        continue;
                    }


                    LocalLightGPU candidateLight =
                        localLightBuffer.lights[
                            lightIndex
                        ];

                    vec4 worldPosEmission =
                        candidateLight.worldPosEmission;


                    emitterRadiance =
                        max(
                            emitterRadiance,
                            evaluateLocalEmitterCandidateRadiance(
                                hitPos,
                                -V,
                                hitAlbedo,
                                candidateLight
                            )
                        );


                    float score =
                        localLightApproxScore(
                            hitPos,
                            N,
                            worldPosEmission,
                            candidateLight.meta.x
                        );


                    if(score <= 0.0)
                    {
                        continue;
                    }


                    if(score > selectedScore[0])
                    {
                        selectedScore[0] =
                            score;

                        selectedIndex[0] =
                            lightIndex;
                    }
                }
            }
        }
    }




    vec3 F0 =
        decodeBaseF0(
            hitF0MetalCode,
            hitAlbedo
        );


    float metallic =
        decodeMetallicFromCode(
            hitF0MetalCode
        );


    float roughness =
        clamp(
            hitRoughness,
            0.04,
            1.0
        );


    vec3 accumulatedLocalLight =
        vec3(0.0);


    // -------------------------------------------------
    // L7.1 B mode:
    // 2 real cube-surface visibility rays per selected light.
    // Up to 4 selected lights -> at most 8 rays / pixel.
    // -------------------------------------------------

for(
    int slot = 0;
    slot < activeLightCount;
    ++slot
)
{
    if(selectedScore[slot] <= 0.0)
    {
        break;
    }


LocalLightGPU selectedLight =
    localLightBuffer
        .lights[
            selectedIndex[slot]
        ];


vec4 worldPosEmission =
    selectedLight
        .worldPosEmission;


uint typeCode =
    selectedLight
        .meta.x;


float emissionLevel =
            clamp(
                worldPosEmission.w,
                1.0,
                15.0
            );


        vec3 lightCenter =
            worldPosEmission.xyz +
            vec3(0.5);


        vec3 centerToLight =
            lightCenter -
            hitPos;


        float centerDistanceSquared =
            dot(
                centerToLight,
                centerToLight
            );


        if(centerDistanceSquared <= 1.0e-6)
        {
            continue;
        }


        float centerDistance =
            sqrt(
                centerDistanceSquared
            );


        float centerRangeFade =
            localLightRangeFade(
                centerDistance,
                typeCode,
                emissionLevel
            );


        if(centerRangeFade <= 0.0)
        {
            continue;
        }


        // 2026-10-31：亮度按光源档案（每档自己的满级亮度），不再统一 emission/15。
        float emission01 =
            localLightProfileBrightness(
                typeCode,
                emissionLevel
            );


        vec3 localLightColor =
            decodeLocalLightColor(
                typeCode
            );


        bool pointLikeEmitter =
            isPointLikeLocalLightType(
                typeCode
            );


        vec3 integratedAreaLight =
            vec3(0.0);


        // =================================================
        // L7.1 / G1 - Effective 2.0x2.0 cube-face emitter
        //
        // Exactly two points are sampled on visible effective
        // cube-face areas.  Each sample independently evaluates:
        //
        //   emitter surface point
        //   distance attenuation
        //   receiver NdotL
        //   emitter-facing cosine
        //   colored glass visibility
        //   GGX + diffuse response
        //
        // This is what creates geometric soft shadows:
        // blocker/source/receiver distances now change penumbra
        // width naturally instead of applying a fixed blur radius.
        // =================================================

for(
    int areaSample = 0;
    areaSample <
        areaSampleCount;
    ++areaSample
)
        {
            vec3 emitterSurfaceNormal =
                vec3(0.0);


            float faceSelectionProbability =
                1.0;


            vec3 sampleLightPos =
                vec3(0.0);


            if(pointLikeEmitter)
            {
                sampleLightPos =
                    sampleLocalLightPointSphereSurface(
                        worldPosEmission.xyz,
                        hitPos,
                        slot,
                        areaSample
                    );
            }
            else
            {
                sampleLightPos =
                    sampleLocalLightCubeSurface(
                        worldPosEmission.xyz,
                        hitPos,
                        slot,
                        areaSample,
                        emitterSurfaceNormal,
                        faceSelectionProbability
                    );
            }


            vec3 sampleToLight =
                sampleLightPos -
                hitPos;


            float sampleDistanceSquared =
                dot(
                    sampleToLight,
                    sampleToLight
                );


            if(sampleDistanceSquared <= 1.0e-8)
            {
                continue;
            }


            float sampleDistance =
                sqrt(
                    sampleDistanceSquared
                );


            float sampleRangeFade =
                localLightRangeFade(
                    sampleDistance,
                    typeCode,
                    emissionLevel
                );


            if(sampleRangeFade <= 0.0)
            {
                continue;
            }


            vec3 sampleDirection =
                sampleToLight /
                sampleDistance;


            float geometricNdotL =
                max(
                    dot(
                        N,
                        sampleDirection
                    ),
                    0.0
                );


            if(geometricNdotL <= 0.0)
            {
                continue;
            }


            float sourceAngularWeight =
                LOCAL_LIGHT_POINT_SPHERE_PROJECTED_AREA;


            // The sphere was sampled over its projected disk, so its
            // exact projected area is the Monte-Carlo source weight.
            // The cube face was importance-selected by projected axis
            // weight, so compensate that face-selection probability.
            if(!pointLikeEmitter)
            {
                float emitterFacing =
                    max(
                        dot(
                            emitterSurfaceNormal,
                            -sampleDirection
                        ),
                        0.0
                    );


                if(emitterFacing <= 0.0)
                {
                    continue;
                }


                sourceAngularWeight =
                    emitterFacing /
                    max(
                        faceSelectionProbability,
                        1.0e-4
                    );
            }


            float shadowMaxDistance =
                sampleDistance -
                LOCAL_LIGHT_CUBE_RAY_END_EPSILON;


            bool sampleVisible =
                true;


            vec3 sampleTransmittance =
                vec3(1.0);


            if(shadowMaxDistance > 0.03)
            {
                hit.hitDistance =
                    1.0;

                hit.albedoPacked = packAlbedoUnorm(
                    vec3(1.0));

                hit.parallaxRayTravel =
                    0.0;

                hit.state = makeTraceState(RT_PURPOSE_GLASS_VISIBILITY);
                // 2026-10-23：可见性射线必须从 albedo = vec3(1.0) 起算 ——
                // 见 HANDOFF-2026-10-21-VERTEX-32B-TERRAIN.md §8。否则累乘起点是
                // 上一条射线的残留 albedo，逐帧不同 ⇒ 静止时被 DLSS-RR 当信号累积。
                hit.albedoPacked = packAlbedoUnorm(vec3(1.0));


#ifdef RT_ENABLE_SER
                // Perf-L10.8: same ray, same flags; SER only reschedules the hit shader.
                if(cam.perfSerAb.y != 0u)
                {
                    serTraceRay(
                        // 2026-10-23：偏移改沿射线；沿法线会在方块边缘漏光（复盘见 :3037）
                        hitPos + sampleDirection * 0.001,
                        0.02,
                        sampleDirection,
                        shadowMaxDistance,
                        gl_RayFlagsTerminateOnFirstHitEXT |
                        gl_RayFlagsSkipClosestHitShaderEXT,
                        0xff,
                        areaSample
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

                        hitPos + sampleDirection * 0.001,

                        0.02,

                        sampleDirection,

                        shadowMaxDistance,

                        0
                    );
                }


                sampleVisible =
                    hit.hitDistance <
                    0.0;


                if(sampleVisible)
                {
                    sampleTransmittance =
                        clamp(
                            unpackAlbedoUnorm(hit.albedoPacked),
                            vec3(0.0),
                            vec3(1.0)
                        );
                }
            }


            if(!sampleVisible)
            {
                continue;
            }


            // Physical receiver cosine.  The old sqrt blend lifted
            // grazing angles and produced the visible floor halo.
            float softDiffuseNdotL =
                geometricNdotL;


            float localSpecularRoughness =
                max(
                    roughness,
                    BSDF_LOCAL_MIN_SPEC_ROUGHNESS
                );


            vec3 H =
                normalize(
                    V +
                    sampleDirection
                );


            float VdotH =
                max(
                    dot(
                        V,
                        H
                    ),
                    0.0
                );


            BsdfTerms bsdf =
                evalBSDFTerms(
                    N,
                    V,
                    sampleDirection,
                    F0,
                    metallic,
                    localSpecularRoughness,
                    NdotV,
                    true
                );


            vec3 diffuseTerm =
                bsdf.kD *
                hitAlbedo *
                softDiffuseNdotL *
                BSDF_LOCAL_DIFFUSE_SCALE;


            vec3 specularTerm =
                bsdf.specular *
                geometricNdotL *
                BSDF_LOCAL_SPECULAR_SCALE;


float distanceAttenuation =
    localLightDistanceAttenuation(
        sampleDistance
    );


            // emitterFacing is the source-side cosine term of
            // the sampled physical cube face.
            float radianceScale =
    3.2 *
    LOCAL_LIGHT_L7_RADIANCE_SCALE * localLightIntensity() *
    emission01 *
    distanceAttenuation *
    sampleRangeFade *
    sourceAngularWeight;


            vec3 sampleRadiance =
                localLightColor *
                radianceScale;


            integratedAreaLight +=
                sampleRadiance *
                sampleTransmittance *
                (
                    diffuseTerm +
                    specularTerm
                );
        }


        accumulatedLocalLight +=
integratedAreaLight /
float(
    areaSampleCount
);
    }


    return
        accumulatedLocalLight;
}