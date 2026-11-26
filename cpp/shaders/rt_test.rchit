#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// LOD / 远场几何的角色表（binding 19）。DH 远场的识别由它回答，pad1 只再表示材质编号。
#include "lod_coverage.glsl"
// 2026-10-07：为用 glassVertexFactorIsFaceShade() 判"普通玻璃"（顶点色里没有注入
// 染料）而包含。与 rt_test_translucent.rchit 共用**同一份**实现与同一个开关
//（GLASS_TINT_NO_DDA），避免两份判据漂移 —— 那个文件一直就是这么包含的。
#define GLASS_TINT_NO_DDA 1
#include "glass_tint_common.glsl"

// 2026-10-20: 结构体单源化，见 hit_payload.glsl（六个 shader 共用同一份布局）。
#include "hit_payload.glsl"
// RT 阶段三计数器（hits lane），整段由 ORION_RT_STAGE_COUNTERS 裁掉。
#include "rt_stage_counters.glsl"
layout(location = 0) rayPayloadInEXT HitPayload payload;

const uint RT_STATE_PURPOSE_MASK       = 0x7u;
const uint RT_STATE_HIT_CLASS_SHIFT    = 3u;
const uint RT_STATE_WATER_EVENT_SHIFT  = 5u;
const uint RT_STATE_GLASS_BIT          = 1u << 7u;
const uint RT_STATE_GLASS_BOTTOM_BIT   = 1u << 8u;
const uint RT_STATE_PROFILE_SHIFT      = 9u;
// 2026-10-07：普通（未染色）玻璃标记，bit 12 —— 与 rt_test_translucent.rchit 同名同值。
// 置位者见下方 t12IsGlass 分支，消费者见 rgen 的 isClearGlassHit()（"当空气处理"）。
const uint RT_STATE_CLEAR_GLASS_BIT    = 1u << 12u;
// 2026-10-24：粒子命中标记，bit 16 —— 与 rt_test.rgen 同名同值。置位者在
// material_resolve.glsl（isParticleMaterial，makeHitState 之后）；消费者：
// rgen 的 RR TransparencyHint（粒子像素降低 RR 历史信任，治残影）。
const uint RT_STATE_PARTICLE_BIT       = 1u << 16u;
const uint RT_STATE_HAND_BIT           = 1u << 20u;

// 2026-10-25 A/B：GI 射线跳过 POM（与 rt_test.rgen 的常量必须逐位一致）。
const uint RT_STATE_GI_NO_POM_BIT      = 1u << 17u;

// 本 invocation 是否为"跳过 POM 的 GI 射线"。在 main() 顶部由 payload.state 播种，
// 由 getPOMDepthScale() 读取 —— 这样 traceParallax() / tracePOMSelfShadow() 里
// 既有的 pomDepthScale<=1e-5 早退分支会原样接管，不需要在 march 中间加分支。
bool g_giRaySkipPOM = false;
// rgen 侧同名同值（rt_test.rgen:6053）。补在这里，供本 shader 的"按射线用途分级"
// 快速路径识别 —— 与下面 RT_PURPOSE_DISTANCE_ONLY 的补法同源（见其上注释）。
const uint RT_PURPOSE_FULL             = 0u;
const uint RT_PURPOSE_CLASSIFY         = 1u;
// 2026-10-06：rgen 的 RR-6.2 主水反射 guide 射线（rgen:RT_PURPOSE_DISTANCE_ONLY）
// 一直在打本 shader 跑完整着色。补上同名常量供快速路径识别（rgen 侧定义不变）。
const uint RT_PURPOSE_DISTANCE_ONLY    = 2u;
const uint RT_PURPOSE_PASSTHROUGH      = 4u;
const uint RT_PURPOSE_WATER_MEDIUM     = 6u;
const uint RT_PURPOSE_PRIMARY_SKY      = 7u;
const uint RT_HIT_OPAQUE               = 0u;
const uint RT_HIT_WATER                = 1u;
const uint RT_HIT_TRANSLUCENT          = 2u;
const uint RT_WATER_NONE               = 0u;
const uint RT_WATER_AIR_TO_WATER       = 1u;
const uint RT_WATER_WATER_TO_AIR       = 2u;

