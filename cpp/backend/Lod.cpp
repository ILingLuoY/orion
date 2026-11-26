// Orion backend — Lod 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"


// -------------------------------------------------------------------------
// Optional Voxy LOD provider input
// -------------------------------------------------------------------------
// The provider's record is four 16-byte vertices per quad:
//   u16 x, u16 y, u16 z, u16 padding, rgba8, material8, light/face bytes.
// It is intentionally decoded here instead of uploadChunk(): that function
// owns Minecraft's 28-byte ABI and must stay strict for normal terrain.
uint16_t voxyU16Le(const uint8_t* bytes)
{
    return static_cast<uint16_t>(bytes[0]) |
           (static_cast<uint16_t>(bytes[1]) << 8u);
}

#if 0
static float voxySrgbToLinear(uint8_t channel)
{
    const float value = static_cast<float>(channel) * (1.0f / 255.0f);
    return value <= 0.04045f
        ? value * (1.0f / 12.92f)
        : std::pow((value + 0.055f) * (1.0f / 1.055f), 2.4f);
}
#endif
uint64_t mixVoxyLodKey(uint64_t value)
{
    value ^= value >> 30u;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27u;
    value *= 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}
uint64_t allocateVoxyLodSpatialKeyLocked(uint64_t sourceKey)
{
    uint64_t candidate = mixVoxyLodKey(sourceKey) & VOXY_LOD_SPATIAL_LOW_MASK;
    while(true)
    {
        const uint64_t spatialKey = VOXY_LOD_RESERVED_Y | candidate;
        const auto owner = g_voxyLodSpatialOwners.find(spatialKey);
        if(owner == g_voxyLodSpatialOwners.end() || owner->second == sourceKey)
        {
            return spatialKey;
        }
        candidate = (candidate + 1u) & VOXY_LOD_SPATIAL_LOW_MASK;
    }
}
bool copyVoxyLodArray(JNIEnv* env, jbyteArray array, std::vector<uint8_t>& output)
{
    constexpr jsize MAX_BYTES = 192 * 1024 * 1024;
    output.clear();
    if(array == nullptr)
    {
        return true;
    }
    const jsize length = env->GetArrayLength(array);
    if(length < 0 || length > MAX_BYTES || (length & 63) != 0)
    {
        return false;
    }
    output.resize(static_cast<size_t>(length));
    if(length != 0)
    {
        env->GetByteArrayRegion(array, 0, length,
            reinterpret_cast<jbyte*>(output.data()));
        if(env->ExceptionCheck())
        {
            env->ExceptionClear();
            output.clear();
            return false;
        }
    }
    return true;
}
VoxyQuadFace voxyQuadFaceOf(const uint8_t* quadBytes)
{
    const float p0x = static_cast<float>(voxyU16Le(quadBytes));
    const float p0y = static_cast<float>(voxyU16Le(quadBytes + 2u));
    const float p0z = static_cast<float>(voxyU16Le(quadBytes + 4u));
    const float p1x = static_cast<float>(voxyU16Le(quadBytes + 16u));
    const float p1y = static_cast<float>(voxyU16Le(quadBytes + 18u));
    const float p1z = static_cast<float>(voxyU16Le(quadBytes + 20u));
    const float p2x = static_cast<float>(voxyU16Le(quadBytes + 32u));
    const float p2y = static_cast<float>(voxyU16Le(quadBytes + 34u));
    const float p2z = static_cast<float>(voxyU16Le(quadBytes + 36u));
    const float nx = (p1y - p0y) * (p2z - p0z) - (p1z - p0z) * (p2y - p0y);
    const float ny = (p1z - p0z) * (p2x - p0x) - (p1x - p0x) * (p2z - p0z);
    const float nz = (p1x - p0x) * (p2y - p0y) - (p1y - p0y) * (p2x - p0x);
    const float ax = std::fabs(nx);
    const float ay = std::fabs(ny);
    const float az = std::fabs(nz);
    if(ay >= ax && ay >= az)
    {
        return {1, ny > 0.0f};
    }
    if(ax >= az)
    {
        return {0, nx > 0.0f};
    }
    return {2, nz > 0.0f};
}

// MC 的面朝向明暗系数（与 shadeFromLight 的 faceTint 同一组数）：
// ±X 都是 0.6、±Z 都是 0.8，只有 Y 需要看符号。
float voxyQuadFaceTint(const uint8_t* quadBytes)
{
    const VoxyQuadFace face = voxyQuadFaceOf(quadBytes);
    if(face.axis == 1)
    {
        return face.positive ? 1.0f : 0.5f;
    }
    return face.axis == 0 ? 0.6f : 0.8f;
}

