#ifndef FOG_FROXEL_COMMON_GLSL
#define FOG_FROXEL_COMMON_GLSL

// ===========================================================================
// Volumetric Fog & Volumetric Light — Shared Froxel Math & Utilities
//
// 3D Froxel grid:
//   XY : frustum-aligned screen grid (froxelW x froxelH)
//   Z  : N = 64 slices (N - 1 = 63 depth segments)
//   Distance distribution (zero-anchored exponential, exact analytical inverse):
//     d(u) = maxDistance * (exp(K * u) - 1) / (exp(K) - 1),   u in [0, 1]
//     u(d) = log(1 + clamp(d / maxDistance, 0, 1) * (exp(K) - 1)) / K
//
// Texture Z conventions (size.z = N, segments = N - 1):
//   - Light volume (Pass 2): texel s in [0 .. N-2] holds segment [d(s/(N-1)), d((s+1)/(N-1))]
//     Unjittered center u = (s + 0.5) / (N - 1) -> history sample W = u * (N - 1) / N.
//   - Integrated volume (Pass 3):
//     Texel 0     = (0, 0, 0, 1)  at d = 0
//     Texel k     = integral [0, d(k/(N-1))]  for k in [1 .. N-1]
//     Apply pass lookup W = (u * (N - 1) + 0.5) / N  (exact half-texel aligned).
// ===========================================================================

const float FOG_PI = 3.14159265359;
const float FOG_NO_GEOMETRY_DISTANCE = 1.0e30;
const float FOG_EXP_K = 4.2;
const float FOG_EXP_SCALE = 65.6863310409; // exp(4.2) - 1.0
const float FOG_INV_EXP_K = 1.0 / 4.2;

// TLAS cull mask for volumetric fog & light ray queries (WorldGeometry.cpp):
//   OPAQUE (0xFF), OMM foliage (0xFF), WATER (0xFF, FORCE_NO_OPAQUE),
//   ENTITY (0xFF), DH far (0xFE), ALPHA glass (0xFB, FORCE_NO_OPAQUE) -> bit 3 = 1
//   PARTICLE (0xF7), LOCAL_PLAYER (0x02), HAND (0x04)                 -> bit 3 = 0
// Combined with rayFlags WITHOUT gl_RayFlagsOpaqueEXT:
//   - Solid terrain/blocks & opaque OMM leaf micro-triangles BLOCK light
//   - Glass windows & water (FORCE_NO_OPAQUE) LET light pass through
//   - Hand, player body, and particles are completely excluded
const uint FOG_WORLD_CULL_MASK = 0x08u;

float fogNormalizedToDistance(float u, float maxDistance)
{
    const float uc = clamp(u, 0.0, 1.0);
    return maxDistance * (exp(FOG_EXP_K * uc) - 1.0) / FOG_EXP_SCALE;
}

float fogSliceBoundaryDistance(int boundaryIndex, int layers, float maxDistance)
{
    const float segments = float(max(layers - 1, 1));
    const float u = clamp(float(boundaryIndex) / segments, 0.0, 1.0);
    return fogNormalizedToDistance(u, maxDistance);
}

float fogSegmentSampleDistance(int segment, float zOffset01, int layers, float maxDistance)
{
    const float segments = float(max(layers - 1, 1));
    const float u = clamp((float(segment) + zOffset01) / segments, 0.0, 1.0);
    return fogNormalizedToDistance(u, maxDistance);
}

float fogDistanceToSegmentU(float dist, float maxDistance)
{
    const float dNorm = clamp(dist / max(maxDistance, 1.0e-3), 0.0, 1.0);
    return clamp(log(1.0 + dNorm * FOG_EXP_SCALE) * FOG_INV_EXP_K, 0.0, 1.0);
}

// Texel s center in light history 3D texture (size.z = layers) corresponds to
// segment u = (s + 0.5) / (layers - 1). Solving (s + 0.5) / layers = w gives:
//   w = u * (layers - 1) / layers.
float fogDistanceToLightHistoryW(float dist, float maxDistance, int layers)
{
    const float u = fogDistanceToSegmentU(dist, maxDistance);
    const float segments = float(max(layers - 1, 1));
    return u * (segments / float(max(layers, 1)));
}

// Texel k center in integrated 3D texture (size.z = layers) holds integral up to
// boundary u = k / (layers - 1), with k = 0 exact neutral (0,0,0,1) at d = 0.
float fogDistanceToIntegratedW(float dist, float maxDistance, int layers)
{
    const float u = fogDistanceToSegmentU(dist, maxDistance);
    const float segments = float(max(layers - 1, 1));
    return (u * segments + 0.5) / float(max(layers, 1));
}

// Reconstruct world-space ray direction for screen UV in [0, 1]^2.
vec3 fogWorldRayDirection(vec2 uv, mat4 projInverse, mat4 viewInverse)
{
    vec2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y;
    const vec4 viewPos = projInverse * vec4(ndc, 1.0, 1.0);
    const vec3 viewTarget = abs(viewPos.w) > 1.0e-6
        ? (viewPos.xyz / viewPos.w) : viewPos.xyz;
    return normalize((viewInverse * vec4(normalize(viewTarget), 0.0)).xyz);
}