uint getRayPurpose(uint state)
{
    return state & RT_STATE_PURPOSE_MASK;
}

uint makeHitState(
    uint purpose,
    uint hitClass,
    uint waterEvent,
    bool glass,
    bool glassBottom,
    uint transparentProfile
)
{
    return
        (purpose & RT_STATE_PURPOSE_MASK) |
        ((hitClass & 0x3u) << RT_STATE_HIT_CLASS_SHIFT) |
        ((waterEvent & 0x3u) << RT_STATE_WATER_EVENT_SHIFT) |
        (glass ? RT_STATE_GLASS_BIT : 0u) |
        (glassBottom ? RT_STATE_GLASS_BOTTOM_BIT : 0u) |
        ((transparentProfile & 0x7u) << RT_STATE_PROFILE_SHIFT);
}

// 2026-10-23：原来这里是 5 份副本之一，已收进透明剖面共享头。
// 本 TU 不 include material_common.glsl（它依赖 rgen 侧的 payload/灯光声明），
// 所以直接引这个无依赖的小头。
#include "transparent_profile_common.glsl"
hitAttributeEXT vec2 attribs;

struct Vertex {
    float x, y, z;
    float u, v;
    float r, g, b, a;
    float materialId;
    float pad1;
    float pad2;
};

layout(buffer_reference, scalar) buffer Vertices {
    Vertex v[];
};

// =====================================================
// 2026-10-21 地形 32B 顶点（与 C++ 的 SectionVertex 逐字节对应）
//
// 本 TU 自带 Vertex/Vertices 且不 include material_common.glsl ⇒ 这里单独持一份
// 解包器；内容与 material_common.glsl:89-135 一致，改那边必须同步改这里。
//
// 判别方式：顶点地址表项（binding 21）的 .w = 该段顶点步长（32 或 48）。
// 解包后仍返回老的 48B Vertex ⇒ 下游 materialId/pad1/pad2 使用点零改动。
// 编译期开关必须与 C++ 的 SECTION_VERTEX_32B_ENABLED 一致。
// =====================================================

// =====================================================
// 2026-11-12：实例变换（实例化第一步）。
//
// TLAS 实例不再恒为单位矩阵：顶点可以按**模型局部空间**上传，命中着色器在读取
// 顶点时把位置变回世界空间。恒等实例下与旧行为数学等价（mat4x3 乘加一次）。
// 法线由世界空间顶点差分得到（cross(pos1-pos0, pos2-pos0)），所以也会自动正确，
// 前提是实例变换只含旋转+平移（当前设计如此）。
//
// ⚠ 必须定义在 #if SECTION_VERTEX_32B **之前**：32B 与 48B 两条顶点路径都要用它。
// =====================================================
vec3 rtObjectToWorld(vec3 localPosition)
{
    return gl_ObjectToWorldEXT * vec4(localPosition, 1.0);
}

#if SECTION_VERTEX_32B
struct SectionVertex32
{
    float x, y, z;
    float u, v;
    uint color;      // RGBA8: r | g<<8 | b<<16 | a<<24
    float materialId;
    float pad2;      // pad2 + (pad1 > 0 ? 64 : 0)
};

layout(buffer_reference, scalar) buffer SectionVertices32 {
    SectionVertex32 v[];
};

Vertex loadSectionVertex(uint64_t addr, uint stride, uint index)
{
    if(stride == 32u)
    {
        const SectionVertex32 packed = SectionVertices32(addr).v[index];

        Vertex unpacked;
        const vec3 world = rtObjectToWorld(vec3(packed.x, packed.y, packed.z));
        unpacked.x = world.x;
        unpacked.y = world.y;
        unpacked.z = world.z;
        unpacked.u = packed.u;
        unpacked.v = packed.v;
        unpacked.r = float(packed.color & 0xFFu) / 255.0;
        unpacked.g = float((packed.color >> 8) & 0xFFu) / 255.0;
        unpacked.b = float((packed.color >> 16) & 0xFFu) / 255.0;
        unpacked.a = float((packed.color >> 24) & 0xFFu) / 255.0;
        unpacked.materialId = packed.materialId;

        const bool rainPuddle = packed.pad2 >= 64.0;
        unpacked.pad1 = rainPuddle ? 0.25 : 0.0;
        unpacked.pad2 = rainPuddle ? (packed.pad2 - 64.0) : packed.pad2;

        return unpacked;
    }

    Vertex raw = Vertices(addr).v[index];
    const vec3 world = rtObjectToWorld(vec3(raw.x, raw.y, raw.z));
    raw.x = world.x;
    raw.y = world.y;
    raw.z = world.z;
    return raw;
}
#else
Vertex loadSectionVertex(uint64_t addr, uint /*stride*/, uint index)
{
    Vertex raw = Vertices(addr).v[index];
    const vec3 world = rtObjectToWorld(vec3(raw.x, raw.y, raw.z));
    raw.x = world.x;
    raw.y = world.y;
    raw.z = world.z;
    return raw;
}
#endif