// ⚠ 本函数有意不接受 `water` 出参：voxy LOD 的水必须留在 alpha 桶，
// 原因见下面 material == MATERIAL_WATER 分支里的长注释。
void appendVoxyLodQuads(
    const std::vector<uint8_t>& input,
    bool transparentPass,
    int originX,
    int originY,
    int originZ,
    int width,
    std::vector<Vertex>& opaque,
    std::vector<Vertex>& alpha)
{
    constexpr uint8_t MATERIAL_LEAVES = 1u;
    constexpr uint8_t MATERIAL_LAVA = 6u;
    constexpr uint8_t MATERIAL_WATER = 12u;
    constexpr uint8_t MATERIAL_AIR = 14u;
    constexpr uint8_t MATERIAL_ILLUMINATED = 15u;
    const uint32_t maxLocal = static_cast<uint32_t>(width) + 1u;

    for(size_t quad = 0; quad < input.size(); quad += 64u)
    {
        const uint8_t material = input[quad + 14u];
        if(material == MATERIAL_AIR)
        {
            continue;
        }
        if(material == MATERIAL_WATER)
        {
            const VoxyQuadFace waterFace = voxyQuadFaceOf(input.data() + quad);
            if(waterFace.axis != 1)
            {
                const int bound = static_cast<int>(waterFace.axis == 0
                    ? voxyU16Le(input.data() + quad)
                    : voxyU16Le(input.data() + quad + 4u));
                if(bound == 0 || bound == width)
                {
                    continue;
                }
            }
        }
        bool valid = true;
        for(uint32_t vertex = 0u; vertex < 4u; ++vertex)
        {
            const uint8_t* source = input.data() + quad + vertex * 16u;
            if(voxyU16Le(source) > maxLocal ||
               voxyU16Le(source + 2u) > 65535u ||
               voxyU16Le(source + 4u) > maxLocal)
            {
                valid = false;
                break;
            }
        }
        if(!valid)
        {
            continue;
        }

        std::vector<Vertex>* destination = &opaque;
        float materialId = 0.0f;
        if(material == MATERIAL_WATER)
        {
            destination = &alpha;
            materialId = 3.0f;
        }
        else if(material == MATERIAL_LEAVES)
        {
            destination = &alpha;
            materialId = 4.0f;
        }
        else if(transparentPass)
        {
            destination = &alpha;
            materialId = 2.0f;
        }

        Vertex quadVertices[4]{};
        const bool voxyEmissiveSolid =
            (material == MATERIAL_LAVA || material == MATERIAL_ILLUMINATED);
        const bool voxyNoBakedShade = voxyEmissiveSolid || material == MATERIAL_WATER;
        const float lodMarker = voxyEmissiveSolid ? -2.0f : -1.0f;
        int mipLevel = 0;
        for(int w = 32; w < width; w <<= 1) ++mipLevel;
        const float atlasSizeInv = 1.0f / static_cast<float>(g_voxyAtlasSize);
        float quadShade = 1.0f;
        if(!voxyNoBakedShade && input[quad + 15u] != 0u)
        {
            const float quadFaceTint = voxyQuadFaceTint(input.data() + quad);
            const float quadLightFactor = std::min(1.0f, std::max(0.0f,
                (static_cast<float>(input[quad + 15u]) * (1.0f / 255.0f))
                    / std::max(quadFaceTint, 1.0e-3f)));
            quadShade = quadFaceTint
                * std::pow(quadLightFactor, VOXY_LOD_SHADE_COMPRESS);
        }
        for(uint32_t vertex = 0u; vertex < 4u; ++vertex)
        {
            const uint8_t* source = input.data() + quad + vertex * 16u;
            Vertex& out = quadVertices[vertex];
            out.x = static_cast<float>(originX + voxyU16Le(source));
            out.y = static_cast<float>(originY + voxyU16Le(source + 2u));
            out.z = static_cast<float>(originZ + voxyU16Le(source + 4u));
            const uint16_t atlasU = voxyU16Le(source + 6u);
            const uint16_t atlasV = voxyU16Le(source + 8u);
            const uint8_t shadeByte = source[15u];
            out.a = static_cast<float>(source[13u]) * (1.0f / 255.0f);
            out.materialId = materialId;
            out.pad1 = lodMarker;
            if((atlasU != 0xFFFFu || atlasV != 0xFFFFu) && shadeByte != 0u)
            {
                out.u = (static_cast<float>(atlasU) + 0.5f) * atlasSizeInv;
                out.v = (static_cast<float>(atlasV) + 0.5f) * atlasSizeInv;
                out.r = static_cast<float>(source[10u]) * (1.0f / 255.0f);
                out.g = static_cast<float>(source[11u]) * (1.0f / 255.0f);
                out.b = static_cast<float>(source[12u]) * (1.0f / 255.0f);
                out.pad2 = static_cast<float>(mipLevel)
                    + std::min(255.0f, std::floor(quadShade * 255.0f + 0.5f))
                        * (1.0f / 256.0f);
            }
            else
            {
                out.u = 0.0f;
                out.v = 0.0f;
                const float shade = quadShade;
                out.r = static_cast<float>(source[10u]) * (1.0f / 255.0f) * shade;
                out.g = static_cast<float>(source[11u]) * (1.0f / 255.0f) * shade;
                out.b = static_cast<float>(source[12u]) * (1.0f / 255.0f) * shade;
                out.pad2 = -1.0f;
            }
        }
        destination->insert(destination->end(), {
            quadVertices[0], quadVertices[1], quadVertices[2],
            quadVertices[0], quadVertices[2], quadVertices[3]
        });
    }
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_uploadVoxyLodMesh(
    JNIEnv* env,
    jclass,
    jlong sourceKey,
    jlong version,
    jint originX,
    jint originY,
    jint originZ,
    jint width,
    jint dataPointWidth,
    jbyteArray opaqueBytes,
    jbyteArray transparentBytes)
{
    if(width <= 0 || width >= 65535 || dataPointWidth <= 0)
    {
        return;
    }
    std::vector<uint8_t> opaqueInput;
    std::vector<uint8_t> transparentInput;
    if(!copyVoxyLodArray(env, opaqueBytes, opaqueInput) ||
       !copyVoxyLodArray(env, transparentBytes, transparentInput))
    {
        return;
    }

    std::vector<Vertex> opaque;
    std::vector<Vertex> alpha;
    opaque.reserve((opaqueInput.size() + transparentInput.size()) / 64u * 6u);
    alpha.reserve((opaqueInput.size() + transparentInput.size()) / 64u * 6u);
    appendVoxyLodQuads(opaqueInput, false, originX, originY, originZ, width, opaque, alpha);
    appendVoxyLodQuads(transparentInput, true, originX, originY, originZ, width, opaque, alpha);


    const uint64_t source = static_cast<uint64_t>(sourceKey);
    std::lock_guard<std::timed_mutex> lock(g_chunkMutex);
    auto existing = g_voxyLodSources.find(source);
    if(existing != g_voxyLodSources.end() && existing->second.version == static_cast<uint64_t>(version))
    {
        return;
    }
    const uint64_t spatialKey = existing != g_voxyLodSources.end()
        ? existing->second.spatialKey : allocateVoxyLodSpatialKeyLocked(source);
    const uint64_t opaqueKey = spatialKey;
    const uint64_t alphaKey = LOD_AS_CLASS_ALPHA | spatialKey;
    if(opaque.empty()) g_chunkMeshes.erase(opaqueKey); else g_chunkMeshes[opaqueKey] = std::move(opaque);
    if(alpha.empty()) g_chunkMeshes.erase(alphaKey); else g_chunkMeshes[alphaKey] = std::move(alpha);
    g_chunkMeshes.erase(LOD_AS_CLASS_WATER | spatialKey);
    g_voxyLodSources[source] = { static_cast<uint64_t>(version), spatialKey };
    g_voxyLodSpatialOwners[spatialKey] = source;
    g_dirtySectionKeys.insert(spatialKey);
    markWorldGeometryDirty();
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_removeVoxyLodMesh(
    JNIEnv*, jclass, jlong sourceKey)
{
    const uint64_t source = static_cast<uint64_t>(sourceKey);
    std::lock_guard<std::timed_mutex> lock(g_chunkMutex);
    const auto entry = g_voxyLodSources.find(source);
    if(entry == g_voxyLodSources.end()) return;
    const uint64_t spatialKey = entry->second.spatialKey;
    g_chunkMeshes.erase(spatialKey);
    g_chunkMeshes.erase(LOD_AS_CLASS_ALPHA | spatialKey);
    g_chunkMeshes.erase(LOD_AS_CLASS_WATER | spatialKey);
    g_voxyLodSpatialOwners.erase(spatialKey);
    g_voxyLodSources.erase(entry);
    g_dirtySectionKeys.insert(spatialKey);
    markWorldGeometryDirty();
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_clearVoxyLodMeshes(JNIEnv*, jclass)
{
    std::lock_guard<std::timed_mutex> lock(g_chunkMutex);
    if(g_voxyLodSources.empty()) return;
    for(const auto& entry : g_voxyLodSources)
    {
        const uint64_t spatialKey = entry.second.spatialKey;
        g_chunkMeshes.erase(spatialKey);
        g_chunkMeshes.erase(LOD_AS_CLASS_ALPHA | spatialKey);
        g_chunkMeshes.erase(LOD_AS_CLASS_WATER | spatialKey);
        g_dirtySectionKeys.insert(spatialKey);
    }
    g_voxyLodSources.clear();
    g_voxyLodSpatialOwners.clear();
    markWorldGeometryDirty();
}
uint64_t allocateDhLodSpatialKeyLocked(uint64_t sectionKey)
{
    uint64_t candidate =
        mixVoxyLodKey(sectionKey ^ DH_LOD_SALT) & VOXY_LOD_SPATIAL_LOW_MASK;
    while(true)
    {
        const uint64_t spatialKey = VOXY_LOD_RESERVED_Y | candidate;
        const auto dh = g_dhLodSpatialOwners.find(spatialKey);
        const auto voxy = g_voxyLodSpatialOwners.find(spatialKey);
        if((dh == g_dhLodSpatialOwners.end() || dh->second == sectionKey) &&
           voxy == g_voxyLodSpatialOwners.end())
        {
            return spatialKey;
        }
        candidate = (candidate + 1u) & VOXY_LOD_SPATIAL_LOW_MASK;
    }
}

/** True when this spatial key belongs to a DH far-field section (vs voxy LOD). */
bool isDhLodSpatialKey(uint64_t spatialKey)
{
    return std::binary_search(
        g_dhLodSpatialSnapshot.begin(),
        g_dhLodSpatialSnapshot.end(),
        spatialKey);
}

// ---------------------------------------------------------------------------
// 2026-11-19 rasterLod：记录流大 SSBO 的区间分配器（bump + free-list）。
// 所有区间 256B 对齐；所有调用方必须已持有 g_chunkMutex（与 uploadDhLodSection
// /removeDhLodSection/recordDhLodRasterPass 同一把锁）。
// 扩容在渲染线程 recordDhLodRasterPass 入口做（FGDeviceWaitIdle + 重建 + 全部
// 节 recordOffset 置 INVALID），上传线程只保留 CPU 记录，绝不碰 Vulkan。
// ---------------------------------------------------------------------------
constexpr VkDeviceSize DH_LOD_RASTER_ALIGN = 256;
constexpr VkDeviceSize DH_LOD_RASTER_MIN_CAPACITY = 64ull * 1024ull * 1024ull;

// 文件内分配器状态（语义上属于 g_dhLodRaster* 全局组；不出头文件）。
static std::vector<std::pair<VkDeviceSize, VkDeviceSize>> g_dhLodRasterFreeRanges;
static VkDeviceSize g_dhLodRasterBumpHighWater = 0;

static VkDeviceSize dhLodRasterPaddedBytes(uint32_t quadCount)
{
    const VkDeviceSize bytes =
        static_cast<VkDeviceSize>(quadCount) * 64ll;
    return (bytes + DH_LOD_RASTER_ALIGN - 1) & ~(DH_LOD_RASTER_ALIGN - 1);
}

static VkDeviceSize dhLodRasterAllocRangeLocked(VkDeviceSize bytes)
{
    if(bytes == 0)
    {
        return DhLodSection::DH_LOD_RASTER_OFFSET_INVALID;
    }

    // 先找 free-list（first-fit）；找不到从 bump 水位分配。
    for(size_t index = 0; index < g_dhLodRasterFreeRanges.size(); ++index)
    {
        if(g_dhLodRasterFreeRanges[index].second >= bytes)
        {
            const VkDeviceSize offset =
                g_dhLodRasterFreeRanges[index].first;
            const VkDeviceSize slack =
                g_dhLodRasterFreeRanges[index].second - bytes;
            if(slack > 0)
            {
                g_dhLodRasterFreeRanges[index] =
                    { offset + bytes, slack };
            }
            else
            {
                g_dhLodRasterFreeRanges.erase(
                    g_dhLodRasterFreeRanges.begin() +
                    static_cast<ptrdiff_t>(index));
            }
            return offset;
        }
    }

    if(g_dhLodRasterRecordCapacity == 0 ||
       g_dhLodRasterBumpHighWater + bytes > g_dhLodRasterRecordCapacity)
    {
        g_dhLodRasterGrowRequested = true;
        return DhLodSection::DH_LOD_RASTER_OFFSET_INVALID;
    }

    const VkDeviceSize offset = g_dhLodRasterBumpHighWater;
    g_dhLodRasterBumpHighWater += bytes;
    return offset;
}

static void dhLodRasterFreeRangeLocked(VkDeviceSize offset, uint32_t quadCount)
{
    if(offset == DhLodSection::DH_LOD_RASTER_OFFSET_INVALID ||
       g_dhLodRasterRecordCapacity == 0 ||
       offset >= g_dhLodRasterRecordCapacity)
    {
        return;
    }

    g_dhLodRasterFreeRanges.push_back(
        { offset, dhLodRasterPaddedBytes(quadCount) });
}

/** Rebuilds g_dhLodSpatialSnapshot.  Caller must hold g_chunkMutex. */
void refreshDhLodSpatialSnapshotLocked()
{
    g_dhLodSpatialSnapshot.clear();
    g_dhLodSpatialSnapshot.reserve(g_dhLodSections.size());
    for(const auto& entry : g_dhLodSections)
    {
        g_dhLodSpatialSnapshot.push_back(entry.second.spatialKey);
    }
    std::sort(
        g_dhLodSpatialSnapshot.begin(),
        g_dhLodSpatialSnapshot.end());
}
void appendDhLodQuads(
    const std::vector<uint8_t>& input,
    int originX,
    int originY,
    int originZ,
    int width,
    std::vector<Vertex>& opaque,
    std::vector<Vertex>& alpha)
{
    constexpr uint8_t MATERIAL_WATER = 12u;
    constexpr uint8_t MATERIAL_AIR = 14u;
    const uint32_t maxLocal = static_cast<uint32_t>(width) + 1u;

    for(size_t quad = 0; quad < input.size(); quad += 64u)
    {
        const uint8_t material = input[quad + 14u];
        if(material == MATERIAL_AIR)
        {
            continue;
        }
        bool valid = true;
        for(uint32_t vertex = 0u; vertex < 4u; ++vertex)
        {
            const uint8_t* source = input.data() + quad + vertex * 16u;
            if(voxyU16Le(source) > maxLocal ||
               voxyU16Le(source + 2u) > 65535u ||
               voxyU16Le(source + 4u) > maxLocal)
            {
                valid = false;
                break;
            }
        }
        if(!valid)
        {
            continue;
        }

        std::vector<Vertex>* destination = &opaque;
        float materialId = 0.0f;
        if(material == MATERIAL_WATER)
        {
            destination = &alpha;
            materialId = 3.0f;
        }

        Vertex quadVertices[4]{};
        const uint8_t dhMaterial = material > 15u ? 0u : material;
        const float lodMarker = -(3.0f + static_cast<float>(dhMaterial));
        const uint16_t tileId = voxyU16Le(input.data() + quad + 6u);
        const bool textured = (tileId != 0u);
        const float pad2Value = textured ? 0.996f : -1.0f;
        for(uint32_t vertex = 0u; vertex < 4u; ++vertex)
        {
            const uint8_t* source = input.data() + quad + vertex * 16u;
            Vertex& out = quadVertices[vertex];
            out.x = static_cast<float>(originX + static_cast<int>(voxyU16Le(source)));
            out.y = static_cast<float>(originY + static_cast<int>(voxyU16Le(source + 2u)));
            out.z = static_cast<float>(originZ + static_cast<int>(voxyU16Le(source + 4u)));
            out.u = static_cast<float>(voxyU16Le(source + 6u));
            out.v = static_cast<float>(voxyU16Le(source + 8u));
            out.r = static_cast<float>(source[10u]) * (1.0f / 255.0f);
            out.g = static_cast<float>(source[11u]) * (1.0f / 255.0f);
            out.b = static_cast<float>(source[12u]) * (1.0f / 255.0f);
            out.a = static_cast<float>(source[13u]) * (1.0f / 255.0f);
            out.materialId = materialId;
            out.pad1 = lodMarker;
            out.pad2 = pad2Value;
        }
        destination->insert(destination->end(), {
            quadVertices[0], quadVertices[1], quadVertices[2],
            quadVertices[0], quadVertices[2], quadVertices[3]
        });
    }
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_uploadDhLodSection(
    JNIEnv* env,
    jclass,
    jlong sectionKey,
    jlong version,
    jint originX,
    jint originY,
    jint originZ,
    jint width,
    jbyteArray quads)
{
    if(width <= 0 || width >= 65535)
    {
        return;
    }
    std::vector<uint8_t> input;
    if(!copyVoxyLodArray(env, quads, input))
    {
        return;
    }

    std::vector<Vertex> opaque;
    std::vector<Vertex> alpha;
    if(!input.empty())
    {
        appendDhLodQuads(input, originX, originY, originZ, width, opaque, alpha);
    }

    const uint64_t section = static_cast<uint64_t>(sectionKey);
    std::lock_guard<std::timed_mutex> lock(g_chunkMutex);
    const auto existing = g_dhLodSections.find(section);
    if(existing != g_dhLodSections.end() &&
       existing->second.version == static_cast<uint64_t>(version))
    {
        return;
    }
    const uint64_t spatialKey = existing != g_dhLodSections.end()
        ? existing->second.spatialKey
        : allocateDhLodSpatialKeyLocked(section);
    const uint64_t opaqueKey = spatialKey;
    const uint64_t alphaKey = LOD_AS_CLASS_ALPHA | spatialKey;
    if(opaque.empty()) g_chunkMeshes.erase(opaqueKey);
    else g_chunkMeshes[opaqueKey] = std::move(opaque);
    if(alpha.empty()) g_chunkMeshes.erase(alphaKey);
    else g_chunkMeshes[alphaKey] = std::move(alpha);
    g_chunkMeshes.erase(LOD_AS_CLASS_WATER | spatialKey);

    // ------------------------------------------------------------------
    // 2026-11-19 rasterLod：保留原始 64B/quad 记录流（G-buffer 光栅投影
    // pass 的 SSBO 数据源）+ push constants 所需的原点/宽度。旗标关闭时只
    // 清零 raster 字段，行为与未接入前逐位一致。区间分配/上 GPU 都推迟到
    // 渲染线程 recordDhLodRasterPass（上传线程不做任何 Vulkan 调用）。
    // ------------------------------------------------------------------
    {
        DhLodSection& stored = g_dhLodSections[section];
        // 归还旧记录区间（同节重传：旧区间作废，下一帧重分配；旗标关闭时
        // offset 本就是 INVALID，空操作）。
        dhLodRasterFreeRangeLocked(
            stored.recordOffset,
            stored.quadCount);
        stored.version = static_cast<uint64_t>(version);
        stored.spatialKey = spatialKey;
        stored.quadCount = g_dhRasterLodEnabled
            ? static_cast<uint32_t>(input.size() / 64u)
            : 0u;
        stored.originX = originX;
        stored.originY = originY;
        stored.originZ = originZ;
        stored.width = width;
        stored.recordVersion = 0u;
        stored.recordOffset = DhLodSection::DH_LOD_RASTER_OFFSET_INVALID;
        if(g_dhRasterLodEnabled)
        {
            stored.records = std::move(input);
        }
        else
        {
            stored.records.clear();
        }
    }

    g_dhLodSpatialOwners[spatialKey] = section;
    g_dirtySectionKeys.insert(spatialKey);
    markWorldGeometryDirty();
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_removeDhLodSection(
    JNIEnv*, jclass, jlong sectionKey)
{
    const uint64_t section = static_cast<uint64_t>(sectionKey);
    std::lock_guard<std::timed_mutex> lock(g_chunkMutex);
    const auto entry = g_dhLodSections.find(section);
    if(entry == g_dhLodSections.end())
    {
        return;
    }
    const uint64_t spatialKey = entry->second.spatialKey;
    g_chunkMeshes.erase(spatialKey);
    g_chunkMeshes.erase(LOD_AS_CLASS_ALPHA | spatialKey);
    g_chunkMeshes.erase(LOD_AS_CLASS_WATER | spatialKey);
    // 2026-11-19 rasterLod：归还记录流区间（旗标关闭时 offset 本就是 INVALID，空操作）。
    dhLodRasterFreeRangeLocked(
        entry->second.recordOffset,
        entry->second.quadCount);
    g_dhLodSpatialOwners.erase(spatialKey);
    g_dhLodSections.erase(entry);
    g_dirtySectionKeys.insert(spatialKey);
    markWorldGeometryDirty();
}

// =====================================================
// 2026-10-09 见 Java_com_example_vulkan_NativeVulkan_clearWorldGeometry 里的调用点说明。
// 定义放在这里（三张映射的声明之后）。**不加锁**：调用方已持有 g_chunkMutex。
//
// 只清映射本身：调用方已经 clear 过 g_chunkMeshes / g_dirtySectionKeys 并调用过
// markWorldGeometryDirty()，对应网格条目已随之消失，所以这里不需要（也不应）再逐条
// erase —— 那是 clearDhLodSections 这条"单独撤某几块 DH"路径的语义，不是世界卸载。
// =====================================================
void clearDhLodSpatialTablesLocked()
{
    // 2026-11-19 rasterLod：世界卸载时归还全部记录区间。
    for(const auto& entry : g_dhLodSections)
    {
        dhLodRasterFreeRangeLocked(
            entry.second.recordOffset,
            entry.second.quadCount);
    }
    g_dhLodSections.clear();
    g_dhLodSpatialOwners.clear();
    g_dhLodSpatialSnapshot.clear();
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_clearDhLodSections(
    JNIEnv*, jclass)
{
    std::lock_guard<std::timed_mutex> lock(g_chunkMutex);
    if(g_dhLodSections.empty())
    {
        return;
    }
    for(const auto& entry : g_dhLodSections)
    {
        const uint64_t spatialKey = entry.second.spatialKey;
        g_chunkMeshes.erase(spatialKey);
        g_chunkMeshes.erase(LOD_AS_CLASS_ALPHA | spatialKey);
        g_chunkMeshes.erase(LOD_AS_CLASS_WATER | spatialKey);
        // 2026-11-19 rasterLod：归还记录流区间。
        dhLodRasterFreeRangeLocked(
            entry.second.recordOffset,
            entry.second.quadCount);
        g_dirtySectionKeys.insert(spatialKey);
    }
    g_dhLodSections.clear();
    g_dhLodSpatialOwners.clear();
    g_dhLodSpatialSnapshot.clear();
    markWorldGeometryDirty();
}

// =====================================================
// Voxy LOD texture atlas (P2/B)
//
// The bridge packs mip0 plus the complete 2x2 downsample chain into one
// RGBA8 byte array.  Java (VoxyLodManager) re-sends the array on world/pack
// changes and periodically as device-recreate insurance; a content hash plus
// the descriptor-set identity make repeat sends free unless something was
// actually torn down and rebuilt underneath.
// =====================================================
void destroyVoxyLodAtlasResources()
{
    if(g_voxyAtlasSampler) { vkDestroySampler(g_device, g_voxyAtlasSampler, nullptr); }
    if(g_voxyAtlasView) { vkDestroyImageView(g_device, g_voxyAtlasView, nullptr); }
    if(g_voxyAtlasImage) { vkDestroyImage(g_device, g_voxyAtlasImage, nullptr); }
    if(g_voxyAtlasMemory) { vkFreeMemory(g_device, g_voxyAtlasMemory, nullptr); }
    g_voxyAtlasSampler = VK_NULL_HANDLE;
    g_voxyAtlasView = VK_NULL_HANDLE;
    g_voxyAtlasImage = VK_NULL_HANDLE;
    g_voxyAtlasMemory = VK_NULL_HANDLE;
    g_voxyAtlasContentHash = 0u;
    g_voxyAtlasDescriptorSet = VK_NULL_HANDLE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_uploadVoxyLodAtlas(
    JNIEnv* env,
    jclass,
    jbyteArray pixels,
    jint size,
    jint mipLevels)
{
    if(g_device == VK_NULL_HANDLE)
    {
        return JNI_FALSE;
    }
    if(pixels == nullptr || size <= 0 || mipLevels <= 0 || mipLevels > 14)
    {
        return g_voxyAtlasImage != VK_NULL_HANDLE ? JNI_TRUE : JNI_FALSE;
    }
    const uint32_t extent = static_cast<uint32_t>(size);
    if((extent & (extent - 1u)) != 0u)
    {
        return g_voxyAtlasImage != VK_NULL_HANDLE ? JNI_TRUE : JNI_FALSE;
    }
    jsize arrayLength = env->GetArrayLength(pixels);
    uint64_t expectedBytes = 0u;
    for(int mip = 0; mip < mipLevels; ++mip)
    {
        const uint32_t mipExtent = std::max(1u, extent >> mip);
        expectedBytes += static_cast<uint64_t>(mipExtent) * mipExtent * 4u;
    }
    if(static_cast<uint64_t>(arrayLength) != expectedBytes)
    {
        return g_voxyAtlasImage != VK_NULL_HANDLE ? JNI_TRUE : JNI_FALSE;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(arrayLength));
    env->GetByteArrayRegion(pixels, 0, arrayLength, reinterpret_cast<jbyte*>(bytes.data()));

    uint64_t contentHash = 1469598103934665603ull;
    for(const uint8_t value : bytes)
    {
        contentHash ^= value;
        contentHash *= 1099511628211ull;
    }
    if(g_voxyAtlasImage != VK_NULL_HANDLE &&
       g_voxyAtlasDescriptorSet == g_rayTracingDescriptorSet &&
       contentHash == g_voxyAtlasContentHash)
    {
        return JNI_TRUE;
    }

    FGDeviceWaitIdle(g_device);
    destroyVoxyLodAtlasResources();
    g_voxyAtlasSize = extent;

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    createBuffer(g_device, g_physicalDevice, bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuffer, stagingMemory);
    void* mapped = nullptr;
    check(vkMapMemory(g_device, stagingMemory, 0u, bytes.size(), 0u, &mapped), "voxy atlas staging map failed");
    std::memcpy(mapped, bytes.data(), bytes.size());
    vkUnmapMemory(g_device, stagingMemory);

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {extent, extent, 1u};
    imageInfo.mipLevels = static_cast<uint32_t>(mipLevels);
    imageInfo.arrayLayers = 1u;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateImage(g_device, &imageInfo, nullptr, &g_voxyAtlasImage), "voxy atlas create image failed");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g_device, g_voxyAtlasImage, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(g_physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocation, nullptr, &g_voxyAtlasMemory), "voxy atlas allocate failed");
    check(vkBindImageMemory(g_device, g_voxyAtlasImage, g_voxyAtlasMemory, 0u), "voxy atlas bind failed");

    VkCommandBufferAllocateInfo commandAllocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandAllocation.commandPool = g_rtCommandPool;
    commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandAllocation.commandBufferCount = 1u;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(g_device, &commandAllocation, &commandBuffer), "voxy atlas command allocation failed");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commandBuffer, &begin), "voxy atlas command begin failed");
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = g_voxyAtlasImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = static_cast<uint32_t>(mipLevels);
    barrier.subresourceRange.layerCount = 1u;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
    std::vector<VkBufferImageCopy> copies(static_cast<size_t>(mipLevels));
    uint64_t bufferOffset = 0u;
    for(int mip = 0; mip < mipLevels; ++mip)
    {
        const uint32_t mipExtent = std::max(1u, extent >> mip);
        copies[static_cast<size_t>(mip)].bufferOffset = bufferOffset;
        copies[static_cast<size_t>(mip)].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copies[static_cast<size_t>(mip)].imageSubresource.mipLevel = static_cast<uint32_t>(mip);
        copies[static_cast<size_t>(mip)].imageSubresource.layerCount = 1u;
        copies[static_cast<size_t>(mip)].imageExtent = {mipExtent, mipExtent, 1u};
        bufferOffset += static_cast<uint64_t>(mipExtent) * mipExtent * 4u;
    }
    vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, g_voxyAtlasImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(mipLevels), copies.data());
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
    check(vkEndCommandBuffer(commandBuffer), "voxy atlas command end failed");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &commandBuffer;
    check(vkQueueSubmit(g_queue, 1u, &submit, VK_NULL_HANDLE), "voxy atlas submit failed");
    check(vkQueueWaitIdle(g_queue), "voxy atlas upload wait failed");
    vkFreeCommandBuffers(g_device, g_rtCommandPool, 1u, &commandBuffer);
    vkDestroyBuffer(g_device, stagingBuffer, nullptr);
    vkFreeMemory(g_device, stagingMemory, nullptr);

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = g_voxyAtlasImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = static_cast<uint32_t>(mipLevels);
    viewInfo.subresourceRange.layerCount = 1u;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_voxyAtlasView), "voxy atlas view failed");
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = static_cast<float>(mipLevels - 1);
    check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_voxyAtlasSampler), "voxy atlas sampler failed");

    if(g_rayTracingDescriptorSet != VK_NULL_HANDLE)
    {
        VkDescriptorImageInfo info{};
        info.sampler = g_voxyAtlasSampler;
        info.imageView = g_voxyAtlasView;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = g_rayTracingDescriptorSet;
        write.dstBinding = 45;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &info;
        vkUpdateDescriptorSets(g_device, 1, &write, 0, nullptr);
    }
    g_voxyAtlasContentHash = contentHash;
    g_voxyAtlasDescriptorSet = g_rayTracingDescriptorSet;
    return JNI_TRUE;
}

