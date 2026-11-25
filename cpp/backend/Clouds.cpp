// Orion backend — Clouds 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"


// -------------------------------------------------------------------------
// Tyndall cloud-noise resources
// -------------------------------------------------------------------------
// The reference cloud implementations sample a persistent shape/detail field
// and a decorrelating curl field.  Generate both once, upload once, and never
// touch them again during normal rendering or resource-pack reloads.
uint32_t cloudNoiseHash(uint32_t value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}
float cloudNoiseUnit(uint32_t x, uint32_t y, uint32_t z, uint32_t seed)
{
    uint32_t value = x * 0x8da6b343u ^ y * 0xd8163841u ^
                     z * 0xcb1ab31fu ^ seed;
    return static_cast<float>(cloudNoiseHash(value) & 0x00ffffffu) /
        static_cast<float>(0x01000000u);
}
float cloudNoiseFade(float value)
{
    return value * value * (3.0f - 2.0f * value);
}
float cloudNoiseLerp(float a, float b, float t)
{
    return a + (b - a) * t;
}
uint32_t cloudNoiseWrap(int value, int period)
{
    value %= period;
    return static_cast<uint32_t>(value < 0 ? value + period : value);
}

// A small periodic value-noise basis is enough here: the GPU's trilinear
// sampler handles interpolation between uploaded texels, while this produces
// coherent cloud masses instead of filtering independent white-noise voxels.
float cloudValueNoise(float x, float y, float z, int period, uint32_t seed)
{
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int z0 = static_cast<int>(std::floor(z));
    const float tx = cloudNoiseFade(x - static_cast<float>(x0));
    const float ty = cloudNoiseFade(y - static_cast<float>(y0));
    const float tz = cloudNoiseFade(z - static_cast<float>(z0));
    const auto sample = [period, seed](int sx, int sy, int sz)
    {
        return cloudNoiseUnit(cloudNoiseWrap(sx, period), cloudNoiseWrap(sy, period),
            cloudNoiseWrap(sz, period), seed);
    };
    const float x00 = cloudNoiseLerp(sample(x0, y0, z0), sample(x0 + 1, y0, z0), tx);
    const float x10 = cloudNoiseLerp(sample(x0, y0 + 1, z0), sample(x0 + 1, y0 + 1, z0), tx);
    const float x01 = cloudNoiseLerp(sample(x0, y0, z0 + 1), sample(x0 + 1, y0, z0 + 1), tx);
    const float x11 = cloudNoiseLerp(sample(x0, y0 + 1, z0 + 1), sample(x0 + 1, y0 + 1, z0 + 1), tx);
    return cloudNoiseLerp(cloudNoiseLerp(x00, x10, ty), cloudNoiseLerp(x01, x11, ty), tz);
}
float cloudFbmNoise(float x, float y, float z, uint32_t seed)
{
    return 0.50f * cloudValueNoise(x * 4.0f, y * 4.0f, z * 4.0f, 4, seed) +
           0.30f * cloudValueNoise(x * 9.0f, y * 9.0f, z * 9.0f, 9, seed ^ 0x9e3779b9u) +
           0.20f * cloudValueNoise(x * 19.0f, y * 19.0f, z * 19.0f, 19, seed ^ 0x85ebca6bu);
}
float cloudCellularNoise(float x, float y, float z, int period, uint32_t seed)
{
    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    const int iz = static_cast<int>(std::floor(z));
    const float fx = x - static_cast<float>(ix);
    const float fy = y - static_cast<float>(iy);
    const float fz = z - static_cast<float>(iz);
    float nearest = 3.0f;
    for(int dz = -1; dz <= 1; ++dz)
    for(int dy = -1; dy <= 1; ++dy)
    for(int dx = -1; dx <= 1; ++dx)
    {
        const int cx = ix + dx, cy = iy + dy, cz = iz + dz;
        const uint32_t wx = cloudNoiseWrap(cx, period);
        const uint32_t wy = cloudNoiseWrap(cy, period);
        const uint32_t wz = cloudNoiseWrap(cz, period);
        const float px = cloudNoiseUnit(wx, wy, wz, seed);
        const float py = cloudNoiseUnit(wx, wy, wz, seed ^ 0x68bc21ebu);
        const float pz = cloudNoiseUnit(wx, wy, wz, seed ^ 0x02e5be93u);
        const float ox = static_cast<float>(dx) + px - fx;
        const float oy = static_cast<float>(dy) + py - fy;
        const float oz = static_cast<float>(dz) + pz - fz;
        nearest = std::min(nearest, ox * ox + oy * oy + oz * oz);
    }
    return std::max(0.0f, 1.0f - std::sqrt(nearest) * 0.577350269f);
}
uint8_t cloudNoiseByte(float value)
{
    const float clamped = std::max(0.0f, std::min(1.0f, value));
    return static_cast<uint8_t>(clamped * 255.0f + 0.5f);
}
void updateCloudNoiseRayDescriptors()
{
    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
       g_cloudShapeNoiseView == VK_NULL_HANDLE ||
       g_cloudShapeNoiseSampler == VK_NULL_HANDLE ||
       g_cloudCurlNoiseView == VK_NULL_HANDLE ||
       g_cloudCurlNoiseSampler == VK_NULL_HANDLE)
    {
        return;
    }

    VkDescriptorImageInfo infos[2]{};
    const bool useV4Shape = (g_cloudV4ShapeView != VK_NULL_HANDLE &&
        g_cloudV4ShapeSampler != VK_NULL_HANDLE);
    infos[0].sampler = useV4Shape ? g_cloudV4ShapeSampler : g_cloudShapeNoiseSampler;
    infos[0].imageView = useV4Shape ? g_cloudV4ShapeView : g_cloudShapeNoiseView;
    infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    infos[1].sampler = g_cloudCurlNoiseSampler;
    infos[1].imageView = g_cloudCurlNoiseView;
    infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[2]{};
    for(uint32_t i = 0u; i < 2u; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = g_rayTracingDescriptorSet;
        writes[i].dstBinding = 38u + i;
        writes[i].descriptorCount = 1u;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(g_device, 2u, writes, 0u, nullptr);
}
void createCloudNoiseResources()
{
    if(g_cloudShapeNoiseImage != VK_NULL_HANDLE)
    {
        return;
    }

    const size_t shapeTexelCount = static_cast<size_t>(CLOUD_SHAPE_NOISE_RESOLUTION) *
        CLOUD_SHAPE_NOISE_RESOLUTION * CLOUD_SHAPE_NOISE_RESOLUTION;
    const size_t curlTexelCount = static_cast<size_t>(CLOUD_CURL_NOISE_RESOLUTION) *
        CLOUD_CURL_NOISE_RESOLUTION * CLOUD_CURL_NOISE_RESOLUTION;
    std::vector<uint8_t> shapePixels(shapeTexelCount * 4u);
    std::vector<uint8_t> curlPixels(curlTexelCount * 4u);

    for(uint32_t z = 0u; z < CLOUD_SHAPE_NOISE_RESOLUTION; ++z)
    for(uint32_t y = 0u; y < CLOUD_SHAPE_NOISE_RESOLUTION; ++y)
    for(uint32_t x = 0u; x < CLOUD_SHAPE_NOISE_RESOLUTION; ++x)
    {
        const size_t index = (static_cast<size_t>(z) * CLOUD_SHAPE_NOISE_RESOLUTION *
            CLOUD_SHAPE_NOISE_RESOLUTION + static_cast<size_t>(y) *
            CLOUD_SHAPE_NOISE_RESOLUTION + x) * 4u;
        const float fx = static_cast<float>(x) / static_cast<float>(CLOUD_SHAPE_NOISE_RESOLUTION);
        const float fy = static_cast<float>(y) / static_cast<float>(CLOUD_SHAPE_NOISE_RESOLUTION);
        const float fz = static_cast<float>(z) / static_cast<float>(CLOUD_SHAPE_NOISE_RESOLUTION);
        const float macro = cloudFbmNoise(fx, fy, fz, 0x13579bdfu);
        const float billow = 1.0f - std::abs(2.0f * cloudFbmNoise(fx + 0.37f, fy + 0.11f, fz + 0.71f, 0x2468ace1u) - 1.0f);
        shapePixels[index + 0u] = cloudNoiseByte(macro);
        shapePixels[index + 1u] = cloudNoiseByte(0.55f * macro + 0.45f * billow);
        shapePixels[index + 2u] = cloudNoiseByte(cloudCellularNoise(fx * 7.0f, fy * 7.0f, fz * 7.0f, 7, 0x9e3779b9u));
        shapePixels[index + 3u] = cloudNoiseByte(cloudCellularNoise(fx * 15.0f, fy * 15.0f, fz * 15.0f, 15, 0xa341316cu));
    }

    {
        const std::string shapeOverridePath =
            ResolveRTXRuntimePath("cloud_noise_shape_64.rgba8");
        if(FILE* fp = std::fopen(shapeOverridePath.empty() ? "cloud_noise_shape_64.rgba8"
            : shapeOverridePath.c_str(), "rb"))
        {
            std::vector<uint8_t> ref(shapeTexelCount * 4u);
            const size_t got = std::fread(ref.data(), 1u, ref.size(), fp);
            std::fclose(fp);

            if(got == ref.size())
            {
                for(size_t i = 0u; i < shapeTexelCount; ++i)
                {
                    shapePixels[i * 4u + 0u] = ref[i * 4u + 0u];
                    shapePixels[i * 4u + 1u] = ref[i * 4u + 1u];
                }

                std::fprintf(stderr,
                    "[CLOUD NOISE] shape R/G <- run/cloud_noise_shape_64.rgba8 (%zu texels)\n",
                    shapeTexelCount);
                std::fflush(stderr);
            }
            else
            {
                std::fprintf(stderr,
                    "[CLOUD NOISE] reference shape noise size mismatch (%zu vs %zu), keeping procedural\n",
                    got, ref.size());
                std::fflush(stderr);
            }
        }
    }

    const uint32_t n = CLOUD_CURL_NOISE_RESOLUTION;
    for(uint32_t z = 0u; z < n; ++z)
    for(uint32_t y = 0u; y < n; ++y)
    for(uint32_t x = 0u; x < n; ++x)
    {
        const uint32_t xp = (x + 1u) % n, xm = (x + n - 1u) % n;
        const uint32_t yp = (y + 1u) % n, ym = (y + n - 1u) % n;
        const uint32_t zp = (z + 1u) % n, zm = (z + n - 1u) % n;
        const uint32_t sx = 0x7f4a7c15u, sy = 0x6a09e667u, sz = 0xbb67ae85u;
        const auto potential = [n](uint32_t px, uint32_t py, uint32_t pz, uint32_t seed)
        {
            return cloudValueNoise(static_cast<float>(px) * 5.0f / static_cast<float>(n),
                static_cast<float>(py) * 5.0f / static_cast<float>(n),
                static_cast<float>(pz) * 5.0f / static_cast<float>(n), 5, seed);
        };
        const float dFzDy = potential(x, yp, z, sz) - potential(x, ym, z, sz);
        const float dFyDz = potential(x, y, zp, sy) - potential(x, y, zm, sy);
        const float dFxDz = potential(x, y, zp, sx) - potential(x, y, zm, sx);
        const float dFzDx = potential(xp, y, z, sz) - potential(xm, y, z, sz);
        const float dFyDx = potential(xp, y, z, sy) - potential(xm, y, z, sy);
        const float dFxDy = potential(x, yp, z, sx) - potential(x, ym, z, sx);
        float curlX = dFzDy - dFyDz;
        float curlY = dFxDz - dFzDx;
        float curlZ = dFyDx - dFxDy;
        const float length = std::sqrt(curlX * curlX + curlY * curlY + curlZ * curlZ);
        if(length > 1.0e-6f)
        {
            curlX /= length;
            curlY /= length;
            curlZ /= length;
        }
        const size_t index = (static_cast<size_t>(z) * n * n +
            static_cast<size_t>(y) * n + x) * 4u;
        curlPixels[index + 0u] = cloudNoiseByte(curlX * 0.5f + 0.5f);
        curlPixels[index + 1u] = cloudNoiseByte(curlY * 0.5f + 0.5f);
        curlPixels[index + 2u] = cloudNoiseByte(curlZ * 0.5f + 0.5f);
        curlPixels[index + 3u] = 255u;
    }

    {
        const std::string curlOverridePath =
            ResolveRTXRuntimePath("cloud_noise_curl_64.rgba8");
        if(FILE* fp = std::fopen(curlOverridePath.empty() ? "cloud_noise_curl_64.rgba8"
            : curlOverridePath.c_str(), "rb"))
        {
            std::vector<uint8_t> ref(curlTexelCount * 4u);
            const size_t got = std::fread(ref.data(), 1u, ref.size(), fp);
            std::fclose(fp);

            if(got == ref.size())
            {
                for(size_t i = 0u; i < curlTexelCount; ++i)
                {
                    curlPixels[i * 4u + 0u] = ref[i * 4u + 0u];
                    curlPixels[i * 4u + 1u] = ref[i * 4u + 1u];
                    curlPixels[i * 4u + 2u] = ref[i * 4u + 2u];
                }

                std::fprintf(stderr,
                    "[CLOUD NOISE] curl RGB <- run/cloud_noise_curl_64.rgba8 (%zu texels)\n",
                    curlTexelCount);
                std::fflush(stderr);
            }
        }
    }

    const auto buildMipChain = [](const std::vector<uint8_t>& base, uint32_t extent,
        std::vector<size_t>& levelOffsets, std::vector<size_t>& levelSizes)
    {
        std::vector<uint8_t> chain;
        std::vector<uint8_t> current = base;
        for(uint32_t dim = extent;;)
        {
            const size_t bytes = static_cast<size_t>(dim) * dim * dim * 4u;
            levelOffsets.push_back(chain.size());
            levelSizes.push_back(bytes);
            chain.insert(chain.end(), current.begin(), current.end());
            if(dim <= 1u)
            {
                break;
            }
            const uint32_t next = dim / 2u;
            std::vector<uint8_t> reduced(static_cast<size_t>(next) * next * next * 4u);
            for(uint32_t z = 0u; z < next; ++z)
            for(uint32_t y = 0u; y < next; ++y)
            for(uint32_t x = 0u; x < next; ++x)
            {
                uint32_t accumulator[4] = {0u, 0u, 0u, 0u};
                for(uint32_t dz = 0u; dz < 2u; ++dz)
                for(uint32_t dy = 0u; dy < 2u; ++dy)
                for(uint32_t dx = 0u; dx < 2u; ++dx)
                {
                    const size_t sourceIndex = ((static_cast<size_t>(2u * z + dz) * dim +
                        (2u * y + dy)) * dim + (2u * x + dx)) * 4u;
                    for(uint32_t channel = 0u; channel < 4u; ++channel)
                    {
                        accumulator[channel] += current[sourceIndex + channel];
                    }
                }
                const size_t destinationIndex = ((static_cast<size_t>(z) * next + y) * next + x) * 4u;
                for(uint32_t channel = 0u; channel < 4u; ++channel)
                {
                    reduced[destinationIndex + channel] =
                        static_cast<uint8_t>((accumulator[channel] + 4u) / 8u);
                }
            }
            current.swap(reduced);
            dim = next;
        }
        return chain;
    };

    std::vector<size_t> shapeMipOffsets, shapeMipSizes, curlMipOffsets, curlMipSizes;
    const std::vector<uint8_t> shapeChain = buildMipChain(
        shapePixels, CLOUD_SHAPE_NOISE_RESOLUTION, shapeMipOffsets, shapeMipSizes);
    const std::vector<uint8_t> curlChain = buildMipChain(
        curlPixels, CLOUD_CURL_NOISE_RESOLUTION, curlMipOffsets, curlMipSizes);
    const uint32_t shapeMipLevels = static_cast<uint32_t>(shapeMipSizes.size());
    const uint32_t curlMipLevels = static_cast<uint32_t>(curlMipSizes.size());

    const VkDeviceSize shapeBytes = static_cast<VkDeviceSize>(shapeChain.size());
    const VkDeviceSize curlBytes = static_cast<VkDeviceSize>(curlChain.size());
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    createBuffer(g_device, g_physicalDevice, shapeBytes + curlBytes,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuffer, stagingMemory);
    void* mapped = nullptr;
    check(vkMapMemory(g_device, stagingMemory, 0u, shapeBytes + curlBytes, 0u, &mapped),
        "cloud noise staging map failed");
    std::memcpy(mapped, shapeChain.data(), shapeChain.size());
    std::memcpy(static_cast<uint8_t*>(mapped) + shapeBytes, curlChain.data(), curlChain.size());
    vkUnmapMemory(g_device, stagingMemory);

    auto createVolume = [](uint32_t extent, uint32_t mipLevels, VkImage& image, VkDeviceMemory& memory,
                           const char* createError, const char* allocateError, const char* bindError)
    {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_3D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = {extent, extent, extent};
        info.mipLevels = mipLevels;
        info.arrayLayers = 1u;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateImage(g_device, &info, nullptr, &image), createError);
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(g_device, image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(g_device, &allocation, nullptr, &memory), allocateError);
        check(vkBindImageMemory(g_device, image, memory, 0u), bindError);
    };
    createVolume(CLOUD_SHAPE_NOISE_RESOLUTION, shapeMipLevels, g_cloudShapeNoiseImage,
        g_cloudShapeNoiseMemory, "cloud shape image create failed",
        "cloud shape image allocate failed", "cloud shape image bind failed");
    createVolume(CLOUD_CURL_NOISE_RESOLUTION, curlMipLevels, g_cloudCurlNoiseImage,
        g_cloudCurlNoiseMemory, "cloud curl image create failed",
        "cloud curl image allocate failed", "cloud curl image bind failed");

    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = g_rtCommandPool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1u;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(g_device, &allocation, &commandBuffer),
        "cloud noise command allocation failed");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commandBuffer, &begin), "cloud noise command begin failed");

    VkImageMemoryBarrier barriers[2]{};
    for(uint32_t i = 0u; i < 2u; ++i)
    {
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].image = i == 0u ? g_cloudShapeNoiseImage : g_cloudCurlNoiseImage;
        barriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[i].subresourceRange.levelCount = i == 0u ? shapeMipLevels : curlMipLevels;
        barriers[i].subresourceRange.layerCount = 1u;
        barriers[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 2u, barriers);

    std::vector<VkBufferImageCopy> copies;
    copies.reserve(shapeMipSizes.size() + curlMipSizes.size());
    for(size_t level = 0u; level < shapeMipSizes.size(); ++level)
    {
        VkBufferImageCopy copy{};
        copy.bufferOffset = shapeMipOffsets[level];
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = static_cast<uint32_t>(level);
        copy.imageSubresource.layerCount = 1u;
        const uint32_t extent = std::max(1u, CLOUD_SHAPE_NOISE_RESOLUTION >> static_cast<uint32_t>(level));
        copy.imageExtent = {extent, extent, extent};
        copies.push_back(copy);
    }
    for(size_t level = 0u; level < curlMipSizes.size(); ++level)
    {
        VkBufferImageCopy copy{};
        copy.bufferOffset = shapeBytes + curlMipOffsets[level];
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = static_cast<uint32_t>(level);
        copy.imageSubresource.layerCount = 1u;
        const uint32_t extent = std::max(1u, CLOUD_CURL_NOISE_RESOLUTION >> static_cast<uint32_t>(level));
        copy.imageExtent = {extent, extent, extent};
        copies.push_back(copy);
    }
    vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, g_cloudShapeNoiseImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, shapeMipLevels, copies.data());
    vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, g_cloudCurlNoiseImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, curlMipLevels, copies.data() + shapeMipLevels);
    for(uint32_t i = 0u; i < 2u; ++i)
    {
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0u, 0u, nullptr, 0u, nullptr, 2u, barriers);
    check(vkEndCommandBuffer(commandBuffer), "cloud noise command end failed");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &commandBuffer;
    check(vkQueueSubmit(g_queue, 1u, &submit, VK_NULL_HANDLE), "cloud noise submit failed");
    check(vkQueueWaitIdle(g_queue), "cloud noise upload wait failed");
    vkFreeCommandBuffers(g_device, g_rtCommandPool, 1u, &commandBuffer);
    vkDestroyBuffer(g_device, stagingBuffer, nullptr);
    vkFreeMemory(g_device, stagingMemory, nullptr);

    auto createViewAndSampler = [](VkImage image, uint32_t mipLevels, VkImageView& view, VkSampler& sampler,
                                   const char* viewError, const char* samplerError)
    {
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = mipLevels;
        viewInfo.subresourceRange.layerCount = 1u;
        check(vkCreateImageView(g_device, &viewInfo, nullptr, &view), viewError);
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = static_cast<float>(mipLevels - 1u);
        check(vkCreateSampler(g_device, &samplerInfo, nullptr, &sampler), samplerError);
    };
    createViewAndSampler(g_cloudShapeNoiseImage, shapeMipLevels, g_cloudShapeNoiseView,
        g_cloudShapeNoiseSampler, "cloud shape view create failed", "cloud shape sampler create failed");
    createViewAndSampler(g_cloudCurlNoiseImage, curlMipLevels, g_cloudCurlNoiseView,
        g_cloudCurlNoiseSampler, "cloud curl view create failed", "cloud curl sampler create failed");
    updateCloudNoiseRayDescriptors();
    RTLog("[CLOUD] Tyndall 128^3 shape/detail + 64^3 curl fields uploaded (mip-mapped, shape=" +
        std::to_string(shapeMipLevels) + " curl=" + std::to_string(curlMipLevels) + " levels)");
}

// -------------------------------------------------------------------------
// 体积云 v2：2D 基础覆盖噪声（binding 9）
//
// 参考 Clouds.fsh:97-100 用 noisetex 的 .g/.r 做 2D 底覆盖（两次频程、随风漂移），
// 并在 VC_LOCAL_COVERAGE 里用同一张图的 .x 当超低频局部覆盖图。Iris 的 noisetex
// 是 64² RG、REPEAT + 模糊；本引擎没有这个 uniform，这里生成一张统计等价的
// 64² RG8（.r 宽频周期 fBm / .g worley 胞元），采样方式与参考一致。
// -------------------------------------------------------------------------
void createCloudBaseNoiseResources()
{
    if(g_cloudBaseNoiseImage != VK_NULL_HANDLE)
    {
        return;
    }

    const uint32_t dim = CLOUD_BASE_NOISE_RESOLUTION;
    std::vector<uint8_t> base(static_cast<size_t>(dim) * dim * 4u);

    bool loadedReferenceNoise = false;
    const std::string baseNoisePath = ResolveRTXRuntimePath("cloud_base_noise_64.rgba8");
    if(FILE* baseNoiseFile = std::fopen(baseNoisePath.empty() ? "cloud_base_noise_64.rgba8"
        : baseNoisePath.c_str(), "rb"))
    {
        const size_t got = std::fread(base.data(), 1u, base.size(), baseNoiseFile);
        std::fclose(baseNoiseFile);
        loadedReferenceNoise = got == base.size();
        if(!loadedReferenceNoise)
        {
            std::fill(base.begin(), base.end(), 0u);
        }
    }
    if(loadedReferenceNoise)
    {
        RTLog("[CLOUD NOISE] base coverage noise = reference noisetex (64x64 RGBA8)");
    }
    else
    {
        RTLog("[CLOUD NOISE] base coverage noise = procedural fallback (reference asset missing)");
        for(uint32_t y = 0u; y < dim; ++y)
        for(uint32_t x = 0u; x < dim; ++x)
        {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(dim);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(dim);
            const float broad = 0.50f * cloudValueNoise(u * 6.0f, v * 6.0f, 0.5f, 6, 0x1f123bb5u) +
                                0.30f * cloudValueNoise(u * 13.0f, v * 13.0f, 0.5f, 13, 0x2c1b3c6du) +
                                0.20f * cloudValueNoise(u * 27.0f, v * 27.0f, 0.5f, 27, 0x4a7c15f3u);
            const float worley = cloudCellularNoise(u * 8.0f, v * 8.0f, 0.5f, 8, 0x9e3779b9u);
            const size_t index = (static_cast<size_t>(y) * dim + x) * 4u;
            base[index + 0u] = cloudNoiseByte(broad);
            base[index + 1u] = cloudNoiseByte(worley);
            base[index + 2u] = cloudNoiseByte(0.5f * (broad + worley));
            base[index + 3u] = 255u;
        }
    }

    std::vector<size_t> levelOffsets;
    std::vector<size_t> levelSizes;
    std::vector<uint8_t> chain;
    std::vector<uint8_t> current = base;
    for(uint32_t width = dim;;)
    {
        const size_t bytes = static_cast<size_t>(width) * width * 4u;
        levelOffsets.push_back(chain.size());
        levelSizes.push_back(bytes);
        chain.insert(chain.end(), current.begin(), current.end());
        if(width <= 1u)
        {
            break;
        }
        const uint32_t next = width / 2u;
        std::vector<uint8_t> reduced(static_cast<size_t>(next) * next * 4u);
        for(uint32_t y = 0u; y < next; ++y)
        for(uint32_t x = 0u; x < next; ++x)
        {
            uint32_t accumulator[4] = {0u, 0u, 0u, 0u};
            for(uint32_t dy = 0u; dy < 2u; ++dy)
            for(uint32_t dx = 0u; dx < 2u; ++dx)
            {
                const uint32_t sx = (2u * x + dx) % width;
                const uint32_t sy = (2u * y + dy) % width;
                const size_t sourceIndex = (static_cast<size_t>(sy) * width + sx) * 4u;
                for(uint32_t channel = 0u; channel < 4u; ++channel)
                {
                    accumulator[channel] += current[sourceIndex + channel];
                }
            }
            const size_t destinationIndex = (static_cast<size_t>(y) * next + x) * 4u;
            for(uint32_t channel = 0u; channel < 4u; ++channel)
            {
                reduced[destinationIndex + channel] =
                    static_cast<uint8_t>((accumulator[channel] + 2u) / 4u);
            }
        }
        current.swap(reduced);
        width = next;
    }
    const uint32_t mipLevels = static_cast<uint32_t>(levelSizes.size());

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {dim, dim, 1u};
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1u;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateImage(g_device, &imageInfo, nullptr, &g_cloudBaseNoiseImage),
        "cloud base noise image create failed");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g_device, g_cloudBaseNoiseImage, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocation, nullptr, &g_cloudBaseNoiseMemory),
        "cloud base noise image allocate failed");
    check(vkBindImageMemory(g_device, g_cloudBaseNoiseImage, g_cloudBaseNoiseMemory, 0u),
        "cloud base noise image bind failed");

    const VkDeviceSize chainBytes = static_cast<VkDeviceSize>(chain.size());
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    createBuffer(g_device, g_physicalDevice, chainBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuffer, stagingMemory);
    void* mapped = nullptr;
    check(vkMapMemory(g_device, stagingMemory, 0u, chainBytes, 0u, &mapped),
        "cloud base noise staging map failed");
    std::memcpy(mapped, chain.data(), chain.size());
    vkUnmapMemory(g_device, stagingMemory);

    VkCommandBufferAllocateInfo commandAllocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandAllocation.commandPool = g_rtCommandPool;
    commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandAllocation.commandBufferCount = 1u;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(g_device, &commandAllocation, &commandBuffer),
        "cloud base noise command allocation failed");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commandBuffer, &begin), "cloud base noise command begin failed");

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = g_cloudBaseNoiseImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = mipLevels;
    barrier.subresourceRange.layerCount = 1u;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);

    std::vector<VkBufferImageCopy> copies;
    copies.reserve(mipLevels);
    for(uint32_t level = 0u; level < mipLevels; ++level)
    {
        VkBufferImageCopy copy{};
        copy.bufferOffset = levelOffsets[level];
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = level;
        copy.imageSubresource.layerCount = 1u;
        const uint32_t extent = std::max(1u, dim >> level);
        copy.imageExtent = {extent, extent, 1u};
        copies.push_back(copy);
    }
    vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, g_cloudBaseNoiseImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mipLevels, copies.data());
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
    check(vkEndCommandBuffer(commandBuffer), "cloud base noise command end failed");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &commandBuffer;
    check(vkQueueSubmit(g_queue, 1u, &submit, VK_NULL_HANDLE), "cloud base noise submit failed");
    check(vkQueueWaitIdle(g_queue), "cloud base noise upload wait failed");
    vkFreeCommandBuffers(g_device, g_rtCommandPool, 1u, &commandBuffer);
    vkDestroyBuffer(g_device, stagingBuffer, nullptr);
    vkFreeMemory(g_device, stagingMemory, nullptr);

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = g_cloudBaseNoiseImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.layerCount = 1u;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_cloudBaseNoiseView),
        "cloud base noise view create failed");

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = static_cast<float>(mipLevels - 1u);
    check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_cloudBaseNoiseSampler),
        "cloud base noise sampler create failed");

    RTLog("[CLOUD NOISE] 64x64 base coverage noise uploaded (" +
        std::to_string(mipLevels) + " levels)");
}
void destroyCloudBaseNoiseResources()
{
    if(g_cloudBaseNoiseSampler) vkDestroySampler(g_device, g_cloudBaseNoiseSampler, nullptr);
    if(g_cloudBaseNoiseView) vkDestroyImageView(g_device, g_cloudBaseNoiseView, nullptr);
    if(g_cloudBaseNoiseImage) vkDestroyImage(g_device, g_cloudBaseNoiseImage, nullptr);
    if(g_cloudBaseNoiseMemory) vkFreeMemory(g_device, g_cloudBaseNoiseMemory, nullptr);
    g_cloudBaseNoiseSampler = VK_NULL_HANDLE;
    g_cloudBaseNoiseView = VK_NULL_HANDLE;
    g_cloudBaseNoiseImage = VK_NULL_HANDLE;
    g_cloudBaseNoiseMemory = VK_NULL_HANDLE;
}