// =====================================================
// 2026-10-09 运动矢量（DLSS-RR）：顶点缓冲**尾部**那段"上一帧位移"。
//
// 起点 = 顶点地址 + 表项的 .z（字节偏移，见 GPUSectionVertexAddress）；每顶点
// 3 个 float，与顶点向量**逐顶点同序**，所以第 i 个顶点的位移就是
// motion[i*3 .. i*3+2]。
//
// ⚠ 段里存的是**相机相对**位移（prev − cur），不是世界位移：Java 只能量到相机
//   相对的量。本文件在读到它之后、写进 payload 之前，用 UBO 的 viewInverse
//   平移列把 Δcam 补上（见下方那一段），所以 payload 里出去的是世界位移。
//
// ★ 刻意声明成 `float motion[]` 而不是 `vec3 motion[]`：vec3 数组在不同
//   block layout 下的 stride 有 12/16 两种可能，而这里必须**恰好 12 字节**
//   （C++ 侧就是 12B/顶点）。用 float[] + 手动乘 3 把这件事钉死，不依赖
//   scalar layout 对 vec3 的处理细节。
// =====================================================
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

// One entry per TLAS Section instance, selected by
// VkAccelerationStructureInstanceKHR::instanceCustomIndex.
//
// 2026-10-09 运动矢量：表项 8B → **16B**（uvec4）。四个 lane：
//   .xy = 顶点缓冲的 64 位设备地址（原样）
//   .z  = 该 Section 顶点缓冲里"上一帧位移"段（**相机相对**）的**字节偏移**
//         （0 = 没有；closest-hit 补 Δcam 之后才是世界位移）
//   .w  = 位移段里的 float 个数（顶点数 × 3），仅供调试核对
// ⚠ 必须与 VulkanCore.cpp 的 GPUSectionVertexAddress 保持逐字节一致；这里改成
//   uvec2 会让 CPU 侧按 16B 步长写的地址被半个 entry 错位读走（整片几何塌掉）。
layout(std430, binding = 21, set = 0) readonly buffer SectionVertexAddressBuffer {
    uvec4 vertexAddresses[];
} sectionVertexAddressBuffer;
layout(binding = 2, set = 0) uniform CameraProperties
{
    #include "camera_ubo.glsl"
} cam;

// The natural wave bands, the persistent interaction field and the binding-33
// sampler all live in the shared include.  rt_test.rgen includes the same file
// for its analytic caustics, so the caustic pattern cannot drift away from the
// visible water normal again.
//
// 2026-10-13 退役：路线 A（WATER_WAVES_NOISETEX）曾在 closest-hit 里把
// cloudShapeNoise 当主波带采样，故这里补过一份 binding 38 声明。该后端已被
// afl getwaves 取代（用户否决噪声贴图），本声明随之失去调用点 ⇒ 屏蔽保留。
// 复活步骤：删掉下面这对 #if 0 / #endif，并把 C++ 里 binding 38 的 stageFlags
// 加回 CLOSEST_HIT（见 VulkanCore.cpp 的 b[36]）。
#if 0
layout(binding = 38, set = 0) uniform sampler3D cloudShapeNoise;
#endif // 0 (退役的路线 A closest-hit 采样)
#include "water_wave_field.glsl"

