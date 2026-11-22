#ifndef TERRAIN_HEIGHT_GLSL
#define TERRAIN_HEIGHT_GLSL

// 2026-11-09：512 -> 128。覆盖范围仍由 TERRAIN_HEIGHT_EXTENT_XY（±256 格）
// 决定，所以只是纹素从 1 格变 4 格。这是**探测射线数**从 262144/帧降到
// 16384/帧的唯一改动点；唯一消费者是 volumetric_fog.glsl 的贴地雾高度
// （低频标量，4 格量化在几米尺度上不可见）。
// 与 OrionInternal.h 的 TERRAIN_HEIGHT_RESOLUTION 必须同步。
const int TERRAIN_HEIGHT_RESOLUTION = 128;
const float TERRAIN_HEIGHT_TEXEL = 1.0 / 128.0;

const float TERRAIN_HEIGHT_EXTENT_XY = 256.0;

const float TERRAIN_HEIGHT_OUTSIDE_FADE_BLOCKS = 32.0;

const float TERRAIN_HEIGHT_WORLD_TOP = 320.0;
const float TERRAIN_HEIGHT_WORLD_BOTTOM = -64.0;
const float TERRAIN_HEIGHT_MARGIN = 32.0;

const float TERRAIN_HEIGHT_EMPTY = -1.0e30;

// 0x08u hits opaque terrain & OMM foliage while excluding hand (0x04),
// local player (0x02), and particles (0xF7).
const uint TERRAIN_HEIGHT_CULL_MASK = 0x08u;

const float TERRAIN_HEIGHT_ORIGIN_BIAS = 0.02;

// 网格中心（XZ）：跟着相机，但量化到纹素尺寸以抑制移动时的"游泳"
// （与太阳阴影图同招）。竖直方向不需要中心 —— 探测范围是世界绝对的。
vec2 terrainHeightCenterXZ()
{
    const float TEXEL_WORLD = 2.0 * TERRAIN_HEIGHT_EXTENT_XY * TERRAIN_HEIGHT_TEXEL;
    vec2 cameraXZ = cam.viewInverse[3].xz;
    return floor(cameraXZ / TEXEL_WORLD) * TEXEL_WORLD;
}

// 探测射线的起点高度与长度。所有纹素共用（只依赖相机 Y），所以调用方
// 可以在循环外算一次。
void terrainHeightProbeRange(out float originY, out float maxDistance)
{
    float cameraY = cam.viewInverse[3].y;
    originY = max(cameraY, TERRAIN_HEIGHT_WORLD_TOP) + TERRAIN_HEIGHT_MARGIN;
    maxDistance = originY -
        (min(cameraY, TERRAIN_HEIGHT_WORLD_BOTTOM) - TERRAIN_HEIGHT_MARGIN);
}

#ifdef TERRAIN_HEIGHT_CONSUMER
vec2 terrainHeightUv(vec2 worldXZ)
{
    return (worldXZ - terrainHeightCenterXZ()) /
        (2.0 * TERRAIN_HEIGHT_EXTENT_XY) + 0.5;
}

// 给定世界 XZ 的地面高度，并给出离盒边界的距离（盒内 = 0，单位格）。
// 该列无地面（虚空/打空）时返回 TERRAIN_HEIGHT_EMPTY。
float terrainGroundHeight(vec2 worldXZ, out float outsideDistance)
{
    vec2 uv = terrainHeightUv(worldXZ);
    vec2 outside = max(-uv, uv - 1.0) * (2.0 * TERRAIN_HEIGHT_EXTENT_XY);
    outsideDistance = max(max(outside.x, outside.y), 0.0);
    if(outsideDistance > 0.0)
    {
        return TERRAIN_HEIGHT_EMPTY;
    }
    return texture(terrainHeightMap, uv).r;
}
#endif
#ifdef TERRAIN_HEIGHT_PRODUCER
float terrainHeightTrace(vec3 origin, vec3 direction, float maxDistance)
{
    rayQueryEXT query;
    // Closest-hit traversal (NO TerminateOnFirstHit) so downward probe finds
    // the true top ground surface rather than an arbitrary cave floor.
    rayQueryInitializeEXT(
        query,
        sceneTlas,
        0u,
        TERRAIN_HEIGHT_CULL_MASK,
        origin,
        TERRAIN_HEIGHT_ORIGIN_BIAS,
        direction,
        maxDistance);
    int guard = 0;
    while(rayQueryProceedEXT(query) && ++guard < 64)
    {
        if(rayQueryGetIntersectionTypeEXT(query, false) ==
           gl_RayQueryCandidateIntersectionTriangleEXT)
        {
            if(rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(query, false) <= 1u)
            {
                rayQueryConfirmIntersectionEXT(query);
            }
        }
    }
    if(rayQueryGetIntersectionTypeEXT(query, true) ==
        gl_RayQueryCommittedIntersectionNoneEXT)
    {
        return maxDistance;
    }
    return rayQueryGetIntersectionTEXT(query, true);
}
#endif
#endif