// -------------------------------------------------------------------------
// 体积云 v3的四张资产
//
// 与 v2 的资产**不是同一套语义**：这里的 128³ 形状体是
// r = Perlin-Worley、g/b/a = 三级 Worley FBM，
// 32³ 细节体是三级 Worley（:549-554），天气图 r=覆盖度/g=云型（:520-537）。
// 因此 v3 不能复用 v2 的 3D 纹理绑定，必须走独立视图。
// -------------------------------------------------------------------------
std::vector<uint8_t> readCloudV3Asset(const char* relativePath, size_t expectedBytes)
{
    std::vector<uint8_t> data;
    const std::string resolved = ResolveRTXRuntimePath(relativePath);
    FILE* file = std::fopen(resolved.empty() ? relativePath : resolved.c_str(), "rb");
    if(!file)
    {
        return data;
    }
    data.resize(expectedBytes);
    const size_t got = std::fread(data.data(), 1u, expectedBytes, file);
    std::fclose(file);
    if(got != expectedBytes)
    {
        data.clear();
    }
    return data;
}
VkSampler createCloudV3Sampler()
{
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    VkSampler sampler = VK_NULL_HANDLE;
    check(vkCreateSampler(g_device, &samplerInfo, nullptr, &sampler),
        "cloud v3 sampler create failed");
    return sampler;
}
void uploadCloudV3Image(
    VkImageType imageType,
    uint32_t width,
    uint32_t height,
    uint32_t depth,
    const std::vector<uint8_t>& pixels,
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& view,
    VkSampler& sampler)
{
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = imageType;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {width, height, imageType == VK_IMAGE_TYPE_3D ? depth : 1u};
    imageInfo.mipLevels = 1u;
    imageInfo.arrayLayers = 1u;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateImage(g_device, &imageInfo, nullptr, &image), "cloud v3 image create failed");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g_device, image, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(g_physicalDevice, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocation, nullptr, &memory), "cloud v3 memory failed");
    check(vkBindImageMemory(g_device, image, memory, 0u), "cloud v3 bind failed");

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    createBuffer(g_device, g_physicalDevice, static_cast<VkDeviceSize>(pixels.size()),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuffer, stagingMemory);
    void* mapped = nullptr;
    check(vkMapMemory(g_device, stagingMemory, 0u, static_cast<VkDeviceSize>(pixels.size()), 0u, &mapped),
        "cloud v3 staging map failed");
    std::memcpy(mapped, pixels.data(), pixels.size());
    vkUnmapMemory(g_device, stagingMemory);

    VkCommandBufferAllocateInfo commandAllocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandAllocation.commandPool = g_rtCommandPool;
    commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandAllocation.commandBufferCount = 1u;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(g_device, &commandAllocation, &commandBuffer),
        "cloud v3 command failed");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commandBuffer, &begin), "cloud v3 begin failed");

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1u;
    barrier.subresourceRange.layerCount = 1u;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);

    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1u;
    copy.imageExtent = imageInfo.extent;
    vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);

    check(vkEndCommandBuffer(commandBuffer), "cloud v3 end failed");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &commandBuffer;
    check(vkQueueSubmit(g_queue, 1u, &submit, VK_NULL_HANDLE), "cloud v3 submit failed");
    check(vkQueueWaitIdle(g_queue), "cloud v3 upload wait failed");
    vkFreeCommandBuffers(g_device, g_rtCommandPool, 1u, &commandBuffer);
    vkDestroyBuffer(g_device, stagingBuffer, nullptr);
    vkFreeMemory(g_device, stagingMemory, nullptr);

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = imageType == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1u;
    viewInfo.subresourceRange.layerCount = 1u;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &view), "cloud v3 view failed");
    sampler = createCloudV3Sampler();
}
void createCloudV3Resources()
{
    if(g_cloudV3ShapeImage != VK_NULL_HANDLE)
    {
        return;
    }

    const std::vector<uint8_t> shape = readCloudV3Asset("cloud_v3_shape_128.rgba8",
        128u * 128u * 128u * 4u);
    const std::vector<uint8_t> detail = readCloudV3Asset("cloud_v3_detail_32.rgba8",
        32u * 32u * 32u * 4u);
    const std::vector<uint8_t> curl = readCloudV3Asset("cloud_v3_curl_128.rgba8",
        128u * 128u * 4u);
    const std::vector<uint8_t> weather = readCloudV3Asset("cloud_v3_weather_512.rgba8",
        512u * 512u * 4u);

    if(shape.empty() || detail.empty() || curl.empty() || weather.empty())
    {
        RTLog("[CLOUD V3] v3 assets missing -> v3 unavailable "
            "(run tools/make_cloud_v3_assets.py)");
        return;
    }

    uploadCloudV3Image(VK_IMAGE_TYPE_3D, 128u, 128u, 128u, shape,
        g_cloudV3ShapeImage, g_cloudV3ShapeMemory, g_cloudV3ShapeView, g_cloudV3ShapeSampler);
    uploadCloudV3Image(VK_IMAGE_TYPE_3D, 32u, 32u, 32u, detail,
        g_cloudV3DetailImage, g_cloudV3DetailMemory, g_cloudV3DetailView, g_cloudV3DetailSampler);
    uploadCloudV3Image(VK_IMAGE_TYPE_2D, 128u, 128u, 1u, curl,
        g_cloudV3CurlImage, g_cloudV3CurlMemory, g_cloudV3CurlView, g_cloudV3CurlSampler);
    uploadCloudV3Image(VK_IMAGE_TYPE_2D, 512u, 512u, 1u, weather,
        g_cloudV3WeatherImage, g_cloudV3WeatherMemory, g_cloudV3WeatherView, g_cloudV3WeatherSampler);

    RTLog("[CLOUD V3] v3 assets uploaded (128^3 shape / 32^3 detail / 128^2 curl / 512^2 weather)");
}

// -------------------------------------------------------------------------
// 体积云 v4 资源：建两张图 + 用 cloud_bevy_noise.comp 烘焙一次
// -------------------------------------------------------------------------
// 2026-10-18 诊断：C++ 的 stdout/stderr 不进 latest.log，只能写文件。
// CWD 是 run/，所以这个文件出现在 run/cloud_v4_debug.log。
void cloudV4DebugLog(const char* text)
{
    if(FILE* file = std::fopen("cloud_v4_debug.log", "a"))
    {
        std::fprintf(file, "%s\n", text);
        std::fclose(file);
    }
}

// 体积云模型选择（2026-10-25）：缺省 = v5（Schneider 重写版）；
// 环境变量 ORION_CLOUD_MODEL=ue 回退 v4 的 UE 风格实现（渲染与烘焙都回）。
// 只在进程启动时读一次；运行期切换不支持（烘焙资产不同）。
int cloudModelRequested()
{
    return 0;
}
void createCloudV4Resources()
{
    {
        static const bool skipCloudV4 = [] {
            std::ifstream marker(ResolveRTXRuntimePath("skip_cloud_v4.flag"));
            return marker.good();
        }();
        if(skipCloudV4)
        {
            cloudV4DebugLog("SKIPPED by skip_cloud_v4.flag");
            return;
        }
    }
    cloudV4DebugLog("enter createCloudV4Resources");
    if(g_cloudV4AtlasImage != VK_NULL_HANDLE)
    {
        return;
    }

    VkImageCreateInfo atlasInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    atlasInfo.imageType = VK_IMAGE_TYPE_2D;
    atlasInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    atlasInfo.extent = {CLOUD_V4_ATLAS_SIZE, CLOUD_V4_ATLAS_SIZE, 1u};
    atlasInfo.mipLevels = 1u;
    atlasInfo.arrayLayers = 1u;
    atlasInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    atlasInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    atlasInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    atlasInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateImage(g_device, &atlasInfo, nullptr, &g_cloudV4AtlasImage),
        "cloud v4 atlas create failed");

    VkImageCreateInfo worleyInfo = atlasInfo;
    worleyInfo.imageType = VK_IMAGE_TYPE_3D;
    worleyInfo.extent = {CLOUD_V4_WORLEY_SIZE, CLOUD_V4_WORLEY_SIZE, CLOUD_V4_WORLEY_SIZE};
    worleyInfo.mipLevels = CLOUD_V4_WORLEY_MIPS;
    worleyInfo.usage = atlasInfo.usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    check(vkCreateImage(g_device, &worleyInfo, nullptr, &g_cloudV4WorleyImage),
        "cloud v4 worley create failed");

    VkImageCreateInfo baseInfo = atlasInfo;
    baseInfo.imageType = VK_IMAGE_TYPE_3D;
    baseInfo.extent = {CLOUD_V4_BASE_SIZE, CLOUD_V4_BASE_SIZE, CLOUD_V4_BASE_SIZE};
    baseInfo.mipLevels = CLOUD_V4_BASE_MIPS;
    baseInfo.usage = atlasInfo.usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    check(vkCreateImage(g_device, &baseInfo, nullptr, &g_cloudV4BaseImage),
        "cloud v4 base volume create failed");

    VkImageCreateInfo shapeInfo = atlasInfo;
    shapeInfo.imageType = VK_IMAGE_TYPE_3D;
    shapeInfo.extent = {CLOUD_V4_SHAPE_SIZE, CLOUD_V4_SHAPE_SIZE, CLOUD_V4_SHAPE_SIZE};
    check(vkCreateImage(g_device, &shapeInfo, nullptr, &g_cloudV4ShapeImage),
        "cloud v4 shape volume create failed");

    auto allocate = [](VkImage image, VkDeviceMemory& memory)
    {
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(g_device, image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = findMemoryType(g_physicalDevice, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(g_device, &allocation, nullptr, &memory),
            "cloud v4 memory failed");
        check(vkBindImageMemory(g_device, image, memory, 0u), "cloud v4 bind failed");
    };
    allocate(g_cloudV4AtlasImage, g_cloudV4AtlasMemory);
    allocate(g_cloudV4WorleyImage, g_cloudV4WorleyMemory);
    allocate(g_cloudV4BaseImage, g_cloudV4BaseMemory);
    allocate(g_cloudV4ShapeImage, g_cloudV4ShapeMemory);

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1u;
    viewInfo.subresourceRange.layerCount = 1u;
    viewInfo.image = g_cloudV4AtlasImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_cloudV4AtlasView),
        "cloud v4 atlas view failed");
    viewInfo.image = g_cloudV4WorleyImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
    viewInfo.subresourceRange.levelCount = CLOUD_V4_WORLEY_MIPS;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_cloudV4WorleyView),
        "cloud v4 worley view failed");
    viewInfo.image = g_cloudV4BaseImage;
    viewInfo.subresourceRange.levelCount = CLOUD_V4_BASE_MIPS;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_cloudV4BaseView),
        "cloud v4 base volume view failed");
    viewInfo.subresourceRange.levelCount = 1u;
    viewInfo.image = g_cloudV4ShapeImage;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_cloudV4ShapeView),
        "cloud v4 shape volume view failed");
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod = 0.0f;
    check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_cloudV4AtlasSampler),
        "cloud v4 atlas sampler failed");
    samplerInfo.maxLod = static_cast<float>(CLOUD_V4_WORLEY_MIPS - 1u);
    check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_cloudV4WorleySampler),
        "cloud v4 worley sampler failed");
    check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_cloudV4BaseSampler),
        "cloud v4 base sampler failed");
    samplerInfo.maxLod = 0.0f;
    check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_cloudV4ShapeSampler),
        "cloud v4 shape sampler failed");

    VkDescriptorSetLayoutBinding bakeBindings[4]{};
    bakeBindings[0] = {0u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bakeBindings[1] = {1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bakeBindings[2] = {2u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bakeBindings[3] = {3u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo bakeLayoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    bakeLayoutInfo.bindingCount = 4u;
    bakeLayoutInfo.pBindings = bakeBindings;
    VkDescriptorSetLayout bakeSetLayout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(g_device, &bakeLayoutInfo, nullptr, &bakeSetLayout),
        "cloud v4 bake layout failed");

    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.size = 16u;
    VkPipelineLayoutCreateInfo bakePipelineLayoutInfo{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    bakePipelineLayoutInfo.setLayoutCount = 1u;
    bakePipelineLayoutInfo.pSetLayouts = &bakeSetLayout;
    bakePipelineLayoutInfo.pushConstantRangeCount = 1u;
    bakePipelineLayoutInfo.pPushConstantRanges = &range;
    VkPipelineLayout bakePipelineLayout = VK_NULL_HANDLE;
    check(vkCreatePipelineLayout(g_device, &bakePipelineLayoutInfo, nullptr, &bakePipelineLayout),
        "cloud v4 bake pipeline layout failed");

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4u};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1u;
    poolInfo.poolSizeCount = 1u;
    poolInfo.pPoolSizes = &poolSize;
    VkDescriptorPool bakePool = VK_NULL_HANDLE;
    check(vkCreateDescriptorPool(g_device, &poolInfo, nullptr, &bakePool),
        "cloud v4 bake pool failed");

    VkDescriptorSetAllocateInfo bakeAlloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    bakeAlloc.descriptorPool = bakePool;
    bakeAlloc.descriptorSetCount = 1u;
    bakeAlloc.pSetLayouts = &bakeSetLayout;
    VkDescriptorSet bakeSet = VK_NULL_HANDLE;
    check(vkAllocateDescriptorSets(g_device, &bakeAlloc, &bakeSet), "cloud v4 bake set failed");

    VkDescriptorImageInfo atlasStorage{VK_NULL_HANDLE, g_cloudV4AtlasView,
        VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo worleyStorage{VK_NULL_HANDLE, g_cloudV4WorleyView,
        VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo baseStorage{VK_NULL_HANDLE, g_cloudV4BaseView,
        VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo shapeStorage{VK_NULL_HANDLE, g_cloudV4ShapeView,
        VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[4]{};
    for(uint32_t index = 0u; index < 4u; ++index)
    {
        writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index].dstSet = bakeSet;
        writes[index].dstBinding = index;
        writes[index].descriptorCount = 1u;
        writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    writes[0].pImageInfo = &atlasStorage;
    writes[1].pImageInfo = &worleyStorage;
    writes[2].pImageInfo = &baseStorage;
    writes[3].pImageInfo = &shapeStorage;
    vkUpdateDescriptorSets(g_device, 4u, writes, 0u, nullptr);

    VkPipeline bakePipeline = VK_NULL_HANDLE;
    try
    {
        const std::vector<uint8_t> code = loadSpv("shaders/cloud_bevy_noise.comp.spv");
        VkShaderModule module = createShaderModule(g_device, code);
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = module;
        stage.pName = "main";
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = stage;
        pipelineInfo.layout = bakePipelineLayout;
        const VkResult result = vkCreateComputePipelines(g_device, getSharedPipelineCache(), 1u,
            &pipelineInfo, nullptr, &bakePipeline);
        vkDestroyShaderModule(g_device, module, nullptr);
        if(result != VK_SUCCESS)
        {
            bakePipeline = VK_NULL_HANDLE;
        }
    }
    catch(const std::exception& error)
    {
        RTLog(std::string("[CLOUD V4] bake shader unavailable (") + error.what() + ")");
        bakePipeline = VK_NULL_HANDLE;
    }

    if(bakePipeline == VK_NULL_HANDLE)
    {
        RTLog("[CLOUD V4] noise bake skipped -> v4 will sample empty textures");
        return;
    }

    VkCommandBufferAllocateInfo commandAllocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandAllocation.commandPool = g_rtCommandPool;
    commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandAllocation.commandBufferCount = 1u;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(g_device, &commandAllocation, &commandBuffer),
        "cloud v4 bake command failed");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commandBuffer, &begin), "cloud v4 bake begin failed");

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1u;
    barrier.subresourceRange.layerCount = 1u;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.image = g_cloudV4AtlasImage;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
    barrier.image = g_cloudV4WorleyImage;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
    barrier.image = g_cloudV4BaseImage;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
    barrier.image = g_cloudV4ShapeImage;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);

    struct { float mode[4]; } push{};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, bakePipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, bakePipelineLayout, 0u,
        1u, &bakeSet, 0u, nullptr);
    push.mode[0] = 0.0f;
    vkCmdPushConstants(commandBuffer, bakePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0u, 16u,
        &push);
    vkCmdDispatch(commandBuffer, CLOUD_V4_ATLAS_SIZE / 8u, CLOUD_V4_ATLAS_SIZE / 8u, 1u);
    push.mode[0] = 1.0f;
    vkCmdPushConstants(commandBuffer, bakePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0u, 16u,
        &push);
    vkCmdDispatch(commandBuffer, CLOUD_V4_WORLEY_SIZE / 8u, CLOUD_V4_WORLEY_SIZE / 8u,
        CLOUD_V4_WORLEY_SIZE);
    push.mode[0] = 2.0f;
    vkCmdPushConstants(commandBuffer, bakePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0u, 16u,
        &push);
    vkCmdDispatch(commandBuffer, CLOUD_V4_BASE_SIZE / 8u, CLOUD_V4_BASE_SIZE / 8u,
        CLOUD_V4_BASE_SIZE);
    push.mode[0] = 3.0f;
    vkCmdPushConstants(commandBuffer, bakePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0u, 16u,
        &push);
    vkCmdDispatch(commandBuffer, CLOUD_V4_SHAPE_SIZE / 8u, CLOUD_V4_SHAPE_SIZE / 8u,
        CLOUD_V4_SHAPE_SIZE);

    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.image = g_cloudV4AtlasImage;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
    barrier.image = g_cloudV4WorleyImage;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
    barrier.image = g_cloudV4BaseImage;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
    barrier.image = g_cloudV4ShapeImage;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);

    {
        VkImageMemoryBarrier toGeneral{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toGeneral.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        toGeneral.subresourceRange.layerCount = 1u;
        toGeneral.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        const VkImage images[3] = {g_cloudV4AtlasImage, g_cloudV4WorleyImage, g_cloudV4BaseImage};
        for(const VkImage image : images)
        {
            toGeneral.image = image;
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u,
                &toGeneral);
        }
    }

    VkPipeline v5NoisePipeline = VK_NULL_HANDLE;
    const bool v5Model = cloudModelRequested() != 1;
    if(v5Model)
    {
        try
        {
            const std::vector<uint8_t> v5Code = loadSpv("shaders/cloud_v5_noise.comp.spv");
            VkShaderModule v5Module = createShaderModule(g_device, v5Code);
            VkPipelineShaderStageCreateInfo v5Stage{
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            v5Stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            v5Stage.module = v5Module;
            v5Stage.pName = "main";
            VkComputePipelineCreateInfo v5Info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            v5Info.stage = v5Stage;
            v5Info.layout = bakePipelineLayout;
            const VkResult v5Result = vkCreateComputePipelines(g_device,
                getSharedPipelineCache(), 1u, &v5Info, nullptr, &v5NoisePipeline);
            vkDestroyShaderModule(g_device, v5Module, nullptr);
            if(v5Result == VK_SUCCESS)
            {
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    v5NoisePipeline);
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    bakePipelineLayout, 0u, 1u, &bakeSet, 0u, nullptr);
                struct { float mode[4]; } v5Push{};
                v5Push.mode[0] = 0.0f;
                vkCmdPushConstants(commandBuffer, bakePipelineLayout,
                    VK_SHADER_STAGE_COMPUTE_BIT, 0u, 16u, &v5Push);
                vkCmdDispatch(commandBuffer, CLOUD_V4_ATLAS_SIZE / 8u,
                    CLOUD_V4_ATLAS_SIZE / 8u, 1u);
                v5Push.mode[0] = 1.0f;
                vkCmdPushConstants(commandBuffer, bakePipelineLayout,
                    VK_SHADER_STAGE_COMPUTE_BIT, 0u, 16u, &v5Push);
                vkCmdDispatch(commandBuffer, CLOUD_V4_WORLEY_SIZE / 8u,
                    CLOUD_V4_WORLEY_SIZE / 8u, CLOUD_V4_WORLEY_SIZE);
                v5Push.mode[0] = 2.0f;
                vkCmdPushConstants(commandBuffer, bakePipelineLayout,
                    VK_SHADER_STAGE_COMPUTE_BIT, 0u, 16u, &v5Push);
                vkCmdDispatch(commandBuffer, CLOUD_V4_BASE_SIZE / 8u,
                    CLOUD_V4_BASE_SIZE / 8u, CLOUD_V4_BASE_SIZE);
                RTLog("[CLOUD V5] assets baked: weather " +
                    std::to_string(CLOUD_V4_ATLAS_SIZE) + "^2 + detail " +
                    std::to_string(CLOUD_V4_WORLEY_SIZE) + "^3 + base " +
                    std::to_string(CLOUD_V4_BASE_SIZE) + "^3 (mips " +
                    std::to_string(CLOUD_V4_WORLEY_MIPS) + "/" +
                    std::to_string(CLOUD_V4_BASE_MIPS) + ")");
            }
            else
            {
                RTLog("[CLOUD V5] noise pipeline creation failed; v5 will sample stale assets");
            }
        }
        catch(const std::exception& error)
        {
            RTLog(std::string("[CLOUD V5] noise shader unavailable (") + error.what() + ")");
        }
    }
    {
        const VkImage mipImages[2] = {g_cloudV4WorleyImage, g_cloudV4BaseImage};
        const uint32_t mipLevels[2] = {CLOUD_V4_WORLEY_MIPS, CLOUD_V4_BASE_MIPS};
        const uint32_t mipExtents[2] = {CLOUD_V4_WORLEY_SIZE, CLOUD_V4_BASE_SIZE};
        const VkImageLayout bakeLayout = v5Model ? VK_IMAGE_LAYOUT_GENERAL
                                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        for(int volume = 0; volume < 2; ++volume)
        {
            if(mipImages[volume] == VK_NULL_HANDLE)
                continue;
            VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toTransfer.oldLayout = bakeLayout;
            toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            toTransfer.subresourceRange.levelCount = 1u;
            toTransfer.subresourceRange.layerCount = 1u;
            toTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
            toTransfer.image = mipImages[volume];
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &toTransfer);

            if(mipLevels[volume] > 1u)
            {
                VkImageMemoryBarrier restMips = toTransfer;
                restMips.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                restMips.srcAccessMask = 0u;
                restMips.subresourceRange.baseMipLevel = 1u;
                restMips.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
                vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &restMips);
            }

            for(uint32_t level = 1u; level < mipLevels[volume]; ++level)
            {
                const uint32_t srcExtent = std::max(1u, mipExtents[volume] >> (level - 1u));
                const uint32_t dstExtent = std::max(1u, mipExtents[volume] >> level);
                VkImageBlit blit{};
                blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.srcSubresource.mipLevel = level - 1u;
                blit.srcSubresource.layerCount = 1u;
                blit.srcOffsets[1] = {static_cast<int32_t>(srcExtent),
                    static_cast<int32_t>(srcExtent), static_cast<int32_t>(srcExtent)};
                blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.dstSubresource.mipLevel = level;
                blit.dstSubresource.layerCount = 1u;
                blit.dstOffsets[1] = {static_cast<int32_t>(dstExtent),
                    static_cast<int32_t>(dstExtent), static_cast<int32_t>(dstExtent)};
                vkCmdBlitImage(commandBuffer,
                    mipImages[volume], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    mipImages[volume], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1u, &blit, VK_FILTER_LINEAR);

                VkImageMemoryBarrier nextLevel{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                nextLevel.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                nextLevel.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                nextLevel.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                nextLevel.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                nextLevel.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                nextLevel.subresourceRange.baseMipLevel = level;
                nextLevel.subresourceRange.levelCount = 1u;
                nextLevel.subresourceRange.layerCount = 1u;
                nextLevel.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                nextLevel.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                nextLevel.image = mipImages[volume];
                vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u,
                    &nextLevel);
            }

            VkImageMemoryBarrier toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toRead.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            toRead.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
            toRead.subresourceRange.layerCount = 1u;
            toRead.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            toRead.image = mipImages[volume];
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &toRead);
        }
    }

    check(vkEndCommandBuffer(commandBuffer), "cloud v4 bake end failed");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &commandBuffer;
    const auto bakeStart = std::chrono::steady_clock::now();
    check(vkQueueSubmit(g_queue, 1u, &submit, VK_NULL_HANDLE), "cloud v4 bake submit failed");
    check(vkQueueWaitIdle(g_queue), "cloud v4 bake wait failed");
    {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - bakeStart).count();
        char timing[128];
        std::snprintf(timing, sizeof(timing), "bake wall time = %lld ms (TDR limit ~2000)",
            static_cast<long long>(ms));
        cloudV4DebugLog(timing);
    }
    vkFreeCommandBuffers(g_device, g_rtCommandPool, 1u, &commandBuffer);

    vkDestroyPipeline(g_device, bakePipeline, nullptr);
    vkDestroyPipelineLayout(g_device, bakePipelineLayout, nullptr);
    vkDestroyDescriptorPool(g_device, bakePool, nullptr);
    vkDestroyDescriptorSetLayout(g_device, bakeSetLayout, nullptr);

    RTLog("[CLOUD V4] bevy clouds: atlas " + std::to_string(CLOUD_V4_ATLAS_SIZE) + "^2 + worley " +
        std::to_string(CLOUD_V4_WORLEY_SIZE) + "^3 + shape " +
        std::to_string(CLOUD_V4_SHAPE_SIZE) + "^3 baked (MIT, evroon/bevy-volumetric-clouds)");

#if 0
    {
        const uint32_t probeSize = CLOUD_V4_BASE_SIZE;
        const VkDeviceSize probeBytes = static_cast<VkDeviceSize>(probeSize) * probeSize * 16u;
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = probeBytes;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer probeBuffer = VK_NULL_HANDLE;
        if(vkCreateBuffer(g_device, &bufferInfo, nullptr, &probeBuffer) == VK_SUCCESS)
        {
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(g_device, probeBuffer, &requirements);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = findMemoryType(g_physicalDevice, requirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VkDeviceMemory probeMemory = VK_NULL_HANDLE;
            if(vkAllocateMemory(g_device, &allocation, nullptr, &probeMemory) == VK_SUCCESS)
            {
                vkBindBufferMemory(g_device, probeBuffer, probeMemory, 0u);
                VkCommandBufferAllocateInfo probeAlloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                probeAlloc.commandPool = g_rtCommandPool;
                probeAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                probeAlloc.commandBufferCount = 1u;
                VkCommandBuffer probeCmd = VK_NULL_HANDLE;
                vkAllocateCommandBuffers(g_device, &probeAlloc, &probeCmd);
                VkCommandBufferBeginInfo probeBegin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                probeBegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(probeCmd, &probeBegin);
                VkImageMemoryBarrier probeBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                probeBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                probeBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                probeBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                probeBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                probeBarrier.image = g_cloudV4BaseImage;
                probeBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                probeBarrier.subresourceRange.levelCount = 1u;
                probeBarrier.subresourceRange.layerCount = 1u;
                probeBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                probeBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(probeCmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &probeBarrier);
                VkBufferImageCopy region{};
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.layerCount = 1u;
                region.imageOffset = {0, 0, static_cast<int32_t>(probeSize / 2u)};
                region.imageExtent = {probeSize, probeSize, 1u};
                vkCmdCopyImageToBuffer(probeCmd, g_cloudV4BaseImage,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, probeBuffer, 1u, &region);
                probeBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                probeBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                probeBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                probeBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(probeCmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &probeBarrier);
                vkEndCommandBuffer(probeCmd);
                VkSubmitInfo probeSubmit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                probeSubmit.commandBufferCount = 1u;
                probeSubmit.pCommandBuffers = &probeCmd;
                vkQueueSubmit(g_queue, 1u, &probeSubmit, VK_NULL_HANDLE);
                vkQueueWaitIdle(g_queue);

                void* mapped = nullptr;
                if(vkMapMemory(g_device, probeMemory, 0u, VK_WHOLE_SIZE, 0u, &mapped) == VK_SUCCESS)
                {
                    const uint16_t* halfs = static_cast<const uint16_t*>(mapped);
                    const size_t count = static_cast<size_t>(probeSize) * probeSize * 4u;
                    size_t nonZero = 0u;
                    uint16_t maximum = 0u;
                    for(size_t i = 0; i < count; ++i)
                    {
                        if(halfs[i] != 0u) ++nonZero;
                        if(halfs[i] > maximum) maximum = halfs[i];
                    }
                    char line[192];
                    std::snprintf(line, sizeof(line),
                        "base slice z=%u: nonzero=%zu/%zu (%.1f%%), maxHalf=0x%04x",
                        probeSize / 2u, nonZero, count,
                        100.0 * double(nonZero) / double(count), int(maximum));
                    cloudV4DebugLog(line);
                    vkUnmapMemory(g_device, probeMemory);
                }
                vkFreeCommandBuffers(g_device, g_rtCommandPool, 1u, &probeCmd);
                vkFreeMemory(g_device, probeMemory, nullptr);
            }
            vkDestroyBuffer(g_device, probeBuffer, nullptr);
        }
    }
#endif

    RTLog("[CLOUD V4] base " + std::to_string(CLOUD_V4_BASE_SIZE) +
        "^3 Perlin-Worley baked -> binding 9 (binding 1 untouched: shared)");
    cloudV4DebugLog(("baked base=" + std::to_string(CLOUD_V4_BASE_SIZE) +
        " worley=" + std::to_string(CLOUD_V4_WORLEY_SIZE) +
        " atlasView=" + (g_cloudV4AtlasView ? "ok" : "null") +
        " baseView=" + (g_cloudV4BaseView ? "ok" : "null")).c_str());
    cloudV4DebugLog("done createCloudV4Resources");
}
void destroyCloudV4Resources()
{
    if(g_cloudV4AtlasSampler) vkDestroySampler(g_device, g_cloudV4AtlasSampler, nullptr);
    if(g_cloudV4WorleySampler) vkDestroySampler(g_device, g_cloudV4WorleySampler, nullptr);
    if(g_cloudV4AtlasView) vkDestroyImageView(g_device, g_cloudV4AtlasView, nullptr);
    if(g_cloudV4WorleyView) vkDestroyImageView(g_device, g_cloudV4WorleyView, nullptr);
    if(g_cloudV4AtlasImage) vkDestroyImage(g_device, g_cloudV4AtlasImage, nullptr);
    if(g_cloudV4WorleyImage) vkDestroyImage(g_device, g_cloudV4WorleyImage, nullptr);
    if(g_cloudV4AtlasMemory) vkFreeMemory(g_device, g_cloudV4AtlasMemory, nullptr);
    if(g_cloudV4WorleyMemory) vkFreeMemory(g_device, g_cloudV4WorleyMemory, nullptr);
    g_cloudV4AtlasSampler = VK_NULL_HANDLE;
    g_cloudV4WorleySampler = VK_NULL_HANDLE;
    g_cloudV4AtlasView = VK_NULL_HANDLE;
    g_cloudV4WorleyView = VK_NULL_HANDLE;
    g_cloudV4AtlasImage = VK_NULL_HANDLE;
    g_cloudV4WorleyImage = VK_NULL_HANDLE;
    g_cloudV4AtlasMemory = VK_NULL_HANDLE;
    g_cloudV4WorleyMemory = VK_NULL_HANDLE;
}
void destroyCloudV3Resources()
{
    if(g_cloudV3ShapeSampler) vkDestroySampler(g_device, g_cloudV3ShapeSampler, nullptr);
    if(g_cloudV3ShapeView) vkDestroyImageView(g_device, g_cloudV3ShapeView, nullptr);
    if(g_cloudV3ShapeImage) vkDestroyImage(g_device, g_cloudV3ShapeImage, nullptr);
    if(g_cloudV3ShapeMemory) vkFreeMemory(g_device, g_cloudV3ShapeMemory, nullptr);
    if(g_cloudV3DetailSampler) vkDestroySampler(g_device, g_cloudV3DetailSampler, nullptr);
    if(g_cloudV3DetailView) vkDestroyImageView(g_device, g_cloudV3DetailView, nullptr);
    if(g_cloudV3DetailImage) vkDestroyImage(g_device, g_cloudV3DetailImage, nullptr);
    if(g_cloudV3DetailMemory) vkFreeMemory(g_device, g_cloudV3DetailMemory, nullptr);
    if(g_cloudV3CurlSampler) vkDestroySampler(g_device, g_cloudV3CurlSampler, nullptr);
    if(g_cloudV3CurlView) vkDestroyImageView(g_device, g_cloudV3CurlView, nullptr);
    if(g_cloudV3CurlImage) vkDestroyImage(g_device, g_cloudV3CurlImage, nullptr);
    if(g_cloudV3CurlMemory) vkFreeMemory(g_device, g_cloudV3CurlMemory, nullptr);
    if(g_cloudV3WeatherSampler) vkDestroySampler(g_device, g_cloudV3WeatherSampler, nullptr);
    if(g_cloudV3WeatherView) vkDestroyImageView(g_device, g_cloudV3WeatherView, nullptr);
    if(g_cloudV3WeatherImage) vkDestroyImage(g_device, g_cloudV3WeatherImage, nullptr);
    if(g_cloudV3WeatherMemory) vkFreeMemory(g_device, g_cloudV3WeatherMemory, nullptr);
    g_cloudV3ShapeSampler = VK_NULL_HANDLE;
    g_cloudV3ShapeView = VK_NULL_HANDLE;
    g_cloudV3ShapeImage = VK_NULL_HANDLE;
    g_cloudV3ShapeMemory = VK_NULL_HANDLE;
    g_cloudV3DetailSampler = VK_NULL_HANDLE;
    g_cloudV3DetailView = VK_NULL_HANDLE;
    g_cloudV3DetailImage = VK_NULL_HANDLE;
    g_cloudV3DetailMemory = VK_NULL_HANDLE;
    g_cloudV3CurlSampler = VK_NULL_HANDLE;
    g_cloudV3CurlView = VK_NULL_HANDLE;
    g_cloudV3CurlImage = VK_NULL_HANDLE;
    g_cloudV3CurlMemory = VK_NULL_HANDLE;
    g_cloudV3WeatherSampler = VK_NULL_HANDLE;
    g_cloudV3WeatherView = VK_NULL_HANDLE;
    g_cloudV3WeatherImage = VK_NULL_HANDLE;
    g_cloudV3WeatherMemory = VK_NULL_HANDLE;
}
void destroyCloudNoiseResources()
{
    if(g_cloudShapeNoiseSampler && g_cloudShapeNoiseSampler != g_cloudV4ShapeSampler)
        vkDestroySampler(g_device, g_cloudShapeNoiseSampler, nullptr);
    if(g_cloudShapeNoiseView && g_cloudShapeNoiseView != g_cloudV4ShapeView)
        vkDestroyImageView(g_device, g_cloudShapeNoiseView, nullptr);
    if(g_cloudShapeNoiseImage) vkDestroyImage(g_device, g_cloudShapeNoiseImage, nullptr);
    if(g_cloudShapeNoiseMemory) vkFreeMemory(g_device, g_cloudShapeNoiseMemory, nullptr);
    if(g_cloudCurlNoiseSampler) vkDestroySampler(g_device, g_cloudCurlNoiseSampler, nullptr);
    if(g_cloudCurlNoiseView) vkDestroyImageView(g_device, g_cloudCurlNoiseView, nullptr);
    if(g_cloudCurlNoiseImage) vkDestroyImage(g_device, g_cloudCurlNoiseImage, nullptr);
    if(g_cloudCurlNoiseMemory) vkFreeMemory(g_device, g_cloudCurlNoiseMemory, nullptr);
    g_cloudShapeNoiseSampler = VK_NULL_HANDLE;
    g_cloudShapeNoiseView = VK_NULL_HANDLE;
    g_cloudShapeNoiseImage = VK_NULL_HANDLE;
    g_cloudShapeNoiseMemory = VK_NULL_HANDLE;
    g_cloudCurlNoiseSampler = VK_NULL_HANDLE;
    g_cloudCurlNoiseView = VK_NULL_HANDLE;
    g_cloudCurlNoiseImage = VK_NULL_HANDLE;
    g_cloudCurlNoiseMemory = VK_NULL_HANDLE;
}