// ---------------------------------------------------------------------------
// Distant Horizons：把 color / depth / blockRatioAtlas 写进 RT 描述符集的
// binding 47 / 48 / 49。
//
// 三个必须遵守的约定，破坏任何一条都会静默出错：
//  1) 先 FGDeviceWaitIdle。描述符集可能仍被在飞的提交引用，原地改写是未定义
//     行为 —— 与区块图集的既有权衡一致（见 recordFrameCommand 开头的
//     atlasPublishDeviceIdle）。
//  2) imageLayout 必须是 VK_IMAGE_LAYOUT_GENERAL。renderpearl 建图时
//     initialLayout=UNDEFINED 后立刻转到 GENERAL，且它**不保存任何 layout
//     状态**、此后的渲染附件与描述符全都按 GENERAL 处理。写
//     SHADER_READ_ONLY_OPTIMAL 会与实际 layout 不符。
//  3) DH 句柄无效时回落绑 `g_atlasImageView`。未写入的描述符被采样是未定义
//     行为，而 atlas 已经是本工程公认的合法采样图；此时 UBO 的
//     dhFarFieldParams[0] 为 0，rgen 根本不会去取。
//
// 四种输入组合映射到三态（2026-09-30 起）：
//   - color+depth+atlas 全有   ⇒ 三个 binding 都绑 DH 真句柄（方案 c + 真纹理）；
//   - 只有 atlas               ⇒ 47/48 用 atlas 占位、49 绑 DH atlas（真纹理）。
//     这是 rendererMode=DISABLED 下的常态：color/depth 永不创建，
//     atlas 由 Java 侧 uploadPendingTiles() 主动建出（见 publishDhBlockRatioAtlas）；
//   - 都没有                   ⇒ 回落 MC 图集（UBO 门 0，实际不会被采样）。
// ---------------------------------------------------------------------------
void ensureDhTextureDescriptors()
{
    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE)
    {
        return;
    }

    const bool dhGroupBound = dhTexturesAvailable();
    const bool dhAtlasOnly  = !dhGroupBound && g_dhAtlasUsable;
    const bool dhBound      = dhGroupBound || dhAtlasOnly;

    if(dhGroupBound)
    {
        if(g_dhTexturesBoundRevision == g_dhTexturesRevision)
        {
            return;
        }
    }
    else if(dhAtlasOnly)
    {
        if(g_dhAtlasBoundRevision == g_dhAtlasRevision)
        {
            return;
        }
    }
    else if(g_dhFallbackWritten && g_dhFallbackAtlasView == g_atlasImageView)
    {
        return;
    }

    VkImageView views[3];
    VkSampler   samplers[3];

    if(dhGroupBound)
    {
        views[0] = g_dhColorView;
        views[1] = g_dhDepthView;
        views[2] = g_dhAtlasView;
        samplers[0] = g_dhColorSampler;
        samplers[1] = g_dhDepthSampler;
        samplers[2] = g_dhAtlasSampler;
    }
    else if(dhAtlasOnly)
    {
        views[0] = views[1] = views[2] = g_dhAtlasView;
        samplers[0] = samplers[1] = samplers[2] = g_dhAtlasSampler;
    }
    else
    {
        views[0] = views[1] = views[2] = g_atlasImageView;
        samplers[0] = samplers[1] = samplers[2] = g_atlasSampler;
    }

    if(views[0] == VK_NULL_HANDLE)
    {
        g_dhTexturesBoundRevision = 0;
        g_dhAtlasBoundRevision = 0;
        g_dhFallbackWritten = false;
        g_dhFallbackAtlasView = VK_NULL_HANDLE;
        return;
    }

    FGDeviceWaitIdle(g_device);

    VkDescriptorImageInfo imageInfos[3]{};
    VkWriteDescriptorSet writes[3]{};

    for(int i = 0; i < 3; ++i)
    {
        imageInfos[i].imageView = views[i];
        imageInfos[i].sampler = samplers[i];
        imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = g_rayTracingDescriptorSet;
        writes[i].dstBinding = 47u + static_cast<uint32_t>(i);
        writes[i].dstArrayElement = 0;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &imageInfos[i];
    }

    vkUpdateDescriptorSets(g_device, 3, writes, 0, nullptr);

    g_dhTexturesBoundRevision =
        dhGroupBound ? g_dhTexturesRevision : 0;
    g_dhAtlasBoundRevision =
        dhAtlasOnly ? g_dhAtlasRevision : 0;
    g_dhFallbackWritten  = !dhBound;
    g_dhFallbackAtlasView = dhBound ? VK_NULL_HANDLE : g_atlasImageView;

    std::ostringstream dhDescLog;
    dhDescLog
        << "[DH TEX] 描述符已写 binding47=color binding48=depth binding49=atlas"
        << " 来源=" << (dhGroupBound
                            ? "DH 句柄"
                            : (dhAtlasOnly ? "仅 atlas" : "回落图集"))
        << " layout=GENERAL(1)";

    if(dhGroupBound)
    {
        dhDescLog << " size=" << g_dhTextureWidth << "x" << g_dhTextureHeight;
    }

    dhDescLog << " revision=" << (dhAtlasOnly ? g_dhAtlasRevision
                                              : g_dhTexturesRevision);

    RTLog(dhDescLog.str());
}