// Reversed-Z sceneDepth (0.0 = sky/miss, 1.0 = near) -> linear distance along ray.
float fogSceneDistanceForDepth(vec2 uv, float depthValue, mat4 projInverse)
{
    if(depthValue <= 1.0e-5)
    {
        return FOG_NO_GEOMETRY_DISTANCE;
    }
    vec2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y;
    const vec4 viewPos = projInverse * vec4(ndc, depthValue, 1.0);
    if(abs(viewPos.w) <= 1.0e-6)
    {
        return FOG_NO_GEOMETRY_DISTANCE;
    }
    return length(viewPos.xyz / viewPos.w);
}

// Henyey-Greenstein phase function.
float fogPhaseHG(float cosTheta, float g)
{
    const float g2 = g * g;
    const float denom = max(1.0e-4, 1.0 + g2 - 2.0 * g * cosTheta);
    return (1.0 - g2) / (4.0 * FOG_PI * denom * sqrt(denom));
}

// Dual-lobe phase for atmospheric haze: smooth forward scatter + isotropic floor.
float fogPhaseHaze(float cosTheta, float g)
{
    const float gc = clamp(g, 0.0, 0.90);
    return 0.65 * fogPhaseHG(cosTheta, gc) + 0.35 * (1.0 / (4.0 * FOG_PI));
}

// Forward-gated dual-lobe phase for volumetric light shafts:
// crisp forward/diagonal sunbeams while naturally suppressing overhead open-sky back-glow.
float fogPhaseShaft(float cosTheta, float g)
{
    const float gc = clamp(g, 0.45, 0.88);
    const float forwardGate = smoothstep(-0.35, 0.25, cosTheta);
    return (0.84 * fogPhaseHG(cosTheta, gc) + 0.16 * fogPhaseHG(cosTheta, 0.30)) *
        mix(0.18, 1.0, forwardGate);
}

// Broad 2nd-order multi-scattering wrap phase for luminous volumetric shafts.
float fogPhaseShaftBroad(float cosTheta, float g)
{
    const float forwardGate = smoothstep(-0.25, 0.35, cosTheta);
    return fogPhaseHG(cosTheta, clamp(g * 0.50, 0.20, 0.50)) *
        mix(0.15, 1.0, forwardGate);
}

// Screen-space interleaved gradient noise for sub-slice Z dithering.
float fogInterleavedGradientNoise(vec2 pixel, uint frame)
{
    const vec2 p = pixel + float(frame & 63u) * vec2(5.588238, 5.588238);
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// Fast 3D hash -> [0, 1).
float fogHash31(ivec3 p)
{
    uint h = uint(p.x) * 374761393u + uint(p.y) * 668265263u + uint(p.z) * 2147483647u;
    h = (h ^ (h >> 13u)) * 1274126177u;
    h ^= h >> 16u;
    return float(h & 0x00FFFFFFu) * (1.0 / 16777216.0);
}

// Low-discrepancy 3D temporal jitter in [0, 1)^3 using R3 quasirandom sequence.
vec3 fogTemporalJitter3D(ivec3 coord, uint frame)
{
    uint seed = uint(coord.x) * 1973u + uint(coord.y) * 9277u + uint(coord.z) * 26699u;
    seed ^= seed >> 16u;
    seed *= 2246822519u;
    seed ^= seed >> 13u;
    const vec3 base = vec3(
        float(seed & 0x3FFu),
        float((seed >> 10u) & 0x3FFu),
        float((seed >> 20u) & 0x3FFu)
    ) * (1.0 / 1024.0);
    const float f = float(frame & 0x3FFu);
    return fract(base + f * vec3(0.7548776662, 0.5698402910, 0.4301597090));
}

// Smooth 3D value noise in [0, 1].
float fogValueNoise3D(vec3 p)
{
    const ivec3 i = ivec3(floor(p));
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);

    const float n000 = fogHash31(i + ivec3(0, 0, 0));
    const float n100 = fogHash31(i + ivec3(1, 0, 0));
    const float n010 = fogHash31(i + ivec3(0, 1, 0));
    const float n110 = fogHash31(i + ivec3(1, 1, 0));
    const float n001 = fogHash31(i + ivec3(0, 0, 1));
    const float n101 = fogHash31(i + ivec3(1, 0, 1));
    const float n011 = fogHash31(i + ivec3(0, 1, 1));
    const float n111 = fogHash31(i + ivec3(1, 1, 1));

    const float nx00 = mix(n000, n100, f.x);
    const float nx10 = mix(n010, n110, f.x);
    const float nx01 = mix(n001, n101, f.x);
    const float nx11 = mix(n011, n111, f.x);

    const float nxy0 = mix(nx00, nx10, f.y);
    const float nxy1 = mix(nx01, nx11, f.y);
    return mix(nxy0, nxy1, f.z);
}

// Lightweight 2-octave 3D fBm in [0, 1] for organic volumetric fog wisps.
float fogFbm3D(vec3 p)
{
    const float a = fogValueNoise3D(p);
    const float b = fogValueNoise3D(p * 2.03 + vec3(13.7, -5.3, 9.1));
    return a * 0.67 + b * 0.33;
}

#endif