// 云的图与描述符集是否仍与"当前窗口尺寸"一致 —— 见 g_volumetricCloudBoundRenderWidth
// 的说明。云图尺寸相同但渲染分辨率变了时也必须重建（描述符/光柱目标跟着变）。
bool volumetricCloudBindingsMatchCurrentSizes(
    uint32_t width,
    uint32_t height)
{
    return
        g_volumetricCloudWidth == width &&
        g_volumetricCloudHeight == height &&
        g_volumetricCloudBoundRenderWidth == g_rtOutputWidth &&
        g_volumetricCloudBoundRenderHeight == g_rtOutputHeight &&
        g_volumetricCloudBoundOutputWidth == g_dlssOutputWidth &&
        g_volumetricCloudBoundOutputHeight == g_dlssOutputHeight;
}

// 2026-10-26：场景深度的 nearest + clamp 采样器。
// 体积云的 depth binding（render set 5 / composite 2）与体积光的 sceneDepth
// （light set 3）都用它。原先只在 createVolumeRestirResources 里懒建，而云的
// 描述符集在**更早**的 createVolumetricCloudResources 里就要写它 ⇒ 首帧拿到
// VK_NULL_HANDLE，被 vkGuard 挡下不写 ⇒ 深度绑定空、体积光恒为 0
// （日志里的 "binding 2/5/3 NULL sampler"）。改成"谁先要谁建"。
// 生命周期：会话级，运行期不销毁 —— destroyVolumeRestirResources 会随 froxel
// 尺寸变化跑，而云的描述符集仍持有旧句柄，销毁即悬空。
VkSampler volDepthSampler()
{
    if(g_volDepthSampler == VK_NULL_HANDLE)
    {
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = 0.0f;
        samplerInfo.compareOp = VK_COMPARE_OP_NEVER;

        check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_volDepthSampler),
            "vol depth sampler failed");
        RTLogFlushed("[VOL] depth sampler created");
    }
    return g_volDepthSampler;
}
namespace
{
    constexpr bool kCLOUD_DESCRIPTOR_GENERATION_GUARD = true;
    uint64_t g_volumetricCloudSetGeneration = 0u;
}