// ---------------------------------------------------------------------------
// DH 图像可见性屏障。
//
// layout 不需要改（renderpearl 的图恒在 GENERAL），所以这不是 layout
// transition，而纯粹是一道执行/内存依赖：DH 在我们的 RT 之前、同一 queue 上
// 渲染完它的 LOD，我们要保证它的 color/depth 写入对我们的 rgen 采样可见。
//
// 为什么用**全局** VkMemoryBarrier 而不是逐张图的 VkImageMemoryBarrier：
// image barrier 必须声明 `subresourceRange.aspectMask`，而 aspect 取决于图像
// 格式。实测（latest.log 的 [DH] 句柄探针）：color = RGBA8_UNORM，
// **depth = D32_FLOAT** —— 所以 depth 必须用 DEPTH_BIT、color 必须用 COLOR_BIT，
// 而同一段循环里两张图写同一个 aspect 必然有一张是错的（校验错误 / 未定义行为）。
// 全局内存屏障不带 aspect，对所有先行写入一次性生效；代价是一道全流水线依赖，
// 每帧一次可以忽略。
//
// srcStage 取 ALL_COMMANDS：DH 的 pass 是动态渲染，我们无法列举它会用到哪些
// stage。dstStage 只到 RT shader。srcAccess 必须包含 COLOR_ATTACHMENT_WRITE
// （DH 把它当渲染目标写）与 MEMORY_WRITE（VMA/清屏路径）。
// ---------------------------------------------------------------------------
void recordDhTextureBarrier(VkCommandBuffer cmd)
{
    if((!dhTexturesAvailable() && !g_dhAtlasUsable) || cmd == VK_NULL_HANDLE)
    {
        return;
    }

    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_SHADER_WRITE_BIT |
        VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT |
        VK_ACCESS_MEMORY_READ_BIT;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0,
        1,
        &barrier,
        0,
        nullptr,
        0,
        nullptr
    );
}