// Pure optical, world-space water ripples.  Integer time harmonics keep the
// field continuous at the Minecraft day wrap; high-frequency slopes fade at
// distance to avoid ray-traced/DLSS aliasing.
#include "rchit_helpers_water.glsl"
layout(binding = 3, set = 0) uniform sampler2D blockAtlas;
// 2026-10-06：实体纹理槽 16 → 32。容量 16 时槽表常年满载，一帧内新出现的纹理
// **必然**拿不到槽而被丢弃（第一人称手、末地传送门就是这么消失的）。最后一项
// 留给本地玩家身体代理，普通实体纹理用 1..(N-1)。
// 2026-10-13：32 → 64。必须与 VulkanCore.cpp 的 ENTITY_RT_TEXTURE_SLOT_COUNT
// 及 EntityRtCapture.MAX_TEXTURE_SLOTS 同步，并且下面的 switch 必须补满 N 个
// case（本设备没开 descriptor indexing，只能静态索引）。
layout(binding = 36, set = 0) uniform sampler2D entityAtlases[128];
// 2026-10-09：实体 LabPBR（`_n`/`_s`）。与 entityAtlases 同槽、同 UV（Java 按
// "同一 Identifier 加 _n/_s" 查找），C++ 侧与 binding 36 同构：每槽一个 view。
// 没有贴图的槽由 C++ 填中性 1×1 兜底（normal=(0,0,0,1)、specular=(0,0,0,1)），
// 于是下面的内容判据自动判"无贴图"而不需要额外的标志位。
layout(binding = 63, set = 0) uniform sampler2D entityNormalAtlases[128];
layout(binding = 64, set = 0) uniform sampler2D entitySpecularAtlases[128];
layout(binding = 37, set = 0) uniform sampler2D particleAtlases[3];
layout(binding = 40, set = 0) uniform sampler2D rainStreakAtlas;
layout(binding = 45, set = 0) uniform sampler2D voxyLodAtlas;
// DH 自己的 blockRatioAtlas（= DH 着色器里的 uBlockAtlas），16px tile、256 个/行。
// 图集没就绪时 C++ 会把它回落到原版方块图集，所以采样前必须确认顶点 tileId != 0
// （Java 侧只在真句柄推送成功后才写 tileId）。
layout(binding = 49, set = 0) uniform sampler2D dhBlockRatioAtlas;

#include "rchit_helpers_atlas.glsl"
layout(binding = 4, set = 0) uniform sampler2D normalAtlas;
layout(binding = 5, set = 0) uniform sampler2D specularAtlas;


// =====================================================
// RT Local Lights - L3 descriptor reservation
//
// Bound for ClosestHit as well so the future local-light shading
// stage can use the same binding 17 without another descriptor
// layout migration.
// =====================================================

struct LocalLightGPU
{
    vec4 worldPosEmission;
    uvec4 meta;
};

layout(std430, binding = 17, set = 0) readonly buffer LocalLightBuffer
{
    uvec4 header;
    LocalLightGPU lights[];
} localLightBuffer;

#include "rchit_helpers_pom.glsl"

// 2026-11-05 B①：materialHitT/materialRayOrigin/materialRayDirection 三个全局量
// 随 helper 段搬到 rchit_helpers_water.glsl 顶部；本 TU 的 helper 也改读全局量
// （resolveHitMaterial 开头写入真值，与命中内建同值，逐位等价）。

#include "material_resolve.glsl"

// 2026-11-21 P0-1：主命中世界位移 per-pixel 旁路槽（binding 98，RGBA16F 全分辨率）。
// rgen 每像素清槽、主射线返回后读回；本 TU 主射线命中时把 resolve 算出的世界位移写回。
// 只在本 TU 声明 —— rcall / K2 内联 TU 不引用该图（material_resolve.glsl 顶部说明）。
layout(binding = 98, set = 0, rgba16f) coherent uniform image2D hitOffsetImage;

