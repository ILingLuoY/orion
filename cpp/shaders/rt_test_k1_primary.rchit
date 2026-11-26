#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// K1 主视线专用的极简 Closest-Hit：只写命中身份五元组与运动矢量到 hitOffsetImage。
// 不包含 material_resolve / rchit_helpers_* / POM / 水波 / 图集采样（~8 KB vs 152 KB），
// 将 K1 命中组从 52.2% I-Cache No-Instructions 停顿中彻底解放；K1 次级玻璃/水射线仍走 rt_test.rchit。

#include "hit_payload.glsl"
#include "rt_stage_counters.glsl"

layout(location = 0) rayPayloadInEXT HitPayload payload;
hitAttributeEXT vec2 attribs;

layout(buffer_reference, scalar) buffer VertexMotions {
    float motion[];
};

vec3 readVertexMotion(VertexMotions motions, uint vertexIndex)
{
    const uint base = vertexIndex * 3u;
    return vec3(
        motions.motion[base + 0u],
        motions.motion[base + 1u],
        motions.motion[base + 2u]
    );
}

layout(std430, binding = 21, set = 0) readonly buffer SectionVertexAddressBuffer {
    uvec4 vertexAddresses[];
} sectionVertexAddressBuffer;

layout(binding = 2, set = 0) uniform CameraProperties
{
    #include "camera_ubo.glsl"
} cam;

layout(binding = 98, set = 0, rgba16f) coherent uniform image2D hitOffsetImage;

void main()
{
    #ifdef ORION_RT_STAGE_COUNTERS
    rtCntHitAccepted();
    #endif

    payload.hitDistance = gl_HitTEXT;
    payload.normal = vec3(
        float(gl_InstanceCustomIndexEXT + gl_GeometryIndexEXT),
        float(gl_PrimitiveID),
        attribs.x
    );
    payload.roughness = attribs.y;

    const uint k1InstanceIndex = uint(payload.normal.x + 0.5);
    const uint k1PrimitiveIndex = uint(payload.normal.y + 0.5);
    const vec3 k1Barycentrics = vec3(
        1.0 - payload.normal.z - payload.roughness,
        payload.normal.z,
        payload.roughness);
    const uvec4 k1SectionEntry =
        sectionVertexAddressBuffer.vertexAddresses[k1InstanceIndex];
    const uint64_t k1RawAddr = packUint2x32(k1SectionEntry.xy);

    if(k1RawAddr != 0ul && k1SectionEntry.z != 0u)
    {
        const bool k1IndexedQuads = (k1SectionEntry.w & 1u) != 0u;
        const uint k1QuadBase = (k1PrimitiveIndex >> 1u) * 4u;
        uint k1i0;
        uint k1i1;
        uint k1i2;
        if(k1IndexedQuads)
        {
            k1i0 = k1QuadBase;
            if((k1PrimitiveIndex & 1u) == 0u)
            {
                k1i1 = k1QuadBase + 1u;
                k1i2 = k1QuadBase + 2u;
            }
            else
            {
                k1i1 = k1QuadBase + 2u;
                k1i2 = k1QuadBase + 3u;
            }
        }
        else
        {
            k1i0 = k1PrimitiveIndex * 3u;
            k1i1 = k1i0 + 1u;
            k1i2 = k1i0 + 2u;
        }

        VertexMotions k1Motions =
            VertexMotions(k1RawAddr + uint64_t(k1SectionEntry.z));

        vec3 k1WorldOffset =
            readVertexMotion(k1Motions, k1i0) * k1Barycentrics.x +
            readVertexMotion(k1Motions, k1i1) * k1Barycentrics.y +
            readVertexMotion(k1Motions, k1i2) * k1Barycentrics.z;

        if(any(notEqual(k1WorldOffset, vec3(0.0))))
        {
            k1WorldOffset +=
                cam.prevViewInverse[3].xyz -
                cam.viewInverse[3].xyz;
        }

        imageStore(
            hitOffsetImage,
            ivec2(gl_LaunchIDEXT.xy),
            vec4(k1WorldOffset, 1.0));
    }
}