// Rebuilds the per-column near-field coverage bitmap from this rebuild's
// near-field Sections.  The grid is their column bounding box, padded by one
// column and clamped to LOD_COVERAGE_MAX_SIZE, so "outside the grid" and
// "inside but no Section" both read back as not covered -- the far field stays
// visible there.  An empty near-field set publishes a zero header, which the
// shader reads as "no coverage data, dedup nothing".
void rebuildLodCoverage(
    const std::vector<RTSectionAS*>& orderedSections
)
{
    if(g_lodCoverageMapped == nullptr || g_lodCoverageWordCount == 0)
    {
        return;
    }

    const size_t coverageBytes =
        sizeof(LodCoverageHeader) +
        static_cast<size_t>(g_lodCoverageWordCount) * sizeof(uint32_t);

    std::memset(g_lodCoverageMapped, 0, coverageBytes);
    markLodCoverageDirty();

    LodCoverageHeader* header =
        static_cast<LodCoverageHeader*>(g_lodCoverageMapped);

    uint32_t* words =
        reinterpret_cast<uint32_t*>(header + 1);

    int32_t minX = 0;
    int32_t minZ = 0;
    int32_t maxX = 0;
    int32_t maxZ = 0;

    bool anyNearField = false;

    for(const RTSectionAS* section : orderedSections)
    {
        if(sectionRoleOf(*section) != SECTION_ROLE_TERRAIN)
        {
            continue;
        }

        const uint64_t spatialKey =
            section->sectionKey & SPATIAL_SECTION_KEY_MASK;

        const int32_t columnX = sectionKeyColumnX(spatialKey);
        const int32_t columnZ = sectionKeyColumnZ(spatialKey);

        if(!anyNearField)
        {
            minX = maxX = columnX;
            minZ = maxZ = columnZ;
            anyNearField = true;
            continue;
        }

        minX = std::min(minX, columnX);
        maxX = std::max(maxX, columnX);
        minZ = std::min(minZ, columnZ);
        maxZ = std::max(maxZ, columnZ);
    }

    if(!anyNearField)
    {
        return;
    }

    --minX;
    --minZ;
    ++maxX;
    ++maxZ;

    const int32_t sizeX = std::min(maxX - minX + 1, LOD_COVERAGE_MAX_SIZE);
    const int32_t sizeZ = std::min(maxZ - minZ + 1, LOD_COVERAGE_MAX_SIZE);

    header->originChunkX = minX;
    header->originChunkZ = minZ;
    header->sizeX = sizeX;
    header->sizeZ = sizeZ;

    for(const RTSectionAS* section : orderedSections)
    {
        if(sectionRoleOf(*section) != SECTION_ROLE_TERRAIN)
        {
            continue;
        }

        const uint64_t spatialKey =
            section->sectionKey & SPATIAL_SECTION_KEY_MASK;

        const int32_t relativeX = sectionKeyColumnX(spatialKey) - minX;
        const int32_t relativeZ = sectionKeyColumnZ(spatialKey) - minZ;

        if(relativeX < 0 || relativeX >= sizeX ||
           relativeZ < 0 || relativeZ >= sizeZ)
        {
            continue;
        }

        const uint32_t index =
            static_cast<uint32_t>(relativeZ) *
                static_cast<uint32_t>(sizeX) +
            static_cast<uint32_t>(relativeX);

        words[index / 4u] |=
            LOD_COVERAGE_FLAG_NEAR_READY << ((index % 4u) * 8u);
    }
}

// ===========================================================================
// 2026-11-19 DH LOD「G-buffer 光栅光影」引擎接线（阶段二余下）
// ===========================================================================
// 组成：
//   ① 输出图：与 DH 图同尺寸（g_dhTextureWidth/Height；DH 未发布时回落渲染
//      分辨率）的 RGBA8 color + R32F depth storage image（createRRGuideImage）。
//      rgen 侧读点 = binding 96/97（mode=14，见 rgen_dh_far.glsl）。
//   ② compute 管线：dh_lod_raster.comp.spv（push constants ivec4×2 = 32B），
//      描述符集 set0 = {0: cam UBO, 1: 记录流大 SSBO, 2/3: 两张输出图}，
//      创建模式仿 Shadow.cpp 的 createSunShadowFarResources。
//   ③ 每帧派发：recordDhLodRasterPass —— 在 RT 主派发（traceK1K2）之前由
//      Frame.cpp 调用，对 g_dhLodSections 里每个节派发一次（g_dhLodSections
//      与 g_chunkMeshes 的 DH 键一一对应，且只有它持有 push constants 所需的
//      原点/宽度/记录数据，故以其为枚举源）。
//   ④ 记录流 SSBO：单一大 device-local 缓冲（256B 对齐区间 + free-list）+
//      常驻 host-visible staging。上传线程（uploadDhLodSection）只保留 CPU
//      记录；渲染线程在本函数里把版本变化节的记录 memcpy 进 staging 并
//      vkCmdCopyBuffer 到 device 缓冲，再逐节 push+dispatch。
//   同步链：clear(TRANSFER) -> compute 写图 -> RT shader 读图，全在同一条
//      command buffer 内用 pipeline barrier 串行；跨帧依赖用旧 layout +
//      srcStage(COMPUTE|RT_SHADER) 的 image barrier 覆盖（同
//      recordSunShadowFarMap 的既有模式）。
//
// 旗标 g_dhRasterLodEnabled=false 时：create/record 首行短路，零资源、零
//   命令、零行为差异。
// ===========================================================================