void createVolumetricCloudResources(uint32_t width, uint32_t height)
{
    const uint32_t cloudWidth = width;
    const uint32_t cloudHeight = height;
    if(g_volumetricCloudImage != VK_NULL_HANDLE &&
       volumetricCloudBindingsMatchCurrentSizes(cloudWidth, cloudHeight))
    {
        return;
    }
    if(g_volumetricCloudImage != VK_NULL_HANDLE)
    {
        destroyVolumetricCloudResources();
    }

    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(
        g_physicalDevice, VK_FORMAT_R16G16B16A16_SFLOAT, &formatProperties
    );
    const VkFormatFeatureFlags requiredFeatures =
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if((formatProperties.optimalTilingFeatures & requiredFeatures) != requiredFeatures)
    {
        throw std::runtime_error(
            "VK_FORMAT_R16G16B16A16_SFLOAT cannot be sampled and stored for volumetric clouds"
        );
    }
    vkGetPhysicalDeviceFormatProperties(g_physicalDevice, VK_FORMAT_R32_SFLOAT, &formatProperties);
    if((formatProperties.optimalTilingFeatures & requiredFeatures) != requiredFeatures)
    {
        throw std::runtime_error(
            "VK_FORMAT_R32_SFLOAT cannot be sampled and stored for volumetric cloud depth"
        );
    }

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imageInfo.extent = { cloudWidth, cloudHeight, 1u };
    imageInfo.mipLevels = 1u;
    imageInfo.arrayLayers = 1u;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateImage(g_device, &imageInfo, nullptr, &g_volumetricCloudImage),
        "volumetric cloud image create failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39630");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g_device, g_volumetricCloudImage, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocation, nullptr, &g_volumetricCloudMemory),
        "volumetric cloud image allocate failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39639");
    check(vkBindImageMemory(g_device, g_volumetricCloudImage,
        g_volumetricCloudMemory, 0u), "volumetric cloud image bind failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39641");

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = g_volumetricCloudImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1u;
    viewInfo.subresourceRange.layerCount = 1u;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_volumetricCloudView),
        "volumetric cloud image view create failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39651");

    check(vkCreateImage(g_device, &imageInfo, nullptr, &g_volumetricCloudHistoryImage),
        "volumetric cloud history image create failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39654");
    vkGetImageMemoryRequirements(g_device, g_volumetricCloudHistoryImage, &requirements);
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocation, nullptr, &g_volumetricCloudHistoryMemory),
        "volumetric cloud history memory allocate failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39660");
    check(vkBindImageMemory(g_device, g_volumetricCloudHistoryImage,
        g_volumetricCloudHistoryMemory, 0u), "volumetric cloud history image bind failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39662");
    viewInfo.image = g_volumetricCloudHistoryImage;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_volumetricCloudHistoryView),
        "volumetric cloud history image view create failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39665");

    check(vkCreateImage(g_device, &imageInfo, nullptr, &g_volumetricCloudResolvedImage),
        "volumetric cloud resolved image create failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39668");
    vkGetImageMemoryRequirements(g_device, g_volumetricCloudResolvedImage, &requirements);
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocation, nullptr, &g_volumetricCloudResolvedMemory),
        "volumetric cloud resolved image allocate failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39674");
    check(vkBindImageMemory(g_device, g_volumetricCloudResolvedImage,
        g_volumetricCloudResolvedMemory, 0u), "volumetric cloud resolved image bind failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39676");
    viewInfo.image = g_volumetricCloudResolvedImage;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_volumetricCloudResolvedView),
        "volumetric cloud resolved image view create failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39679");

    imageInfo.format = VK_FORMAT_R32G32_SFLOAT;
    check(vkCreateImage(g_device, &imageInfo, nullptr, &g_volumetricCloudDepthImage),
        "volumetric cloud depth image create failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39684");
    vkGetImageMemoryRequirements(g_device, g_volumetricCloudDepthImage, &requirements);
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocation, nullptr, &g_volumetricCloudDepthMemory),
        "volumetric cloud depth image allocate failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39690");
    check(vkBindImageMemory(g_device, g_volumetricCloudDepthImage,
        g_volumetricCloudDepthMemory, 0u), "volumetric cloud depth image bind failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39692");

    viewInfo.image = g_volumetricCloudDepthImage;
    viewInfo.format = VK_FORMAT_R32G32_SFLOAT;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_volumetricCloudDepthView),
        "volumetric cloud depth image view create failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39697");

    // =====================================================================
    // 3D Froxel Volumetric Fog & Light resources (decoupled from cloud res):
    //   - Density 3D     : (hazeSigmaT, shaftSigmaS, skyOpenness, 1)
    //   - Light 3D       : current frame lit froxel (inScatter.rgb, totalSigmaT)
    //   - LightHistory 3D: previous frame lit froxel for temporal reprojection
    //   - Integrated 3D  : front-to-back accumulated (scatter.rgb, transmittance)
    // =====================================================================
    const uint32_t baseFogW = g_rtOutputWidth > 0u ? g_rtOutputWidth : cloudWidth;
    const uint32_t baseFogH = g_rtOutputHeight > 0u ? g_rtOutputHeight : cloudHeight;
    const uint32_t froxelWidth = std::clamp((baseFogW + 5u) / 6u, 96u, 240u);
    const uint32_t froxelHeight = std::clamp((baseFogH + 5u) / 6u, 54u, 136u);
    g_fogFroxelWidth = froxelWidth;
    g_fogFroxelHeight = froxelHeight;
    createVolumeRestir3DImage(froxelWidth, froxelHeight, FOG_FROXEL_DEPTH,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        g_fogFroxelDensityImage, g_fogFroxelDensityMemory, g_fogFroxelDensityView);
    createVolumeRestir3DImage(froxelWidth, froxelHeight, FOG_FROXEL_DEPTH,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        g_fogFroxelShadowImage, g_fogFroxelShadowMemory, g_fogFroxelShadowView);
    createVolumeRestir3DImage(froxelWidth, froxelHeight, FOG_FROXEL_DEPTH,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        g_fogFroxelLightHistoryImage, g_fogFroxelLightHistoryMemory, g_fogFroxelLightHistoryView);
    createVolumeRestir3DImage(froxelWidth, froxelHeight, FOG_FROXEL_DEPTH,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        g_fogFroxelIntegratedImage, g_fogFroxelIntegratedMemory, g_fogFroxelIntegratedView);
    g_fogFroxelHistoryNeedsClear.store(true);

    {
        // 2026-11-16 关键修复：imageInfo 此时的 format 已被上面的深度图改成
        // R32G32_SFLOAT —— 直接复制会让雾屏幕图建错格式，而 view 声明的是
        // RGBA16F ⇒ 格式不匹配 ⇒ march 的 imageStore 写不进预期通道、apply/v5
        // 读回乱值 ⇒ "整条雾屏幕链从创建起就是坏的"（用户：关云/开云都没雾）。
        VkImageCreateInfo screenInfo = imageInfo;
        screenInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        screenInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        check(vkCreateImage(g_device, &screenInfo, nullptr,
            &g_fogScreenColorImage), "fog screen color image create failed");
        vkGetImageMemoryRequirements(g_device, g_fogScreenColorImage, &requirements);
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(g_device, &allocation, nullptr,
            &g_fogScreenColorMemory), "fog screen color allocate failed");
        check(vkBindImageMemory(g_device, g_fogScreenColorImage,
            g_fogScreenColorMemory, 0u), "fog screen color bind failed");
        viewInfo.image = g_fogScreenColorImage;
        viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        check(vkCreateImageView(g_device, &viewInfo, nullptr,
            &g_fogScreenColorView), "fog screen color view create failed");

        VkImageCreateInfo distInfo = imageInfo;
        distInfo.format = VK_FORMAT_R16G16_SFLOAT;
        distInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        check(vkCreateImage(g_device, &distInfo, nullptr,
            &g_fogScreenDistImage), "fog screen dist image create failed");
        vkGetImageMemoryRequirements(g_device, g_fogScreenDistImage, &requirements);
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(g_device, &allocation, nullptr,
            &g_fogScreenDistMemory), "fog screen dist allocate failed");
        check(vkBindImageMemory(g_device, g_fogScreenDistImage,
            g_fogScreenDistMemory, 0u), "fog screen dist bind failed");
        viewInfo.image = g_fogScreenDistImage;
        viewInfo.format = VK_FORMAT_R16G16_SFLOAT;
        check(vkCreateImageView(g_device, &viewInfo, nullptr,
            &g_fogScreenDistView), "fog screen dist view create failed");
    }

    check(vkCreateImage(g_device, &imageInfo, nullptr, &g_volumetricCloudHistoryDepthImage),
        "volumetric cloud history depth image create failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39700");
    vkGetImageMemoryRequirements(g_device, g_volumetricCloudHistoryDepthImage, &requirements);
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocation, nullptr, &g_volumetricCloudHistoryDepthMemory),
        "volumetric cloud history depth image allocate failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39706");
    check(vkBindImageMemory(g_device, g_volumetricCloudHistoryDepthImage,
        g_volumetricCloudHistoryDepthMemory, 0u), "volumetric cloud history depth image bind failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39708");

    viewInfo.image = g_volumetricCloudHistoryDepthImage;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_volumetricCloudHistoryDepthView),
        "volumetric cloud history depth image view create failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39712");

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_volumetricCloudSampler),
        "volumetric cloud sampler create failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39723");

    {
        const uint32_t godRayWidth = std::max(1u, g_rtOutputWidth);
        const uint32_t godRayHeight = std::max(1u, g_rtOutputHeight);
        VkImageCreateInfo godRayImageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        godRayImageInfo.imageType = VK_IMAGE_TYPE_2D;
        godRayImageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        godRayImageInfo.extent = { godRayWidth, godRayHeight, 1u };
        godRayImageInfo.mipLevels = 1u;
        godRayImageInfo.arrayLayers = 1u;
        godRayImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        godRayImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        godRayImageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        godRayImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkImageViewCreateInfo godRayViewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        godRayViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        godRayViewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        godRayViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        godRayViewInfo.subresourceRange.levelCount = 1u;
        godRayViewInfo.subresourceRange.layerCount = 1u;

        struct GodRayImageBind
        {
            VkImage* image;
            VkDeviceMemory* memory;
            VkImageView* view;
        };
        const GodRayImageBind godRayImages[2] = {
            { &g_godRayTargetImage, &g_godRayTargetMemory, &g_godRayTargetView },
            { &g_godRayHistoryImage, &g_godRayHistoryMemory, &g_godRayHistoryView }
        };
        for(const GodRayImageBind& bind : godRayImages)
        {
            check(vkCreateImage(g_device, &godRayImageInfo, nullptr, bind.image),
                "god ray image create failed");
                RTLogFlushed("[VOLCLOUD] passed check at L39773");
            VkMemoryRequirements godRayRequirements{};
            vkGetImageMemoryRequirements(g_device, *bind.image, &godRayRequirements);
            VkMemoryAllocateInfo godRayAllocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            godRayAllocation.allocationSize = godRayRequirements.size;
            godRayAllocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
                godRayRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            check(vkAllocateMemory(g_device, &godRayAllocation, nullptr, bind.memory),
                "god ray image allocate failed");
                RTLogFlushed("[VOLCLOUD] passed check at L39781");
            check(vkBindImageMemory(g_device, *bind.image, *bind.memory, 0u),
                "god ray image bind failed");
                RTLogFlushed("[VOLCLOUD] passed check at L39783");
            godRayViewInfo.image = *bind.image;
            check(vkCreateImageView(g_device, &godRayViewInfo, nullptr, bind.view),
                "god ray image view create failed");
                RTLogFlushed("[VOLCLOUD] passed check at L39786");
        }
        g_godRayWidth = godRayWidth;
        g_godRayHeight = godRayHeight;
        g_godRayTargetLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        g_godRayHistoryLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        g_godRayHistoryNeedsClear.store(true);
    }

    // Bindings 0..29 (30 entries total). Binding 29 = equirect sky LUT (g_skyScatteringView).
    VkDescriptorSetLayoutBinding renderBindings[30]{};
    renderBindings[0] = { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[1] = { 1u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[2] = { 2u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[3] = { 3u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[4] = { 4u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[5] = { 5u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[6] = { 6u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[7] = { 7u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[8] = { 8u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[9] = { 9u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[10] = { 10u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[11] = { 11u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[12] = { 12u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[13] = { 13u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[14] = { 14u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[15] = { 15u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[16] = { 16u, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[17] = { 17u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[18] = { 18u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[19] = { 19u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[20] = { 20u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[21] = { 21u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[22] = { 22u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[23] = { 23u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[24] = { 24u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[25] = { 25u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    // 26 = previous frame lit froxel 3D (temporal reprojection sampler)
    renderBindings[26] = { 26u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    // 27 = integrated 3D froxel LUT (Pass 3 write)
    renderBindings[27] = { 27u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    // 28 = integrated 3D froxel LUT (Pass 4 trilinear read)
    renderBindings[28] = { 28u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    // 29 = equirect sky scattering LUT (real Bruneton/Hillaire sky for cloud ambient & aerial perspective)
    renderBindings[29] = { 29u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo renderLayoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    renderLayoutInfo.bindingCount = 30u;
    renderLayoutInfo.pBindings = renderBindings;
    check(vkCreateDescriptorSetLayout(g_device, &renderLayoutInfo, nullptr,
        &g_volumetricCloudRenderSetLayout), "volumetric cloud render set layout failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39846");

    VkDescriptorSetLayoutBinding resolveBindings[6]{};
    resolveBindings[0] = { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    for(uint32_t i = 1u; i < 5u; ++i)
    {
        resolveBindings[i] = { i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    }
    resolveBindings[5] = { 5u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo resolveLayoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    resolveLayoutInfo.bindingCount = 6u;
    resolveLayoutInfo.pBindings = resolveBindings;
    check(vkCreateDescriptorSetLayout(g_device, &resolveLayoutInfo, nullptr,
        &g_volumetricCloudResolveSetLayout), "volumetric cloud resolve set layout failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39864");

    VkDescriptorSetLayoutBinding compositeBindings[7]{};
    compositeBindings[0] = { 0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    compositeBindings[1] = { 1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    compositeBindings[2] = { 2u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    compositeBindings[3] = { 3u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    compositeBindings[4] = { 4u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    compositeBindings[5] = { 5u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    compositeBindings[6] = { 6u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo compositeLayoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    compositeLayoutInfo.bindingCount = 7u;
    compositeLayoutInfo.pBindings = compositeBindings;
    check(vkCreateDescriptorSetLayout(g_device, &compositeLayoutInfo, nullptr,
        &g_volumetricCloudCompositeSetLayout), "volumetric cloud composite set layout failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39893");

    VkDescriptorSetLayoutBinding lightBindings[7]{};
    lightBindings[0] = { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    lightBindings[1] = { 1u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    lightBindings[2] = { 2u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    lightBindings[3] = { 3u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    lightBindings[4] = { 4u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    lightBindings[5] = { 8u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    lightBindings[6] = { 9u, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo lightLayoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    lightLayoutInfo.bindingCount = 7u;
    lightLayoutInfo.pBindings = lightBindings;
    check(vkCreateDescriptorSetLayout(g_device, &lightLayoutInfo, nullptr,
        &g_volumetricLightSetLayout), "volumetric light set layout failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39970");

    // Pool sized to cover all 4 sets (renderSet + resolveSet + compositeSet + lightSet)
    // with safe headroom: 2 AS, 4 UBO, 28 samplers, 12 storage images, 3 SSBO.
    VkDescriptorPoolSize poolSizes[5] = {
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 4u },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 6u },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 40u },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16u },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6u }
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 4u;
    poolInfo.poolSizeCount = 5u;
    poolInfo.pPoolSizes = poolSizes;
    check(vkCreateDescriptorPool(g_device, &poolInfo, nullptr,
        &g_volumetricCloudDescriptorPool), "volumetric cloud descriptor pool failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39999");

    {
        VkDescriptorSetLayoutBinding godRayBindings[4]{};
        godRayBindings[0] = { 0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        godRayBindings[1] = { 1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        godRayBindings[2] = { 2u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        godRayBindings[3] = { 3u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo godRayLayoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
        };
        godRayLayoutInfo.bindingCount = 4u;
        godRayLayoutInfo.pBindings = godRayBindings;
        check(vkCreateDescriptorSetLayout(g_device, &godRayLayoutInfo, nullptr,
            &g_godRayCompositeSetLayout), "god ray composite set layout failed");
            RTLogFlushed("[VOLCLOUD] passed check at L40026");

        VkDescriptorPoolSize godRayPoolSizes[3] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2u },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u }
        };
        VkDescriptorPoolCreateInfo godRayPoolInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
        };
        godRayPoolInfo.maxSets = 1u;
        godRayPoolInfo.poolSizeCount = 3u;
        godRayPoolInfo.pPoolSizes = godRayPoolSizes;
        check(vkCreateDescriptorPool(g_device, &godRayPoolInfo, nullptr,
            &g_godRayDescriptorPool), "god ray composite pool failed");
            RTLogFlushed("[VOLCLOUD] passed check at L40040");

        VkDescriptorSetAllocateInfo godRaySetAllocation{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
        };
        godRaySetAllocation.descriptorPool = g_godRayDescriptorPool;
        godRaySetAllocation.descriptorSetCount = 1u;
        godRaySetAllocation.pSetLayouts = &g_godRayCompositeSetLayout;
        check(vkAllocateDescriptorSets(g_device, &godRaySetAllocation,
            &g_godRayCompositeSet), "god ray composite set allocation failed");
            RTLogFlushed("[VOLCLOUD] passed check at L40049");

        VkDescriptorImageInfo godRayInputInfo{};
        godRayInputInfo.sampler = g_volumetricCloudSampler;
        godRayInputInfo.imageView = g_godRayTargetView;
        godRayInputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo godRaySceneInfo{};
        godRaySceneInfo.imageView = g_rtOutputView;
        godRaySceneInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        godRaySceneInfo.sampler = g_volumetricCloudSampler;
        VkDescriptorImageInfo godRayDepthInfo{};
        godRayDepthInfo.sampler = g_volumetricCloudSampler;
        godRayDepthInfo.imageView = g_dlssDepthView;
        godRayDepthInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorBufferInfo godRayCameraInfo{};
        godRayCameraInfo.buffer = g_cameraUboBuffer;
        godRayCameraInfo.range = sizeof(CameraUBO);
        VkWriteDescriptorSet godRayWrites[4]{};
        godRayWrites[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_godRayCompositeSet, 0u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &godRayInputInfo, nullptr, nullptr };
        godRayWrites[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_godRayCompositeSet, 1u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &godRaySceneInfo, nullptr, nullptr };
        godRayWrites[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_godRayCompositeSet, 2u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &godRayDepthInfo, nullptr, nullptr };
        godRayWrites[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_godRayCompositeSet, 3u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &godRayCameraInfo, nullptr };
        vkUpdateDescriptorSets(g_device, 4u, godRayWrites, 0u, nullptr);
    }

    VkDescriptorSetLayout setLayouts[4] = {
        g_volumetricCloudRenderSetLayout, g_volumetricCloudResolveSetLayout,
        g_volumetricCloudCompositeSetLayout, g_volumetricLightSetLayout
    };
    VkDescriptorSet descriptorSets[4]{};
    VkDescriptorSetAllocateInfo setAllocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setAllocation.descriptorPool = g_volumetricCloudDescriptorPool;
    setAllocation.descriptorSetCount = 4u;
    setAllocation.pSetLayouts = setLayouts;
    check(vkAllocateDescriptorSets(g_device, &setAllocation, descriptorSets),
        "volumetric cloud descriptor set allocation failed");
        RTLogFlushed("[VOLCLOUD] passed check at L40094");
    g_volumetricCloudRenderSet = descriptorSets[0];
    g_volumetricCloudResolveSet = descriptorSets[1];
    g_volumetricCloudCompositeSet = descriptorSets[2];
    g_volumetricLightSet = descriptorSets[3];
    ++g_volumetricCloudSetGeneration;

    VkDescriptorBufferInfo cameraInfo{};
    cameraInfo.buffer = g_cameraUboBuffer;
    cameraInfo.range = sizeof(CameraUBO);
    const bool cloudV3Active = volumetricCloudV3Requested() && g_cloudV3ShapeImage != VK_NULL_HANDLE;
    VkDescriptorImageInfo shapeInfo{};
    shapeInfo.sampler = cloudV3Active ? g_cloudV3ShapeSampler : g_cloudShapeNoiseSampler;
    shapeInfo.imageView = cloudV3Active ? g_cloudV3ShapeView : g_cloudShapeNoiseView;
    shapeInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo curlInfo{};
    curlInfo.sampler = cloudV3Active ? g_cloudV3DetailSampler : g_cloudCurlNoiseSampler;
    curlInfo.imageView = cloudV3Active ? g_cloudV3DetailView : g_cloudCurlNoiseView;
    curlInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo v3CurlInfo{};
    v3CurlInfo.sampler = g_cloudV3CurlView ? g_cloudV3CurlSampler : g_cloudBaseNoiseSampler;
    v3CurlInfo.imageView = g_cloudV3CurlView ? g_cloudV3CurlView : g_cloudBaseNoiseView;
    v3CurlInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo v3WeatherInfo{};
    v3WeatherInfo.sampler = g_cloudV3WeatherView ? g_cloudV3WeatherSampler : g_cloudBaseNoiseSampler;
    v3WeatherInfo.imageView = g_cloudV3WeatherView ? g_cloudV3WeatherView : g_cloudBaseNoiseView;
    v3WeatherInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if(g_cloudV4AtlasView != VK_NULL_HANDLE)
    {
        v3WeatherInfo.sampler = g_cloudV4AtlasSampler;
        v3WeatherInfo.imageView = g_cloudV4AtlasView;
    }

    VkDescriptorImageInfo cloudBaseNoiseInfo{};
    cloudBaseNoiseInfo.sampler = g_cloudBaseNoiseSampler;
    cloudBaseNoiseInfo.imageView = g_cloudBaseNoiseView;
    cloudBaseNoiseInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if(g_cloudV4BaseView != VK_NULL_HANDLE)
    {
        cloudBaseNoiseInfo.sampler = g_cloudV4BaseSampler;
        cloudBaseNoiseInfo.imageView = g_cloudV4BaseView;
        v3CurlInfo.sampler = g_cloudV4WorleySampler;
        v3CurlInfo.imageView = g_cloudV4WorleyView;
    }
    VkDescriptorImageInfo cloudRawInfo{};
    cloudRawInfo.sampler = g_volumetricCloudSampler;
    cloudRawInfo.imageView = g_volumetricCloudView;
    cloudRawInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo cloudStorageInfo{};
    cloudStorageInfo.imageView = g_volumetricCloudView;
    cloudStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo cloudResolvedInfo{};
    cloudResolvedInfo.sampler = g_volumetricCloudSampler;
    cloudResolvedInfo.imageView = g_volumetricCloudResolvedView;
    cloudResolvedInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo cloudResolvedStorageInfo{};
    cloudResolvedStorageInfo.imageView = g_volumetricCloudResolvedView;
    cloudResolvedStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo cloudHistoryInfo{};
    cloudHistoryInfo.sampler = g_volumetricCloudSampler;
    cloudHistoryInfo.imageView = g_volumetricCloudHistoryView;
    cloudHistoryInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo cloudDepthStorageInfo{};
    cloudDepthStorageInfo.imageView = g_volumetricCloudDepthView;
    cloudDepthStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo cloudDepthInfo{};
    cloudDepthInfo.sampler = g_volumetricCloudSampler;
    cloudDepthInfo.imageView = g_volumetricCloudDepthView;
    cloudDepthInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo cloudHistoryDepthInfo{};
    cloudHistoryDepthInfo.sampler = g_volumetricCloudSampler;
    cloudHistoryDepthInfo.imageView = g_volumetricCloudHistoryDepthView;
    cloudHistoryDepthInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo sceneStorageInfo{};
    sceneStorageInfo.imageView = g_rtOutputView;
    sceneStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo dlssOutputStorageInfo{};
    dlssOutputStorageInfo.imageView = g_dlssOutputView != VK_NULL_HANDLE
        ? g_dlssOutputView : g_rtOutputView;
    dlssOutputStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo depthInfo{};
    depthInfo.sampler = g_volumetricCloudSampler;
    depthInfo.imageView = g_dlssDepthView;
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    depthInfo.sampler = volDepthSampler();
    VkDescriptorBufferInfo aeStateInfo{};
    aeStateInfo.buffer = g_aeStateBuffer;
    aeStateInfo.range = VK_WHOLE_SIZE;
    VkDescriptorBufferInfo fogLightListInfo{};
    fogLightListInfo.buffer = g_localLightGpuBuffer;
    fogLightListInfo.offset = 0u;
    fogLightListInfo.range = VK_WHOLE_SIZE;
    VkDescriptorBufferInfo fogLightGridInfo{};
    fogLightGridInfo.buffer = g_localLightGridGpuBuffer;
    fogLightGridInfo.offset = 0u;
    fogLightGridInfo.range = VK_WHOLE_SIZE;
    VkDescriptorImageInfo sunShadowInfo{};
    sunShadowInfo.sampler = g_sunShadowSampler;
    sunShadowInfo.imageView = g_sunShadowView;
    sunShadowInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    const bool sunShadowBound = g_sunShadowView != VK_NULL_HANDLE &&
        g_sunShadowSampler != VK_NULL_HANDLE;
    VkDescriptorImageInfo sunShadowFarInfo{};
    sunShadowFarInfo.sampler = g_sunShadowFarSampler;
    sunShadowFarInfo.imageView = g_sunShadowFarView;
    sunShadowFarInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    const bool sunShadowFarBound = g_sunShadowFarView != VK_NULL_HANDLE &&
        g_sunShadowFarSampler != VK_NULL_HANDLE;
    VkDescriptorImageInfo terrainHeightInfo{};
    terrainHeightInfo.sampler = g_terrainHeightSampler;
    terrainHeightInfo.imageView = g_terrainHeightView;
    terrainHeightInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    const bool terrainHeightBound = g_terrainHeightView != VK_NULL_HANDLE &&
        g_terrainHeightSampler != VK_NULL_HANDLE;
    VkDescriptorImageInfo godRayHistoryInfo{};
    godRayHistoryInfo.sampler = g_volumetricCloudSampler;
    godRayHistoryInfo.imageView = g_godRayHistoryView;
    godRayHistoryInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo skyLutInfo{};
    const bool skyLutReady = (g_skyScatteringView != VK_NULL_HANDLE &&
        g_skyAtmosphereSampler != VK_NULL_HANDLE &&
        g_skyScatteringLayout == VK_IMAGE_LAYOUT_GENERAL);
    skyLutInfo.sampler = skyLutReady ? g_skyAtmosphereSampler : v3WeatherInfo.sampler;
    skyLutInfo.imageView = skyLutReady ? g_skyScatteringView : v3WeatherInfo.imageView;
    skyLutInfo.imageLayout = skyLutReady ? VK_IMAGE_LAYOUT_GENERAL
        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet writes[64]{};
    writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudRenderSet, 0u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &cameraInfo, nullptr };
    writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudRenderSet, 1u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &shapeInfo, nullptr, nullptr };
    writes[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudRenderSet, 2u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &curlInfo, nullptr, nullptr };
    writes[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudRenderSet, 3u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &cloudStorageInfo, nullptr, nullptr };
    writes[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudRenderSet, 4u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &cloudDepthStorageInfo, nullptr, nullptr };
    writes[5] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudResolveSet, 0u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &cameraInfo, nullptr };
    writes[6] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudResolveSet, 1u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cloudRawInfo, nullptr, nullptr };
    writes[7] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudResolveSet, 2u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cloudHistoryInfo, nullptr, nullptr };
    writes[8] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudResolveSet, 3u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cloudDepthInfo, nullptr, nullptr };
    writes[9] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudResolveSet, 4u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cloudHistoryDepthInfo, nullptr, nullptr };
    writes[10] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudResolveSet, 5u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &cloudResolvedStorageInfo, nullptr, nullptr };
    writes[11] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudCompositeSet, 0u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cloudResolvedInfo, nullptr, nullptr };
    writes[12] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudCompositeSet, 1u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &sceneStorageInfo, nullptr, nullptr };
    writes[13] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudCompositeSet, 2u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo, nullptr, nullptr };
    writes[14] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudCompositeSet, 3u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &cameraInfo, nullptr };
    writes[15] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudRenderSet, 5u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo, nullptr, nullptr };
    writes[16] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricLightSet, 0u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &cameraInfo, nullptr };
    writes[17] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricLightSet, 1u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &shapeInfo, nullptr, nullptr };
    writes[18] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricLightSet, 2u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &curlInfo, nullptr, nullptr };
    writes[19] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricLightSet, 3u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo, nullptr, nullptr };
    VkDescriptorImageInfo godRayStorageInfo{};
    godRayStorageInfo.imageView = g_godRayTargetView;
    godRayStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[20] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricLightSet, 4u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &godRayStorageInfo, nullptr, nullptr };
    writes[21] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudCompositeSet, 4u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &aeStateInfo, nullptr };
    writes[22] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudCompositeSet, 5u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cloudDepthInfo, nullptr, nullptr };
    writes[23] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudCompositeSet, 6u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &dlssOutputStorageInfo, nullptr, nullptr };
    writes[24] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudRenderSet, 9u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cloudBaseNoiseInfo, nullptr, nullptr };
    writes[25] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudRenderSet, 10u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &v3CurlInfo, nullptr, nullptr };
    writes[26] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudRenderSet, 11u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &v3WeatherInfo, nullptr, nullptr };
    uint32_t writeCount = 27u;
    if(sunShadowBound)
    {
        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudRenderSet, 6u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &sunShadowInfo, nullptr, nullptr };
    }
    if(sunShadowFarBound)
    {
        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudRenderSet, 8u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &sunShadowFarInfo, nullptr, nullptr };
    }
    if(terrainHeightBound)
    {
        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudRenderSet, 7u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &terrainHeightInfo, nullptr, nullptr };
    }
    writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricLightSet, 8u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &godRayHistoryInfo, nullptr, nullptr };
    writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudRenderSet, 29u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &skyLutInfo, nullptr, nullptr };
    if(fogLightListInfo.buffer != VK_NULL_HANDLE &&
       fogLightGridInfo.buffer != VK_NULL_HANDLE)
    {
        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudRenderSet, 12u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &fogLightListInfo, nullptr };
        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudRenderSet, 13u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &fogLightGridInfo, nullptr };
        g_cloudFogLightSetBound = g_volumetricCloudRenderSet;
        g_cloudFogLightListBound = g_localLightGpuBuffer;
        g_cloudFogLightGridBound = g_localLightGridGpuBuffer;
        g_cloudFogLightDescriptorsReady.store(true);
    }
    // 2026-11-15：froxel 重构的 6 条新 binding。
    // （image info 必须声明在本函数作用域：vkUpdateDescriptorSets 在后面才调用。）
    VkDescriptorImageInfo fogDensitySamplerInfo{};
    VkDescriptorImageInfo fogDensityStorageInfo{};
    VkDescriptorImageInfo fogColorStorageInfo{};
    VkDescriptorImageInfo fogDistStorageInfo{};
    VkDescriptorImageInfo fogColorSamplerInfo{};
    VkDescriptorImageInfo fogDistSamplerInfo{};
    VkDescriptorImageInfo fogShadowStorageInfo{};
    VkDescriptorImageInfo fogShadowSamplerInfo{};
    VkDescriptorImageInfo fogLightHistorySamplerInfo{};
    VkDescriptorImageInfo fogIntegratedStorageInfo{};
    VkDescriptorImageInfo fogIntegratedSamplerInfo{};

    if(g_fogFroxelDensityView != VK_NULL_HANDLE)
    {
        fogDensitySamplerInfo.sampler = g_volumetricCloudSampler;
        fogDensitySamplerInfo.imageView = g_fogFroxelDensityView;
        fogDensitySamplerInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        fogDensityStorageInfo.imageView = g_fogFroxelDensityView;
        fogDensityStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudRenderSet, 17u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &fogDensitySamplerInfo,
            nullptr, nullptr };
        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudRenderSet, 18u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &fogDensityStorageInfo,
            nullptr, nullptr };
    }
    if(g_fogFroxelShadowView != VK_NULL_HANDLE)
    {
        fogShadowStorageInfo.imageView = g_fogFroxelShadowView;
        fogShadowStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        fogShadowSamplerInfo.sampler = g_volumetricCloudSampler;
        fogShadowSamplerInfo.imageView = g_fogFroxelShadowView;
        fogShadowSamplerInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudRenderSet, 23u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &fogShadowStorageInfo,
            nullptr, nullptr };
        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudRenderSet, 25u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &fogShadowSamplerInfo,
            nullptr, nullptr };
    }
    if(g_fogFroxelLightHistoryView != VK_NULL_HANDLE)
    {
        fogLightHistorySamplerInfo.sampler = g_volumetricCloudSampler;
        fogLightHistorySamplerInfo.imageView = g_fogFroxelLightHistoryView;
        fogLightHistorySamplerInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudRenderSet, 26u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &fogLightHistorySamplerInfo,
            nullptr, nullptr };
    }
    if(g_fogFroxelIntegratedView != VK_NULL_HANDLE)
    {
        fogIntegratedStorageInfo.imageView = g_fogFroxelIntegratedView;
        fogIntegratedStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        fogIntegratedSamplerInfo.sampler = g_volumetricCloudSampler;
        fogIntegratedSamplerInfo.imageView = g_fogFroxelIntegratedView;
        fogIntegratedSamplerInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudRenderSet, 27u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &fogIntegratedStorageInfo,
            nullptr, nullptr };
        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudRenderSet, 28u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &fogIntegratedSamplerInfo,
            nullptr, nullptr };
    }
    if(g_fogScreenColorView != VK_NULL_HANDLE && g_fogScreenDistView != VK_NULL_HANDLE)
    {
        fogColorStorageInfo.imageView = g_fogScreenColorView;
        fogColorStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        fogDistStorageInfo.imageView = g_fogScreenDistView;
        fogDistStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        fogColorSamplerInfo.sampler = g_volumetricCloudSampler;
        fogColorSamplerInfo.imageView = g_fogScreenColorView;
        fogColorSamplerInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        fogDistSamplerInfo.sampler = g_volumetricCloudSampler;
        fogDistSamplerInfo.imageView = g_fogScreenDistView;
        fogDistSamplerInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudRenderSet, 19u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &fogColorStorageInfo,
            nullptr, nullptr };
        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudRenderSet, 20u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &fogDistStorageInfo,
            nullptr, nullptr };
        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudRenderSet, 21u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &fogColorSamplerInfo,
            nullptr, nullptr };
        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudRenderSet, 22u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &fogDistSamplerInfo,
            nullptr, nullptr };
    }
    VkAccelerationStructureKHR fogTlasInitHandle = g_tlas;
    VkWriteDescriptorSetAccelerationStructureKHR fogTlasInitInfo{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR
    };
    fogTlasInitInfo.accelerationStructureCount = 1u;
    fogTlasInitInfo.pAccelerationStructures = &fogTlasInitHandle;
    if(kCLOUD_DESCRIPTOR_GENERATION_GUARD &&
       fogTlasInitHandle != VK_NULL_HANDLE &&
       writeCount < 64u)
    {
        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &fogTlasInitInfo,
            g_volumetricCloudRenderSet, 16u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, nullptr,
            nullptr, nullptr };
    }
    if(writeCount > 64u)
    {
        RTLogFlushed("[VK GUARD] cloud: writeCount overflow " +
            std::to_string(writeCount) + " > 64");
        writeCount = 64u;
    }

    {
        uint32_t safeCount = 0u;

        for(uint32_t i = 0u; i < writeCount; ++i)
        {
            VkWriteDescriptorSet w = writes[i];

            if(w.sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET ||
               w.descriptorType == 0u ||
               w.descriptorCount == 0u)
            {
                RTLogFlushed("[VK GUARD] cloud: skip uninitialized slot " + std::to_string(i));
                continue;
            }

            bool bad = false;

            if(w.pImageInfo != nullptr)
            {
                for(uint32_t k = 0u; k < w.descriptorCount; ++k)
                {
                    if(w.pImageInfo[k].imageView == VK_NULL_HANDLE)
                    {
                        RTLogFlushed("[VK GUARD] cloud: binding " + std::to_string(w.dstBinding) + " NULL imageView");
                        bad = true;
                    }

                    if((w.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                        w.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) &&
                       w.pImageInfo[k].sampler == VK_NULL_HANDLE)
                    {
                        RTLogFlushed("[VK GUARD] cloud: binding " + std::to_string(w.dstBinding) + " NULL sampler");
                        bad = true;
                    }
                }
            }

            if(bad)
            {
                continue;
            }

            writes[safeCount++] = w;
        }

        vkUpdateDescriptorSets(g_device, safeCount, writes, 0u, nullptr);
        // 2026-11-15：把实际写入条数打出来。绑定数量必须与 writeCount 一致，
        // 否则就是又被数组上限截断了（上一轮 binding 23 就是这么丢的）。
        RTLogFlushed("[VOLCLOUD] render set descriptors written: " +
            std::to_string(safeCount));
    }
    {
        VkDescriptorSetLayoutBinding atrousBindings[3]{};
        atrousBindings[0] = { 0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        atrousBindings[1] = { 1u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        atrousBindings[2] = { 2u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo atrousSetLayoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
        };
        atrousSetLayoutInfo.bindingCount = 3u;
        atrousSetLayoutInfo.pBindings = atrousBindings;
        check(vkCreateDescriptorSetLayout(g_device, &atrousSetLayoutInfo, nullptr,
            &g_volumetricCloudAtrousSetLayout), "cloud atrous set layout failed");

        VkDescriptorPoolSize atrousPoolSizes[2] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4u },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2u }
        };
        VkDescriptorPoolCreateInfo atrousPoolInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
        };
        atrousPoolInfo.maxSets = 2u;
        atrousPoolInfo.poolSizeCount = 2u;
        atrousPoolInfo.pPoolSizes = atrousPoolSizes;
        check(vkCreateDescriptorPool(g_device, &atrousPoolInfo, nullptr,
            &g_volumetricCloudAtrousDescriptorPool), "cloud atrous descriptor pool failed");

        VkDescriptorSetLayout atrousSetLayouts[2] = {
            g_volumetricCloudAtrousSetLayout, g_volumetricCloudAtrousSetLayout
        };
        VkDescriptorSet atrousSets[2]{};
        VkDescriptorSetAllocateInfo atrousSetAllocation{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
        };
        atrousSetAllocation.descriptorPool = g_volumetricCloudAtrousDescriptorPool;
        atrousSetAllocation.descriptorSetCount = 2u;
        atrousSetAllocation.pSetLayouts = atrousSetLayouts;
        check(vkAllocateDescriptorSets(g_device, &atrousSetAllocation, atrousSets),
            "cloud atrous descriptor set allocation failed");
        g_volumetricCloudAtrousSetForward = atrousSets[0];
        g_volumetricCloudAtrousSetBackward = atrousSets[1];

        VkDescriptorImageInfo atrousResolvedInfo{};
        atrousResolvedInfo.sampler = g_volumetricCloudSampler;
        atrousResolvedInfo.imageView = g_volumetricCloudResolvedView;
        atrousResolvedInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo atrousRawInfo{};
        atrousRawInfo.sampler = g_volumetricCloudSampler;
        atrousRawInfo.imageView = g_volumetricCloudView;
        atrousRawInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo atrousDepthInfo{};
        atrousDepthInfo.sampler = g_volumetricCloudSampler;
        atrousDepthInfo.imageView = g_volumetricCloudDepthView;
        atrousDepthInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo atrousResolvedStorageInfo{};
        atrousResolvedStorageInfo.imageView = g_volumetricCloudResolvedView;
        atrousResolvedStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo atrousRawStorageInfo{};
        atrousRawStorageInfo.imageView = g_volumetricCloudView;
        atrousRawStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet atrousWrites[6]{};
        atrousWrites[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudAtrousSetForward, 0u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &atrousResolvedInfo, nullptr, nullptr };
        atrousWrites[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudAtrousSetForward, 1u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &atrousDepthInfo, nullptr, nullptr };
        atrousWrites[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudAtrousSetForward, 2u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &atrousRawStorageInfo, nullptr, nullptr };
        atrousWrites[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudAtrousSetBackward, 0u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &atrousRawInfo, nullptr, nullptr };
        atrousWrites[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudAtrousSetBackward, 1u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &atrousDepthInfo, nullptr, nullptr };
        atrousWrites[5] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudAtrousSetBackward, 2u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &atrousResolvedStorageInfo, nullptr, nullptr };
        vkUpdateDescriptorSets(g_device, 6u, atrousWrites, 0u, nullptr);
    }
    g_volumetricCloudWidth = cloudWidth;
    g_volumetricCloudHeight = cloudHeight;
    g_volumetricCloudBoundRenderWidth = g_rtOutputWidth;
    g_volumetricCloudBoundRenderHeight = g_rtOutputHeight;
    g_volumetricCloudBoundOutputWidth = g_dlssOutputWidth;
    g_volumetricCloudBoundOutputHeight = g_dlssOutputHeight;
    g_volumetricCloudLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_volumetricCloudHistoryLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_volumetricCloudResolvedLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_volumetricCloudDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_volumetricCloudHistoryDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_volumetricCloudHistoryNeedsClear.store(true);
    g_cloudFogLightSetBound = VK_NULL_HANDLE;
    g_cloudFogLightListBound = VK_NULL_HANDLE;
    g_cloudFogLightGridBound = VK_NULL_HANDLE;
    g_cloudFogLightDescriptorsReady.store(false);
    ++g_rrGuideViewGeneration;
}
void createVolumetricCloudPipeline(uint32_t width, uint32_t height)
{
    const uint32_t cloudWidth = width;
    const uint32_t cloudHeight = height;
    if(g_volumetricCloudRenderPipelineLayout != VK_NULL_HANDLE &&
       volumetricCloudBindingsMatchCurrentSizes(cloudWidth, cloudHeight))
    {
        return;
    }
    createSunShadowResources();
    createSunShadowFarResources();
    createTerrainHeightResources();
    createRRGuideRelaxResources();
    createVolumetricCloudResources(width, height);
    const std::vector<uint8_t> resolveCode = loadSpv("shaders/volumetric_cloud_resolve.comp.spv");
    const std::vector<uint8_t> compositeCode = loadSpv("shaders/volumetric_cloud_composite.comp.spv");
    const std::vector<uint8_t> lightCode = loadSpv("shaders/volumetric_light.comp.spv");
    VkShaderModule resolveShader = createShaderModule(g_device, resolveCode);
    VkShaderModule compositeShader = createShaderModule(g_device, compositeCode);
    VkShaderModule lightShader = createShaderModule(g_device, lightCode);
    try
    {
        VkPipelineLayoutCreateInfo renderLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        renderLayoutInfo.setLayoutCount = 1u;
        renderLayoutInfo.pSetLayouts = &g_volumetricCloudRenderSetLayout;
        check(vkCreatePipelineLayout(g_device, &renderLayoutInfo, nullptr,
            &g_volumetricCloudRenderPipelineLayout), "volumetric cloud render pipeline layout failed");
        VkPipelineLayoutCreateInfo resolveLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        resolveLayoutInfo.setLayoutCount = 1u;
        resolveLayoutInfo.pSetLayouts = &g_volumetricCloudResolveSetLayout;
        check(vkCreatePipelineLayout(g_device, &resolveLayoutInfo, nullptr,
            &g_volumetricCloudResolvePipelineLayout), "volumetric cloud resolve pipeline layout failed");
        VkPipelineLayoutCreateInfo compositeLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        compositeLayoutInfo.setLayoutCount = 1u;
        compositeLayoutInfo.pSetLayouts = &g_volumetricCloudCompositeSetLayout;
        VkPushConstantRange compositePushRange{};
        compositePushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        compositePushRange.offset = 0u;
        compositePushRange.size = sizeof(CloudCompositePush);
        compositeLayoutInfo.pushConstantRangeCount = 1u;
        compositeLayoutInfo.pPushConstantRanges = &compositePushRange;
        check(vkCreatePipelineLayout(g_device, &compositeLayoutInfo, nullptr,
            &g_volumetricCloudCompositePipelineLayout), "volumetric cloud composite pipeline layout failed");
        VkPipelineLayoutCreateInfo lightLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        lightLayoutInfo.setLayoutCount = 1u;
        lightLayoutInfo.pSetLayouts = &g_volumetricLightSetLayout;
        check(vkCreatePipelineLayout(g_device, &lightLayoutInfo, nullptr,
            &g_volumetricLightPipelineLayout), "volumetric light pipeline layout failed");
        VkPipelineShaderStageCreateInfo renderStage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
        };
        renderStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        renderStage.module = resolveShader;
        renderStage.pName = "main";
        if(g_cloudV4AtlasView != VK_NULL_HANDLE &&
           g_volumetricCloudRenderPipelineV3 == VK_NULL_HANDLE)
        {
            try
            {
                const std::vector<uint8_t> renderCodeV3 = loadSpv(
                    "shaders/volumetric_clouds_v5.comp.spv");
                VkShaderModule renderShaderV3 = createShaderModule(g_device, renderCodeV3);
                VkPipelineShaderStageCreateInfo renderStageV3{
                    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
                };
                renderStageV3.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                renderStageV3.module = renderShaderV3;
                renderStageV3.pName = "main";
                VkComputePipelineCreateInfo renderPipelineInfoV3{
                    VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
                };
                renderPipelineInfoV3.stage = renderStageV3;
                renderPipelineInfoV3.layout = g_volumetricCloudRenderPipelineLayout;
                const VkResult v3Result = vkCreateComputePipelines(g_device,
                    getSharedPipelineCache(), 1u, &renderPipelineInfoV3, nullptr,
                    &g_volumetricCloudRenderPipelineV3);
                vkDestroyShaderModule(g_device, renderShaderV3, nullptr);
                if(v3Result != VK_SUCCESS)
                {
                    g_volumetricCloudRenderPipelineV3 = VK_NULL_HANDLE;
                    RTLog("[CLOUD V5] pipeline creation failed (" +
                        std::to_string(static_cast<int>(v3Result)) +
                        "), no cloud render pass this session");
                }
                else
                {
                    RTLog("[CLOUD V5] v5 cloud pipeline created");
                }
            }
            catch(const std::exception& error)
            {
                g_volumetricCloudRenderPipelineV3 = VK_NULL_HANDLE;
                RTLog(std::string("[CLOUD V5] shader unavailable (") + error.what() +
                    "), no cloud render pass this session");
            }
        }
        // =====================================================================
        // 2026-11-15：体积雾 froxel 重构的两个 compute pass。
        // 失败时保持句柄为 NULL ⇒ recordFogFroxelPasses() 直接跳过 ⇒
        // v5 采样的两张屏幕图未写入（雾消失）——开发期可接受，日志会说明原因。
        // =====================================================================
        if(g_fogFroxelDensityPipeline == VK_NULL_HANDLE)
        {
            try
            {
                const std::vector<uint8_t> densityCode = loadSpv(
                    "shaders/fog_froxel_density.comp.spv");
                VkShaderModule densityShader = createShaderModule(g_device, densityCode);

                VkPushConstantRange froxelPush{};
                froxelPush.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                froxelPush.offset = 0u;
                froxelPush.size = 16u;
                VkPipelineLayoutCreateInfo froxelLayoutInfo{
                    VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
                };
                froxelLayoutInfo.setLayoutCount = 1u;
                froxelLayoutInfo.pSetLayouts = &g_volumetricCloudRenderSetLayout;
                froxelLayoutInfo.pushConstantRangeCount = 1u;
                froxelLayoutInfo.pPushConstantRanges = &froxelPush;
                check(vkCreatePipelineLayout(g_device, &froxelLayoutInfo, nullptr,
                    &g_fogFroxelDensityPipelineLayout),
                    "fog froxel density pipeline layout failed");

                VkPipelineShaderStageCreateInfo froxelStage{
                    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
                };
                froxelStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                froxelStage.module = densityShader;
                froxelStage.pName = "main";
                VkComputePipelineCreateInfo froxelPipelineInfo{
                    VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
                };
                froxelPipelineInfo.stage = froxelStage;
                froxelPipelineInfo.layout = g_fogFroxelDensityPipelineLayout;
                const VkResult densityResult = vkCreateComputePipelines(g_device,
                    getSharedPipelineCache(), 1u, &froxelPipelineInfo, nullptr,
                    &g_fogFroxelDensityPipeline);
                vkDestroyShaderModule(g_device, densityShader, nullptr);
                if(densityResult == VK_SUCCESS)
                {
                    RTLog("[FOG FROXEL] density pass created");
                }
                else
                {
                    RTLog("[FOG FROXEL] density pass creation failed (" +
                        std::to_string(static_cast<int>(densityResult)) + ")");
                }
            }
            catch(const std::exception& error)
            {
                RTLog(std::string("[FOG FROXEL] density pass unavailable (") +
                    error.what() + ")");
            }
        }
        if(g_fogFroxelLightPipeline == VK_NULL_HANDLE)
        {
            try
            {
                const std::vector<uint8_t> lightCode = loadSpv(
                    "shaders/fog_froxel_light.comp.spv");
                VkShaderModule fogLightShader = createShaderModule(g_device, lightCode);

                VkPushConstantRange froxelPush{};
                froxelPush.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                froxelPush.offset = 0u;
                froxelPush.size = 16u;
                VkPipelineLayoutCreateInfo froxelLayoutInfo{
                    VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
                };
                froxelLayoutInfo.setLayoutCount = 1u;
                froxelLayoutInfo.pSetLayouts = &g_volumetricCloudRenderSetLayout;
                froxelLayoutInfo.pushConstantRangeCount = 1u;
                froxelLayoutInfo.pPushConstantRanges = &froxelPush;
                check(vkCreatePipelineLayout(g_device, &froxelLayoutInfo, nullptr,
                    &g_fogFroxelLightPipelineLayout),
                    "fog froxel light pipeline layout failed");

                VkPipelineShaderStageCreateInfo froxelStage{
                    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
                };
                froxelStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                froxelStage.module = fogLightShader;
                froxelStage.pName = "main";
                VkComputePipelineCreateInfo froxelPipelineInfo{
                    VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
                };
                froxelPipelineInfo.stage = froxelStage;
                froxelPipelineInfo.layout = g_fogFroxelLightPipelineLayout;
                const VkResult lightResult = vkCreateComputePipelines(g_device,
                    getSharedPipelineCache(), 1u, &froxelPipelineInfo, nullptr,
                    &g_fogFroxelLightPipeline);
                vkDestroyShaderModule(g_device, fogLightShader, nullptr);
                if(lightResult == VK_SUCCESS)
                {
                    RTLog("[FOG FROXEL] light pass created");
                }
                else
                {
                    RTLog("[FOG FROXEL] light pass creation failed (" +
                        std::to_string(static_cast<int>(lightResult)) + ")");
                }
            }
            catch(const std::exception& error)
            {
                RTLog(std::string("[FOG FROXEL] light pass unavailable (") +
                    error.what() + ")");
            }
        }
        if(g_fogScreenApplyPipeline == VK_NULL_HANDLE)
        {
            try
            {
                const std::vector<uint8_t> applyCode = loadSpv(
                    "shaders/fog_screen_apply.comp.spv");
                VkShaderModule applyShader = createShaderModule(g_device, applyCode);

                VkPushConstantRange applyPush{};
                applyPush.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                applyPush.offset = 0u;
                applyPush.size = 16u;
                VkPipelineLayoutCreateInfo applyLayoutInfo{
                    VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
                };
                applyLayoutInfo.setLayoutCount = 1u;
                applyLayoutInfo.pSetLayouts = &g_volumetricCloudRenderSetLayout;
                applyLayoutInfo.pushConstantRangeCount = 1u;
                applyLayoutInfo.pPushConstantRanges = &applyPush;
                check(vkCreatePipelineLayout(g_device, &applyLayoutInfo, nullptr,
                    &g_fogScreenApplyPipelineLayout),
                    "fog screen apply pipeline layout failed");

                VkPipelineShaderStageCreateInfo applyStage{
                    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
                };
                applyStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                applyStage.module = applyShader;
                applyStage.pName = "main";
                VkComputePipelineCreateInfo applyPipelineInfo{
                    VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
                };
                applyPipelineInfo.stage = applyStage;
                applyPipelineInfo.layout = g_fogScreenApplyPipelineLayout;
                const VkResult applyResult = vkCreateComputePipelines(g_device,
                    getSharedPipelineCache(), 1u, &applyPipelineInfo, nullptr,
                    &g_fogScreenApplyPipeline);
                vkDestroyShaderModule(g_device, applyShader, nullptr);
                if(applyResult == VK_SUCCESS)
                {
                    RTLog("[FOG FROXEL] apply pass created");
                }
                else
                {
                    RTLog("[FOG FROXEL] apply pass creation failed (" +
                        std::to_string(static_cast<int>(applyResult)) + ")");
                }
            }
            catch(const std::exception& error)
            {
                RTLog(std::string("[FOG FROXEL] apply pass unavailable (") +
                    error.what() + ")");
            }
        }
        if(g_fogScreenMarchPipeline == VK_NULL_HANDLE)
        {
            try
            {
                const std::vector<uint8_t> marchCode = loadSpv(
                    "shaders/fog_screen_march.comp.spv");
                VkShaderModule marchShader = createShaderModule(g_device, marchCode);

                VkPipelineLayoutCreateInfo marchLayoutInfo{
                    VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
                };
                marchLayoutInfo.setLayoutCount = 1u;
                marchLayoutInfo.pSetLayouts = &g_volumetricCloudRenderSetLayout;
                check(vkCreatePipelineLayout(g_device, &marchLayoutInfo, nullptr,
                    &g_fogScreenMarchPipelineLayout),
                    "fog screen march pipeline layout failed");

                VkPipelineShaderStageCreateInfo marchStage{
                    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
                };
                marchStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                marchStage.module = marchShader;
                marchStage.pName = "main";
                VkComputePipelineCreateInfo marchPipelineInfo{
                    VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
                };
                marchPipelineInfo.stage = marchStage;
                marchPipelineInfo.layout = g_fogScreenMarchPipelineLayout;
                const VkResult marchResult = vkCreateComputePipelines(g_device,
                    getSharedPipelineCache(), 1u, &marchPipelineInfo, nullptr,
                    &g_fogScreenMarchPipeline);
                vkDestroyShaderModule(g_device, marchShader, nullptr);
                if(marchResult == VK_SUCCESS)
                {
                    RTLog("[FOG FROXEL] screen march pass created");
                }
                else
                {
                    RTLog("[FOG FROXEL] screen march pass creation failed (" +
                        std::to_string(static_cast<int>(marchResult)) + ")");
                }
            }
            catch(const std::exception& error)
            {
                RTLog(std::string("[FOG FROXEL] screen march pass unavailable (") +
                    error.what() + ")");
            }
        }
        if(g_fogFroxelCompositePipeline == VK_NULL_HANDLE)
        {
            try
            {
                const std::vector<uint8_t> compositeCode = loadSpv(
                    "shaders/fog_froxel_composite.comp.spv");
                VkShaderModule froxelCompositeShader =
                    createShaderModule(g_device, compositeCode);

                VkPushConstantRange froxelPush{};
                froxelPush.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                froxelPush.offset = 0u;
                froxelPush.size = 16u;
                VkPipelineLayoutCreateInfo froxelLayoutInfo{
                    VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
                };
                froxelLayoutInfo.setLayoutCount = 1u;
                froxelLayoutInfo.pSetLayouts = &g_volumetricCloudRenderSetLayout;
                froxelLayoutInfo.pushConstantRangeCount = 1u;
                froxelLayoutInfo.pPushConstantRanges = &froxelPush;
                check(vkCreatePipelineLayout(g_device, &froxelLayoutInfo, nullptr,
                    &g_fogFroxelCompositePipelineLayout),
                    "fog froxel composite pipeline layout failed");

                VkPipelineShaderStageCreateInfo froxelStage{
                    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
                };
                froxelStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                froxelStage.module = froxelCompositeShader;
                froxelStage.pName = "main";
                VkComputePipelineCreateInfo froxelPipelineInfo{
                    VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
                };
                froxelPipelineInfo.stage = froxelStage;
                froxelPipelineInfo.layout = g_fogFroxelCompositePipelineLayout;
                const VkResult compositeResult = vkCreateComputePipelines(g_device,
                    getSharedPipelineCache(), 1u, &froxelPipelineInfo, nullptr,
                    &g_fogFroxelCompositePipeline);
                vkDestroyShaderModule(g_device, froxelCompositeShader, nullptr);
                if(compositeResult == VK_SUCCESS)
                {
                    RTLog("[FOG FROXEL] composite pass created");
                }
                else
                {
                    RTLog("[FOG FROXEL] composite pass creation failed (" +
                        std::to_string(static_cast<int>(compositeResult)) + ")");
                }
            }
            catch(const std::exception& error)
            {
                RTLog(std::string("[FOG FROXEL] composite pass unavailable (") +
                    error.what() + ")");
            }
        }
        VkPipelineShaderStageCreateInfo resolveStage = renderStage;
        resolveStage.module = resolveShader;
        VkComputePipelineCreateInfo resolvePipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
        };
        resolvePipelineInfo.stage = resolveStage;
        resolvePipelineInfo.layout = g_volumetricCloudResolvePipelineLayout;
        check(vkCreateComputePipelines(g_device, getSharedPipelineCache(), 1u, &resolvePipelineInfo,
            nullptr, &g_volumetricCloudResolvePipeline), "volumetric cloud resolve pipeline failed");
        VkPipelineShaderStageCreateInfo compositeStage = renderStage;
        compositeStage.module = compositeShader;
        VkComputePipelineCreateInfo compositePipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
        };
        compositePipelineInfo.stage = compositeStage;
        compositePipelineInfo.layout = g_volumetricCloudCompositePipelineLayout;
        check(vkCreateComputePipelines(g_device, getSharedPipelineCache(), 1u, &compositePipelineInfo,
            nullptr, &g_volumetricCloudCompositePipeline), "volumetric cloud composite pipeline failed");
        VkPipelineShaderStageCreateInfo lightStage = renderStage;
        lightStage.module = lightShader;
        VkComputePipelineCreateInfo lightPipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
        };
        lightPipelineInfo.stage = lightStage;
        lightPipelineInfo.layout = g_volumetricLightPipelineLayout;
        check(vkCreateComputePipelines(g_device, getSharedPipelineCache(), 1u, &lightPipelineInfo,
            nullptr, &g_volumetricLightPipeline), "volumetric light pipeline failed");

        const std::vector<uint8_t> godRayCompositeCode =
            loadSpv("shaders/god_ray_composite.comp.spv");
        VkShaderModule godRayCompositeShader =
            createShaderModule(g_device, godRayCompositeCode);
        VkPipelineLayoutCreateInfo godRayLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        godRayLayoutInfo.setLayoutCount = 1u;
        godRayLayoutInfo.pSetLayouts = &g_godRayCompositeSetLayout;
        check(vkCreatePipelineLayout(g_device, &godRayLayoutInfo, nullptr,
            &g_godRayCompositePipelineLayout), "god ray composite pipeline layout failed");
        VkPipelineShaderStageCreateInfo godRayStage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
        };
        godRayStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        godRayStage.module = godRayCompositeShader;
        godRayStage.pName = "main";
        VkComputePipelineCreateInfo godRayPipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
        };
        godRayPipelineInfo.stage = godRayStage;
        godRayPipelineInfo.layout = g_godRayCompositePipelineLayout;
        const VkResult godRayPipelineResult = vkCreateComputePipelines(
            g_device, getSharedPipelineCache(), 1u, &godRayPipelineInfo, nullptr,
            &g_godRayCompositePipeline);
        vkDestroyShaderModule(g_device, godRayCompositeShader, nullptr);
        check(godRayPipelineResult, "god ray composite pipeline failed");

        {
            const std::vector<uint8_t> atrousCode =
                loadSpv("shaders/volumetric_cloud_atrous.comp.spv");
            VkShaderModule atrousShader = createShaderModule(g_device, atrousCode);
            VkPushConstantRange atrousPushRange{};
            atrousPushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            atrousPushRange.offset = 0u;
            atrousPushRange.size = 16u;
            VkPipelineLayoutCreateInfo atrousPipelineLayoutInfo{
                VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
            };
            atrousPipelineLayoutInfo.setLayoutCount = 1u;
            atrousPipelineLayoutInfo.pSetLayouts = &g_volumetricCloudAtrousSetLayout;
            atrousPipelineLayoutInfo.pushConstantRangeCount = 1u;
            atrousPipelineLayoutInfo.pPushConstantRanges = &atrousPushRange;
            check(vkCreatePipelineLayout(g_device, &atrousPipelineLayoutInfo, nullptr,
                &g_volumetricCloudAtrousPipelineLayout), "cloud atrous pipeline layout failed");
            VkPipelineShaderStageCreateInfo atrousStage{
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
            };
            atrousStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            atrousStage.module = atrousShader;
            atrousStage.pName = "main";
            VkComputePipelineCreateInfo atrousPipelineInfo{
                VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
            };
            atrousPipelineInfo.stage = atrousStage;
            atrousPipelineInfo.layout = g_volumetricCloudAtrousPipelineLayout;
            const VkResult atrousPipelineResult = vkCreateComputePipelines(
                g_device, getSharedPipelineCache(), 1u, &atrousPipelineInfo, nullptr,
                &g_volumetricCloudAtrousPipeline);
            vkDestroyShaderModule(g_device, atrousShader, nullptr);
            check(atrousPipelineResult, "cloud atrous pipeline failed");
        }
    }
    catch(...)
    {
            vkDestroyShaderModule(g_device, resolveShader, nullptr);
        vkDestroyShaderModule(g_device, compositeShader, nullptr);
        vkDestroyShaderModule(g_device, lightShader, nullptr);
        destroyVolumetricCloudResources();
        throw;
    }
    vkDestroyShaderModule(g_device, resolveShader, nullptr);
    vkDestroyShaderModule(g_device, compositeShader, nullptr);
    vkDestroyShaderModule(g_device, lightShader, nullptr);
    RTLog("[CLOUD] full-resolution world-anchored render, temporal resolve, and composite pipelines created");
}

