
uint pcg_hash(inout uint state) {
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    state = (state * 747796405u) + 2891336453u;
    return (word >> 22u) ^ word;
}

float rand(inout uint state) {
    return float(pcg_hash(state)) / 4294967296.0;
}

// =====================================================
// 2026-10-26 稀疏渲染白噪声。
//
// 逐字移植 RTX Remix（_ref/dxvk-remix）：
//   src/dxvk/shaders/rtx/utility/noise.slangh       uintHash / whiteNoise
//   src/dxvk/shaders/rtx/utility/packing.slangh     unorm23ToFloat
//   src/dxvk/shaders/rtx/pass/sparse_rendering/per_pixel_rate.slangh
//
// ⚠ 2026-10-27 更正：Remix 的 per-pixel rate **默认是蓝噪声**
// （Options::perPixelRateNoiseSource 默认 BlueNoise128x128x64x8），
// 白噪声只是可选项 PerPixelRateNoiseSource::WhiteNoise
// （whiteNoise(pixelCoordinate, frameIdx, 0u)）。原注释写反了。
// 下面这条 uintHash 保留：蓝噪声表缺失时的兜底与 A/B 用。
// =====================================================
uint uintHash(uint x)
{
    x ^= x >> 16;
    x *= 0x21f0aaadU;
    x ^= x >> 15;
    x *= 0x735a2d97U;
    x ^= x >> 15;
    return x;
}

float unorm23ToFloat(uint x)
{
    const uint ieeeMantissa = 0x007FFFFFu;
    const uint ieeeOne = 0x3F800000u;
    x &= ieeeMantissa;
    x |= ieeeOne;
    return uintBitsToFloat(x) - 1.0;
}

float vptWhiteNoise(uvec2 pixelCoord, uint temporalIdx, uint seed)
{
    return unorm23ToFloat(
        uintHash(
            uintHash(pixelCoord.x) ^
            uintHash(pixelCoord.y) ^
            uintHash(temporalIdx) ^
            uintHash(seed)
        )
    );
}

// =====================================================
// 2026-10-27 稀疏渲染判定阈值。逐行照搬 RTX Remix：
//   per_pixel_rate.slangh  calculatePerPixelRateThreshold()
//     BlueNoise128x128x64x8 分支：
//       BlueNoise.Load(int4(pixel.x & 127, pixel.y & 127, frameIdx & 63, 0)).r
// 数据与玻璃 alpha blend 共用 binding 76 的同一份 128x128x64 R8 表
// （_ref/dxvk-remix/src/util/util_blueNoise_128x128x64.h，MIT）。
// 与 Remix 一致：不做 min(x, 1-1/255) 钳制 —— 那是 getNextSampleBlueNoise
// 的 RNG 口径，不是 per-pixel rate 的口径。
// =====================================================
float vptSparseBlueNoiseThreshold(uvec2 pixelCoord, uint frameIdx)
{
    const uint x = pixelCoord.x & 127u;
    const uint y = pixelCoord.y & 127u;
    const uint layer = frameIdx & 63u;
    const uint byteIndex = 16384u * layer + 128u * y + x;
    const uint word = glassBlueNoise.words[byteIndex >> 2u];
    const uint raw = (word >> ((byteIndex & 3u) * 8u)) & 0xFFu;
    return float(raw) * (1.0 / 255.0);
}

// =====================================================
// 2026-10-27 稀疏辐亮度的无偏缩放。
// Remix: sparse_rendering.slangh:39-52
//   clampSamplingRateForRadianceScale(r) = max(r, 1/255)
//   calculateRadianceScaleFromSamplingRate(r) = 1 / max(r, 1/255)
// 施加范围见 litColor 处：只乘被稀疏化的辐射度项，不乘自发光/环境底/HitT。
// rate 为 0（稀疏关）或 1（全采样）时返回 1，与旧画面逐位一致。
// =====================================================
float vptSparseRadianceScale()
{
    const float rate = clamp(cam.sparseParams.y, 0.0, 100.0) * 0.01;

    if(rate <= 0.0 || rate >= 1.0)
    {
        return 1.0;
    }

    return 1.0 / max(rate, 1.0 / 255.0);
}
// =====================================================
// DLSS frame-level Halton(2,3) jitter
// 32 phases for current 2.0x Performance scaling.
//
// sequenceIndex starts at 1.
// Returned value is pixel-space offset in [-0.5, 0.5).
// =====================================================

float haltonValue(
    uint index,
    uint base
) {
    float f = 1.0;
    float result = 0.0;

    uint i = index;

    while (i > 0u) {
        f /= float(base);

        result +=
            f * float(i % base);

        i /= base;
    }

    return result;
}


vec2 getDLSSFrameJitter(
    uint frameIndex
) {
    const uint PHASE_COUNT = 32u;

    uint sequenceIndex =
        (frameIndex % PHASE_COUNT) + 1u;

    return vec2(
        haltonValue(sequenceIndex, 2u),
        haltonValue(sequenceIndex, 3u)
    ) - vec2(0.5);
}

vec3 sampleHemisphereCosine(vec3 normal, inout uint state) {
    float r1 = rand(state);
    float r2 = rand(state);
    float phi = 2.0 * 3.14159265 * r1;
    float r = sqrt(r2);
    float x = r * cos(phi);
    float y = r * sin(phi);
    float z = sqrt(max(0.0, 1.0 - x * x - y * y));

    vec3 up = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, normal));
    vec3 bitangent = cross(normal, tangent);

    return tangent * x + bitangent * y + normal * z;
}

vec3 sampleGGX(vec3 normal, float roughness, inout uint state) {
    float alpha = max(roughness * roughness, 0.001); 
    float alpha2 = alpha * alpha;
    float r1 = rand(state);
    float r2 = rand(state);
    float phi = 2.0 * 3.14159265 * r1;
    float cosTheta = sqrt((1.0 - r2) / (1.0 + (alpha2 - 1.0) * r2));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    vec3 H = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    vec3 up = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, normal));
    vec3 bitangent = cross(normal, tangent);

    return tangent * H.x + bitangent * H.y + normal * H.z;
}

// 软化太阳方向采样（用于柔和阴影）
vec3 sampleSunDirection(vec3 sunDir, inout uint state) {
    float r1 = rand(state);
    float r2 = rand(state);
    float angle = 6.2831853 * r1;
    // Solar angular radius is about 0.27 degrees. The former 0.86-degree
    // disk averaged narrow architectural shadows into an overcast look.
    float radius = sqrt(r2) * 0.0047;

    vec3 up = abs(sunDir.y) < 0.999 ? vec3(0,1,0) : vec3(1,0,0);
    vec3 tangent = normalize(cross(up, sunDir));
    vec3 bitangent = cross(sunDir, tangent);

    return normalize(sunDir + tangent * cos(angle) * radius + bitangent * sin(angle) * radius);
}