// push constants 内存布局（必须与 dh_lod_raster.comp 的 SectionPush 一致）
struct DhLodRasterPush
{
    int32_t originAndWidth[4];   // x,y,z = section 最小角（世界格），w = blockWidth
    int32_t countsAndDims[4];    // x = quadCount, y = 图宽, z = 图高, w = 记录字节偏移
};
static_assert(sizeof(DhLodRasterPush) == 32,
    "DhLodRasterPush must match dh_lod_raster.comp SectionPush (ivec4 x2)");

// RT set 的 binding 96/97 是否已写上（set 重建/资源重建后重写）
static VkDescriptorSet g_dhLodRasterBoundSet = VK_NULL_HANDLE;

static void dhLodRasterDestroyImages()
{
    if(g_dhLodRasterColorView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(g_device, g_dhLodRasterColorView, nullptr);
        g_dhLodRasterColorView = VK_NULL_HANDLE;
    }
    if(g_dhLodRasterColorImage != VK_NULL_HANDLE)
    {
        vkDestroyImage(g_device, g_dhLodRasterColorImage, nullptr);
        vkFreeMemory(g_device, g_dhLodRasterColorMemory, nullptr);
        g_dhLodRasterColorImage = VK_NULL_HANDLE;
        g_dhLodRasterColorMemory = VK_NULL_HANDLE;
    }
    if(g_dhLodRasterDepthView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(g_device, g_dhLodRasterDepthView, nullptr);
        g_dhLodRasterDepthView = VK_NULL_HANDLE;
    }
    if(g_dhLodRasterDepthImage != VK_NULL_HANDLE)
    {
        vkDestroyImage(g_device, g_dhLodRasterDepthImage, nullptr);
        vkFreeMemory(g_device, g_dhLodRasterDepthMemory, nullptr);
        g_dhLodRasterDepthImage = VK_NULL_HANDLE;
        g_dhLodRasterDepthMemory = VK_NULL_HANDLE;
    }
    g_dhLodRasterColorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_dhLodRasterDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

// 扩容记录流缓冲：必须已持锁 + 设备空闲后调用。扩后全部节重分配。
static bool dhLodRasterGrowRecordBuffersLocked()
{
    if(g_device == VK_NULL_HANDLE || g_physicalDevice == VK_NULL_HANDLE)
    {
        return false;
    }

    const VkDeviceSize newCapacity =
        g_dhLodRasterRecordCapacity == 0
            ? DH_LOD_RASTER_MIN_CAPACITY
            : std::max(
                  g_dhLodRasterRecordCapacity * 2,
                  g_dhLodRasterRecordCapacity + DH_LOD_RASTER_MIN_CAPACITY);

    FGDeviceWaitIdle(g_device);

    if(g_dhLodRasterRecordStagingMapped != nullptr)
    {
        vkUnmapMemory(g_device, g_dhLodRasterRecordStagingMemory);
        g_dhLodRasterRecordStagingMapped = nullptr;
    }
    if(g_dhLodRasterRecordStaging != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(g_device, g_dhLodRasterRecordStaging, nullptr);
        vkFreeMemory(g_device, g_dhLodRasterRecordStagingMemory, nullptr);
        g_dhLodRasterRecordStaging = VK_NULL_HANDLE;
        g_dhLodRasterRecordStagingMemory = VK_NULL_HANDLE;
    }
    if(g_dhLodRasterRecordDevice != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(g_device, g_dhLodRasterRecordDevice, nullptr);
        vkFreeMemory(g_device, g_dhLodRasterRecordDeviceMemory, nullptr);
        g_dhLodRasterRecordDevice = VK_NULL_HANDLE;
        g_dhLodRasterRecordDeviceMemory = VK_NULL_HANDLE;
    }

    try
    {
        createBuffer(g_device, g_physicalDevice, newCapacity,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            g_dhLodRasterRecordDevice, g_dhLodRasterRecordDeviceMemory);
        createBuffer(g_device, g_physicalDevice, newCapacity,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            g_dhLodRasterRecordStaging, g_dhLodRasterRecordStagingMemory);
        check(vkMapMemory(g_device, g_dhLodRasterRecordStagingMemory, 0u,
                newCapacity, 0u, &g_dhLodRasterRecordStagingMapped),
            "dh lod raster staging map failed");
    }
    catch(...)
    {
        RTLog("[DH-RASTER] 记录流缓冲扩容失败，本帧跳过投影");
        g_dhLodRasterRecordCapacity = 0u;
        g_dhLodRasterBumpHighWater = 0u;
        g_dhLodRasterFreeRanges.clear();
        for(auto& entry : g_dhLodSections)
        {
            entry.second.recordOffset =
                DhLodSection::DH_LOD_RASTER_OFFSET_INVALID;
            entry.second.recordVersion = 0u;
        }
        g_dhLodRasterGrowRequested = false;
        return false;
    }

    const VkDeviceSize oldCapacity = g_dhLodRasterRecordCapacity;
    g_dhLodRasterRecordCapacity = newCapacity;
    g_dhLodRasterBumpHighWater = 0u;
    g_dhLodRasterFreeRanges.clear();
    g_dhLodRasterGrowRequested = false;

    // 旧区间全部失效：渲染线程在后续帧重新分配 + 重拷（CPU 记录仍在）。
    for(auto& entry : g_dhLodSections)
    {
        entry.second.recordOffset =
            DhLodSection::DH_LOD_RASTER_OFFSET_INVALID;
        entry.second.recordVersion = 0u;
    }

    // 记录流 SSBO 描述符重新指向新缓冲。
    if(g_dhLodRasterSet != VK_NULL_HANDLE)
    {
        VkDescriptorBufferInfo recordInfo{};
        recordInfo.buffer = g_dhLodRasterRecordDevice;
        recordInfo.offset = 0u;
        recordInfo.range = newCapacity;
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = g_dhLodRasterSet;
        write.dstBinding = 1u;
        write.descriptorCount = 1u;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &recordInfo;
        vkUpdateDescriptorSets(g_device, 1u, &write, 0u, nullptr);
    }

    RTLog("[DH-RASTER] 记录流缓冲扩容 " +
        std::to_string(static_cast<uint64_t>(oldCapacity / (1024ull * 1024ull))) +
        "MB -> " +
        std::to_string(static_cast<uint64_t>(newCapacity / (1024ull * 1024ull))) +
        "MB，全部节待重分配");
    return true;
}

void createDhLodRasterResources(uint32_t renderWidth, uint32_t renderHeight)
{
    if(!g_dhRasterLodEnabled || g_device == VK_NULL_HANDLE ||
       g_physicalDevice == VK_NULL_HANDLE)
    {
        return;
    }

    // 尺寸：跟随 DH 图（publishDhTextures 下发的 W×H）；DH 未发布时回落渲染分辨率。
    const uint32_t width =
        (g_dhTexturesValid && g_dhTextureWidth > 0) ?
            static_cast<uint32_t>(g_dhTextureWidth) :
            std::max(renderWidth, 1u);
    const uint32_t height =
        (g_dhTexturesValid && g_dhTextureHeight > 0) ?
            static_cast<uint32_t>(g_dhTextureHeight) :
            std::max(renderHeight, 1u);

    const bool resourcesCurrent =
        g_dhLodRasterColorImage != VK_NULL_HANDLE &&
        g_dhLodRasterDepthImage != VK_NULL_HANDLE &&
        g_dhLodRasterWidth == width &&
        g_dhLodRasterHeight == height &&
        g_dhLodRasterBoundSet == g_rayTracingDescriptorSet;

    if(resourcesCurrent && g_dhLodRasterPipeline != VK_NULL_HANDLE)
    {
        return;
    }

    // 尺寸/set 变化 ⇒ 重建图（销毁前等空闲，尺寸变化是低频事件）。
    if(!resourcesCurrent)
    {
        if(g_dhLodRasterColorImage != VK_NULL_HANDLE ||
           g_dhLodRasterDepthImage != VK_NULL_HANDLE)
        {
            FGDeviceWaitIdle(g_device);
            dhLodRasterDestroyImages();
        }

        RTLogFlushed(std::string("[DH-RASTER] 创建资源 img=") +
            std::to_string(width) + "x" + std::to_string(height) +
            "（DH 发布=" + (g_dhTexturesValid ? "1" : "0") + "）");

        createRRGuideImage(
            width,
            height,
            VK_FORMAT_R8G8B8A8_UNORM,
            g_dhLodRasterColorImage,
            g_dhLodRasterColorMemory,
            g_dhLodRasterColorView,
            g_dhLodRasterColorLayout);

        createRRGuideImage(
            width,
            height,
            VK_FORMAT_R32_SFLOAT,
            g_dhLodRasterDepthImage,
            g_dhLodRasterDepthMemory,
            g_dhLodRasterDepthView,
            g_dhLodRasterDepthLayout);

        g_dhLodRasterColorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        g_dhLodRasterDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        g_dhLodRasterWidth = width;
        g_dhLodRasterHeight = height;
    }

    // 记录流缓冲（首次建 64MB；后续按需扩容）。仅在锁内碰记录簿记，
    // 描述符/管线创建不持 g_chunkMutex（避免阻塞 Java 上传线程）。
    {
        std::lock_guard<std::timed_mutex> lock(g_chunkMutex);
        if(g_dhLodRasterRecordDevice == VK_NULL_HANDLE)
        {
            g_dhLodRasterGrowRequested = true;
            dhLodRasterGrowRecordBuffersLocked();
        }
    }

    // RT set binding 96/97（rgen mode=14 读本项目光栅图；rasterLod 关闭时
    // 本函数不执行，着色器也不动态访问这两个槽 —— 与 binding 94 的
    // ensureMatReuseBuffer 同一约定：共享 layout 先声明，描述符按需写）。
    if(g_rayTracingDescriptorSet != VK_NULL_HANDLE)
    {
        VkDescriptorImageInfo infos[2]{};
        infos[0].imageView = g_dhLodRasterColorView;
        infos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        infos[1].imageView = g_dhLodRasterDepthView;
        infos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = g_rayTracingDescriptorSet;
        writes[0].dstBinding = 96u;
        writes[0].descriptorCount = 1u;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &infos[0];
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = g_rayTracingDescriptorSet;
        writes[1].dstBinding = 97u;
        writes[1].descriptorCount = 1u;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &infos[1];
        vkUpdateDescriptorSets(g_device, 2u, writes, 0u, nullptr);
        g_dhLodRasterBoundSet = g_rayTracingDescriptorSet;
    }

    if(g_dhLodRasterPipeline != VK_NULL_HANDLE)
    {
        return;
    }

    // compute set layout：0=cam UBO / 1=记录流 SSBO / 2=color / 3=depth
    VkDescriptorSetLayoutBinding bindings[4]{};
    bindings[0] = { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bindings[1] = { 1u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bindings[2] = { 2u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bindings[3] = { 3u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    layoutInfo.bindingCount = 4u;
    layoutInfo.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(g_device, &layoutInfo, nullptr,
        &g_dhLodRasterSetLayout), "dh lod raster set layout failed");

    VkDescriptorPoolSize poolSizes[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2u }
    };
    VkDescriptorPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
    };
    poolInfo.maxSets = 1u;
    poolInfo.poolSizeCount = 3u;
    poolInfo.pPoolSizes = poolSizes;
    check(vkCreateDescriptorPool(g_device, &poolInfo, nullptr,
        &g_dhLodRasterDescriptorPool), "dh lod raster descriptor pool failed");

    VkDescriptorSetAllocateInfo setAllocation{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
    };
    setAllocation.descriptorPool = g_dhLodRasterDescriptorPool;
    setAllocation.descriptorSetCount = 1u;
    setAllocation.pSetLayouts = &g_dhLodRasterSetLayout;
    check(vkAllocateDescriptorSets(g_device, &setAllocation,
        &g_dhLodRasterSet), "dh lod raster descriptor set failed");

    VkDescriptorBufferInfo cameraInfo{};
    cameraInfo.buffer = g_cameraUboBuffer;
    cameraInfo.range = sizeof(CameraUBO);
    VkDescriptorBufferInfo recordInfo{};
    recordInfo.buffer = g_dhLodRasterRecordDevice;
    recordInfo.offset = 0u;
    recordInfo.range = g_dhLodRasterRecordCapacity;
    VkDescriptorImageInfo colorInfo{};
    colorInfo.imageView = g_dhLodRasterColorView;
    colorInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo depthInfo{};
    depthInfo.imageView = g_dhLodRasterDepthView;
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[4]{};
    writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_dhLodRasterSet, 0u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &cameraInfo, nullptr };
    writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_dhLodRasterSet, 1u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &recordInfo, nullptr };
    writes[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_dhLodRasterSet, 2u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &colorInfo, nullptr, nullptr };
    writes[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_dhLodRasterSet, 3u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &depthInfo, nullptr, nullptr };
    vkUpdateDescriptorSets(g_device, 4u, writes, 0u, nullptr);

    const std::vector<uint8_t> code =
        loadSpv("shaders/dh_lod_raster.comp.spv");
    VkShaderModule shader = createShaderModule(g_device, code);
    try
    {
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.offset = 0u;
        pushRange.size = sizeof(DhLodRasterPush);

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        pipelineLayoutInfo.setLayoutCount = 1u;
        pipelineLayoutInfo.pSetLayouts = &g_dhLodRasterSetLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1u;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        check(vkCreatePipelineLayout(g_device, &pipelineLayoutInfo, nullptr,
            &g_dhLodRasterPipelineLayout), "dh lod raster pipeline layout failed");

        VkPipelineShaderStageCreateInfo stage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
        };
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shader;
        stage.pName = "main";
        VkComputePipelineCreateInfo pipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
        };
        pipelineInfo.stage = stage;
        pipelineInfo.layout = g_dhLodRasterPipelineLayout;
        check(vkCreateComputePipelines(g_device, getSharedPipelineCache(), 1u,
            &pipelineInfo, nullptr, &g_dhLodRasterPipeline),
            "dh lod raster pipeline failed");
    }
    catch(...)
    {
        vkDestroyShaderModule(g_device, shader, nullptr);
        destroyDhLodRasterResources();
        throw;
    }
    vkDestroyShaderModule(g_device, shader, nullptr);

    RTLog("[DH-RASTER] 资源就绪 " + std::to_string(width) + "x" +
        std::to_string(height) + "（RGBA8 color + R32F depth，每节一次 dispatch，"+
        "binding 96/97 -> rgen mode=14）");
}