// ═══════════════════════════════════════════════════════════════════════════
// 2026-10-27 云着色器**热重载**：改完 SPV 不用再冷启游戏。
//
// 为什么原来必须重启：管线只在 createVolumetricCloudPipeline() 里建一次，而那个
// 函数被 volumetricCloudBindingsMatchCurrentSizes() 挡住（尺寸不变就直接 return）。
//
// 这里的做法：每 ~20 帧 stat 一次这 4 个 SPV 的修改时间，一变就用**现有布局**
// 重建对应的 compute pipeline，并清掉云的时域历史（避免旧画面残留）。
//
// 覆盖范围：volumetric_clouds_v5 / cloud_resolve / cloud_composite / volumetric_light
// —— 也就是"改密度装配、廓形、受光、合成"这些都能即时看。
// 不覆盖：cloud_v5_noise（烘焙器）。改 hpRenderBase/Detail/Weather 这类**烘焙函数**
// 仍需要重启（3D 体积在启动时烘一次，重烘要重建 image + 描述符，风险大不值当）。
// ═══════════════════════════════════════════════════════════════════════════
uint64_t cloudSpvStampOf(const char* relative)
{
    const std::string path = ResolveRTXRuntimePath(relative);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if(!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data))
    {
        return 0ull;
    }
    return (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32)
        | static_cast<uint64_t>(data.ftLastWriteTime.dwLowDateTime);
}
uint64_t cloudSpvStampAll()
{
    // 2026-11-15：加入体积雾 froxel 的三个 pass（密度 / 光照 / 合成）。
    static const char* const kFiles[9] = {
        "shaders/volumetric_clouds_v5.comp.spv",
        "shaders/volumetric_cloud_resolve.comp.spv",
        "shaders/volumetric_cloud_composite.comp.spv",
        "shaders/volumetric_light.comp.spv",
        "shaders/fog_froxel_density.comp.spv",
        "shaders/fog_froxel_light.comp.spv",
        "shaders/fog_froxel_composite.comp.spv",
        "shaders/fog_screen_apply.comp.spv",
        "shaders/fog_screen_march.comp.spv",
    };
    uint64_t stamp = 1469598103934665603ull;
    for(int i = 0; i < 9; ++i)
    {
        stamp ^= cloudSpvStampOf(kFiles[i]);
        stamp *= 1099511628211ull;
    }
    return stamp;
}
void hotReloadVolumetricCloudPipelines()
{
    if(g_device == VK_NULL_HANDLE) return;
    if(g_volumetricCloudRenderPipelineLayout == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(g_device);

    bool any = false;
    auto replace = [&any](const char* spv, VkPipelineLayout layout,
                          VkPipeline* target, const char* tag)
    {
        // 2026-11-15：layout 为空时不能建管线（vkCreateComputePipelines 传 NULL
        // layout 会直接崩驱动）。云资源重建会把 froxel 的 layout 置 NULL 再重建，
        // 热重载若恰好撞进这个窗口就会闪退。
        if(layout == VK_NULL_HANDLE)
        {
            RTLog(std::string("[CLOUD HOT] skip ") + tag + " (layout null)");
            return;
        }
        try
        {
            const std::vector<uint8_t> code = loadSpv(spv);
            VkShaderModule module = createShaderModule(g_device, code);
            VkPipelineShaderStageCreateInfo stage{
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
            };
            stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stage.module = module;
            stage.pName = "main";
            VkComputePipelineCreateInfo info{
                VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
            };
            info.stage = stage;
            info.layout = layout;
            VkPipeline fresh = VK_NULL_HANDLE;
            const VkResult result = vkCreateComputePipelines(g_device,
                getSharedPipelineCache(), 1u, &info, nullptr, &fresh);
            vkDestroyShaderModule(g_device, module, nullptr);
            if(result == VK_SUCCESS && fresh != VK_NULL_HANDLE)
            {
                if(*target != VK_NULL_HANDLE)
                {
                    vkDestroyPipeline(g_device, *target, nullptr);
                }
                *target = fresh;
                any = true;
                RTLog(std::string("[CLOUD HOT] reloaded ") + tag);
            }
            else
            {
                RTLog(std::string("[CLOUD HOT] create failed ") + tag +
                    " (" + std::to_string(static_cast<int>(result)) + ")，保留旧管线");
            }
        }
        catch(const std::exception& error)
        {
            RTLog(std::string("[CLOUD HOT] ") + tag + " 读取失败: " + error.what());
        }
    };

    replace("shaders/volumetric_clouds_v5.comp.spv",
        g_volumetricCloudRenderPipelineLayout, &g_volumetricCloudRenderPipelineV3, "render");
    replace("shaders/volumetric_cloud_resolve.comp.spv",
        g_volumetricCloudResolvePipelineLayout, &g_volumetricCloudResolvePipeline, "resolve");
    replace("shaders/volumetric_cloud_composite.comp.spv",
        g_volumetricCloudCompositePipelineLayout, &g_volumetricCloudCompositePipeline, "composite");
    replace("shaders/volumetric_light.comp.spv",
        g_volumetricLightPipelineLayout, &g_volumetricLightPipeline, "light");
    // 2026-11-15：froxel 三件套也支持热重载（调块状/噪点/漏光时不必重启）。
    replace("shaders/fog_froxel_density.comp.spv",
        g_fogFroxelDensityPipelineLayout, &g_fogFroxelDensityPipeline, "fogfroxel-density");
    replace("shaders/fog_froxel_light.comp.spv",
        g_fogFroxelLightPipelineLayout, &g_fogFroxelLightPipeline, "fogfroxel-light");
    replace("shaders/fog_froxel_composite.comp.spv",
        g_fogFroxelCompositePipelineLayout, &g_fogFroxelCompositePipeline, "fogfroxel-composite");
    replace("shaders/fog_screen_apply.comp.spv",
        g_fogScreenApplyPipelineLayout, &g_fogScreenApplyPipeline, "fogfroxel-apply");
    replace("shaders/fog_screen_march.comp.spv",
        g_fogScreenMarchPipelineLayout, &g_fogScreenMarchPipeline, "fog-screen-march");

    if(any)
    {
        g_volumetricCloudHistoryNeedsClear.store(true);
        RTLog("[CLOUD HOT] cloud history cleared");
    }
}
void maybeHotReloadVolumetricCloudShaders()
{
    if(++g_cloudSpvWatchCountdown < 20) return;
    g_cloudSpvWatchCountdown = 0;
    const uint64_t now = cloudSpvStampAll();
    if(g_cloudSpvStampSeen == 0ull)
    {
        g_cloudSpvStampSeen = now;
        return;
    }
    if(now == g_cloudSpvStampSeen) return;
    g_cloudSpvStampSeen = now;
    hotReloadVolumetricCloudPipelines();
}
// ===========================================================================
// 2026-11-12：云影图（cloud shadow map）
//
// 供体积雾的"云隙光"查询：从云层沿太阳方向积分光学厚度，输出 exp(-τ)。
// 与主云 pass 用同一个密度函数（rt_cloud_shadow.comp 调 hpEvaluateCloudProperties），
// 所以光柱与真实云隙在位置上一致。
//
// 资源全部**懒创建**，并把结果挂到云 pass 描述符集的 binding 14 上
//（render set 可能比本 pass 晚创建，所以每次记录时检查是否需要重绑）。
// 任何一步失败都只让云隙光退化（binding 14 保持未绑定 → 雾侧回落到旧噪声），
// 不影响云/雾本身。
// ===========================================================================
namespace
{
    constexpr uint32_t CLOUD_SHADOW_RES = 256u;

    VkImage g_cloudShadowImage = VK_NULL_HANDLE;
    VkDeviceMemory g_cloudShadowMemory = VK_NULL_HANDLE;
    VkImageView g_cloudShadowView = VK_NULL_HANDLE;
    VkSampler g_cloudShadowSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout g_cloudShadowSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool g_cloudShadowPool = VK_NULL_HANDLE;
    VkDescriptorSet g_cloudShadowSet = VK_NULL_HANDLE;
    VkPipelineLayout g_cloudShadowPipelineLayout = VK_NULL_HANDLE;
    VkPipeline g_cloudShadowPipeline = VK_NULL_HANDLE;
    bool g_cloudShadowFailed = false;
    bool g_cloudShadowLayoutReady = false;
    VkDescriptorSet g_cloudShadowBoundSet = VK_NULL_HANDLE;
    uint64_t g_cloudShadowBoundGen = 0u;

    bool createCloudShadowPass()
    {
        if(g_cloudShadowPipeline != VK_NULL_HANDLE)
        {
            return true;
        }
        if(g_cloudShadowFailed || g_device == VK_NULL_HANDLE ||
           g_physicalDevice == VK_NULL_HANDLE || g_cameraUboBuffer == VK_NULL_HANDLE ||
           g_cloudBaseNoiseView == VK_NULL_HANDLE)
        {
            return false;
        }

        try
        {
            VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            imageInfo.extent = { CLOUD_SHADOW_RES, CLOUD_SHADOW_RES, 1u };
            imageInfo.mipLevels = 1u;
            imageInfo.arrayLayers = 1u;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            check(vkCreateImage(g_device, &imageInfo, nullptr, &g_cloudShadowImage),
                "cloud shadow image create failed");

            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(g_device, g_cloudShadowImage, &requirements);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
                requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            check(vkAllocateMemory(g_device, &allocation, nullptr, &g_cloudShadowMemory),
                "cloud shadow memory allocate failed");
            check(vkBindImageMemory(g_device, g_cloudShadowImage, g_cloudShadowMemory, 0u),
                "cloud shadow image bind failed");

            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = g_cloudShadowImage;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1u;
            viewInfo.subresourceRange.layerCount = 1u;
            check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_cloudShadowView),
                "cloud shadow view create failed");

            VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            samplerInfo.magFilter = VK_FILTER_LINEAR;
            samplerInfo.minFilter = VK_FILTER_LINEAR;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.maxLod = 0.0f;
            check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_cloudShadowSampler),
                "cloud shadow sampler create failed");

            VkDescriptorSetLayoutBinding bindings[5]{};
            bindings[0] = { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            for(uint32_t i = 1u; i < 4u; ++i)
            {
                bindings[i] = { i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
                    VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            }
            bindings[4] = { 4u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

            VkDescriptorSetLayoutCreateInfo layoutInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
            };
            layoutInfo.bindingCount = 5u;
            layoutInfo.pBindings = bindings;
            check(vkCreateDescriptorSetLayout(g_device, &layoutInfo, nullptr,
                &g_cloudShadowSetLayout), "cloud shadow set layout failed");

            VkDescriptorPoolSize poolSizes[3] = {
                { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u },
                { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3u },
                { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u }
            };
            VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.maxSets = 1u;
            poolInfo.poolSizeCount = 3u;
            poolInfo.pPoolSizes = poolSizes;
            check(vkCreateDescriptorPool(g_device, &poolInfo, nullptr, &g_cloudShadowPool),
                "cloud shadow pool failed");

            VkDescriptorSetAllocateInfo setAllocation{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
            };
            setAllocation.descriptorPool = g_cloudShadowPool;
            setAllocation.descriptorSetCount = 1u;
            setAllocation.pSetLayouts = &g_cloudShadowSetLayout;
            check(vkAllocateDescriptorSets(g_device, &setAllocation, &g_cloudShadowSet),
                "cloud shadow set allocate failed");

            VkDescriptorBufferInfo cameraInfo{};
            cameraInfo.buffer = g_cameraUboBuffer;
            cameraInfo.range = sizeof(CameraUBO);

            VkDescriptorImageInfo weatherInfo{};
            weatherInfo.sampler = g_cloudV4AtlasView
                ? g_cloudV4AtlasSampler : g_cloudBaseNoiseSampler;
            weatherInfo.imageView = g_cloudV4AtlasView
                ? g_cloudV4AtlasView : g_cloudBaseNoiseView;
            weatherInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo baseInfo{};
            baseInfo.sampler = g_cloudV4BaseView
                ? g_cloudV4BaseSampler : g_cloudBaseNoiseSampler;
            baseInfo.imageView = g_cloudV4BaseView
                ? g_cloudV4BaseView : g_cloudBaseNoiseView;
            baseInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo erosionInfo{};
            erosionInfo.sampler = g_cloudV4WorleyView
                ? g_cloudV4WorleySampler
                : (g_cloudV3CurlView ? g_cloudV3CurlSampler : g_cloudBaseNoiseSampler);
            erosionInfo.imageView = g_cloudV4WorleyView
                ? g_cloudV4WorleyView
                : (g_cloudV3CurlView ? g_cloudV3CurlView : g_cloudBaseNoiseView);
            erosionInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo storageInfo{};
            storageInfo.imageView = g_cloudShadowView;
            storageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSet writes[5]{};
            writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                g_cloudShadowSet, 0u, 0u, 1u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                nullptr, &cameraInfo, nullptr };
            writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                g_cloudShadowSet, 1u, 0u, 1u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                &weatherInfo, nullptr, nullptr };
            writes[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                g_cloudShadowSet, 2u, 0u, 1u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                &baseInfo, nullptr, nullptr };
            writes[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                g_cloudShadowSet, 3u, 0u, 1u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                &erosionInfo, nullptr, nullptr };
            writes[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                g_cloudShadowSet, 4u, 0u, 1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                &storageInfo, nullptr, nullptr };
            vkUpdateDescriptorSets(g_device, 5u, writes, 0u, nullptr);

            const std::vector<uint8_t> code = loadSpv("shaders/rt_cloud_shadow.comp.spv");
            VkShaderModule shader = createShaderModule(g_device, code);

            VkPipelineLayoutCreateInfo pipelineLayoutInfo{
                VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
            };
            pipelineLayoutInfo.setLayoutCount = 1u;
            pipelineLayoutInfo.pSetLayouts = &g_cloudShadowSetLayout;
            check(vkCreatePipelineLayout(g_device, &pipelineLayoutInfo, nullptr,
                &g_cloudShadowPipelineLayout), "cloud shadow pipeline layout failed");

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
            pipelineInfo.layout = g_cloudShadowPipelineLayout;
            check(vkCreateComputePipelines(g_device, getSharedPipelineCache(), 1u,
                &pipelineInfo, nullptr, &g_cloudShadowPipeline),
                "cloud shadow pipeline failed");

            vkDestroyShaderModule(g_device, shader, nullptr);

            RTLog("[CLOUD SHADOW] map ready (256x256, extent 512)");
            return true;
        }
        catch(...)
        {
            RTLog("[CLOUD SHADOW] creation failed; cloud gap light stays off");
            g_cloudShadowFailed = true;
            return false;
        }
    }

    // 记录云影图。每帧一次，256² / 8 = 32×32 组。
    void recordCloudShadowMap(VkCommandBuffer cmd)
    {
        if(cmd == VK_NULL_HANDLE || !createCloudShadowPass())
        {
            return;
        }

        // 把结果挂到云 pass 的 binding 14（render set 可能晚于本 pass 创建）
        if((g_cloudShadowBoundSet != g_volumetricCloudRenderSet ||
            (kCLOUD_DESCRIPTOR_GENERATION_GUARD &&
             g_cloudShadowBoundGen != g_volumetricCloudSetGeneration)) &&
           g_volumetricCloudRenderSet != VK_NULL_HANDLE)
        {
            VkDescriptorImageInfo info{};
            info.sampler = g_cloudShadowSampler;
            info.imageView = g_cloudShadowView;
            info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = g_volumetricCloudRenderSet;
            write.dstBinding = 14u;
            write.descriptorCount = 1u;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &info;
            vkUpdateDescriptorSets(g_device, 1u, &write, 0u, nullptr);

            g_cloudShadowBoundSet = g_volumetricCloudRenderSet;
            g_cloudShadowBoundGen = g_volumetricCloudSetGeneration;
        }

        // 2026-11-18：**恢复消费方**。雾的太阳项现在真的按这张图做云隙光
        // （fog_screen_march.comp 里按像素采样 binding 14）。云关着时 shader 会
        // 直接用 1.0（cloudParams1.w < 0.5），所以不必为"云关"清图。
        // 回退：ORION_CLOUD_SHADOW_OFF=1 跳过 dispatch（雾退回无云遮挡）。
        const bool consumerEnabled = []()
        {
            const char* raw = std::getenv("ORION_CLOUD_SHADOW_OFF");

            return !(raw != nullptr && raw[0] == '1');
        }();
        if(!consumerEnabled)
        {
            return;
        }

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = g_cloudShadowLayoutReady
            ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = g_cloudShadowImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1u;
        barrier.subresourceRange.layerCount = 1u;
        barrier.srcAccessMask = g_cloudShadowLayoutReady ? VK_ACCESS_SHADER_READ_BIT : 0u;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            g_cloudShadowLayoutReady
                ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr,
            1u, &barrier);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_cloudShadowPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            g_cloudShadowPipelineLayout, 0u, 1u, &g_cloudShadowSet, 0u, nullptr);
        vkCmdDispatch(cmd, (CLOUD_SHADOW_RES + 7u) / 8u, (CLOUD_SHADOW_RES + 7u) / 8u, 1u);

        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr,
            1u, &barrier);

        g_cloudShadowLayoutReady = true;
    }

    // =======================================================================
    // 2026-11-12：太阳可见性体积（S1）
    //
    // 与云影图同一个套路：懒创建 + 每帧 dispatch + 挂到云 pass 的 binding 15。
    // 尺寸固定 64×64×32（相机视锥对齐，深度覆盖 [near, 32] 格）。
    // 每 texel 一条朝太阳的 ray query ⇒ 近场太阳遮挡是**真值**，不再依赖
    // 阴影图的 PCF 与"无数据"判据（那是洞穴漏光的来源）。
    // =======================================================================
    constexpr uint32_t SUN_VIS_W = 64u;
    constexpr uint32_t SUN_VIS_H = 64u;
    constexpr uint32_t SUN_VIS_D = 32u;

    VkImage g_sunVisImage = VK_NULL_HANDLE;
    VkDeviceMemory g_sunVisMemory = VK_NULL_HANDLE;
    VkImageView g_sunVisView = VK_NULL_HANDLE;
    VkSampler g_sunVisSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout g_sunVisSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool g_sunVisPool = VK_NULL_HANDLE;
    VkDescriptorSet g_sunVisSet = VK_NULL_HANDLE;
    VkPipelineLayout g_sunVisPipelineLayout = VK_NULL_HANDLE;
    VkPipeline g_sunVisPipeline = VK_NULL_HANDLE;
    bool g_sunVisFailed = false;
    bool g_sunVisLayoutReady = false;
    VkDescriptorSet g_sunVisBoundSet = VK_NULL_HANDLE;

    // 2026-11-14：binding 1（TLAS）当前写进去的是哪个 g_tlas。
    // 与 g_sunShadowTlasHandle / g_terrainHeightTlasHandle 同一套约定。
    VkAccelerationStructureKHR g_sunVisTlasHandle = VK_NULL_HANDLE;

    bool createSunVisibilityPass()
    {
        if(g_sunVisPipeline != VK_NULL_HANDLE)
        {
            return true;
        }
        if(g_sunVisFailed || g_device == VK_NULL_HANDLE ||
           g_physicalDevice == VK_NULL_HANDLE || g_cameraUboBuffer == VK_NULL_HANDLE ||
           g_tlas == VK_NULL_HANDLE)
        {
            return false;
        }

        try
        {
            VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imageInfo.imageType = VK_IMAGE_TYPE_3D;
            imageInfo.format = VK_FORMAT_R8_UNORM;
            imageInfo.extent = { SUN_VIS_W, SUN_VIS_H, SUN_VIS_D };
            imageInfo.mipLevels = 1u;
            imageInfo.arrayLayers = 1u;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            check(vkCreateImage(g_device, &imageInfo, nullptr, &g_sunVisImage),
                "sun visibility image create failed");

            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(g_device, g_sunVisImage, &requirements);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
                requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            check(vkAllocateMemory(g_device, &allocation, nullptr, &g_sunVisMemory),
                "sun visibility memory allocate failed");
            check(vkBindImageMemory(g_device, g_sunVisImage, g_sunVisMemory, 0u),
                "sun visibility image bind failed");

            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = g_sunVisImage;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
            viewInfo.format = VK_FORMAT_R8_UNORM;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1u;
            viewInfo.subresourceRange.layerCount = 1u;
            check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_sunVisView),
                "sun visibility view create failed");

            VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            samplerInfo.magFilter = VK_FILTER_LINEAR;
            samplerInfo.minFilter = VK_FILTER_LINEAR;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.maxLod = 0.0f;
            check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_sunVisSampler),
                "sun visibility sampler create failed");

            VkDescriptorSetLayoutBinding bindings[3]{};
            bindings[0] = { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            bindings[1] = { 1u, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            bindings[2] = { 2u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

            VkDescriptorSetLayoutCreateInfo layoutInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
            };
            layoutInfo.bindingCount = 3u;
            layoutInfo.pBindings = bindings;
            check(vkCreateDescriptorSetLayout(g_device, &layoutInfo, nullptr,
                &g_sunVisSetLayout), "sun visibility set layout failed");

            VkDescriptorPoolSize poolSizes[3] = {
                { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u },
                { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u },
                { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u }
            };
            VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.maxSets = 1u;
            poolInfo.poolSizeCount = 3u;
            poolInfo.pPoolSizes = poolSizes;
            check(vkCreateDescriptorPool(g_device, &poolInfo, nullptr, &g_sunVisPool),
                "sun visibility pool failed");

            VkDescriptorSetAllocateInfo setAllocation{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
            };
            setAllocation.descriptorPool = g_sunVisPool;
            setAllocation.descriptorSetCount = 1u;
            setAllocation.pSetLayouts = &g_sunVisSetLayout;
            check(vkAllocateDescriptorSets(g_device, &setAllocation, &g_sunVisSet),
                "sun visibility set allocate failed");

            VkDescriptorBufferInfo cameraInfo{};
            cameraInfo.buffer = g_cameraUboBuffer;
            cameraInfo.range = sizeof(CameraUBO);

            VkDescriptorImageInfo storageInfo{};
            storageInfo.imageView = g_sunVisView;
            storageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSetAccelerationStructureKHR tlasInfo{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR
            };
            tlasInfo.accelerationStructureCount = 1u;
            tlasInfo.pAccelerationStructures = &g_tlas;

            VkWriteDescriptorSet writes[3]{};
            writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                g_sunVisSet, 0u, 0u, 1u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                nullptr, &cameraInfo, nullptr };
            writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                g_sunVisSet, 1u, 0u, 1u, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                nullptr, nullptr, nullptr };
            writes[1].pNext = &tlasInfo;
            writes[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                g_sunVisSet, 2u, 0u, 1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                &storageInfo, nullptr, nullptr };
            vkUpdateDescriptorSets(g_device, 3u, writes, 0u, nullptr);

            const std::vector<uint8_t> code = loadSpv("shaders/rt_sun_volume.comp.spv");
            VkShaderModule shader = createShaderModule(g_device, code);

            VkPipelineLayoutCreateInfo pipelineLayoutInfo{
                VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
            };
            pipelineLayoutInfo.setLayoutCount = 1u;
            pipelineLayoutInfo.pSetLayouts = &g_sunVisSetLayout;
            check(vkCreatePipelineLayout(g_device, &pipelineLayoutInfo, nullptr,
                &g_sunVisPipelineLayout), "sun visibility pipeline layout failed");

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
            pipelineInfo.layout = g_sunVisPipelineLayout;
            check(vkCreateComputePipelines(g_device, getSharedPipelineCache(), 1u,
                &pipelineInfo, nullptr, &g_sunVisPipeline),
                "sun visibility pipeline failed");

            vkDestroyShaderModule(g_device, shader, nullptr);

            RTLog("[SUN VIS] volume ready (64x64x32, near..32 blocks)");
            return true;
        }
        catch(...)
        {
            RTLog("[SUN VIS] creation failed; sun term keeps using the shadow map");
            g_sunVisFailed = true;
            return false;
        }
    }

    void recordSunVisibilityVolume(VkCommandBuffer cmd)
    {
        if(cmd == VK_NULL_HANDLE)
        {
            return;
        }

        // 2026-11-15：S1 太阳可见性体积已被 froxel 光照 pass（ray query）取代，
        // v5 / 雾都不再采样它 —— 下面直接跳过这个 pass，省掉每帧一次
        // 64×64×32 的 TLAS 射线烘制（约 131k 条射线）。
        // 但 fogTlas（binding 16）的懒绑定必须仍然执行：froxel 光照 pass 要用它。
        // 回退：把 kEnableSunVisibilityVolume 改成 true。
        constexpr bool kEnableSunVisibilityVolume = false;

        // 2026-11-15：**立刻返回**。S1 停用后 createSunVisibilityPass() 不再被调用，
        // g_sunVisSet / g_sunVisPipeline 全是 NULL；而下面紧跟的
        // `if(g_sunVisTlasHandle != g_tlas)` 分支会 vkUpdateDescriptorSets 写 NULL set
        // ⇒ 驱动 AV（hs_err: nvoglv64 ← vulkan-1 ← rtx_backend，注入点
        // renderItemInHand 的 injectRtBeforeGui，三次崩溃都紧跟 rt_cloud_shadow 之后）。
        // 回退 S1：把 kEnableSunVisibilityVolume 改 true，并恢复 createSunVisibilityPass 调用。
        if(!kEnableSunVisibilityVolume)
        {
            return;
        }

        // 2026-11-14 TDR 根因修复：binding 1 必须跟随 g_tlas。
        //
        // 原实现只在 createSunVisibilityPass() 里写过一次 binding 1，而 g_tlas
        // 会随实例容量增长被销毁重建（parkTlasStorage / releaseRetirementSlot
        // → destroyAS）⇒ 描述符指向已销毁的 AS ⇒ rt_sun_volume.comp 的
        // inline ray query 遍历已释放的 BVH 内存 ⇒ GPU 挂死、且因为内存仍在
        // 驱动堆里映射着而不报 page fault（实测 vkGetDeviceFaultInfoEXT 只回
        // -13 DEVICE_LOST）。
        //
        // 流式加载（实例数 4165→4364）期间每涨一次就重建一次 TLAS，
        // 所以表现为"边走边崩"。
        if(g_tlas == VK_NULL_HANDLE)
        {
            return;
        }

        if(g_sunVisTlasHandle != g_tlas)
        {
            VkAccelerationStructureKHR tlasHandle = g_tlas;

            VkWriteDescriptorSetAccelerationStructureKHR tlasInfo{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR
            };
            tlasInfo.accelerationStructureCount = 1u;
            tlasInfo.pAccelerationStructures = &tlasHandle;

            VkWriteDescriptorSet tlasWrite{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
            };
            tlasWrite.dstSet = g_sunVisSet;
            tlasWrite.dstBinding = 1u;
            tlasWrite.descriptorCount = 1u;
            tlasWrite.descriptorType =
                VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            tlasWrite.pNext = &tlasInfo;

            vkUpdateDescriptorSets(g_device, 1u, &tlasWrite, 0u, nullptr);

            g_sunVisTlasHandle = g_tlas;
        }

        // 2026-11-15：体积雾的 binding 16（TLAS）同样必须跟随 g_tlas ——
        // ray query 遍历已销毁的 BVH 会和 S1 那条一样直接 TDR（见上面注释）。
        if(g_volumetricCloudRenderSet != VK_NULL_HANDLE)
        {
            static VkAccelerationStructureKHR boundFogTlas = VK_NULL_HANDLE;
            static VkDescriptorSet boundFogTlasSet = VK_NULL_HANDLE;
            static uint64_t boundFogTlasGen = 0u;

            if(boundFogTlas != g_tlas ||
               boundFogTlasSet != g_volumetricCloudRenderSet ||
               (kCLOUD_DESCRIPTOR_GENERATION_GUARD &&
                boundFogTlasGen != g_volumetricCloudSetGeneration))
            {
                VkAccelerationStructureKHR tlasHandle = g_tlas;

                VkWriteDescriptorSetAccelerationStructureKHR tlasInfo{
                    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR
                };
                tlasInfo.accelerationStructureCount = 1u;
                tlasInfo.pAccelerationStructures = &tlasHandle;

                VkWriteDescriptorSet tlasWrite{
                    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
                };
                tlasWrite.dstSet = g_volumetricCloudRenderSet;
                tlasWrite.dstBinding = 16u;
                tlasWrite.descriptorCount = 1u;
                tlasWrite.descriptorType =
                    VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
                tlasWrite.pNext = &tlasInfo;

                vkUpdateDescriptorSets(g_device, 1u, &tlasWrite, 0u, nullptr);

                boundFogTlas = g_tlas;
                boundFogTlasSet = g_volumetricCloudRenderSet;
                boundFogTlasGen = g_volumetricCloudSetGeneration;

                RTLogFlushed("[FOG RAY] TLAS bound to cloud render set binding 16 gen=" +
                    std::to_string(boundFogTlasGen));
            }
        }

        if(!kEnableSunVisibilityVolume)
        {
            return;   // S1 体积停用（froxel 光照 pass 已用 ray query 取代）
        }

        if(g_sunVisBoundSet != g_volumetricCloudRenderSet &&
           g_volumetricCloudRenderSet != VK_NULL_HANDLE)
        {
            VkDescriptorImageInfo info{};
            info.sampler = g_sunVisSampler;
            info.imageView = g_sunVisView;
            info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = g_volumetricCloudRenderSet;
            write.dstBinding = 15u;
            write.descriptorCount = 1u;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &info;
            vkUpdateDescriptorSets(g_device, 1u, &write, 0u, nullptr);

            g_sunVisBoundSet = g_volumetricCloudRenderSet;
        }

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = g_sunVisLayoutReady
            ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = g_sunVisImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1u;
        barrier.subresourceRange.layerCount = 1u;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr,
            1u, &barrier);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_sunVisPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            g_sunVisPipelineLayout, 0u, 1u, &g_sunVisSet, 0u, nullptr);
        vkCmdDispatch(cmd,
            (SUN_VIS_W + 7u) / 8u, (SUN_VIS_H + 7u) / 8u, SUN_VIS_D);

        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr,
            1u, &barrier);

        g_sunVisLayoutReady = true;
    }
}

