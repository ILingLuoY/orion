#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_scalar_block_layout : require
// ③/④ 内联材质路径（2026-11-05）：B① 块原样，由 rgen_k2_material.glsl 在
// ORION_K2_MATERIAL_INLINE 下 include。callable 路径硬崩 parked（见 D1 记忆），
// 本路径是 K2 稳定档。（K1 的 rchit 只落命中身份）。
// 本块只在 ORION_K2_TRANSPORT 下编入（rt_test.rgen，rgen_main_body 之前），
// 是 material_resolve.glsl 的 includer 契约。图集布局与 Vertex 家族从
// rt_test.rchit **原样拷贝**（rchit 的 helper 与 material_common.glsl 是 drift
// 过的两份，必须跟 rchit 这份走）；CameraProperties(binding 2)/LocalLightBuffer
// (binding 17)/glass_tint/water_wave_field rgen 侧已有，不重复。

// ---- 图集布局（rt_test.rchit 452-472 原样）----
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
layout(binding = 63, set = 0) uniform sampler2D entityNormalAtlases[64];
layout(binding = 64, set = 0) uniform sampler2D entitySpecularAtlases[64];
layout(binding = 37, set = 0) uniform sampler2D particleAtlases[3];
layout(binding = 40, set = 0) uniform sampler2D rainStreakAtlas;
layout(binding = 45, set = 0) uniform sampler2D voxyLodAtlas;
// DH 自己的 blockRatioAtlas（= DH 着色器里的 uBlockAtlas），16px tile、256 个/行。
// 图集没就绪时 C++ 会把它回落到原版方块图集，所以采样前必须确认顶点 tileId != 0
// （Java 侧只在真句柄推送成功后才写 tileId）。
layout(binding = 49, set = 0) uniform sampler2D dhBlockRatioAtlas;

// rchit 1039-1040 原样：pom/atlas helper 段要用的法线/高光图集。
layout(binding = 4, set = 0) uniform sampler2D normalAtlas;
layout(binding = 5, set = 0) uniform sampler2D specularAtlas;

// ---- 顶点家族（rt_test.rchit 87-201 原样，去掉 CameraProperties）----
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
        unpacked.x = packed.x;
        unpacked.y = packed.y;
        unpacked.z = packed.z;
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

    return Vertices(addr).v[index];
}
#else
Vertex loadSectionVertex(uint64_t addr, uint /*stride*/, uint index)
{
    return Vertices(addr).v[index];
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

// ---- materialLodRole 依赖的 LOD 常量与角色表 ----
// rgen 侧 binding 19 已以 serLodRoles 名义声明（rgen_trace_state.glsl，SER 用）。
// 这里按 Vulkan 的同型别名再挂一个只读块：两者读同一 descriptor，布局逐字节同。
// 常量与 lod_coverage.glsl / SER_ROLE_* 逐位同值（LOD_ROLE_TERRAIN=0、limit=1<<16）。
#define LOD_ROLE_TERRAIN  0u
#define LOD_ROLE_VOXY_LOD 1u
#define LOD_ROLE_DH_FAR   2u
#define LOD_COVERAGE_SLOT_LIMIT (1u << 16)
layout(std430, binding = 19, set = 0) readonly buffer K2LodRoles {
    uint roles[];
} lodRoles;

// material_resolve.glsl 读这个开关（rchit 侧由 main() 顶部播种；rgen 侧 GI
// 次级射线在发射前直接换 ray flags / 不走本解析，这里恒 false）。
bool g_giRaySkipPOM = false;

// makeHitState：material_common.glsl 的定义原样（常量来自 rgen_trace_state.glsl，
// 逐位同值；rgen TU 不 include material_common，这里补一份 —— 与 rchit 头部的
// 副本逐字相同）。
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

// transparent_profile_common：rchit 在头部 include（rchit 82），rgen TU 没有；
// material_resolve / pom helper 都要用。
#include "transparent_profile_common.glsl"

// ---- rchit/rgen 漂移符号的改名载入 ----
// rgen 已有自己 drift 过的天空光函数与 noon 常量（rt_test.rgen 4529-5264，值都
// 不一样）。K2 的材质解析语义以 rchit 为准 ⇒ 把 rchit helper（与 material_resolve
// 调用点）里的这套符号经宏改名载入，rgen 侧原有定义与 rgen_main_body 的调用点
// 不受影响（它们都在本块之前编译；宏在本文件末尾 #undef）。
#define getTimeDrivenSunDirection      k2GetTimeDrivenSunDirection
#define getTimeDrivenSunElevation      k2GetTimeDrivenSunElevation
#define getTimeDrivenSkyLightRadianceScale k2GetTimeDrivenSkyLightRadianceScale
#define getTimeDrivenSkyLightDirection k2GetTimeDrivenSkyLightDirection
#define TIME_DRIVEN_SUN_NOON_SCALE     k2TimeDrivenSunNoonScale
#define TIME_DRIVEN_MOON_NOON_SCALE    k2TimeDrivenMoonNoonScale
// 顶层级 const（rgen:4811 也有同名定义，值同为 2.0；改名避免重定义）。
#define RAIN_VISIBILITY_GAIN           k2RainVisibilityGain

#include "rchit_helpers_water.glsl"
#include "rchit_helpers_atlas.glsl"
#include "rchit_helpers_pom.glsl"
#include "material_resolve.glsl"

#undef getTimeDrivenSunDirection
#undef getTimeDrivenSunElevation
#undef getTimeDrivenSkyLightRadianceScale
#undef getTimeDrivenSkyLightDirection
#undef TIME_DRIVEN_SUN_NOON_SCALE
#undef TIME_DRIVEN_MOON_NOON_SCALE
#undef RAIN_VISIBILITY_GAIN