void recordDhLodRasterPass(VkCommandBuffer cmd)
{
    if(cmd == VK_NULL_HANDLE || !g_dhRasterLodEnabled ||
       g_dhLodRasterPipeline == VK_NULL_HANDLE ||
       g_dhLodRasterPipelineLayout == VK_NULL_HANDLE ||
       g_dhLodRasterSet == VK_NULL_HANDLE ||
       g_dhLodRasterColorImage == VK_NULL_HANDLE ||
       g_dhLodRasterDepthImage == VK_NULL_HANDLE ||
       g_dhLodRasterRecordDevice == VK_NULL_HANDLE ||
       g_dhLodRasterRecordStagingMapped == nullptr ||
       g_dhLodRasterWidth == 0u || g_dhLodRasterHeight == 0u)
    {
        // 2026-11-19 入口门诊断：首 3 次拦截逐成员打印，定位哪一项把投影 pass 拦住。
        static std::atomic<uint32_t> s_gateLogCount{0u};
        if(s_gateLogCount.fetch_add(1u, std::memory_order_relaxed) < 3u)
        {
            RTLogFlushed(std::string("[DH-RASTER] 入口门拦截：cmd=") +
                (cmd == VK_NULL_HANDLE ? "null" : "ok") +
                " 旗标=" + (g_dhRasterLodEnabled ? "1" : "0") +
                " pipe=" + (g_dhLodRasterPipeline == VK_NULL_HANDLE ? "null" : "ok") +
                " layout=" + (g_dhLodRasterPipelineLayout == VK_NULL_HANDLE ? "null" : "ok") +
                " set=" + (g_dhLodRasterSet == VK_NULL_HANDLE ? "null" : "ok") +
                " color=" + (g_dhLodRasterColorImage == VK_NULL_HANDLE ? "null" : "ok") +
                " depth=" + (g_dhLodRasterDepthImage == VK_NULL_HANDLE ? "null" : "ok") +
                " rec=" + (g_dhLodRasterRecordDevice == VK_NULL_HANDLE ? "null" : "ok") +
                " stage=" + (g_dhLodRasterRecordStagingMapped == nullptr ? "null" : "ok") +
                " img=" + std::to_string(g_dhLodRasterWidth) + "x" +
                std::to_string(g_dhLodRasterHeight));
        }
        return;
    }

    std::lock_guard<std::timed_mutex> lock(g_chunkMutex);

    if(g_dhLodRasterGrowRequested)
    {
        dhLodRasterGrowRecordBuffersLocked();
    }

    bool anyDispatch = false;
    bool anyMissingRecords = false;

    // ---- ① 版本变化的节：分配区间 + 写 staging + 当帧 copyBuffer ------------
    for(auto& entry : g_dhLodSections)
    {
        DhLodSection& section = entry.second;
        if(section.quadCount == 0u || section.records.empty())
        {
            if(section.quadCount != 0u)
            {
                anyMissingRecords = true;
            }
            continue;
        }

        const VkDeviceSize bytes =
            dhLodRasterPaddedBytes(section.quadCount);

        if(section.recordOffset ==
           DhLodSection::DH_LOD_RASTER_OFFSET_INVALID)
        {
            const VkDeviceSize offset =
                dhLodRasterAllocRangeLocked(bytes);
            if(offset == DhLodSection::DH_LOD_RASTER_OFFSET_INVALID)
            {
                continue;   // 扩容已请求，下一帧重分配
            }
            section.recordOffset = offset;
        }

        if(section.recordVersion == section.version)
        {
            continue;       // GPU 上已是当前版本
        }

        const size_t rawBytes =
            static_cast<size_t>(section.quadCount) * 64u;
        std::memcpy(
            reinterpret_cast<uint8_t*>(g_dhLodRasterRecordStagingMapped) +
                static_cast<size_t>(section.recordOffset),
            section.records.data(),
            rawBytes);

        VkBufferCopy region{};
        region.srcOffset = section.recordOffset;
        region.dstOffset = section.recordOffset;
        region.size = bytes;
        vkCmdCopyBuffer(
            cmd,
            g_dhLodRasterRecordStaging,
            g_dhLodRasterRecordDevice,
            1u,
            &region);
        section.recordVersion = section.version;
    }

    // ---- ② 跨帧 image barrier（上一帧 RT 读 + 上帧 compute 写 -> 本帧 clear）-
    VkImageMemoryBarrier clearBarrier{};
    clearBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    clearBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    clearBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    clearBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clearBarrier.subresourceRange.levelCount = 1u;
    clearBarrier.subresourceRange.layerCount = 1u;

    auto recordClearBarrier = [&](VkImage image, VkImageLayout& tracked)
    {
        clearBarrier.oldLayout = tracked;
        clearBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        clearBarrier.srcAccessMask = tracked == VK_IMAGE_LAYOUT_UNDEFINED
            ? 0u
            : (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        clearBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        clearBarrier.image = image;
        vkCmdPipelineBarrier(
            cmd,
            tracked == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                : (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                   VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR),
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0u, 0u, nullptr, 0u, nullptr, 1u, &clearBarrier);
        tracked = VK_IMAGE_LAYOUT_GENERAL;
    };
    recordClearBarrier(g_dhLodRasterColorImage, g_dhLodRasterColorLayout);
    recordClearBarrier(g_dhLodRasterDepthImage, g_dhLodRasterDepthLayout);

    // ---- ③ 当帧清零（未覆盖像素 = 0.0 = 无几何；避免陈旧像素残留）-----------
    const VkClearColorValue zeroColor{};
    const VkImageSubresourceRange fullRange{
        VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u };
    vkCmdClearColorImage(cmd, g_dhLodRasterColorImage,
        VK_IMAGE_LAYOUT_GENERAL, &zeroColor, 1u, &fullRange);
    vkCmdClearColorImage(cmd, g_dhLodRasterDepthImage,
        VK_IMAGE_LAYOUT_GENERAL, &zeroColor, 1u, &fullRange);

    // clear -> compute 写图
    clearBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    clearBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    clearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    clearBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    clearBarrier.image = g_dhLodRasterColorImage;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr,
        1u, &clearBarrier);
    clearBarrier.image = g_dhLodRasterDepthImage;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr,
        1u, &clearBarrier);

    // ---- ④ 记录拷贝 -> compute 读（SSBO 可见） -------------------------------
    VkMemoryBarrier recordBarrier{};
    recordBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    recordBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    recordBarrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 1u, &recordBarrier,
        0u, nullptr, 0u, nullptr);

    // ---- ⑤ 逐节派发 -------------------------------------------------------
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_dhLodRasterPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_dhLodRasterPipelineLayout, 0u, 1u, &g_dhLodRasterSet, 0u, nullptr);

    for(const auto& entry : g_dhLodSections)
    {
        const DhLodSection& section = entry.second;
        if(section.quadCount == 0u || section.records.empty() ||
           section.recordOffset ==
               DhLodSection::DH_LOD_RASTER_OFFSET_INVALID)
        {
            continue;
        }

        DhLodRasterPush push{};
        push.originAndWidth[0] = section.originX;
        push.originAndWidth[1] = section.originY;
        push.originAndWidth[2] = section.originZ;
        push.originAndWidth[3] = section.width;
        push.countsAndDims[0] = static_cast<int32_t>(section.quadCount);
        push.countsAndDims[1] = static_cast<int32_t>(g_dhLodRasterWidth);
        push.countsAndDims[2] = static_cast<int32_t>(g_dhLodRasterHeight);
        push.countsAndDims[3] = static_cast<int32_t>(section.recordOffset);

        vkCmdPushConstants(
            cmd,
            g_dhLodRasterPipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT,
            0u,
            sizeof(DhLodRasterPush),
            &push);
        const uint32_t groups =
            (section.quadCount + 63u) / 64u;
        vkCmdDispatch(cmd, groups, 1u, 1u);
        anyDispatch = true;
    }

    // 2026-11-19 派发计数诊断：首 3 次 + 每 600 帧一次，量化「图为什么没内容」。
    {
        static std::atomic<uint32_t> s_dispatchLogFrame{0u};
        const uint32_t frameIdx =
            s_dispatchLogFrame.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if(frameIdx <= 3u || frameIdx % 600u == 0u)
        {
            uint32_t readySections = 0u;
            uint64_t readyQuads = 0u;
            for(const auto& entry : g_dhLodSections)
            {
                const DhLodSection& s = entry.second;
                if(s.quadCount != 0u && !s.records.empty() &&
                   s.recordOffset !=
                       DhLodSection::DH_LOD_RASTER_OFFSET_INVALID)
                {
                    ++readySections;
                    readyQuads += s.quadCount;
                }
            }
            RTLogFlushed(std::string("[DH-RASTER] 帧计数=") +
                std::to_string(frameIdx) +
                " 本帧派发=" + (anyDispatch ? "有" : "无") +
                " 就绪节=" + std::to_string(readySections) +
                " quad=" + std::to_string(readyQuads) +
                " img=" + std::to_string(g_dhLodRasterWidth) + "x" +
                std::to_string(g_dhLodRasterHeight) +
                " 旗标=" + (g_dhRasterLodEnabled ? "1" : "0"));
        }
    }

    if(anyMissingRecords)
    {
        static std::atomic<uint32_t> s_missingLogCount{0u};
        if(s_missingLogCount.fetch_add(1u, std::memory_order_relaxed) < 3u)
        {
            RTLog("[DH-RASTER] 有节在旗标开启前上传、无保留记录，跳过投影" +
                std::string("（重新上传后恢复）"));
        }
    }

    if(!anyDispatch)
    {
        return;
    }

    // ---- ⑥ compute 写图 -> RT shader 读 -----------------------------------
    clearBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    clearBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    clearBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    clearBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    clearBarrier.image = g_dhLodRasterColorImage;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0u, 0u, nullptr, 0u, nullptr, 1u, &clearBarrier);
    clearBarrier.image = g_dhLodRasterDepthImage;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0u, 0u, nullptr, 0u, nullptr, 1u, &clearBarrier);
}