// =====================================================================
// 2026-11-15：体积雾 froxel 重构（Pass 1 密度 → Pass 3 合成）。
// 在云 pass 之前录：v5 里 evaluateAnalyticFog 已被替换为采样两张屏幕图。
// 依赖：g_volumetricCloudRenderSet 的 binding 17..22 已写入。
// =====================================================================
namespace
{
    // 2026-11-20：体积光/雾链路的**数值探针**（ORION_FOG_PROBE=1）。
    // 目的：不再靠"看着黑/亮"猜。每 240 帧把三样东西回读一次并打印统计：
    //   ① fogScreenColor（合成输出：RGB = 内散射，A = 透射率）
    //   ② froxel 密度体中间层（R = 雾消光，G = 体积光介质）
    //   ③ froxel 太阳可见性体中间层（R = 0/1，线性插值后 0..1）
    // 这三组数就能唯一确定"全黑"是透射率被杀、散射为 0、还是密度体是垃圾。
    bool fogProbeEnabled()
    {
        static int cached = -1;
        if(cached < 0)
        {
            const char* env = std::getenv("ORION_FOG_PROBE");
            cached = (env != nullptr && env[0] == '1') ? 1 : 0;
        }
        return cached == 1;
    }

    float fogProbeHalf(uint16_t h)
    {
        const uint32_t sign = (static_cast<uint32_t>(h) & 0x8000u) << 16;
        uint32_t expo = (static_cast<uint32_t>(h) >> 10) & 0x1Fu;
        uint32_t mant = static_cast<uint32_t>(h) & 0x3FFu;
        uint32_t bits;
        if(expo == 0x1Fu)
        {
            bits = sign | 0x7F800000u | (mant << 13);
        }
        else if(expo == 0u)
        {
            if(mant == 0u)
            {
                bits = sign;
            }
            else
            {
                expo = 113u;
                while((mant & 0x400u) == 0u)
                {
                    mant <<= 1;
                    --expo;
                }
                mant &= 0x3FFu;
                bits = sign | (expo << 23) | (mant << 13);
            }
        }
        else
        {
            bits = sign | ((expo + 112u) << 23) | (mant << 13);
        }
        float out;
        std::memcpy(&out, &bits, sizeof(out));
        return out;
    }

    VkBuffer s_fogProbeBuffer = VK_NULL_HANDLE;
    VkDeviceMemory s_fogProbeMemory = VK_NULL_HANDLE;
    uint8_t* s_fogProbeMapped = nullptr;
    VkDeviceSize s_fogProbeBytes = 0u;
    uint32_t s_fogProbeCounter = 0u;
    bool s_fogProbePending = false;
    uint32_t s_fogProbeScreenPixels = 0u;
    uint32_t s_fogProbeSlicePixels = 0u;

    void fogProbeLog()
    {
        if(!fogProbeEnabled() || !s_fogProbePending || s_fogProbeMapped == nullptr)
        {
            return;
        }
        s_fogProbePending = false;

        auto readHalf = [](const uint8_t* base, size_t index) {
            uint16_t raw;
            std::memcpy(&raw, base + index * 2u, sizeof(raw));
            return fogProbeHalf(raw);
        };

        float rgbSum = 0.0f;
        float rgbMax = 0.0f;
        float minT = 1.0e30f;
        float maxT = -1.0e30f;
        float tSum = 0.0f;
        uint32_t nonFinite = 0u;
        const uint8_t* screen = s_fogProbeMapped;
        for(uint32_t i = 0u; i < s_fogProbeScreenPixels; ++i)
        {
            const float r = readHalf(screen, i * 4u + 0u);
            const float g = readHalf(screen, i * 4u + 1u);
            const float b = readHalf(screen, i * 4u + 2u);
            const float a = readHalf(screen, i * 4u + 3u);
            if(!(r == r) || !(g == g) || !(b == b) || !(a == a))
            {
                ++nonFinite;
                continue;
            }
            const float lum = r + g + b;
            rgbSum += lum;
            rgbMax = std::max(rgbMax, lum);
            minT = std::min(minT, a);
            maxT = std::max(maxT, a);
            tSum += a;
        }

        const uint8_t* density = s_fogProbeMapped + s_fogProbeScreenPixels * 8u;
        float hazeSum = 0.0f, hazeMax = 0.0f, shaftSum = 0.0f, shaftMax = 0.0f;
        uint32_t hazeBig = 0u;
        for(uint32_t i = 0u; i < s_fogProbeSlicePixels; ++i)
        {
            const float r = readHalf(density, i * 4u + 0u);
            const float g = readHalf(density, i * 4u + 1u);
            if(r == r && g == g)
            {
                hazeSum += r;
                hazeMax = std::max(hazeMax, r);
                shaftSum += g;
                shaftMax = std::max(shaftMax, g);
                if(r > 0.05f)
                {
                    ++hazeBig;
                }
            }
        }

        const uint8_t* shadow = s_fogProbeMapped + s_fogProbeScreenPixels * 8u +
            static_cast<size_t>(s_fogProbeSlicePixels) * 8u;
        float visSum = 0.0f, visMin = 1.0e30f, visMax = -1.0e30f;
        float shadowGSum = 0.0f;
        float shadowSamplerSum = 0.0f;
        for(uint32_t i = 0u; i < s_fogProbeSlicePixels; ++i)
        {
            const float v = readHalf(shadow, i * 4u + 0u);
            const float gv = readHalf(shadow, i * 4u + 1u);
            const float bv = readHalf(shadow, i * 4u + 2u);
            if(v == v)
            {
                visSum += v;
                visMin = std::min(visMin, v);
                visMax = std::max(visMax, v);
            }
            if(gv == gv && bv == bv)
            {
                shadowGSum += gv;
                shadowSamplerSum += bv;
            }
        }

        const float pixels = float(s_fogProbeScreenPixels > 0u
            ? s_fogProbeScreenPixels : 1u);
        const float slicePixels = float(s_fogProbeSlicePixels > 0u
            ? s_fogProbeSlicePixels : 1u);
        char buffer[512];
        std::snprintf(buffer, sizeof(buffer),
            "[FOG PROBE] screen rgbMean=%.3f rgbMax=%.3f T[min=%.4f mean=%.4f max=%.4f] "
            "nan=%u | medium sigmaT mean=%.4f max=%.4f sigmaS mean=%.4f max=%.4f "
            "over05=%u | vis mean=%.3f min=%.3f max=%.3f "
            "| shadow sigmaS=%.4f sunT=%.3f",
            double(rgbSum / pixels), double(rgbMax),
            double(minT), double(tSum / pixels), double(maxT), nonFinite,
            double(hazeSum / slicePixels), double(hazeMax),
            double(shaftSum / slicePixels), double(shaftMax), hazeBig,
            double(visSum / slicePixels), double(visMin), double(visMax),
            double(shadowGSum / slicePixels),
            double(shadowSamplerSum / slicePixels));
        RTLogFlushed(buffer);
    }
}

void syncFogRenderTargets()
{
    if(g_rtOutputView == VK_NULL_HANDLE ||
       g_dlssDepthView == VK_NULL_HANDLE ||
       g_volumetricCloudRenderSet == VK_NULL_HANDLE)
    {
        return;
    }

    // Keep binding 24 (g_rtOutputView) and binding 5 (g_dlssDepthView) strictly synchronized
    // across resizes and descriptor set recreation (generation guard prevents ABA reuse bugs).
    static VkImageView boundApplyTarget = VK_NULL_HANDLE;
    static VkImageView boundApplyDepth = VK_NULL_HANDLE;
    static VkDescriptorSet boundApplySet = VK_NULL_HANDLE;
    static uint64_t boundApplyGen = 0u;
    if(boundApplyTarget != g_rtOutputView ||
       boundApplyDepth != g_dlssDepthView ||
       boundApplySet != g_volumetricCloudRenderSet ||
       (kCLOUD_DESCRIPTOR_GENERATION_GUARD &&
        boundApplyGen != g_volumetricCloudSetGeneration))
    {
        VkDescriptorImageInfo targetInfo{};
        targetInfo.imageView = g_rtOutputView;
        targetInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo depthInfo{};
        depthInfo.sampler = volDepthSampler();
        depthInfo.imageView = g_dlssDepthView;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = g_volumetricCloudRenderSet;
        writes[0].dstBinding = 24u;
        writes[0].descriptorCount = 1u;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &targetInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = g_volumetricCloudRenderSet;
        writes[1].dstBinding = 5u;
        writes[1].descriptorCount = 1u;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &depthInfo;

        vkUpdateDescriptorSets(g_device, 2u, writes, 0u, nullptr);
        boundApplyTarget = g_rtOutputView;
        boundApplyDepth = g_dlssDepthView;
        boundApplySet = g_volumetricCloudRenderSet;
        boundApplyGen = g_volumetricCloudSetGeneration;
        RTLogFlushed("[FOG APPLY] bound rtOutputView + dlssDepthView " +
            std::to_string(g_rtOutputWidth) + "x" +
            std::to_string(g_rtOutputHeight) + " gen=" +
            std::to_string(boundApplyGen));
    }
}