void main()
{
    // RT 计数器：closest-hit 被硬件派发执行即计一次（含 purpose-5 可见性探针；
    // 提前 return 的 invocation 同样计入）。每次 invocation 至多一次 atomicAdd。
    #ifdef ORION_RT_STAGE_COUNTERS
    rtCntHitAccepted();
    #endif

    // 2026-11-11 太阳可见度（软影 A 方案，形态 1）：
    // 这条射线只需要"遮挡物距离"，把 gl_HitTEXT 写进 payload 后立刻返回，
    // **不进材质解析**。它替代了原来"整条射线都 SkipClosestHitShader"的做法
    // （那样 any-hit 拿不到任何 t）。purpose 5 = RT_PURPOSE_GLASS_VISIBILITY，
    // 与 rt_test.rahit 的同名常量一致。
    const uint rtFastVisPurpose = payload.state & 0x7u;
    if(rtFastVisPurpose == 5u || rtFastVisPurpose == 3u)
    {
        payload.hitDistance = gl_HitTEXT;
        return;
    }

    // 2026-10-25：GI 射线跳过 POM 的开关来自 rgen 播种的 state 位（bit 17）。
    // 必须在 resolveHitMaterial() 之前播种 —— 材质解析里就会调 traceParallax()。
    g_giRaySkipPOM =
        (payload.state & RT_STATE_GI_NO_POM_BIT) != 0u;

    // 2026-10-21 根因修复（生产模式崩溃的真正原因）：
    // resolveHitMaterial() 的 miss 判定读 payload.hitDistance，取命中身份读
    // payload.normal.x/.y/.z 与 payload.roughness（见 material_resolve.glsl:41-63）。
    // 重构这段播种时丢了 ⇒ 生产模式拿**上一条射线的陈旧值**去索引
    // sectionVertexAddressBuffer.vertexAddresses[instanceIndex]（无边界检查）：
    // 越界读到垃圾指针，再被 Vertices(rawAddr) 当 buffer_reference 解引用 ⇒
    // GPU 挂死，"Item in hand" 的 RT 注入 5 秒等不到信号量。
    // 延迟档由派发器播种（rt_test.rgen:18041-18042），所以只有生产模式崩。
    payload.hitDistance = gl_HitTEXT;
    // 2026-10-29 合并 BLAS 基座：normal.x 携带的是**资源槽位**（实例基址 + 几何
    // 序号），不是裸的 instanceCustomIndex。一 geometry/实例时 gl_GeometryIndexEXT
    // 恒为 0 ⇒ 与改动前逐位相同；合并后它把同一实例内的多个 Section 区分开
    // （下游 material_resolve.glsl:48 直接把这个值当 sectionVertexAddressBuffer
    // 的索引，所以必须在这里就加好，否则会所有 geometry 都读同一段顶点）。
    payload.normal = vec3(
        float(gl_InstanceCustomIndexEXT + gl_GeometryIndexEXT),
        float(gl_PrimitiveID),
        attribs.x
    );
    payload.roughness = attribs.y;

#ifdef ORION_K1_PRIMARY
    // B①：K1 主变体的主射线只落身份（上面五行），材质解析整体延期到 K2 的
    // rgen 侧。主射线的 purpose 恒为 RT_PURPOSE_PRIMARY_SKY（rgen 发射前
    // makeTraceState 播种）；次级射线（玻璃留 K1 的行内输运）继续走完整解析。
    if(getRayPurpose(payload.state) == RT_PURPOSE_PRIMARY_SKY)
    {
        // 2026-11-21 P0-1（K1 补写站）：拆分模式下主射线在本分支早退，从不进入
        // resolveHitMaterial，binding 98 的写槽端（main 的 resolve 之后）因此永不执行
        // ⇒ 实体自身运动 MV 仍是常量 0。这里只补"顶点运动采样"一件事：不解材质、
        // 不采样图集、不做 POM、不改 payload，算完写槽后照原样 return。
        //
        // 语义与 material_resolve.glsl:164-208（P0-1 主写站）逐条对齐：
        //   · 身份取上方刚播种的 payload.normal（.x 实例槽 / .y 图元 / .z attribs.x）
        //     与 payload.roughness(=attribs.y)，重心坐标同构；
        //   · 步长/索引布局按 sectionAddressEntry.w 分派（地形 32B 与索引化 quad）；
        //   · sectionAddressEntry.z == 0 ⇒ 该段无运动数据，位移保持 0，整块不写；
        //   · Δcam 判据**必须**保持 any(notEqual(offset, 0))，禁止改成 .z != 0
        //     （动态层每帧给未配对段补整段 0，.z 分不出"有效零位移"与"未配对"，
        //      照改会重现 2026-10-22 走动时实体/粒子双倍位移鬼影）。
        //   · 与 material_resolve.glsl 的同一算法若日后调整必须同步两处。
        // 写槽约束：本像素要么写"真实位移 + valid=1"，要么整块不写（rgen 已先清成 0），
        // 不产生第三种半状态；天空/未命中走 rmiss，不执行本段。
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
        return;
    }
#endif

    // =====================================================================
    // 2026-10-29 消融档 17「noMatEval」：跳过材质解析，只把 payload 填成
    // 中性灰 + 反向法线，让 rgen 的路径继续按"命中了一个普通 opaque 面"走。
    //
    // 目的：量「材质求值」（图集采样 / PBR 解码 / POM / 水波 / 玻璃解析）的价格
    // 上界。HANDOFF-2026-10-29 §3 的分箱延迟材质只有在材质求值占 trace 的可观
    // 比例时才可能回本，否则固定成本（计数 + 前缀和 + 多一趟 pass）必亏。
    //
    // 注意口径：本档连**主射线**的材质求值也一起跳（rchit 无法区分主/次射线），
    // 且玻璃/水命中会退化成 opaque ⇒ 后续路径变短。因此 17.5 - X 是**上界**：
    // 若连上界都不足以抵消一次额外 pass，就可以直接终止这条优化方向。
    //
    // 生产档（stage 0）走 else 分支，零成本。观感在本档下明显变灰，仅供计数。
    // =====================================================================
    // 2026-10-30：28/29/30 是「无材质 + 关掉某类次级射线」的组合臂，
    // 材质求值同样跳过（档号见 VulkanCore.cpp 的档位表）。
    // ⚠ 必须写成闭区间 [28,30]：原来只写 `>= 28u` 时，档 31（OMM 4 态对照）
    //   也被吞进来，量到的是「无材质 + 关 GI」，而不是 4 态 OMM。
    if(cam.perfAblationStage.x == 17u ||
       (cam.perfAblationStage.x >= 28u &&
        cam.perfAblationStage.x <= 30u))
    {
        payload.albedoPacked = packAlbedoUnorm(vec3(0.18));
        payload.normal = -normalize(gl_WorldRayDirectionEXT);
        payload.roughness = 0.5;
        payload.f0Metal = 0.0;
        payload.emission = vec3(0.0);
        payload.parallaxRayTravel = 0.0;
        payload.pomSelfShadow = 1.0;
        payload.pomMicroOcclusion = 1.0;
        payload.glassAlpha = 1.0;
        payload.state = makeHitState(
            getRayPurpose(payload.state), RT_HIT_OPAQUE,
            RT_WATER_NONE, false, false, 0u);
        return;
    }

    // RT 计数器：此处是 K1 命中侧真正执行"完整材质解析"的唯一入口
    // （purpose 5 早退 / K1 主射线早退 / 消融档 17 都在上面已经 return，不计入）。
    // invocation 私有累加，main 末尾一次 atomicAdd。
    #ifdef ORION_RT_STAGE_COUNTERS
    rtCntHitMateval(getRayPurpose(payload.state) == RT_PURPOSE_PRIMARY_SKY);
    #endif
    resolveHitMaterial(gl_WorldRayOriginEXT, gl_WorldRayDirectionEXT);

    // 2026-11-21 P0-1：主射线命中把世界位移写进旁路槽。目的闸与
    // material_resolve.glsl 运动块同一道（RT_PURPOSE_PRIMARY_SKY）：次级反射/AO/
    // GI/探针命中不进本分支，不会覆盖主像素槽。坐标用 gl_LaunchIDEXT.xy，与 rgen
    // 清槽/读回的槽位索引一致。
    if(getRayPurpose(payload.state) == RT_PURPOSE_PRIMARY_SKY)
    {
        imageStore(
            hitOffsetImage,
            ivec2(gl_LaunchIDEXT.xy),
            vec4(g_hitWorldOffset, g_hitOffsetValid ? 1.0 : 0.0));
    }

    #ifdef ORION_RT_STAGE_COUNTERS
    rtCntFlushHit();
    #endif
}