void destroyDhLodRasterResources()
{
    if(g_device == VK_NULL_HANDLE)
    {
        return;
    }

    FGDeviceWaitIdle(g_device);

    if(g_dhLodRasterPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_dhLodRasterPipeline, nullptr);
        g_dhLodRasterPipeline = VK_NULL_HANDLE;
    }
    if(g_dhLodRasterPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(g_device, g_dhLodRasterPipelineLayout, nullptr);
        g_dhLodRasterPipelineLayout = VK_NULL_HANDLE;
    }
    if(g_dhLodRasterDescriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(g_device, g_dhLodRasterDescriptorPool, nullptr);
        g_dhLodRasterDescriptorPool = VK_NULL_HANDLE;
        g_dhLodRasterSet = VK_NULL_HANDLE;
    }
    if(g_dhLodRasterSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(
            g_device, g_dhLodRasterSetLayout, nullptr);
        g_dhLodRasterSetLayout = VK_NULL_HANDLE;
    }

    dhLodRasterDestroyImages();

    if(g_dhLodRasterRecordStagingMapped != nullptr)
    {
        vkUnmapMemory(g_device, g_dhLodRasterRecordStagingMemory);
        g_dhLodRasterRecordStagingMapped = nullptr;
    }
    if(g_dhLodRasterRecordStaging != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(g_device, g_dhLodRasterRecordStaging, nullptr);
        vkFreeMemory(g_device, g_dhLodRasterRecordStagingMemory, nullptr);
        g_dhLodRasterRecordStaging = VK_NULL_HANDLE;
        g_dhLodRasterRecordStagingMemory = VK_NULL_HANDLE;
    }
    if(g_dhLodRasterRecordDevice != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(g_device, g_dhLodRasterRecordDevice, nullptr);
        vkFreeMemory(g_device, g_dhLodRasterRecordDeviceMemory, nullptr);
        g_dhLodRasterRecordDevice = VK_NULL_HANDLE;
        g_dhLodRasterRecordDeviceMemory = VK_NULL_HANDLE;
    }

    {
        std::lock_guard<std::timed_mutex> lock(g_chunkMutex);
        g_dhLodRasterRecordCapacity = 0u;
        g_dhLodRasterBumpHighWater = 0u;
        g_dhLodRasterFreeRanges.clear();
        g_dhLodRasterGrowRequested = false;
        for(auto& entry : g_dhLodSections)
        {
            entry.second.recordOffset =
                DhLodSection::DH_LOD_RASTER_OFFSET_INVALID;
            entry.second.recordVersion = 0u;
        }
    }

    g_dhLodRasterWidth = 0u;
    g_dhLodRasterHeight = 0u;
    g_dhLodRasterBoundSet = VK_NULL_HANDLE;
}