void recordFogFroxelPasses(VkCommandBuffer cmd)
{
    fogProbeLog();
    if(!g_fogEnabled.load() || g_cameraInWater.load() != 0u)
    {
        return;
    }

    const bool fogFroxelReady =
        cmd != VK_NULL_HANDLE &&
        g_fogFroxelDensityPipeline != VK_NULL_HANDLE &&
        g_fogFroxelLightPipeline != VK_NULL_HANDLE &&
        g_fogFroxelCompositePipeline != VK_NULL_HANDLE &&
        g_fogFroxelDensityPipelineLayout != VK_NULL_HANDLE &&
        g_fogFroxelLightPipelineLayout != VK_NULL_HANDLE &&
        g_fogFroxelCompositePipelineLayout != VK_NULL_HANDLE &&
        g_fogFroxelDensityView != VK_NULL_HANDLE &&
        g_fogFroxelShadowView != VK_NULL_HANDLE &&
        g_fogFroxelLightHistoryView != VK_NULL_HANDLE &&
        g_fogFroxelIntegratedView != VK_NULL_HANDLE &&
        g_fogFroxelWidth > 0u && g_fogFroxelHeight > 0u &&
        g_volumetricCloudRenderSet != VK_NULL_HANDLE;
    if(!fogFroxelReady)
    {
        static uint32_t fogSkipLogCounter = 0u;
        if((fogSkipLogCounter++ % 300u) == 0u)
        {
            auto bit = [](bool value) {
                return value ? std::string("1") : std::string("0");
            };
            RTLogFlushed(std::string("[FOG FROXEL] skipped: ") +
                "denPipe=" + bit(g_fogFroxelDensityPipeline != VK_NULL_HANDLE) +
                " litPipe=" + bit(g_fogFroxelLightPipeline != VK_NULL_HANDLE) +
                " cmpPipe=" + bit(g_fogFroxelCompositePipeline != VK_NULL_HANDLE) +
                " denView=" + bit(g_fogFroxelDensityView != VK_NULL_HANDLE) +
                " litView=" + bit(g_fogFroxelShadowView != VK_NULL_HANDLE) +
                " histView=" + bit(g_fogFroxelLightHistoryView != VK_NULL_HANDLE) +
                " intView=" + bit(g_fogFroxelIntegratedView != VK_NULL_HANDLE) +
                " set=" + bit(g_volumetricCloudRenderSet != VK_NULL_HANDLE));
        }
        return;
    }

    if(kCLOUD_DESCRIPTOR_GENERATION_GUARD)
    {
        syncFogRenderTargets();
    }

    const uint32_t froxelWidth = g_fogFroxelWidth;
    const uint32_t froxelHeight = g_fogFroxelHeight;
    const float maxDistance = std::max(g_fogMaxDistance.load(), 16.0f);

    // Clear 3D temporal history once after creation/resize/settings change.
    bool historyValid = true;
    if(g_fogFroxelHistoryNeedsClear.exchange(false))
    {
        historyValid = false;
        VkImageMemoryBarrier histClearBarrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        histClearBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        histClearBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        histClearBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        histClearBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        histClearBarrier.srcAccessMask = 0u;
        histClearBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        histClearBarrier.image = g_fogFroxelLightHistoryImage;
        histClearBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr,
            1u, &histClearBarrier);

        const VkClearColorValue zeroVal{{0.0f, 0.0f, 0.0f, 0.0f}};
        vkCmdClearColorImage(cmd, g_fogFroxelLightHistoryImage,
            VK_IMAGE_LAYOUT_GENERAL, &zeroVal, 1u, &histClearBarrier.subresourceRange);

        histClearBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        histClearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        histClearBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr,
            1u, &histClearBarrier);
    }

    // Transition 3D write targets to GENERAL (discard previous frame contents).
    VkImageMemoryBarrier fogTargets[3]{};
    for(uint32_t i = 0u; i < 3u; ++i)
    {
        fogTargets[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        fogTargets[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        fogTargets[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        fogTargets[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        fogTargets[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        fogTargets[i].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u };
        fogTargets[i].srcAccessMask = 0u;
        fogTargets[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    }
    fogTargets[0].image = g_fogFroxelDensityImage;
    fogTargets[1].image = g_fogFroxelShadowImage;
    fogTargets[2].image = g_fogFroxelIntegratedImage;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr,
        3u, fogTargets);

    {
        static uint32_t fogFroxelLogCounter = 0u;
        if((fogFroxelLogCounter++ % 600u) == 0u)
        {
            RTLogFlushed("[FOG FROXEL] grid=" +
                std::to_string(froxelWidth) + "x" +
                std::to_string(froxelHeight) + "x" +
                std::to_string(FOG_FROXEL_DEPTH) +
                " rt=" + std::to_string(g_rtOutputWidth) + "x" +
                std::to_string(g_rtOutputHeight) +
                " maxDist=" + std::to_string(maxDistance) +
                " cloudShadow=" + std::to_string(g_cloudShadowLayoutReady ? 1 : 0));
        }
    }

    // Pass 1: Decoupled 3D medium evaluation (haze + shaft aerosol + cave openness).
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_fogFroxelDensityPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_fogFroxelDensityPipelineLayout, 0u, 1u, &g_volumetricCloudRenderSet, 0u, nullptr);
    const float densityPush[4] = {
        float(FOG_FROXEL_DEPTH), maxDistance, 0.0f, 0.0f };
    vkCmdPushConstants(cmd, g_fogFroxelDensityPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(densityPush), densityPush);
    vkCmdDispatch(cmd, (froxelWidth + 3u) / 4u, (froxelHeight + 3u) / 4u,
        (FOG_FROXEL_DEPTH + 3u) / 4u);

    {
        VkMemoryBarrier densityToLight{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        densityToLight.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        densityToLight.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 1u, &densityToLight,
            0u, nullptr, 0u, nullptr);
    }

    // Pass 2: Volumetric lighting (exact RT ray query + cloud shadow + local lights + 3D temporal EMA).
    const float cloudShadowValid =
        (g_cloudEnabled.load() && g_cloudShadowLayoutReady &&
         g_cloudShadowBoundSet == g_volumetricCloudRenderSet &&
         (!kCLOUD_DESCRIPTOR_GENERATION_GUARD ||
          g_cloudShadowBoundGen == g_volumetricCloudSetGeneration)) ? 1.0f : 0.0f;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_fogFroxelLightPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_fogFroxelLightPipelineLayout, 0u, 1u, &g_volumetricCloudRenderSet, 0u, nullptr);
    const float lightPush[4] = {
        float(FOG_FROXEL_DEPTH), maxDistance,
        historyValid ? 1.0f : 0.0f, cloudShadowValid };
    vkCmdPushConstants(cmd, g_fogFroxelLightPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(lightPush), lightPush);
    vkCmdDispatch(cmd, (froxelWidth + 3u) / 4u, (froxelHeight + 3u) / 4u,
        (FOG_FROXEL_DEPTH + 3u) / 4u);

    // Copy current lit 3D froxel volume into history 3D volume for next frame's reprojection,
    // and ensure Pass 3 can read g_fogFroxelShadowImage concurrently.
    {
        VkImageMemoryBarrier preCopy[2]{};
        preCopy[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        preCopy[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        preCopy[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        preCopy[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        preCopy[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        preCopy[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        preCopy[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        preCopy[0].image = g_fogFroxelShadowImage;
        preCopy[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u };

        preCopy[1] = preCopy[0];
        preCopy[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        preCopy[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        preCopy[1].image = g_fogFroxelLightHistoryImage;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u, 0u, nullptr, 0u, nullptr, 2u, preCopy);

        VkImageCopy copyRegion{};
        copyRegion.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
        copyRegion.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
        copyRegion.extent = { froxelWidth, froxelHeight, FOG_FROXEL_DEPTH };
        vkCmdCopyImage(cmd, g_fogFroxelShadowImage, VK_IMAGE_LAYOUT_GENERAL,
            g_fogFroxelLightHistoryImage, VK_IMAGE_LAYOUT_GENERAL, 1u, &copyRegion);

        VkImageMemoryBarrier postCopy{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        postCopy.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        postCopy.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        postCopy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        postCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        postCopy.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        postCopy.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        postCopy.image = g_fogFroxelLightHistoryImage;
        postCopy.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr,
            1u, &postCopy);
    }

    // Pass 3: Front-to-back analytical accumulation into 3D integrated LUT (binding 27).
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_fogFroxelCompositePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_fogFroxelCompositePipelineLayout, 0u, 1u, &g_volumetricCloudRenderSet, 0u, nullptr);
    const float compositePush[4] = {
        float(FOG_FROXEL_DEPTH), maxDistance, 0.0f, 0.0f };
    vkCmdPushConstants(cmd, g_fogFroxelCompositePipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(compositePush), compositePush);
    vkCmdDispatch(cmd, (froxelWidth + 7u) / 8u, (froxelHeight + 7u) / 8u, 1u);

    {
        VkImageMemoryBarrier integratedReady{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        integratedReady.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        integratedReady.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        integratedReady.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        integratedReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        integratedReady.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u };
        integratedReady.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        integratedReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        integratedReady.image = g_fogFroxelIntegratedImage;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr,
            1u, &integratedReady);
    }
}

// Pass 4: Full RT-resolution exact-depth apply of the integrated 3D froxel volume onto g_rtOutputImage.
void recordFogScreenApply(VkCommandBuffer cmd)
{
    if(!g_fogEnabled.load() || g_cameraInWater.load() != 0u)
    {
        return;
    }
    if(cmd == VK_NULL_HANDLE ||
       g_fogScreenApplyPipeline == VK_NULL_HANDLE ||
       g_fogScreenApplyPipelineLayout == VK_NULL_HANDLE ||
       g_fogFroxelIntegratedView == VK_NULL_HANDLE ||
       g_rtOutputView == VK_NULL_HANDLE ||
       g_dlssDepthView == VK_NULL_HANDLE ||
       g_volumetricCloudRenderSet == VK_NULL_HANDLE ||
       g_rtOutputWidth == 0u || g_rtOutputHeight == 0u)
    {
        return;
    }

    syncFogRenderTargets();

    {
        VkImageMemoryBarrier sceneBarrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        sceneBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        sceneBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        sceneBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sceneBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sceneBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        sceneBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        sceneBarrier.image = g_rtOutputImage;
        sceneBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr,
            1u, &sceneBarrier);
    }

    const float maxDistance = std::max(g_fogMaxDistance.load(), 16.0f);
    const float applyPush[4] = {
        float(FOG_FROXEL_DEPTH), maxDistance, 0.0f, 0.0f };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_fogScreenApplyPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_fogScreenApplyPipelineLayout, 0u, 1u, &g_volumetricCloudRenderSet, 0u, nullptr);
    vkCmdPushConstants(cmd, g_fogScreenApplyPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(applyPush), applyPush);
    vkCmdDispatch(cmd, (g_rtOutputWidth + 7u) / 8u, (g_rtOutputHeight + 7u) / 8u, 1u);
}

namespace
{
    // binding 1 / 6 是 g_rtOutputView / g_dlssOutputView，两者都会在分辨率
    // 变化时被重建；set 虽然跟着云资源一起重建，但这里再跟踪一次，避免重蹈
    // "描述符冻死指向已销毁视图" 的覆辙（11-14 太阳可见性 / ReSTIR 合成两次）。
    VkImageView g_cloudCompositeBoundNear = VK_NULL_HANDLE;
    VkImageView g_cloudCompositeBoundFar = VK_NULL_HANDLE;

    // 2026-11-16：本帧云链是否真的产出了 g_volumetricCloudResolvedImage。
    // 远带合成必须确认这一点，否则云被关掉 / 管线缺失的帧会把上一帧的云
    // 再合成一遍（画面表现为云"冻结"或双份亮度）。
    uint64_t g_cloudCompositeFrameSerial = ~0ull;

    void refreshCloudCompositeTargets()
    {
        if(g_volumetricCloudCompositeSet == VK_NULL_HANDLE)
        {
            return;
        }

        if(g_cloudCompositeBoundNear == g_rtOutputView &&
           g_cloudCompositeBoundFar == g_dlssOutputView)
        {
            return;
        }

        if(g_rtOutputView == VK_NULL_HANDLE || g_dlssOutputView == VK_NULL_HANDLE)
        {
            return;
        }

        VkDescriptorImageInfo nearInfo{};
        nearInfo.imageView = g_rtOutputView;
        nearInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo farInfo{};
        farInfo.imageView = g_dlssOutputView;
        farInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = g_volumetricCloudCompositeSet;
        writes[0].dstBinding = 1u;
        writes[0].descriptorCount = 1u;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &nearInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = g_volumetricCloudCompositeSet;
        writes[1].dstBinding = 6u;
        writes[1].descriptorCount = 1u;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &farInfo;

        vkUpdateDescriptorSets(g_device, 2u, writes, 0u, nullptr);

        g_cloudCompositeBoundNear = g_rtOutputView;
        g_cloudCompositeBoundFar = g_dlssOutputView;
    }

    VkImageView g_cloudSkyLutBoundView = VK_NULL_HANDLE;
    VkDescriptorSet g_cloudSkyLutBoundSet = VK_NULL_HANDLE;
    uint64_t g_cloudSkyLutBoundGen = 0u;

    void syncCloudSkyLutDescriptor()
    {
        if(g_volumetricCloudRenderSet == VK_NULL_HANDLE ||
           g_skyScatteringView == VK_NULL_HANDLE ||
           g_skyAtmosphereSampler == VK_NULL_HANDLE ||
           g_skyScatteringLayout != VK_IMAGE_LAYOUT_GENERAL)
        {
            return;
        }
        if(g_cloudSkyLutBoundSet == g_volumetricCloudRenderSet &&
           g_cloudSkyLutBoundView == g_skyScatteringView &&
           (!kCLOUD_DESCRIPTOR_GENERATION_GUARD ||
            g_cloudSkyLutBoundGen == g_volumetricCloudSetGeneration))
        {
            return;
        }

        VkDescriptorImageInfo skyLutInfo{};
        skyLutInfo.sampler = g_skyAtmosphereSampler;
        skyLutInfo.imageView = g_skyScatteringView;
        skyLutInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet skyLutWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        skyLutWrite.dstSet = g_volumetricCloudRenderSet;
        skyLutWrite.dstBinding = 29u;
        skyLutWrite.descriptorCount = 1u;
        skyLutWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        skyLutWrite.pImageInfo = &skyLutInfo;
        vkUpdateDescriptorSets(g_device, 1u, &skyLutWrite, 0u, nullptr);

        g_cloudSkyLutBoundSet = g_volumetricCloudRenderSet;
        g_cloudSkyLutBoundView = g_skyScatteringView;
        g_cloudSkyLutBoundGen = g_volumetricCloudSetGeneration;
    }
}

// 2026-11-16：雾的独立屏幕 march（与云开关解耦）。
// 输出两张屏幕图：19=rgba16f scattering+transmittance、20=rg16f (first,mean)。
// 云开着：v5 采样（binding 21/22）；云关着：fog_screen_apply 合成到场景图。
//
// 2026-11-20（用户裁决）：**已停用，不再有任何调用点**。雾的唯一实现是
// recordFogFroxelPasses（fog_froxel_density/light/composite）。这里保留函数体
// 仅作 A/B 回退：要恢复就把它接回 recordVolumetricClouds 里 froxel 的位置，
// 并同时停掉 froxel，避免两条链对同一批像素重复积分。
void recordFogScreenMarch(VkCommandBuffer cmd)
{
    constexpr bool kEnableFogScreenMarch = true;
    if(!kEnableFogScreenMarch)
    {
        return;
    }
    if(cmd == VK_NULL_HANDLE ||
       g_fogScreenMarchPipeline == VK_NULL_HANDLE ||
       g_fogScreenMarchPipelineLayout == VK_NULL_HANDLE ||
       g_fogScreenColorView == VK_NULL_HANDLE ||
       g_fogScreenDistView == VK_NULL_HANDLE ||
       g_volumetricCloudRenderSet == VK_NULL_HANDLE ||
       g_volumetricCloudWidth == 0u || g_volumetricCloudHeight == 0u)
    {
        return;
    }

    VkImageMemoryBarrier fogBarriers[2]{};
    for(uint32_t i = 0u; i < 2u; ++i)
    {
        fogBarriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        fogBarriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        fogBarriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        fogBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        fogBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        fogBarriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        fogBarriers[i].subresourceRange.levelCount = 1u;
        fogBarriers[i].subresourceRange.layerCount = 1u;
        fogBarriers[i].srcAccessMask = 0u;
        fogBarriers[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    }
    fogBarriers[0].image = g_fogScreenColorImage;
    fogBarriers[1].image = g_fogScreenDistImage;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr,
        2u, fogBarriers);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_fogScreenMarchPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_fogScreenMarchPipelineLayout, 0u, 1u,
        &g_volumetricCloudRenderSet, 0u, nullptr);
    {
        static uint32_t marchLogCounter = 0u;
        if((marchLogCounter++ % 300u) == 0u)
        {
            RTLogFlushed("[FOG MARCH] dispatch " +
                std::to_string(g_volumetricCloudWidth) + "x" +
                std::to_string(g_volumetricCloudHeight));
        }
    }
    vkCmdDispatch(cmd, (g_volumetricCloudWidth + 7u) / 8u,
        (g_volumetricCloudHeight + 7u) / 8u, 1u);

    {
        VkImageMemoryBarrier done[2]{};
        for(uint32_t i = 0u; i < 2u; ++i)
        {
            done[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            done[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            done[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            done[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            done[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            done[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            done[i].subresourceRange.levelCount = 1u;
            done[i].subresourceRange.layerCount = 1u;
            done[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            done[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        }
        done[0].image = g_fogScreenColorImage;
        done[1].image = g_fogScreenDistImage;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr,
            2u, done);
    }
}

// 2026-11-20：[GODRAY PROBE] 把体积光中间图回读成数字。
// 目的：判断"看不见"是断在 march 输出（rgb≈0 / a≈1），还是断在合成之后。
// 每 120 帧录一次 copy，下一次统计上一帧的内容（不做全设备等待）。
static float godRayHalfToFloat(uint16_t value)
{
    const uint32_t exponent = (value >> 10) & 0x1Fu;
    const uint32_t mantissa = value & 0x3FFu;
    const float sign = (value & 0x8000u) ? -1.0f : 1.0f;

    if(exponent == 0u)
    {
        return sign * (float(mantissa) / 1024.0f) * 6.103515625e-05f;
    }
    if(exponent == 0x1Fu)
    {
        return mantissa != 0u ? 0.0f : sign * 65504.0f;
    }
    return sign * (1.0f + float(mantissa) / 1024.0f) *
        powf(2.0f, float(int(exponent) - 15));
}

static void godRayProbe(VkCommandBuffer cmd)
{
    static VkBuffer probeBuffer = VK_NULL_HANDLE;
    static VkDeviceMemory probeMemory = VK_NULL_HANDLE;
    static uint16_t* probeMapped = nullptr;
    static uint32_t probeFrame = 0u;
    static bool probeCopyPending = false;

    if(cmd == VK_NULL_HANDLE || g_godRayTargetImage == VK_NULL_HANDLE ||
       g_godRayWidth == 0u || g_godRayHeight == 0u)
    {
        return;
    }

    if((probeFrame++ % 120u) != 0u)
    {
        return;
    }

    const VkDeviceSize bytes =
        VkDeviceSize(g_godRayWidth) * VkDeviceSize(g_godRayHeight) * 8u;

    if(probeBuffer == VK_NULL_HANDLE)
    {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if(vkCreateBuffer(g_device, &info, nullptr, &probeBuffer) != VK_SUCCESS)
        {
            return;
        }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(g_device, probeBuffer, &requirements);
        VkMemoryAllocateInfo allocation{
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if(vkAllocateMemory(g_device, &allocation, nullptr, &probeMemory) !=
            VK_SUCCESS)
        {
            vkDestroyBuffer(g_device, probeBuffer, nullptr);
            probeBuffer = VK_NULL_HANDLE;
            return;
        }
        vkBindBufferMemory(g_device, probeBuffer, probeMemory, 0u);
        vkMapMemory(g_device, probeMemory, 0u, VK_WHOLE_SIZE, 0u,
            reinterpret_cast<void**>(&probeMapped));
    }

    if(probeMapped == nullptr)
    {
        return;
    }

    // 统计上一帧拷贝下来的内容。
    if(probeCopyPending)
    {
        double rgbSum = 0.0;
        double aSum = 0.0;
        float rgbMax = 0.0f;
        float aMin = 1.0e30f;
        const uint32_t pixels = g_godRayWidth * g_godRayHeight;
        for(uint32_t i = 0u; i < pixels; ++i)
        {
            const uint16_t* texel = probeMapped + size_t(i) * 4u;
            const float r = godRayHalfToFloat(texel[0]);
            const float g = godRayHalfToFloat(texel[1]);
            const float b = godRayHalfToFloat(texel[2]);
            const float a = godRayHalfToFloat(texel[3]);
            const float lum = (r + g + b) * (1.0f / 3.0f);
            rgbSum += double(lum);
            rgbMax = std::max(rgbMax, lum);
            aSum += double(a);
            aMin = std::min(aMin, a);
        }
        RTLogFlushed(std::string("[GODRAY PROBE] rgb mean=") +
            std::to_string(rgbSum / double(pixels)) + " max=" +
            std::to_string(rgbMax) + " a mean=" +
            std::to_string(aSum / double(pixels)) + " min=" +
            std::to_string(aMin));
        probeCopyPending = false;
    }

    VkImageMemoryBarrier toSrc{};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.image = g_godRayTargetImage;
    toSrc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toSrc.subresourceRange.levelCount = 1u;
    toSrc.subresourceRange.layerCount = 1u;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1u;
    region.imageExtent = { g_godRayWidth, g_godRayHeight, 1u };
    vkCmdCopyImageToBuffer(cmd, g_godRayTargetImage, VK_IMAGE_LAYOUT_GENERAL,
        probeBuffer, 1u, &region);

    VkImageMemoryBarrier back{};
    back.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    back.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    back.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    back.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    back.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT |
        VK_ACCESS_SHADER_READ_BIT;
    back.image = g_godRayTargetImage;
    back.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    back.subresourceRange.levelCount = 1u;
    back.subresourceRange.layerCount = 1u;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u,
        &back);

    probeCopyPending = true;
}

// =============================================================================
// 2026-11-18：`volumetric_light.comp` 这条"独立均匀雾"光柱在雾开启时跳过。
//
// 2026-11-20 事实订正（原注释写错了，别再据此推断）：
//   该着色器第 17 行是 `#define VL_AIR_GODRAYS 0`（自 0.42 起，从未改成 1），
//   而整块 march 都包在 `#if VL_AIR_GODRAYS` 里，所以宏为 0 时 main 唯一执行到的
//   是 709 行 `#else storeGodRayNeutral(pixel); #endif` —— 逐像素写 (0,0,0,1)。
//   实测：[GODRAY] dispatch=1 但 [GODRAY PROBE] rgb=0 / a=1。
//   也就是说这条老链**从来不是**雾或光柱的来源，它一直在空转。
//
// 因此现阶段（2026-11-20 用户裁决）：
//   · 体积雾/体积光 = froxel 链（recordFogFroxelPasses）独占，与本函数无关；
//   · 本函数只保留给老链的 dispatch/composite 与 ReSTIR 体积光路径（后者
//     由 vol_restir_* 写 g_godRayTargetImage，需要 god_ray_composite 消费）。
//   · 老链这条空转的 march 每次全屏写一遍中性值，画面无影响；要彻底省掉就
//     把下面返回值改成 false，但要确认 volRestirEnabled() == 0 或另找消费者。
// =============================================================================
static bool godRayHazePassEnabled()
{
    // 2026-11-20（用户裁决：回到早期路线）：这一遍重新设为**主路径**。
    //
    // ⚠ 上述"主路径"的前提不成立（见上方订正）：老链在 VL_AIR_GODRAYS=0 下
    // 只写中性值。保留 true 仅是为了维持 god_ray_composite 的调度与 ReSTIR
    // 体积光的消费链，不作为雾的来源。
    //
    // 保留环境变量做 A/B：ORION_GODRAY_LEGACY=0 可强制关掉。
    static const bool legacy = []()
    {
        const char* raw = std::getenv("ORION_GODRAY_LEGACY");

        if(raw != nullptr && raw[0] == '0')
        {
            return false;
        }
        return true;
    }();

    return legacy;
}

void recordVolumetricClouds(VkCommandBuffer cmd)
{
    // 闸门 ORION_CLOUD_SKIP_WHEN_OFF（默认开）：g_cloudEnabled=false 时跳过整条
    // 云链（云影图 / sunVis 体积 / 雾 froxel / march / resolve / 历史 copy / 云合成）。
    // 下游仍有两个消费者在云关闭后采样云图：AE 测光直方图（resolved.a 判云/天空，
    // rr_ae_histogram.comp:117-122）与 RR guide-relax（resolved+depth，
    // rr_guide_relax.comp:107-116）。跳过链后这些图会停在上一帧旧内容，因此在
    // enabled→disabled 的那一帧补一次清空（只一次，不每帧清）：resolved 写成
    // {0,0,0,1}（a=1 = 透射率 1 = 无云，两处 shader 均据此判无云），depth 写成
    // {1000000,0,0,0}（远到 guide-relax 的 farCloudDistance 条件不成立）。
    // 雾屏幕图 / 历史图只被云链内部 descriptor set 绑定，下游无消费者，不清。
    // 回退：ORION_CLOUD_SKIP_WHEN_OFF=0 恢复照旧录制整条链。
    static const bool skipWhenOff = []()
    {
        const char* raw = std::getenv("ORION_CLOUD_SKIP_WHEN_OFF");
        return !(raw != nullptr && raw[0] == '0');
    }();

    const bool cloudActive = !(skipWhenOff && !g_cloudEnabled.load());
    static bool s_cloudWasActive = true;
    static bool s_cloudClearedWhileOff = false;

    if(!cloudActive)
    {
        if(cmd != VK_NULL_HANDLE &&
           (!s_cloudWasActive || !s_cloudClearedWhileOff) &&
           g_volumetricCloudResolvedImage != VK_NULL_HANDLE &&
           g_volumetricCloudDepthImage != VK_NULL_HANDLE)
        {
            // 一次性清空 resolved + depth，防止下游读到上一帧云残留。这处只在
            // enabled→disabled 那一帧跑一次，不在热路径，src 放宽没有热路径成本。
            // 规范要求 srcAccessMask 的每一位都被 srcStageMask 支持：TOP_OF_PIPE 是
            // 纯执行阶段、不支持任何读写位。按 oldLayout 分两组提交，别一刀切：
            //   非 UNDEFINED（有上一帧旧读）：src ALL_COMMANDS + SHADER_READ，读方可能
            //   来自 RT/COMPUTE，stage 不确定，用 ALL_COMMANDS 覆盖；
            //   UNDEFINED（无旧内容可读）：src TOP_OF_PIPE + access 0。
            VkImageMemoryBarrier oldBarriers[2]{};
            VkImageMemoryBarrier freshBarriers[2]{};
            uint32_t oldCount = 0u;
            uint32_t freshCount = 0u;
            for(uint32_t i = 0u; i < 2u; ++i)
            {
                const VkImage image =
                    (i == 0u ? g_volumetricCloudResolvedImage
                             : g_volumetricCloudDepthImage);
                const VkImageLayout oldLayout =
                    (i == 0u ? g_volumetricCloudResolvedLayout
                             : g_volumetricCloudDepthLayout);
                VkImageMemoryBarrier& b =
                    (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED
                        ? freshBarriers[freshCount] : oldBarriers[oldCount]);
                b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                b.oldLayout = oldLayout;
                b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.srcAccessMask =
                    oldLayout == VK_IMAGE_LAYOUT_UNDEFINED
                    ? 0u : VK_ACCESS_SHADER_READ_BIT;
                b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                b.image = image;
                b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                b.subresourceRange.levelCount = 1u;
                b.subresourceRange.layerCount = 1u;
                if(oldLayout == VK_IMAGE_LAYOUT_UNDEFINED) { ++freshCount; }
                else { ++oldCount; }
            }
            if(oldCount > 0u)
            {
                vkCmdPipelineBarrier(cmd,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr,
                    oldCount, oldBarriers);
            }
            if(freshCount > 0u)
            {
                vkCmdPipelineBarrier(cmd,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr,
                    freshCount, freshBarriers);
            }

            const VkImageSubresourceRange emptyRange{
                VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u };
            const VkClearColorValue emptyResolved{{0.0f, 0.0f, 0.0f, 1.0f}};
            vkCmdClearColorImage(cmd, g_volumetricCloudResolvedImage,
                VK_IMAGE_LAYOUT_GENERAL, &emptyResolved, 1u, &emptyRange);
            const VkClearColorValue emptyDepth{{1000000.0f, 0.0f, 0.0f, 0.0f}};
            vkCmdClearColorImage(cmd, g_volumetricCloudDepthImage,
                VK_IMAGE_LAYOUT_GENERAL, &emptyDepth, 1u, &emptyRange);

            VkImageMemoryBarrier backBarriers[2]{};
            for(uint32_t i = 0u; i < 2u; ++i)
            {
                backBarriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                backBarriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                backBarriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
                backBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                backBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                backBarriers[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                backBarriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                backBarriers[i].image =
                    (i == 0u ? g_volumetricCloudResolvedImage
                             : g_volumetricCloudDepthImage);
                backBarriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                backBarriers[i].subresourceRange.levelCount = 1u;
                backBarriers[i].subresourceRange.layerCount = 1u;
            }
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr,
                2u, backBarriers);
            g_volumetricCloudResolvedLayout = VK_IMAGE_LAYOUT_GENERAL;
            g_volumetricCloudDepthLayout = VK_IMAGE_LAYOUT_GENERAL;
            s_cloudClearedWhileOff = true;
        }

        s_cloudWasActive = false;

        if(cmd != VK_NULL_HANDLE)
        {
            writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_SNEAR);
            writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_HEIGHT);
        }
    }
    else
    {
        s_cloudWasActive = true;
        s_cloudClearedWhileOff = false;
    }

    maybeHotReloadVolumetricCloudShaders();

    // =====================================================================
    // 2026-11-15：v5 的太阳遮挡走 inline ray query（binding 16 = TLAS）。
    //
    // 崩溃实测：进世界第一帧 g_tlas 还没构建 ⇒ recordSunVisibilityVolume 里的
    // 懒绑定整段被 return 跳过（日志里 [FOG RAY] 一次都没出现）⇒ binding 16 是
    // 未定义描述符 ⇒ GPU 侧 rayQueryProceedEXT 无限循环 ⇒ TDR ⇒ 驱动 AV
    //（hs_err: nvoglv64 ← vulkan-1 ← rtx_backend，注入点 renderItemInHand）。
    //
    // 现在：绑定不再依赖 S1 pass；没有 TLAS 的那一帧直接跳过整条云/雾链。
    // =====================================================================
    if(g_tlas == VK_NULL_HANDLE)
    {
        return;
    }
    if(g_volumetricCloudRenderSet != VK_NULL_HANDLE)
    {
        static VkAccelerationStructureKHR fogTlasBound = VK_NULL_HANDLE;
        static VkDescriptorSet fogTlasBoundSet = VK_NULL_HANDLE;
        static uint64_t fogTlasBoundGen = 0u;
        if(fogTlasBound != g_tlas ||
           fogTlasBoundSet != g_volumetricCloudRenderSet ||
           (kCLOUD_DESCRIPTOR_GENERATION_GUARD &&
            fogTlasBoundGen != g_volumetricCloudSetGeneration))
        {
            VkAccelerationStructureKHR tlasHandle = g_tlas;
            VkWriteDescriptorSetAccelerationStructureKHR tlasInfo{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR
            };
            tlasInfo.accelerationStructureCount = 1u;
            tlasInfo.pAccelerationStructures = &tlasHandle;

            VkWriteDescriptorSet tlasWrite{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
            };
            tlasWrite.dstSet = g_volumetricCloudRenderSet;
            tlasWrite.dstBinding = 16u;
            tlasWrite.descriptorCount = 1u;
            tlasWrite.descriptorType =
                VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            tlasWrite.pNext = &tlasInfo;
            vkUpdateDescriptorSets(g_device, 1u, &tlasWrite, 0u, nullptr);

            fogTlasBound = g_tlas;
            fogTlasBoundSet = g_volumetricCloudRenderSet;
            fogTlasBoundGen = g_volumetricCloudSetGeneration;
            RTLogFlushed("[FOG RAY] TLAS bound (frame-level) gen=" +
                std::to_string(fogTlasBoundGen));
        }
    }

    // Unconditional synchronization for RT/Glass depth & TLAS before clouds or fog.
    if(cmd != VK_NULL_HANDLE)
    {
        VkMemoryBarrier asBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        asBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        asBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;

        VkImageMemoryBarrier preVolBarriers[2]{};
        uint32_t preVolImageCount = 0u;
        if(g_rtOutputImage != VK_NULL_HANDLE)
        {
            VkImageMemoryBarrier& b = preVolBarriers[preVolImageCount++];
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            b.image = g_rtOutputImage;
            b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u };
        }
        if(g_dlssDepthImage != VK_NULL_HANDLE)
        {
            VkImageMemoryBarrier& b = preVolBarriers[preVolImageCount++];
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            b.image = g_dlssDepthImage;
            b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u };
        }
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u, 1u, &asBarrier, 0u, nullptr,
            preVolImageCount, preVolBarriers);
    }

    const bool fogActive = g_fogEnabled.load() && g_cameraInWater.load() == 0u;
    updateCloudLocalLightDescriptors();
    syncCloudSkyLutDescriptor();

    if(cloudActive)
    {
        // Bake cloud shadow map BEFORE fog so volumetric shafts align with real cloud gaps.
        recordCloudShadowMap(cmd);
        rtCheckpoint(cmd, "cloud:shadow");
    }

    if(fogActive || cloudActive)
    {
        recordTerrainHeightMap(cmd);
    }

    if(cloudActive)
    {
    recordSunVisibilityVolume(cmd);
    rtCheckpoint(cmd, "cloud:sunvis");

    if(cmd == VK_NULL_HANDLE || g_volumetricCloudRenderPipelineV3 == VK_NULL_HANDLE ||
       g_volumetricCloudResolvePipeline == VK_NULL_HANDLE ||
       g_volumetricCloudCompositePipeline == VK_NULL_HANDLE)
    {
        if(cmd != VK_NULL_HANDLE)
        {
            writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_SNEAR);
            writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_HEIGHT);
        }
        goto after_clouds;
    }

    recordSunShadowMap(cmd);
    rtCheckpoint(cmd, "cloud:sunshadow");
    writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_SNEAR);

    rtCheckpoint(cmd, "cloud:terrain");
    writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_HEIGHT);
    VkImageMemoryBarrier cloudBarriers[3]{};
    cloudBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    cloudBarriers[0].oldLayout = g_volumetricCloudLayout;
    cloudBarriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    cloudBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    cloudBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    cloudBarriers[0].srcAccessMask = g_volumetricCloudLayout == VK_IMAGE_LAYOUT_UNDEFINED
        ? 0u : VK_ACCESS_SHADER_READ_BIT;
    cloudBarriers[0].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    cloudBarriers[0].image = g_volumetricCloudImage;
    cloudBarriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cloudBarriers[0].subresourceRange.levelCount = 1u;
    cloudBarriers[0].subresourceRange.layerCount = 1u;
    cloudBarriers[1] = cloudBarriers[0];
    cloudBarriers[1].oldLayout = g_volumetricCloudDepthLayout;
    cloudBarriers[1].srcAccessMask = g_volumetricCloudDepthLayout == VK_IMAGE_LAYOUT_UNDEFINED
        ? 0u : VK_ACCESS_SHADER_READ_BIT;
    cloudBarriers[1].image = g_volumetricCloudDepthImage;
    cloudBarriers[2] = cloudBarriers[0];
    cloudBarriers[2].oldLayout = g_volumetricCloudResolvedLayout;
    cloudBarriers[2].srcAccessMask = g_volumetricCloudResolvedLayout == VK_IMAGE_LAYOUT_UNDEFINED
        ? 0u : VK_ACCESS_SHADER_READ_BIT;
    cloudBarriers[2].image = g_volumetricCloudResolvedImage;
    vkCmdPipelineBarrier(cmd,
        g_volumetricCloudLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
            g_volumetricCloudDepthLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
            g_volumetricCloudResolvedLayout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 3u, cloudBarriers);
    g_volumetricCloudLayout = VK_IMAGE_LAYOUT_GENERAL;
    g_volumetricCloudDepthLayout = VK_IMAGE_LAYOUT_GENERAL;
    g_volumetricCloudResolvedLayout = VK_IMAGE_LAYOUT_GENERAL;

    if(g_volumetricCloudHistoryNeedsClear.exchange(false))
    {
        VkImageMemoryBarrier historyBarriers[2]{};
        historyBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        historyBarriers[0].oldLayout = g_volumetricCloudHistoryLayout;
        historyBarriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        historyBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        historyBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        historyBarriers[0].srcAccessMask = g_volumetricCloudHistoryLayout == VK_IMAGE_LAYOUT_UNDEFINED
            ? 0u : VK_ACCESS_SHADER_READ_BIT;
        historyBarriers[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        historyBarriers[0].image = g_volumetricCloudHistoryImage;
        historyBarriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        historyBarriers[0].subresourceRange.levelCount = 1u;
        historyBarriers[0].subresourceRange.layerCount = 1u;
        historyBarriers[1] = historyBarriers[0];
        historyBarriers[1].oldLayout = g_volumetricCloudHistoryDepthLayout;
        historyBarriers[1].srcAccessMask = g_volumetricCloudHistoryDepthLayout == VK_IMAGE_LAYOUT_UNDEFINED
            ? 0u : VK_ACCESS_SHADER_READ_BIT;
        historyBarriers[1].image = g_volumetricCloudHistoryDepthImage;
        vkCmdPipelineBarrier(cmd,
            g_volumetricCloudHistoryLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
                g_volumetricCloudHistoryDepthLayout == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 2u, historyBarriers);
        const VkClearColorValue emptyCloud = {{0.0f, 0.0f, 0.0f, 1.0f}};
        vkCmdClearColorImage(cmd, g_volumetricCloudHistoryImage, VK_IMAGE_LAYOUT_GENERAL,
            &emptyCloud, 1u, &historyBarriers[0].subresourceRange);
        const VkClearColorValue emptyCloudDepth = {{1000000.0f, 0.0f, 0.0f, 0.0f}};
        vkCmdClearColorImage(cmd, g_volumetricCloudHistoryDepthImage, VK_IMAGE_LAYOUT_GENERAL,
            &emptyCloudDepth, 1u, &historyBarriers[1].subresourceRange);
        for(uint32_t i = 0u; i < 2u; ++i)
        {
            historyBarriers[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            historyBarriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 2u, historyBarriers);
        g_volumetricCloudHistoryLayout = VK_IMAGE_LAYOUT_GENERAL;
        g_volumetricCloudHistoryDepthLayout = VK_IMAGE_LAYOUT_GENERAL;
    }

    // 2026-11-22 (Batch 2): preVolBarriers at lines 4945-4984 already synchronized
    // ALL_COMMANDS -> COMPUTE_SHADER on g_rtOutputImage & g_dlssDepthImage with zero
    // RT commands issued since. Rollback: set kCLOUD_SKIP_REDUNDANT_RT_BARRIER = false.
    static constexpr bool kCLOUD_SKIP_REDUNDANT_RT_BARRIER = true;
    if(!kCLOUD_SKIP_REDUNDANT_RT_BARRIER)
    {
        VkImageMemoryBarrier sceneBarriers[2]{};
        for(uint32_t i = 0u; i < 2u; ++i)
        {
            sceneBarriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            sceneBarriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            sceneBarriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            sceneBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            sceneBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            sceneBarriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            sceneBarriers[i].dstAccessMask = i == 0u
                ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
                : VK_ACCESS_SHADER_READ_BIT;
            sceneBarriers[i].image = i == 0u ? g_rtOutputImage : g_dlssDepthImage;
            sceneBarriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            sceneBarriers[i].subresourceRange.levelCount = 1u;
            sceneBarriers[i].subresourceRange.layerCount = 1u;
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 2u, sceneBarriers);
    }

    const VkPipeline cloudRenderPipeline = g_volumetricCloudRenderPipelineV3;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cloudRenderPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_volumetricCloudRenderPipelineLayout, 0u, 1u, &g_volumetricCloudRenderSet,
        0u, nullptr);
    const uint32_t temporalMode = g_cloudTemporalMode.load();
    const uint32_t marchWidth = (temporalMode >= 1u)
        ? (g_volumetricCloudWidth + 1u) / 2u : g_volumetricCloudWidth;
    const uint32_t marchHeight = (temporalMode >= 2u)
        ? (g_volumetricCloudHeight + 1u) / 2u : g_volumetricCloudHeight;
    vkCmdDispatch(cmd, (marchWidth + 7u) / 8u, (marchHeight + 7u) / 8u, 1u);

    rtCheckpoint(cmd, "cloud:march");

    VkImageMemoryBarrier resolveBarriers[2]{};
    resolveBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    resolveBarriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    resolveBarriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    resolveBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    resolveBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    resolveBarriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    resolveBarriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    resolveBarriers[0].image = g_volumetricCloudImage;
    resolveBarriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    resolveBarriers[0].subresourceRange.levelCount = 1u;
    resolveBarriers[0].subresourceRange.layerCount = 1u;
    resolveBarriers[1] = resolveBarriers[0];
    resolveBarriers[1].image = g_volumetricCloudDepthImage;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 2u, resolveBarriers);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_volumetricCloudResolvePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_volumetricCloudResolvePipelineLayout, 0u, 1u, &g_volumetricCloudResolveSet,
        0u, nullptr);
    vkCmdDispatch(cmd, (g_volumetricCloudWidth + 7u) / 8u,
        (g_volumetricCloudHeight + 7u) / 8u, 1u);

    rtCheckpoint(cmd, "cloud:resolve");

    // 2026-11-13：云 atrous 去噪调度块删除。它只服务已退役的"远云脱离RR"档
    // （postRrComposite 开启时才跑），该档把云合成写进 RR 输入图导致切回旧档
    // 黑屏；atrous 管线/描述符资源仍随云资源一起创建/销毁（无害保留）。
    // kCloudAtrousIterations 常量保留在 OrionInternal.h 供回退参考。

    VkImageMemoryBarrier copyBarriers[4]{};
    copyBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    copyBarriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    copyBarriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    copyBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    copyBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    copyBarriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    copyBarriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    copyBarriers[0].image = g_volumetricCloudResolvedImage;
    copyBarriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyBarriers[0].subresourceRange.levelCount = 1u;
    copyBarriers[0].subresourceRange.layerCount = 1u;
    copyBarriers[1] = copyBarriers[0];
    copyBarriers[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    copyBarriers[1].image = g_volumetricCloudDepthImage;
    copyBarriers[2] = copyBarriers[0];
    copyBarriers[2].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    copyBarriers[2].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    copyBarriers[2].image = g_volumetricCloudHistoryImage;
    copyBarriers[3] = copyBarriers[2];
    copyBarriers[3].image = g_volumetricCloudHistoryDepthImage;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 4u, copyBarriers);

    VkImageCopy historyCopy{};
    historyCopy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    historyCopy.srcSubresource.layerCount = 1u;
    historyCopy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    historyCopy.dstSubresource.layerCount = 1u;
    historyCopy.extent = {g_volumetricCloudWidth, g_volumetricCloudHeight, 1u};
    vkCmdCopyImage(cmd, g_volumetricCloudResolvedImage, VK_IMAGE_LAYOUT_GENERAL,
        g_volumetricCloudHistoryImage, VK_IMAGE_LAYOUT_GENERAL, 1u, &historyCopy);
    vkCmdCopyImage(cmd, g_volumetricCloudDepthImage, VK_IMAGE_LAYOUT_GENERAL,
        g_volumetricCloudHistoryDepthImage, VK_IMAGE_LAYOUT_GENERAL, 1u, &historyCopy);

    for(uint32_t i = 0u; i < 2u; ++i)
    {
        copyBarriers[i].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        copyBarriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    for(uint32_t i = 2u; i < 4u; ++i)
    {
        copyBarriers[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        copyBarriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 4u, copyBarriers);

    if(g_aeStateBuffer != VK_NULL_HANDLE)
    {
        VkBufferMemoryBarrier aeStateBarrier{};
        aeStateBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        aeStateBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aeStateBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        aeStateBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        aeStateBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        aeStateBarrier.buffer = g_aeStateBuffer;
        aeStateBarrier.offset = 0u;
        aeStateBarrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 1u,
            &aeStateBarrier, 0u, nullptr);
    }

    // 2026-11-13：pre-RR 云合成成为唯一路径（"远云脱离RR"开关已退役）。
    {
        // 2026-11-16：binding 1/6 会随 RT / RR 分辨率变化；这里与远带共用同一套
        // 句柄跟踪，避免"描述符冻死指向已销毁视图"（11-14 已踩过两次）。
        refreshCloudCompositeTargets();

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_volumetricCloudCompositePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            g_volumetricCloudCompositePipelineLayout, 0u, 1u, &g_volumetricCloudCompositeSet,
            0u, nullptr);
        CloudCompositePush compositePush{};
        compositePush.splitDistance =
            cloudFarBandSplitActive() ? cloudFarBandSplitDistance() : 0.0f;
        compositePush.mode = 0.0f;
        // 2026-11-15：白点不再从 AeState.cloudWhite 取（那会形成
        // "云亮 → AE 压曝光 → 白点被拉大 → 云被压回白点" 的自反馈环）。
        compositePush.whiteScale =
            (hdrOutputActive() ? AE_HDR_CLOUD_WHITE_SCALE : 1.0f) *
            g_cloudShoulderScale;
        // 与下游曝光强度同口径。2026-11-23 起 rr_hdr_encode.comp 也改读
        // PostPush.exposureStrength（不再用编译期常量 0.6），所以 SDR/HDR
        // 统一走 UI 的"自动曝光强度"。ORION_CLOUD_SHOULDER_STRENGTH 仍可
        // 强制成旧口径（1.0）做 A/B。
        compositePush.exposureStrength =
            g_cloudShoulderStrength >= 0.0f
                ? g_cloudShoulderStrength
                : g_postExposureStrength.load();
        vkCmdPushConstants(cmd, g_volumetricCloudCompositePipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(compositePush), &compositePush);
        vkCmdDispatch(cmd, (g_rtOutputWidth + 7u) / 8u,
            (g_rtOutputHeight + 7u) / 8u, 1u);

        // 本帧云链已经产出新的 resolved 云缓冲，远带合成可以安全复用。
        g_cloudCompositeFrameSerial = g_perfL10FrameSerial;

        rtCheckpoint(cmd, "cloud:composite");
    }
    } // if(cloudActive)

after_clouds:
    // Unified Volumetric Fog & Volumetric Light pipeline:
    // Runs AFTER near-band clouds and composites directly onto g_rtOutputImage
    // at full RT resolution with exact per-pixel sceneDepth, identically whether
    // volumetric clouds are ON or OFF.
    if(fogActive)
    {
        recordFogFroxelPasses(cmd);
        rtCheckpoint(cmd, "cloud:fogfroxel");
        recordFogScreenApply(cmd);
        rtCheckpoint(cmd, "cloud:fogapply");
    }

    // Underwater god rays (volumetric_light.comp) & VolReSTIR composite (god_ray_composite.comp).
    // Air god rays are handled by the unified froxel pipeline above; skip this entire block
    // when neither VolReSTIR nor underwater god rays are active (avoids idle full-screen passes).
    const bool underwaterGodRayActive =
        (g_cameraInWater.load() != 0u && g_godRayEnabled.load());
    const bool volRestirCompositeActive =
        (volRestirEnabled() != 0u &&
         g_volRestirCompositePipeline != VK_NULL_HANDLE &&
         g_volRestirCompositeSet != VK_NULL_HANDLE &&
         g_volRestirIntegratePipeline != VK_NULL_HANDLE);

    if((underwaterGodRayActive || volRestirCompositeActive) &&
       g_tlas != VK_NULL_HANDLE &&
       g_godRayCompositePipeline != VK_NULL_HANDLE &&
       g_godRayCompositeSet != VK_NULL_HANDLE &&
       g_godRayTargetImage != VK_NULL_HANDLE &&
       g_godRayWidth > 0u && g_godRayHeight > 0u)
    {
        VkImageMemoryBarrier godRayBarriers[2]{};
        for(VkImageMemoryBarrier& barrier : godRayBarriers)
        {
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1u;
            barrier.subresourceRange.layerCount = 1u;
        }
        godRayBarriers[0].srcAccessMask =
            VK_ACCESS_SHADER_READ_BIT |
            VK_ACCESS_SHADER_WRITE_BIT |
            VK_ACCESS_TRANSFER_READ_BIT;
        godRayBarriers[0].dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT |
            VK_ACCESS_SHADER_WRITE_BIT |
            VK_ACCESS_TRANSFER_WRITE_BIT;
        godRayBarriers[0].image = g_godRayTargetImage;
        godRayBarriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        godRayBarriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        godRayBarriers[1].image = g_godRayHistoryImage;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            0u, 0u, nullptr, 0u, nullptr, 2u,
            godRayBarriers);

        if(g_godRayHistoryNeedsClear.exchange(false) &&
           g_godRayHistoryImage != VK_NULL_HANDLE)
        {
            VkImageSubresourceRange godRayRange{};
            godRayRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            godRayRange.levelCount = 1u;
            godRayRange.layerCount = 1u;
            const VkClearColorValue godRayNeutral{{0.0f, 0.0f, 0.0f, 1.0f}};
            vkCmdClearColorImage(cmd, g_godRayHistoryImage,
                VK_IMAGE_LAYOUT_GENERAL, &godRayNeutral, 1u, &godRayRange);
            VkImageMemoryBarrier godRayClearBarrier{};
            godRayClearBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            godRayClearBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            godRayClearBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            godRayClearBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            godRayClearBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            godRayClearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            godRayClearBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            godRayClearBarrier.image = g_godRayHistoryImage;
            godRayClearBarrier.subresourceRange = godRayRange;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u,
                &godRayClearBarrier);
        }

        {
            static VkDescriptorSet lastGodRayTlasSet = VK_NULL_HANDLE;
            static VkAccelerationStructureKHR lastGodRayTlas = VK_NULL_HANDLE;
            static uint64_t lastGodRayTlasGen = 0u;

            if(g_tlas != VK_NULL_HANDLE &&
               (g_volumetricLightSet != lastGodRayTlasSet ||
                g_tlas != lastGodRayTlas ||
                (kCLOUD_DESCRIPTOR_GENERATION_GUARD &&
                 lastGodRayTlasGen != g_volumetricCloudSetGeneration)))
            {
                VkAccelerationStructureKHR tlasHandle = g_tlas;
                VkWriteDescriptorSetAccelerationStructureKHR tlasDescriptor{};
                tlasDescriptor.sType =
                    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
                tlasDescriptor.accelerationStructureCount = 1u;
                tlasDescriptor.pAccelerationStructures = &tlasHandle;

                VkWriteDescriptorSet tlasWrite{};
                tlasWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                tlasWrite.pNext = &tlasDescriptor;
                tlasWrite.dstSet = g_volumetricLightSet;
                tlasWrite.dstBinding = 9u;
                tlasWrite.descriptorCount = 1u;
                tlasWrite.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

                vkUpdateDescriptorSets(g_device, 1u, &tlasWrite, 0u, nullptr);

                RTLog(
                    "[GODRAY TLAS] binding 9 rebound set=" +
                    std::to_string(
                        static_cast<unsigned long long>(
                            reinterpret_cast<uintptr_t>(g_volumetricLightSet))) +
                    " tlas=" +
                    std::to_string(
                        static_cast<unsigned long long>(
                            reinterpret_cast<uintptr_t>(g_tlas))) +
                    " instances=" +
                    std::to_string(g_tlasInstanceCount)
                );

                lastGodRayTlasSet = g_volumetricLightSet;
                lastGodRayTlas = g_tlas;
                lastGodRayTlasGen = g_volumetricCloudSetGeneration;
            }
        }

        {
            VkMemoryBarrier asReadBarrier{};
            asReadBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            asReadBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
            asReadBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;

            vkCmdPipelineBarrier(
                cmd,
                VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0u,
                1u,
                &asReadBarrier,
                0u,
                nullptr,
                0u,
                nullptr
            );
        }

        // Optional diagnostic ray self-test (disabled by default; enable via ORION_RAY_SELFTEST=1).
        static const bool raySelfTestEnabled = []()
        {
            const char* raw = std::getenv("ORION_RAY_SELFTEST");
            return raw != nullptr && raw[0] == '1';
        }();
        if(raySelfTestEnabled && volRestirEnabled() == 0u)
        {
            static VkBuffer selfTestBuffer = VK_NULL_HANDLE;
            static VkDeviceMemory selfTestMemory = VK_NULL_HANDLE;
            static float* selfTestMapped = nullptr;
            static VkDescriptorSetLayout selfTestSetLayout = VK_NULL_HANDLE;
            static VkDescriptorPool selfTestPool = VK_NULL_HANDLE;
            static VkDescriptorSet selfTestSet = VK_NULL_HANDLE;
            static VkPipelineLayout selfTestPipelineLayout = VK_NULL_HANDLE;
            static VkPipeline selfTestPipeline = VK_NULL_HANDLE;
            static bool selfTestBroken = false;
            static VkDescriptorSet selfTestBoundSet = VK_NULL_HANDLE;
            static VkAccelerationStructureKHR selfTestBoundTlas = VK_NULL_HANDLE;
            static uint32_t selfTestLastLoggedSerial = 0xFFFFFFFFu;

            if(!selfTestBroken && selfTestPipeline == VK_NULL_HANDLE)
            {
                try
                {
                    createBuffer(
                        g_device,
                        g_physicalDevice,
                        64u,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        selfTestBuffer,
                        selfTestMemory
                    );

                    void* mapped = nullptr;
                    check(
                        vkMapMemory(g_device, selfTestMemory, 0u, VK_WHOLE_SIZE, 0u,
                            &mapped),
                        "ray self test map failed"
                    );
                    selfTestMapped = static_cast<float*>(mapped);

                    VkDescriptorSetLayoutBinding bindings[3]{};
                    bindings[0] = { 0u, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u,
                        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
                    bindings[1] = { 1u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u,
                        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
                    bindings[2] = { 2u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
                        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

                    VkDescriptorSetLayoutCreateInfo setLayoutInfo{
                        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
                    };
                    setLayoutInfo.bindingCount = 3u;
                    setLayoutInfo.pBindings = bindings;
                    check(
                        vkCreateDescriptorSetLayout(g_device, &setLayoutInfo, nullptr,
                            &selfTestSetLayout),
                        "ray self test set layout failed"
                    );

                    VkDescriptorPoolSize poolSizes[3] = {
                        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u },
                        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u },
                        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u }
                    };
                    VkDescriptorPoolCreateInfo poolInfo{
                        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
                    };
                    poolInfo.maxSets = 1u;
                    poolInfo.poolSizeCount = 3u;
                    poolInfo.pPoolSizes = poolSizes;
                    check(
                        vkCreateDescriptorPool(g_device, &poolInfo, nullptr,
                            &selfTestPool),
                        "ray self test pool failed"
                    );

                    VkDescriptorSetAllocateInfo allocInfo{
                        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
                    };
                    allocInfo.descriptorPool = selfTestPool;
                    allocInfo.descriptorSetCount = 1u;
                    allocInfo.pSetLayouts = &selfTestSetLayout;
                    check(
                        vkAllocateDescriptorSets(g_device, &allocInfo, &selfTestSet),
                        "ray self test descriptor set failed"
                    );

                    VkPipelineLayoutCreateInfo layoutInfo{
                        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
                    };
                    layoutInfo.setLayoutCount = 1u;
                    layoutInfo.pSetLayouts = &selfTestSetLayout;
                    check(
                        vkCreatePipelineLayout(g_device, &layoutInfo, nullptr,
                            &selfTestPipelineLayout),
                        "ray self test pipeline layout failed"
                    );

                    const std::vector<uint8_t> code =
                        loadSpv("shaders/ray_selftest.comp.spv");
                    VkShaderModule module = createShaderModule(g_device, code);

                    VkPipelineShaderStageCreateInfo stage{
                        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
                    };
                    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                    stage.module = module;
                    stage.pName = "main";

                    VkComputePipelineCreateInfo pipelineInfo{
                        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
                    };
                    pipelineInfo.stage = stage;
                    pipelineInfo.layout = selfTestPipelineLayout;

                    const VkResult pipelineResult = vkCreateComputePipelines(
                        g_device,
                        getSharedPipelineCache(),
                        1u,
                        &pipelineInfo,
                        nullptr,
                        &selfTestPipeline
                    );
                    vkDestroyShaderModule(g_device, module, nullptr);
                    check(pipelineResult, "ray self test pipeline failed");

                    RTLog("[RAYTEST] pipeline ready");
                }
                catch(...)
                {
                    selfTestBroken = true;
                    RTLog("[RAYTEST] setup failed (见 stderr)");
                }
            }

            if(selfTestPipeline != VK_NULL_HANDLE &&
               g_tlas != VK_NULL_HANDLE &&
               g_cameraUboBuffer != VK_NULL_HANDLE &&
               selfTestMapped != nullptr)
            {
                if(selfTestSet != selfTestBoundSet || g_tlas != selfTestBoundTlas)
                {
                    VkAccelerationStructureKHR tlasHandle = g_tlas;
                    VkWriteDescriptorSetAccelerationStructureKHR asi{
                        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR
                    };
                    asi.accelerationStructureCount = 1u;
                    asi.pAccelerationStructures = &tlasHandle;

                    VkDescriptorBufferInfo resultInfo{};
                    resultInfo.buffer = selfTestBuffer;
                    resultInfo.offset = 0u;
                    resultInfo.range = VK_WHOLE_SIZE;

                    VkDescriptorBufferInfo cameraInfo{};
                    cameraInfo.buffer = g_cameraUboBuffer;
                    cameraInfo.offset = 0u;
                    cameraInfo.range = VK_WHOLE_SIZE;

                    VkWriteDescriptorSet writes[3]{};
                    writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &asi,
                        selfTestSet, 0u, 0u, 1u,
                        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                        nullptr, nullptr, nullptr };
                    writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                        selfTestSet, 1u, 0u, 1u,
                        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &resultInfo, nullptr };
                    writes[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                        selfTestSet, 2u, 0u, 1u,
                        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &cameraInfo, nullptr };

                    vkUpdateDescriptorSets(g_device, 3u, writes, 0u, nullptr);

                    selfTestBoundSet = selfTestSet;
                    selfTestBoundTlas = g_tlas;
                }

                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, selfTestPipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    selfTestPipelineLayout, 0u, 1u, &selfTestSet, 0u, nullptr);
                vkCmdDispatch(cmd, 1u, 1u, 1u);

                rtCheckpoint(cmd, "cloud:raytest");

                VkMemoryBarrier hostBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                hostBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_HOST_BIT, 0u, 1u, &hostBarrier, 0u, nullptr, 0u,
                    nullptr);

                if(g_perfL10FrameSerial % 600u == 0u &&
                   selfTestLastLoggedSerial != g_perfL10FrameSerial)
                {
                    selfTestLastLoggedSerial = g_perfL10FrameSerial;

                    RTLog(
                        "[RAYTEST] camY=" +
                        std::to_string(selfTestMapped[7]) +
                        " | 向下射线命中(1/0) m04[TS]=" +
                        std::to_string(selfTestMapped[0]) +
                        " m04[T]=" + std::to_string(selfTestMapped[1]) +
                        " m04[none]=" + std::to_string(selfTestMapped[2]) +
                        " mFF[TS]=" + std::to_string(selfTestMapped[3]) +
                        " mFF[T]=" + std::to_string(selfTestMapped[4]) +
                        " mFF[none]=" + std::to_string(selfTestMapped[5]) +
                        " | 组合0命中距离=" + std::to_string(selfTestMapped[6])
                    );
                }
            }
        }

        static bool s_godRayDispatchLogged = false;
        const bool godRayWillDispatch =
            godRayHazePassEnabled() &&
            underwaterGodRayActive &&
            g_volumetricLightPipeline != VK_NULL_HANDLE &&
            g_volumetricLightSet != VK_NULL_HANDLE;
        if(!s_godRayDispatchLogged)
        {
            s_godRayDispatchLogged = true;
            RTLog("[GODRAY] gate enabled=" +
                std::to_string(godRayHazePassEnabled() ? 1 : 0) +
                " volRestir=" + std::to_string(volRestirEnabled()) +
                " pipe=" + std::to_string(g_volumetricLightPipeline != VK_NULL_HANDLE ? 1 : 0) +
                " set=" + std::to_string(g_volumetricLightSet != VK_NULL_HANDLE ? 1 : 0) +
                " master=" + std::to_string(g_godRayEnabled.load() ? 1 : 0) +
                " target=" + std::to_string(g_godRayWidth) + "x" + std::to_string(g_godRayHeight) +
                " dispatch=" + std::to_string(godRayWillDispatch ? 1 : 0));
        }

        if(godRayWillDispatch)
        {
            if(volRestirEnabled() == 0u)
            {
                VkImageSubresourceRange clearRange{};
                clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                clearRange.levelCount = 1u;
                clearRange.layerCount = 1u;
                const VkClearColorValue neutralColor{{0.0f, 0.0f, 0.0f, 1.0f}};
                vkCmdClearColorImage(cmd, g_godRayTargetImage,
                    VK_IMAGE_LAYOUT_GENERAL, &neutralColor, 1u, &clearRange);
                VkImageMemoryBarrier clearToCompute{};
                clearToCompute.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                clearToCompute.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                clearToCompute.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                clearToCompute.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                clearToCompute.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                clearToCompute.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                clearToCompute.dstAccessMask =
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                clearToCompute.image = g_godRayTargetImage;
                clearToCompute.subresourceRange = clearRange;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr,
                    1u, &clearToCompute);
            }

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_volumetricLightPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                g_volumetricLightPipelineLayout, 0u, 1u, &g_volumetricLightSet, 0u, nullptr);
            vkCmdDispatch(cmd, (g_godRayWidth + 7u) / 8u, (g_godRayHeight + 7u) / 8u, 1u);

            rtCheckpoint(cmd, "cloud:vollight");
        }

        VkImageMemoryBarrier godRayReadBarrier{};
        godRayReadBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        godRayReadBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        godRayReadBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        godRayReadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        godRayReadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        godRayReadBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        godRayReadBarrier.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        godRayReadBarrier.image = g_godRayTargetImage;
        godRayReadBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        godRayReadBarrier.subresourceRange.levelCount = 1u;
        godRayReadBarrier.subresourceRange.layerCount = 1u;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            0u, 0u, nullptr, 0u, nullptr, 1u, &godRayReadBarrier);

        VkImageMemoryBarrier godRaySceneBarrier = godRayReadBarrier;
        godRaySceneBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        godRaySceneBarrier.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        godRaySceneBarrier.image = g_rtOutputImage;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u,
            &godRaySceneBarrier);

        // 2026-11-18：与上面的 march 同步跳过（否则这里会把上一帧的陈旧内容
        // 合成进画面）。
        if(godRayHazePassEnabled())
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_godRayCompositePipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                g_godRayCompositePipelineLayout, 0u, 1u, &g_godRayCompositeSet, 0u, nullptr);
            vkCmdDispatch(cmd, (g_rtOutputWidth + 7u) / 8u, (g_rtOutputHeight + 7u) / 8u, 1u);

            rtCheckpoint(cmd, "cloud:godray");
        }

        if(godRayHazePassEnabled() &&
           volRestirEnabled() == 0u &&
           g_godRayHistoryImage != VK_NULL_HANDLE &&
           g_godRayWidth == g_rtOutputWidth &&
           g_godRayHeight == g_rtOutputHeight)
        {
            VkImageCopy godRayCopy{};
            godRayCopy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            godRayCopy.srcSubresource.layerCount = 1u;
            godRayCopy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            godRayCopy.dstSubresource.layerCount = 1u;
            godRayCopy.extent = { g_godRayWidth, g_godRayHeight, 1u };
            vkCmdCopyImage(cmd, g_godRayTargetImage, VK_IMAGE_LAYOUT_GENERAL,
                g_godRayHistoryImage, VK_IMAGE_LAYOUT_GENERAL, 1u, &godRayCopy);
        }

        static const bool godRayProbeOn = []()
        {
            const char* raw = std::getenv("ORION_GODRAY_PROBE");
            return raw != nullptr && raw[0] == '1';
        }();
        if(godRayProbeOn)
        {
            godRayProbe(cmd);
        }
    }
}

// ============================================================
// 2026-11-16：远带云在 RR 之后按显示分辨率合成。
//
// 为什么放这里：RR 的输出图（display 分辨率）只有 slEvaluate 之后才有效。
// 云是低频内容，不需要 RR 的去噪；而在超性能档下让 RR 把 render 分辨率的
// 云上采样 3~4 倍，等于把云糊成一片。距离分带后：
//   · 近带（云命中距离 < splitDistance）留在 RR 输入，交给 RR 在同一分辨率、
//     同一时域里处理 ⇒ 云穿过建筑 / 飞进云里这类深度交叠不会缺遮挡；
//   · 远带在这里直接合成 ⇒ 细节上限由云 pass 分辨率决定，与 RR 挡位解耦。
//
// 遮挡：shader 里用"render 深度 footprint 取 min"的保守判据（见
// farBandMinSceneDistance），宁可轮廓外少画 1~2 像素，也不把云溢到前景。
// ============================================================

void recordCloudFarBandComposite(VkCommandBuffer cmd)
{
    if(cmd == VK_NULL_HANDLE || !cloudFarBandSplitActive())
    {
        return;
    }

    if(g_volumetricCloudCompositePipeline == VK_NULL_HANDLE ||
       g_volumetricCloudCompositeSet == VK_NULL_HANDLE ||
       g_volumetricCloudCompositePipelineLayout == VK_NULL_HANDLE ||
       g_dlssOutputView == VK_NULL_HANDLE ||
       g_volumetricCloudResolvedImage == VK_NULL_HANDLE ||
       g_dlssOutputWidth == 0u ||
       g_dlssOutputHeight == 0u)
    {
        return;
    }

    {
        static bool s_farBandEnterLogged = false;

        if(!s_farBandEnterLogged)
        {
            s_farBandEnterLogged = true;

            RTLogFlushed(
                "[CLOUD FARBAND] armed split=" +
                std::to_string(cloudFarBandSplitDistance()) +
                " output=" + std::to_string(g_dlssOutputWidth) + "x" +
                std::to_string(g_dlssOutputHeight) +
                " render=" + std::to_string(g_rtOutputWidth) + "x" +
                std::to_string(g_rtOutputHeight));
        }
    }

    // 2026-11-16：只在本帧云链真的跑过时合成，否则会把上一帧的云再叠一遍。
    if(g_cloudCompositeFrameSerial != g_perfL10FrameSerial)
    {
        return;
    }

    // 本帧 RR/SR 必须真的产出了新输出（eWarnOutOfVRAM 会沿用旧图）。
    if(!g_slOutputFreshThisFrame.load(std::memory_order_relaxed))
    {
        return;
    }

    // 屏障假设 GENERAL->GENERAL；RR 刚切换 / 布局未就绪的帧直接跳过。
    if(g_dlssOutputLayout != VK_IMAGE_LAYOUT_GENERAL)
    {
        return;
    }

    refreshCloudCompositeTargets();

    {
        // 过了全部守卫 ⇒ 本帧真的会发这一发 dispatch。
        static bool s_farBandDispatchLogged = false;

        if(!s_farBandDispatchLogged)
        {
            s_farBandDispatchLogged = true;

            RTLogFlushed(
                "[CLOUD FARBAND] dispatch split=" +
                std::to_string(cloudFarBandSplitDistance()) +
                " output=" + std::to_string(g_dlssOutputWidth) + "x" +
                std::to_string(g_dlssOutputHeight) +
                " cloud=" + std::to_string(g_volumetricCloudWidth) + "x" +
                std::to_string(g_volumetricCloudHeight) +
                " render=" + std::to_string(g_rtOutputWidth) + "x" +
                std::to_string(g_rtOutputHeight));
        }
    }

    VkImageMemoryBarrier barriers[2]{};

    for(VkImageMemoryBarrier& barrier : barriers)
    {
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.srcAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1u;
        barrier.subresourceRange.layerCount = 1u;
    }

    barriers[0].image = g_dlssOutputImage;
    barriers[1].image = g_volumetricCloudResolvedImage;
    barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0u, 0u, nullptr, 0u, nullptr, 2u, barriers);

    CloudCompositePush push{};
    push.splitDistance = cloudFarBandSplitDistance();
    push.mode = 1.0f;
    push.whiteScale =
        (hdrOutputActive() ? AE_HDR_CLOUD_WHITE_SCALE : 1.0f) *
        g_cloudShoulderScale;
    // 同 compositePush 分支：HDR/SDR 都用 UI 的自动曝光强度（2026-11-23）。
    push.exposureStrength =
        g_cloudShoulderStrength >= 0.0f
            ? g_cloudShoulderStrength
            : g_postExposureStrength.load();

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_volumetricCloudCompositePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_volumetricCloudCompositePipelineLayout, 0u, 1u,
        &g_volumetricCloudCompositeSet, 0u, nullptr);
    vkCmdPushConstants(cmd, g_volumetricCloudCompositePipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(push), &push);
    vkCmdDispatch(cmd,
        (g_dlssOutputWidth + 7u) / 8u,
        (g_dlssOutputHeight + 7u) / 8u,
        1u);
}

void destroyVolumetricCloudResources()
{
    if(g_device != VK_NULL_HANDLE)
    {
        const VkResult idleResult = vkDeviceWaitIdle(g_device);
        if(idleResult != VK_SUCCESS)
        {
            RTLog("[CLOUD] vkDeviceWaitIdle before rebuild returned " +
                std::to_string(static_cast<int>(idleResult)));
            logDeviceFaultDiagnostics("cloudRebuild/vkDeviceWaitIdle");
        }
    }
    if(g_volumetricCloudRenderPipelineV3) vkDestroyPipeline(g_device, g_volumetricCloudRenderPipelineV3, nullptr);
    if(g_volumetricCloudResolvePipeline) vkDestroyPipeline(g_device, g_volumetricCloudResolvePipeline, nullptr);
    if(g_volumetricCloudCompositePipeline) vkDestroyPipeline(g_device, g_volumetricCloudCompositePipeline, nullptr);
    if(g_volumetricLightPipeline) vkDestroyPipeline(g_device, g_volumetricLightPipeline, nullptr);
    if(g_volumetricCloudRenderPipelineLayout) vkDestroyPipelineLayout(g_device, g_volumetricCloudRenderPipelineLayout, nullptr);
    if(g_volumetricCloudResolvePipelineLayout) vkDestroyPipelineLayout(g_device, g_volumetricCloudResolvePipelineLayout, nullptr);
    if(g_volumetricCloudCompositePipelineLayout) vkDestroyPipelineLayout(g_device, g_volumetricCloudCompositePipelineLayout, nullptr);
    if(g_volumetricLightPipelineLayout) vkDestroyPipelineLayout(g_device, g_volumetricLightPipelineLayout, nullptr);
    if(g_volumetricCloudDescriptorPool) vkDestroyDescriptorPool(g_device, g_volumetricCloudDescriptorPool, nullptr);
    if(g_volumetricCloudRenderSetLayout) vkDestroyDescriptorSetLayout(g_device, g_volumetricCloudRenderSetLayout, nullptr);
    if(g_volumetricCloudResolveSetLayout) vkDestroyDescriptorSetLayout(g_device, g_volumetricCloudResolveSetLayout, nullptr);
    if(g_volumetricCloudCompositeSetLayout) vkDestroyDescriptorSetLayout(g_device, g_volumetricCloudCompositeSetLayout, nullptr);
    if(g_volumetricLightSetLayout) vkDestroyDescriptorSetLayout(g_device, g_volumetricLightSetLayout, nullptr);
    if(g_volumetricCloudAtrousPipeline) vkDestroyPipeline(g_device, g_volumetricCloudAtrousPipeline, nullptr);
    if(g_volumetricCloudAtrousPipelineLayout) vkDestroyPipelineLayout(g_device, g_volumetricCloudAtrousPipelineLayout, nullptr);
    if(g_volumetricCloudAtrousDescriptorPool) vkDestroyDescriptorPool(g_device, g_volumetricCloudAtrousDescriptorPool, nullptr);
    if(g_volumetricCloudAtrousSetLayout) vkDestroyDescriptorSetLayout(g_device, g_volumetricCloudAtrousSetLayout, nullptr);
    // 2026-11-15：froxel 重构的资源销毁。
    if(g_fogFroxelCompositePipeline) vkDestroyPipeline(g_device, g_fogFroxelCompositePipeline, nullptr);
    if(g_fogFroxelCompositePipelineLayout) vkDestroyPipelineLayout(g_device, g_fogFroxelCompositePipelineLayout, nullptr);
    if(g_fogFroxelLightPipeline) vkDestroyPipeline(g_device, g_fogFroxelLightPipeline, nullptr);
    if(g_fogFroxelLightPipelineLayout) vkDestroyPipelineLayout(g_device, g_fogFroxelLightPipelineLayout, nullptr);
    if(g_fogScreenApplyPipeline) vkDestroyPipeline(g_device, g_fogScreenApplyPipeline, nullptr);
    if(g_fogScreenApplyPipelineLayout) vkDestroyPipelineLayout(g_device, g_fogScreenApplyPipelineLayout, nullptr);
    if(g_fogScreenMarchPipeline) vkDestroyPipeline(g_device, g_fogScreenMarchPipeline, nullptr);
    if(g_fogScreenMarchPipelineLayout) vkDestroyPipelineLayout(g_device, g_fogScreenMarchPipelineLayout, nullptr);
    g_fogScreenApplyPipeline = VK_NULL_HANDLE;
    g_fogScreenApplyPipelineLayout = VK_NULL_HANDLE;
    g_fogScreenMarchPipeline = VK_NULL_HANDLE;
    g_fogScreenMarchPipelineLayout = VK_NULL_HANDLE;
    if(g_fogFroxelDensityPipeline) vkDestroyPipeline(g_device, g_fogFroxelDensityPipeline, nullptr);
    if(g_fogFroxelDensityPipelineLayout) vkDestroyPipelineLayout(g_device, g_fogFroxelDensityPipelineLayout, nullptr);
    if(g_fogFroxelShadowView) vkDestroyImageView(g_device, g_fogFroxelShadowView, nullptr);
    if(g_fogFroxelShadowImage) vkDestroyImage(g_device, g_fogFroxelShadowImage, nullptr);
    if(g_fogFroxelShadowMemory) vkFreeMemory(g_device, g_fogFroxelShadowMemory, nullptr);
    if(g_fogFroxelLightHistoryView) vkDestroyImageView(g_device, g_fogFroxelLightHistoryView, nullptr);
    if(g_fogFroxelLightHistoryImage) vkDestroyImage(g_device, g_fogFroxelLightHistoryImage, nullptr);
    if(g_fogFroxelLightHistoryMemory) vkFreeMemory(g_device, g_fogFroxelLightHistoryMemory, nullptr);
    if(g_fogFroxelIntegratedView) vkDestroyImageView(g_device, g_fogFroxelIntegratedView, nullptr);
    if(g_fogFroxelIntegratedImage) vkDestroyImage(g_device, g_fogFroxelIntegratedImage, nullptr);
    if(g_fogFroxelIntegratedMemory) vkFreeMemory(g_device, g_fogFroxelIntegratedMemory, nullptr);
    g_fogFroxelLightPipeline = VK_NULL_HANDLE;
    g_fogFroxelLightPipelineLayout = VK_NULL_HANDLE;
    g_fogFroxelShadowView = VK_NULL_HANDLE;
    g_fogFroxelShadowImage = VK_NULL_HANDLE;
    g_fogFroxelShadowMemory = VK_NULL_HANDLE;
    g_fogFroxelLightHistoryView = VK_NULL_HANDLE;
    g_fogFroxelLightHistoryImage = VK_NULL_HANDLE;
    g_fogFroxelLightHistoryMemory = VK_NULL_HANDLE;
    g_fogFroxelIntegratedView = VK_NULL_HANDLE;
    g_fogFroxelIntegratedImage = VK_NULL_HANDLE;
    g_fogFroxelIntegratedMemory = VK_NULL_HANDLE;
    g_fogFroxelWidth = 0u;
    g_fogFroxelHeight = 0u;
    g_fogFroxelHistoryNeedsClear.store(true);
    if(g_fogScreenDistView) vkDestroyImageView(g_device, g_fogScreenDistView, nullptr);
    if(g_fogScreenDistImage) vkDestroyImage(g_device, g_fogScreenDistImage, nullptr);
    if(g_fogScreenDistMemory) vkFreeMemory(g_device, g_fogScreenDistMemory, nullptr);
    if(g_fogScreenColorView) vkDestroyImageView(g_device, g_fogScreenColorView, nullptr);
    if(g_fogScreenColorImage) vkDestroyImage(g_device, g_fogScreenColorImage, nullptr);
    if(g_fogScreenColorMemory) vkFreeMemory(g_device, g_fogScreenColorMemory, nullptr);
    if(g_fogFroxelDensityView) vkDestroyImageView(g_device, g_fogFroxelDensityView, nullptr);
    if(g_fogFroxelDensityImage) vkDestroyImage(g_device, g_fogFroxelDensityImage, nullptr);
    if(g_fogFroxelDensityMemory) vkFreeMemory(g_device, g_fogFroxelDensityMemory, nullptr);
    g_fogFroxelCompositePipeline = VK_NULL_HANDLE;
    g_fogFroxelCompositePipelineLayout = VK_NULL_HANDLE;
    g_fogFroxelDensityPipeline = VK_NULL_HANDLE;
    g_fogFroxelDensityPipelineLayout = VK_NULL_HANDLE;
    g_fogScreenDistView = VK_NULL_HANDLE;
    g_fogScreenDistImage = VK_NULL_HANDLE;
    g_fogScreenDistMemory = VK_NULL_HANDLE;
    g_fogScreenColorView = VK_NULL_HANDLE;
    g_fogScreenColorImage = VK_NULL_HANDLE;
    g_fogScreenColorMemory = VK_NULL_HANDLE;
    g_fogFroxelDensityView = VK_NULL_HANDLE;
    g_fogFroxelDensityImage = VK_NULL_HANDLE;
    g_fogFroxelDensityMemory = VK_NULL_HANDLE;
    if(g_volumetricCloudSampler) vkDestroySampler(g_device, g_volumetricCloudSampler, nullptr);
    if(g_volumetricCloudHistoryDepthView) vkDestroyImageView(g_device, g_volumetricCloudHistoryDepthView, nullptr);
    if(g_volumetricCloudHistoryDepthImage) vkDestroyImage(g_device, g_volumetricCloudHistoryDepthImage, nullptr);
    if(g_volumetricCloudHistoryDepthMemory) vkFreeMemory(g_device, g_volumetricCloudHistoryDepthMemory, nullptr);
    if(g_volumetricCloudDepthView) vkDestroyImageView(g_device, g_volumetricCloudDepthView, nullptr);
    if(g_volumetricCloudDepthImage) vkDestroyImage(g_device, g_volumetricCloudDepthImage, nullptr);
    if(g_volumetricCloudDepthMemory) vkFreeMemory(g_device, g_volumetricCloudDepthMemory, nullptr);
    if(g_volumetricCloudHistoryView) vkDestroyImageView(g_device, g_volumetricCloudHistoryView, nullptr);
    if(g_volumetricCloudHistoryImage) vkDestroyImage(g_device, g_volumetricCloudHistoryImage, nullptr);
    if(g_volumetricCloudHistoryMemory) vkFreeMemory(g_device, g_volumetricCloudHistoryMemory, nullptr);
    if(g_volumetricCloudResolvedView) vkDestroyImageView(g_device, g_volumetricCloudResolvedView, nullptr);
    if(g_volumetricCloudResolvedImage) vkDestroyImage(g_device, g_volumetricCloudResolvedImage, nullptr);
    if(g_volumetricCloudResolvedMemory) vkFreeMemory(g_device, g_volumetricCloudResolvedMemory, nullptr);
    if(g_volumetricCloudView) vkDestroyImageView(g_device, g_volumetricCloudView, nullptr);
    if(g_volumetricCloudImage) vkDestroyImage(g_device, g_volumetricCloudImage, nullptr);
    if(g_volumetricCloudMemory) vkFreeMemory(g_device, g_volumetricCloudMemory, nullptr);
    g_volumetricCloudImage = VK_NULL_HANDLE;
    g_volumetricCloudMemory = VK_NULL_HANDLE;
    g_volumetricCloudView = VK_NULL_HANDLE;
    g_volumetricCloudHistoryImage = VK_NULL_HANDLE;
    g_volumetricCloudHistoryMemory = VK_NULL_HANDLE;
    g_volumetricCloudHistoryView = VK_NULL_HANDLE;
    g_volumetricCloudResolvedImage = VK_NULL_HANDLE;
    g_volumetricCloudResolvedMemory = VK_NULL_HANDLE;
    g_volumetricCloudResolvedView = VK_NULL_HANDLE;
    g_volumetricCloudDepthImage = VK_NULL_HANDLE;
    g_volumetricCloudDepthMemory = VK_NULL_HANDLE;
    g_volumetricCloudDepthView = VK_NULL_HANDLE;
    g_volumetricCloudHistoryDepthImage = VK_NULL_HANDLE;
    g_volumetricCloudHistoryDepthMemory = VK_NULL_HANDLE;
    g_volumetricCloudHistoryDepthView = VK_NULL_HANDLE;
    g_volumetricCloudSampler = VK_NULL_HANDLE;
    ++g_rrGuideViewGeneration;
    if(g_godRayCompositePipeline) vkDestroyPipeline(g_device, g_godRayCompositePipeline, nullptr);
    if(g_godRayCompositePipelineLayout) vkDestroyPipelineLayout(g_device, g_godRayCompositePipelineLayout, nullptr);
    if(g_godRayDescriptorPool) vkDestroyDescriptorPool(g_device, g_godRayDescriptorPool, nullptr);
    if(g_godRayCompositeSetLayout) vkDestroyDescriptorSetLayout(g_device, g_godRayCompositeSetLayout, nullptr);
    if(g_godRayHistoryView) vkDestroyImageView(g_device, g_godRayHistoryView, nullptr);
    if(g_godRayHistoryImage) vkDestroyImage(g_device, g_godRayHistoryImage, nullptr);
    if(g_godRayHistoryMemory) vkFreeMemory(g_device, g_godRayHistoryMemory, nullptr);
    if(g_godRayTargetView) vkDestroyImageView(g_device, g_godRayTargetView, nullptr);
    if(g_godRayTargetImage) vkDestroyImage(g_device, g_godRayTargetImage, nullptr);
    if(g_godRayTargetMemory) vkFreeMemory(g_device, g_godRayTargetMemory, nullptr);

    g_volumetricCloudRenderSetLayout = VK_NULL_HANDLE;
    g_volumetricCloudResolveSetLayout = VK_NULL_HANDLE;
    g_volumetricCloudCompositeSetLayout = VK_NULL_HANDLE;
    g_volumetricCloudDescriptorPool = VK_NULL_HANDLE;
    g_volumetricCloudRenderSet = VK_NULL_HANDLE;
    g_volumetricCloudResolveSet = VK_NULL_HANDLE;
    g_volumetricCloudCompositeSet = VK_NULL_HANDLE;
    g_volumetricCloudRenderPipelineLayout = VK_NULL_HANDLE;
    g_volumetricCloudResolvePipelineLayout = VK_NULL_HANDLE;
    g_volumetricCloudCompositePipelineLayout = VK_NULL_HANDLE;
    g_volumetricCloudRenderPipelineV3 = VK_NULL_HANDLE;
    g_volumetricCloudResolvePipeline = VK_NULL_HANDLE;
    g_volumetricCloudCompositePipeline = VK_NULL_HANDLE;
    g_volumetricCloudAtrousPipeline = VK_NULL_HANDLE;
    g_volumetricCloudAtrousPipelineLayout = VK_NULL_HANDLE;
    g_volumetricCloudAtrousDescriptorPool = VK_NULL_HANDLE;
    g_volumetricCloudAtrousSetLayout = VK_NULL_HANDLE;
    g_volumetricCloudAtrousSetForward = VK_NULL_HANDLE;
    g_volumetricCloudAtrousSetBackward = VK_NULL_HANDLE;
    g_volumetricLightPipeline = VK_NULL_HANDLE;
    g_volumetricLightPipelineLayout = VK_NULL_HANDLE;
    g_volumetricLightSetLayout = VK_NULL_HANDLE;
    g_volumetricLightSet = VK_NULL_HANDLE;
    g_volumetricCloudWidth = 0u;
    g_volumetricCloudHeight = 0u;
    g_volumetricCloudBoundRenderWidth = 0u;
    g_volumetricCloudBoundRenderHeight = 0u;
    g_volumetricCloudBoundOutputWidth = 0u;
    g_volumetricCloudBoundOutputHeight = 0u;
    g_volumetricCloudLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_volumetricCloudHistoryLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_volumetricCloudResolvedLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_volumetricCloudDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_volumetricCloudHistoryDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_volumetricCloudHistoryNeedsClear.store(true);
    g_godRayCompositePipeline = VK_NULL_HANDLE;
    g_godRayCompositePipelineLayout = VK_NULL_HANDLE;
    g_godRayDescriptorPool = VK_NULL_HANDLE;
    g_godRayCompositeSetLayout = VK_NULL_HANDLE;
    g_godRayCompositeSet = VK_NULL_HANDLE;
    g_godRayTargetView = VK_NULL_HANDLE;
    g_godRayTargetImage = VK_NULL_HANDLE;
    g_godRayTargetMemory = VK_NULL_HANDLE;
    g_godRayHistoryView = VK_NULL_HANDLE;
    g_godRayHistoryImage = VK_NULL_HANDLE;
    g_godRayHistoryMemory = VK_NULL_HANDLE;
    g_godRayTargetLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_godRayHistoryLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_godRayHistoryNeedsClear.store(true);
    g_godRayWidth = 0u;
    g_godRayHeight = 0u;
    g_cloudCompositeBoundNear = VK_NULL_HANDLE;
    g_cloudCompositeBoundFar = VK_NULL_HANDLE;
    g_cloudSkyLutBoundView = VK_NULL_HANDLE;
    g_cloudSkyLutBoundSet = VK_NULL_HANDLE;
    g_cloudSkyLutBoundGen = 0u;
    g_cloudShadowBoundSet = VK_NULL_HANDLE;
    g_cloudShadowBoundGen = 0u;
    ++g_volumetricCloudSetGeneration;
}
