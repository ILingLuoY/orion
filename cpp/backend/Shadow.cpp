// Orion backend — Shadow 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"


// ---------------------------------------------------------------
// 太阳阴影图：资源、描述符、管线
//
// 完整动机见 SUN_SHADOW_RESOLUTION 处的注释与 cpp/shaders/sun_shadow.glsl。
// 这里只强调两点工程约束：
//   1. 尺寸恒为定尺（当前 1024²），与窗口无关，所以这套资源不进体积云的重建路径；
//   2. 它需要 ray query，而 ray query 是**独立扩展**（不被
//      VK_KHR_ray_tracing_pipeline 蕴含），设备侧由 Java 的
//      VulkanBackendMixin 启用。这里再查一次物理设备能力，不支持时
//      安静地退回"只有云影"的旧行为，而不是创建一条必然报错的管线。
// ---------------------------------------------------------------
void createSunShadowResources()
{
    if(g_sunShadowPipeline != VK_NULL_HANDLE)
    {
        return;
    }
    if(g_physicalDevice == VK_NULL_HANDLE || g_device == VK_NULL_HANDLE ||
       g_instance == VK_NULL_HANDLE)
    {
        return;
    }

    auto getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceFeatures2"));
    if(getFeatures2 == nullptr)
    {
        getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceFeatures2KHR"));
    }
    if(getFeatures2 != nullptr)
    {
        VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR
        };
        VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features2.pNext = &rayQueryFeatures;
        getFeatures2(g_physicalDevice, &features2);
        if(rayQueryFeatures.rayQuery != VK_TRUE)
        {
            RTLog("[SUNSHADOW] ray query unsupported on this device; "
                "volumetric light/fog keep cloud shadows only");
            return;
        }
    }

    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(g_physicalDevice, VK_FORMAT_R32_SFLOAT,
        &formatProperties);
    const VkFormatFeatureFlags requiredFeatures =
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if((formatProperties.optimalTilingFeatures & requiredFeatures) != requiredFeatures)
    {
        RTLog("[SUNSHADOW] R32_SFLOAT cannot be both stored and sampled; disabled");
        return;
    }

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R32_SFLOAT;
    imageInfo.extent = { SUN_SHADOW_RESOLUTION, SUN_SHADOW_RESOLUTION, 1u };
    imageInfo.mipLevels = 1u;
    imageInfo.arrayLayers = 1u;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateImage(g_device, &imageInfo, nullptr, &g_sunShadowImage),
        "sun shadow image create failed");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g_device, g_sunShadowImage, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocation, nullptr, &g_sunShadowMemory),
        "sun shadow image allocate failed");
    check(vkBindImageMemory(g_device, g_sunShadowImage, g_sunShadowMemory, 0u),
        "sun shadow image bind failed");

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = g_sunShadowImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R32_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1u;
    viewInfo.subresourceRange.layerCount = 1u;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_sunShadowView),
        "sun shadow image view create failed");

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_sunShadowSampler),
        "sun shadow sampler create failed");

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
        &g_sunShadowSetLayout), "sun shadow set layout failed");

    VkDescriptorPoolSize poolSizes[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u },
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u }
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1u;
    poolInfo.poolSizeCount = 3u;
    poolInfo.pPoolSizes = poolSizes;
    check(vkCreateDescriptorPool(g_device, &poolInfo, nullptr,
        &g_sunShadowDescriptorPool), "sun shadow descriptor pool failed");

    VkDescriptorSetAllocateInfo setAllocation{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
    };
    setAllocation.descriptorPool = g_sunShadowDescriptorPool;
    setAllocation.descriptorSetCount = 1u;
    setAllocation.pSetLayouts = &g_sunShadowSetLayout;
    check(vkAllocateDescriptorSets(g_device, &setAllocation, &g_sunShadowSet),
        "sun shadow descriptor set allocation failed");

    VkDescriptorBufferInfo cameraInfo{};
    cameraInfo.buffer = g_cameraUboBuffer;
    cameraInfo.range = sizeof(CameraUBO);
    VkDescriptorImageInfo shadowStorageInfo{};
    shadowStorageInfo.imageView = g_sunShadowView;
    shadowStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet writes[2]{};
    writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_sunShadowSet, 0u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &cameraInfo, nullptr };
    writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_sunShadowSet, 2u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &shadowStorageInfo, nullptr, nullptr };
    vkUpdateDescriptorSets(g_device, 2u, writes, 0u, nullptr);

    if(g_tlas != VK_NULL_HANDLE)
    {
        VkWriteDescriptorSetAccelerationStructureKHR tlasInfo{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR
        };
        tlasInfo.accelerationStructureCount = 1u;
        tlasInfo.pAccelerationStructures = &g_tlas;
        VkWriteDescriptorSet tlasWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        tlasWrite.pNext = &tlasInfo;
        tlasWrite.dstSet = g_sunShadowSet;
        tlasWrite.dstBinding = 1u;
        tlasWrite.descriptorCount = 1u;
        tlasWrite.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        vkUpdateDescriptorSets(g_device, 1u, &tlasWrite, 0u, nullptr);
        g_sunShadowTlasHandle = g_tlas;
    }

    const std::vector<uint8_t> code = loadSpv("shaders/rt_sun_shadow.comp.spv");
    VkShaderModule shader = createShaderModule(g_device, code);
    try
    {
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        pipelineLayoutInfo.setLayoutCount = 1u;
        pipelineLayoutInfo.pSetLayouts = &g_sunShadowSetLayout;
        check(vkCreatePipelineLayout(g_device, &pipelineLayoutInfo, nullptr,
            &g_sunShadowPipelineLayout), "sun shadow pipeline layout failed");
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
        pipelineInfo.layout = g_sunShadowPipelineLayout;
        check(vkCreateComputePipelines(g_device, getSharedPipelineCache(), 1u,
            &pipelineInfo, nullptr, &g_sunShadowPipeline), "sun shadow pipeline failed");
    }
    catch(...)
    {
        vkDestroyShaderModule(g_device, shader, nullptr);
        destroySunShadowResources();
        throw;
    }
    vkDestroyShaderModule(g_device, shader, nullptr);
    g_sunShadowLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_sunShadowAvailable.store(true);
    RTLog("[SUNSHADOW] 256x256 sun shadow map ready (ray query, 65536 rays/frame)");
}
bool sunShadowForceAlways()
{
    static const bool forced =
        (std::getenv("ORION_SUN_SHADOW_ALWAYS") != nullptr);
    return forced;
}

// 2026-11-01：相位阈值的太阳高度自适应（修"夕阳时阴影抖"）。
//
// 病灶：固定 EPS = 1e-4 对应 8.3 Hz 重建（太阳 1200 s 走完 1.0 相位）。
// 同样的 0.12 s，阴影边缘的位移 ∝ 1 / sin²(太阳高度)：
//   60° → 0.008 格（看不见）；10° → 0.29 格；5° → 0.82 格（h=10 格时）
// 于是夕阳时呈现"一格一跳"的阶梯状抖动，而正午完全正常。
//
// 修法：只在低角度收紧阈值（提高重建频率）。
//   高度 >= 0.35（约 20°）→ 1e-4   （8 Hz，与改动前逐位一致）
//   高度 <= 0.05（约 3.5°）→ 2e-5   （约 42 Hz，位移降到 ~0.16 格）
// 单次重建 1024² × 320 格 ≈ 0.5 ms，42 Hz 摊到每帧约 2.2 ms，
// 且只发生在夕阳那几分钟；正午与夜晚逐位不变。
static float sunShadowPhaseEpsForHeight(float sunHeight)
{
    const float t = std::clamp(
        (sunHeight - 0.05f) / (0.35f - 0.05f), 0.0f, 1.0f);
    const float smooth = t * t * (3.0f - 2.0f * t);
    return 2.0e-5f + (SUN_SHADOW_PHASE_EPS - 2.0e-5f) * smooth;
}

// 返回 true = 这一帧需要重建，并顺便把"当前签名"提交进去。
bool sunShadowSignatureChanged(
    float texelWorld,
    float& builtPhase,
    uint64_t& builtRevision,
    float builtCenter[3]
)
{
    // 2026-11-18 一次性诊断：把"太阳高度/远影开关"打到日志，用于判定
    // 雾的太阳项恒为 0 是"太阳在地平线下"还是"阴影数据坏"。
    // 必须放在上面 early-return 之前，否则 sunShadowForceAlways() 时会跳过。
    {
        static bool s_sunElevLogged = false;
        if(!s_sunElevLogged && g_cameraUboMapped != nullptr)
        {
            s_sunElevLogged = true;
            const CameraUBO* probe = static_cast<const CameraUBO*>(g_cameraUboMapped);
            const float probePhase = probe->historyParamsPadding1;
            const float probeWrapped = probePhase - std::floor(probePhase);
            const float probeSunHeight =
                0.81915204429f * std::sin(probeWrapped * 6.28318530718f);
            char sunElev[256];
            _snprintf_s(sunElev, sizeof(sunElev), _TRUNCATE,
                "[SUN ELEV] phase=%.5f sunHeight=%.4f farFlag=%.1f "
                "farExtent=%.1f forceAlways=%d",
                probePhase, probeSunHeight,
                static_cast<double>(probe->sunShadowFarParams[3]),
                static_cast<double>(probe->sunShadowFarParams[0]),
                sunShadowForceAlways() ? 1 : 0);
            RTLogFlushed(sunElev);
        }
    }

    if(sunShadowForceAlways() || g_cameraUboMapped == nullptr)
    {
        return true;
    }

    const CameraUBO* ubo =
        static_cast<const CameraUBO*>(g_cameraUboMapped);
    const float phase = ubo->historyParamsPadding1;
    const uint64_t revision =
        probeGeometryRevision();

    // 与 sun_shadow.glsl 的 sunShadowLightDirection() 同一个公式（同一份
    // historyParams.w），所以这里的"太阳高度"与生产者的正交基严格一致。
    float phaseEps = SUN_SHADOW_PHASE_EPS;
    {
        const float wrapped = phase - std::floor(phase);
        const float sunHeight = 0.81915204429f *
            std::sin(wrapped * 6.28318530718f);
        phaseEps = sunShadowPhaseEpsForHeight(sunHeight);
    }

    float center[3];
    for(int axis = 0; axis < 3; ++axis)
    {
        const float position = ubo->viewInverse[12 + axis];
        center[axis] = std::floor(position / texelWorld) * texelWorld;
    }

    const bool changed =
        builtRevision != revision ||
        std::fabs(phase - builtPhase) > phaseEps ||
        center[0] != builtCenter[0] ||
        center[1] != builtCenter[1] ||
        center[2] != builtCenter[2];

    if(changed)
    {
        builtRevision = revision;
        builtPhase = phase;
        builtCenter[0] = center[0];
        builtCenter[1] = center[1];
        builtCenter[2] = center[2];
    }

    return changed;
}
// ===========================================================================
// 2026-11-09：太阳阴影图内容探针（ORION_SUNSHADOW_PROBE=1）
//
// 病征（用户）："太阳的体积光穿墙，所有体积光都穿墙"。
// 解析雾的太阳项（volumetric_fog.glsl 的 rayColor）遮挡**只**来自这张图，所以
// 只要图里的值是错的，雾就会整片受光。图里存的是"沿太阳方向首个遮挡体距离"
// （消费者算 depth = extentZ - dot(local, sunDir)，命中判据 depth < stored + bias），
// 因此：
//   · 生产者一条都没打中 ⇒ stored 恒 ≈ 320（2×EXTENT_Z）⇒ 处处判定受光；
//   · 图有梯度 ⇒ 图是好的，问题在消费者/调用参数。
// 这条日志就是把这两种情况分开：min/mean/max + "无命中占比"。
// ===========================================================================
namespace
{
    bool sunShadowProbeEnabled()
    {
        static const bool cached = []() -> bool
        {
            const char* raw = std::getenv("ORION_SUNSHADOW_PROBE");
            return raw != nullptr && raw[0] == '1';
        }();
        return cached;
    }

    constexpr uint32_t SUN_SHADOW_PROBE_INTERVAL = 120u;

    VkBuffer s_sunShadowProbeBuffer = VK_NULL_HANDLE;
    VkDeviceMemory s_sunShadowProbeMemory = VK_NULL_HANDLE;
    float* s_sunShadowProbeMapped = nullptr;
    uint32_t s_sunShadowProbeCounter = 0u;
    bool s_sunShadowProbePending = false;

    void sunShadowProbeLog()
    {
        if(!s_sunShadowProbePending || s_sunShadowProbeMapped == nullptr)
        {
            return;
        }
        s_sunShadowProbePending = false;

        const size_t texelCount =
            static_cast<size_t>(SUN_SHADOW_RESOLUTION) * SUN_SHADOW_RESOLUTION;
        float lo = 1.0e30f;
        float hi = -1.0e30f;
        double sum = 0.0;
        size_t noHit = 0u;
        size_t zero = 0u;

        for(size_t i = 0u; i < texelCount; ++i)
        {
            const float value = s_sunShadowProbeMapped[i];
            lo = std::min(lo, value);
            hi = std::max(hi, value);
            sum += static_cast<double>(value);

            if(value >= 319.0f)
            {
                ++noHit;
            }
            if(value <= 0.0f)
            {
                ++zero;
            }
        }

        // ---- 消费者侧复算（与 sun_shadow.glsl 的 sunShadowVisibilityIn 同口径）----
        // 图好不好是一回事；雾实际读到什么才是判定"洞穴里被判受光"的关键。
        // 这里在 CPU 上用**同一张图 + 同一套公式**算相机周围几个点的可见性：
        //   origin = 相机本身、down6 = 脚下 6 格（应被挡）、up6 = 头上 6 格（洞内应被挡）、
        //   sun20 = 朝太阳方向 20 格（露天才应受光）
        // 输出 1/0 + depth + stored，可直接判定是"图错"还是"公式/取位错"。
        std::string consumeLog;

        if(g_cameraUboMapped != nullptr)
        {
            const CameraUBO* probeUbo =
                static_cast<const CameraUBO*>(g_cameraUboMapped);
            const float phase = probeUbo->historyParamsPadding1;
            const float angle = (phase - std::floor(phase)) * 6.28318530718f;
            float sun[3] = {
                std::cos(angle),
                0.81915204429f * std::sin(angle),
                0.57357643635f * std::sin(angle)
            };
            const float sunLength =
                std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);

            if(sunLength > 1.0e-8f)
            {
                sun[0] /= sunLength;
                sun[1] /= sunLength;
                sun[2] /= sunLength;

                // 与 sun_shadow.glsl 的 sunShadowBasis 同口径：
                //   right = normalize(cross(vec3(0,1,0), sun))
                //   up    = cross(sun, right)
                float right[3] = { sun[2], 0.0f, -sun[0] };
                const float rightLength =
                    std::sqrt(right[0] * right[0] + right[2] * right[2]);

                if(rightLength > 1.0e-8f)
                {
                    right[0] /= rightLength;
                    right[2] /= rightLength;

                    const float upAxis[3] = {
                        sun[1] * right[2] - sun[2] * right[1],
                        sun[2] * right[0] - sun[0] * right[2],
                        sun[0] * right[1] - sun[1] * right[0]
                    };

                    const float extentXY = 160.0f;
                    const float extentZ = 160.0f;
                    const float nearBias = 0.06f;
                    const float quantum = 2.0f * extentXY / 256.0f;

                    float camera[3] = {
                        probeUbo->viewInverse[12],
                        probeUbo->viewInverse[13],
                        probeUbo->viewInverse[14]
                    };
                    float center[3];
                    for(int axis = 0; axis < 3; ++axis)
                    {
                        center[axis] = std::floor(camera[axis] / quantum) * quantum;
                    }

                    auto probeAt = [&](float dx, float dy, float dz) -> std::string
                    {
                        const float local[3] = {
                            camera[0] + dx - center[0],
                            camera[1] + dy - center[1],
                            camera[2] + dz - center[2]
                        };
                        const float u =
                            (local[0] * right[0] + local[1] * right[1] +
                             local[2] * right[2]) / (2.0f * extentXY) + 0.5f;
                        const float v =
                            (local[0] * upAxis[0] + local[1] * upAxis[1] +
                             local[2] * upAxis[2]) / (2.0f * extentXY) + 0.5f;

                        if(u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
                        {
                            return std::string("oob");
                        }

                        const int tx = std::min(
                            static_cast<int>(SUN_SHADOW_RESOLUTION) - 1,
                            std::max(0, static_cast<int>(u * SUN_SHADOW_RESOLUTION)));
                        const int ty = std::min(
                            static_cast<int>(SUN_SHADOW_RESOLUTION) - 1,
                            std::max(0, static_cast<int>(v * SUN_SHADOW_RESOLUTION)));
                        const float stored = s_sunShadowProbeMapped[
                            static_cast<size_t>(ty) * SUN_SHADOW_RESOLUTION +
                            static_cast<size_t>(tx)];
                        const float depth = extentZ -
                            (local[0] * sun[0] + local[1] * sun[1] + local[2] * sun[2]);
                        const bool lit = depth < stored + nearBias;

                        return std::to_string(lit ? 1 : 0) + "/" +
                            std::to_string(static_cast<int>(depth)) + "/" +
                            std::to_string(static_cast<int>(stored));
                    };

                    consumeLog =
                        " cam=(" + probeAt(0.0f, 0.0f, 0.0f) + ")" +
                        " down6=(" + probeAt(0.0f, -6.0f, 0.0f) + ")" +
                        " up6=(" + probeAt(0.0f, 6.0f, 0.0f) + ")" +
                        " sun20=(" + probeAt(sun[0] * 20.0f, sun[1] * 20.0f,
                            sun[2] * 20.0f) + ")" +
                        " sunY=" + std::to_string(sun[1]);
                }
            }
        }

        const double mean = texelCount > 0u ? sum / static_cast<double>(texelCount) : 0.0;
        const double noHitPercent =
            100.0 * static_cast<double>(noHit) / static_cast<double>(texelCount);
        const double zeroPercent =
            100.0 * static_cast<double>(zero) / static_cast<double>(texelCount);

        RTLogFlushed(
            std::string("[SUNSHADOW PROBE] n=") + std::to_string(texelCount) +
            " min=" + std::to_string(lo) +
            " mean=" + std::to_string(mean) +
            " max=" + std::to_string(hi) +
            " noHit%=" + std::to_string(noHitPercent) +
            " zero%=" + std::to_string(zeroPercent) +
            " |[vis/depth/stored]" + consumeLog);
    }

    void recordSunShadowProbe(VkCommandBuffer cmd)
    {
        if(!sunShadowProbeEnabled() || cmd == VK_NULL_HANDLE ||
           g_sunShadowImage == VK_NULL_HANDLE ||
           g_sunShadowLayout != VK_IMAGE_LAYOUT_GENERAL)
        {
            return;
        }

        if(s_sunShadowProbeBuffer == VK_NULL_HANDLE)
        {
            if(g_device == VK_NULL_HANDLE || g_physicalDevice == VK_NULL_HANDLE)
            {
                return;
            }

            const VkDeviceSize bytes =
                static_cast<VkDeviceSize>(SUN_SHADOW_RESOLUTION) *
                SUN_SHADOW_RESOLUTION * sizeof(float);

            try
            {
                createBuffer(g_device, g_physicalDevice, bytes,
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    s_sunShadowProbeBuffer, s_sunShadowProbeMemory);
            }
            catch(...)
            {
                RTLog("[SUNSHADOW PROBE] buffer create failed; probe off");
                s_sunShadowProbeBuffer = VK_NULL_HANDLE;
                return;
            }

            if(vkMapMemory(g_device, s_sunShadowProbeMemory, 0u, VK_WHOLE_SIZE, 0u,
                   reinterpret_cast<void**>(&s_sunShadowProbeMapped)) != VK_SUCCESS)
            {
                s_sunShadowProbeMapped = nullptr;
                return;
            }

            RTLog("[SUNSHADOW PROBE] ready (1024^2 R32F readback)");
        }

        if(++s_sunShadowProbeCounter < SUN_SHADOW_PROBE_INTERVAL)
        {
            return;
        }
        s_sunShadowProbeCounter = 0u;

        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toTransfer.image = g_sunShadowImage;
        toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.levelCount = 1u;
        toTransfer.subresourceRange.layerCount = 1u;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &toTransfer);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1u;
        region.imageExtent = { SUN_SHADOW_RESOLUTION, SUN_SHADOW_RESOLUTION, 1u };
        vkCmdCopyImageToBuffer(cmd, g_sunShadowImage, VK_IMAGE_LAYOUT_GENERAL,
            s_sunShadowProbeBuffer, 1u, &region);

        VkImageMemoryBarrier backToShader = toTransfer;
        backToShader.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        backToShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u,
            &backToShader);

        s_sunShadowProbePending = true;
    }
}

void recordSunShadowMap(VkCommandBuffer cmd)
{
    sunShadowProbeLog();
    recordSunShadowProbe(cmd);

    if(cmd == VK_NULL_HANDLE || !g_sunShadowAvailable.load() ||
       g_sunShadowPipeline == VK_NULL_HANDLE || g_sunShadowSet == VK_NULL_HANDLE ||
       g_sunShadowImage == VK_NULL_HANDLE || g_tlas == VK_NULL_HANDLE ||
       g_sunShadowTlasHandle != g_tlas)
    {
        return;
    }

    if(!sunShadowSignatureChanged(
        SUN_SHADOW_NEAR_TEXEL_WORLD,
        g_sunShadowBuiltPhase,
        g_sunShadowBuiltRevision,
        g_sunShadowBuiltCenter))
    {
        return;
    }

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = g_sunShadowLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.srcAccessMask = g_sunShadowLayout == VK_IMAGE_LAYOUT_UNDEFINED
        ? 0u : (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.image = g_sunShadowImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1u;
    barrier.subresourceRange.layerCount = 1u;
    vkCmdPipelineBarrier(cmd,
        g_sunShadowLayout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u,
        &barrier);
    g_sunShadowLayout = VK_IMAGE_LAYOUT_GENERAL;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_sunShadowPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_sunShadowPipelineLayout, 0u, 1u, &g_sunShadowSet, 0u, nullptr);
    const uint32_t groups =
        (SUN_SHADOW_RESOLUTION + SUN_SHADOW_WORKGROUP_SIZE - 1u) /
        SUN_SHADOW_WORKGROUP_SIZE;
    vkCmdDispatch(cmd, groups, groups, 1u);

    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u,
        &barrier);
}
void destroySunShadowResources()
{
    if(g_sunShadowPipeline) vkDestroyPipeline(g_device, g_sunShadowPipeline, nullptr);
    if(g_sunShadowPipelineLayout) vkDestroyPipelineLayout(g_device, g_sunShadowPipelineLayout, nullptr);
    if(g_sunShadowDescriptorPool) vkDestroyDescriptorPool(g_device, g_sunShadowDescriptorPool, nullptr);
    if(g_sunShadowSetLayout) vkDestroyDescriptorSetLayout(g_device, g_sunShadowSetLayout, nullptr);
    if(g_sunShadowSampler) vkDestroySampler(g_device, g_sunShadowSampler, nullptr);
    if(g_sunShadowView) vkDestroyImageView(g_device, g_sunShadowView, nullptr);
    if(g_sunShadowImage) vkDestroyImage(g_device, g_sunShadowImage, nullptr);
    if(g_sunShadowMemory) vkFreeMemory(g_device, g_sunShadowMemory, nullptr);
    g_sunShadowPipeline = VK_NULL_HANDLE;
    g_sunShadowPipelineLayout = VK_NULL_HANDLE;
    g_sunShadowDescriptorPool = VK_NULL_HANDLE;
    g_sunShadowSet = VK_NULL_HANDLE;
    g_sunShadowSetLayout = VK_NULL_HANDLE;
    g_sunShadowSampler = VK_NULL_HANDLE;
    g_sunShadowView = VK_NULL_HANDLE;
    g_sunShadowImage = VK_NULL_HANDLE;
    g_sunShadowMemory = VK_NULL_HANDLE;
    g_sunShadowLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_sunShadowTlasHandle = VK_NULL_HANDLE;
    g_sunShadowAvailable.store(false);
}

// ---------------------------------------------------------------
// 远场太阳阴影图：资源、描述符、管线（2026-10-01）
//
// 与上面近场那张逐条同构，只有三处不同：
//   1. 尺寸 1024²、extent 由 CameraUBO 的 sunShadowFarParams 下发（生产者从 UBO
//      读，与 rgen 消费者同源），不是编译期常量；
//   2. 消费者是 rgen（远场分支），所以在 RT 派发**之前**记录 —— 近场那张是给
//      体积 pass 的，可以留在原位；
//   3. 后置屏障的 dstStage 必须含 RAY_TRACING_SHADER_BIT_KHR。
//
// 资源照建（描述符必须始终有效），关掉时只是不派发、且 UBO 的 w 置 0，
// rgen 远场分支全短路。开关：
//   ORION_SUNSHADOW_FAR=0  → 关
//   ORION_SUNSHADOW_FAR=1  → 强制开（2026-10-24 之前的默认行为）
//   未设                   → **自适应：仅在 DH 远场合成块开着时派发**
//
// 2026-10-24 为什么要自适应默认关：这张图的消费者是 rgen 的远场分支，而远场
// 遮挡体只可能来自 DH 的 LOD 实例。没有 DH 时，视距以外的几何根本不在 TLAS 里
// （距离剔除 120 格 + 视距 128 格），1024² = 1,048,576 根射线全部 miss ——
// 纯开销。判据用 g_dhFarFieldMode > 0.5，与实例 mask 特判（:68722）同一口径。
//
// 2026-11-19 rasterLod（挂载点⑦）：DH 键已从 TLAS 摘除（挂载点③），这张图的
// 射线同样全部 miss —— 自适应档在 rasterLod 开启时同步关断；显式
// ORION_SUNSHADOW_FAR=1 仍可强制开（用户显式意图优先）。
//
// 注意：不能退回"static 只读一次"，因为 DH 合成块可以在运行时开关；按帧求值
// 只是两次读 + 一次比较，且派发/UBO 两条路径本来就每帧问一次。
bool sunShadowFarEnabled()
{
    static const int s_override = []() -> int
    {
        const char* raw = std::getenv("ORION_SUNSHADOW_FAR");

        if(raw == nullptr)
        {
            return -1;
        }

        return (std::string(raw) != "0") ? 1 : 0;
    }();

    if(sunShadowFarAbEnabled())
    {
        return sunShadowFarAbArm() != 0u;
    }

    const bool enabled =
        (s_override >= 0)
            ? (s_override != 0)
            : (g_dhFarFieldMode > 0.5f && !g_dhRasterLodEnabled);

    static std::atomic<int> s_lastLoggedState{-1};

    const int state = enabled ? 1 : 0;

    if(s_lastLoggedState.exchange(state, std::memory_order_relaxed) != state)
    {
        RTLog(
            std::string("[SUNSHADOW-FAR] dispatch=") + (enabled ? "on" : "off") +
            " dhFarFieldMode=" + std::to_string(g_dhFarFieldMode) +
            " rasterLod=" + (g_dhRasterLodEnabled ? "1" : "0") +
            (s_override >= 0 ? " (env override)" : " (auto: needs DH composite)")
        );
    }

    return enabled;
}

// 诊断旋钮（2026-10-01，B1）：本地光聚合缓冲（bindings 17/18）的每帧重建是否执行。
//
// 动机：recordFrameCommand 里那句注释断言 "The light list is not consumed yet
// ... so this changes no pixels"，但 shader 侧 localLightBuffer / localLightGridBuffer
// 的读取点分布在好几簇函数里，静态断定需要一条条追调用图。这里给一个运行期开关，
// 让"跳过重建"这条臂能被实测比对，而不是只靠读代码。
//
// 静态追查的结论（供比对）：所有读取点都不可达 ——
//   · evaluatePrimaryLocalLights(8773) / evaluateSecondaryDiffuseLocalLights(9697)
//     / evaluateLocalEmitterRadianceAtHit(6384) / evaluateLocalEmitterSurfaceRadiance(6546)
//     全文件无调用点；
//   · g2b* 引导簇的入口被 !LL_P3_ENABLE_G2B_LOCAL_GUIDE 短路，而该常量 = false，
//     编译期恒真 ⇒ 引导分支整段被 DCE；
//   · rt_test.rchit / rt_test_translucent.rchit 只在 binding 17 上声明，从不读取。
// 也就是说这个开关预期"开了等于没开"。若实测有像素差 ⇒ 上面哪条追错了。
//
// ORION_LOCAL_LIGHT_REBUILD=0 ⇒ 跳过每帧刷新（初始化那一次仍在 setup 里执行，
// binding 不会悬空）。只读一次，改完必须重启客户端。
// 未设或非 "0" = 照旧重建（默认）。
bool localLightRebuildEnabled()
{
    static const bool enabled = []() {
        const char* raw = std::getenv("ORION_LOCAL_LIGHT_REBUILD");
        if(raw == nullptr)
        {
            return false;
        }
        return std::string(raw) != "0";
    }();
    return enabled;
}

// 盒半宽（格）。默认 SUN_SHADOW_FAR_EXTENT_XY = 384，1024 分辨率下 0.75 格/纹素
// （与近场同密度）。环境变量 ORION_SUNSHADOW_FAR_EXTENT 可在**不重编**的情况下
// 调宽/调窄：调宽 = 覆盖更远的山体投影，代价是纹素变粗（阴影细节下降），射线也
// 更长。extent 走 CameraUBO，生产者与消费者同时跟着变，不会对不上。
float sunShadowFarExtent()
{
    static const float extent = []() {
        const char* raw = std::getenv("ORION_SUNSHADOW_FAR_EXTENT");
        if(raw == nullptr)
        {
            return SUN_SHADOW_FAR_EXTENT_XY;
        }
        const float parsed = static_cast<float>(std::atof(raw));
        return parsed >= 64.0f ? parsed : SUN_SHADOW_FAR_EXTENT_XY;
    }();
    return extent;
}
void createSunShadowFarResources()
{
    if(g_sunShadowFarPipeline != VK_NULL_HANDLE)
    {
        return;
    }
    if(g_physicalDevice == VK_NULL_HANDLE || g_device == VK_NULL_HANDLE ||
       g_instance == VK_NULL_HANDLE)
    {
        return;
    }

    auto getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceFeatures2"));
    if(getFeatures2 == nullptr)
    {
        getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceFeatures2KHR"));
    }
    if(getFeatures2 != nullptr)
    {
        VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR
        };
        VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features2.pNext = &rayQueryFeatures;
        getFeatures2(g_physicalDevice, &features2);
        if(rayQueryFeatures.rayQuery != VK_TRUE)
        {
            RTLog("[SUNSHADOW-FAR] ray query unsupported; far shadow map disabled");
            return;
        }
    }

    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(g_physicalDevice, VK_FORMAT_R32_SFLOAT,
        &formatProperties);
    const VkFormatFeatureFlags requiredFeatures =
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if((formatProperties.optimalTilingFeatures & requiredFeatures) != requiredFeatures)
    {
        RTLog("[SUNSHADOW-FAR] R32_SFLOAT cannot be both stored and sampled; disabled");
        return;
    }

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R32_SFLOAT;
    imageInfo.extent = { SUN_SHADOW_FAR_RESOLUTION, SUN_SHADOW_FAR_RESOLUTION, 1u };
    imageInfo.mipLevels = 1u;
    imageInfo.arrayLayers = 1u;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateImage(g_device, &imageInfo, nullptr, &g_sunShadowFarImage),
        "sun shadow far image create failed");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g_device, g_sunShadowFarImage, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocation, nullptr, &g_sunShadowFarMemory),
        "sun shadow far image allocate failed");
    check(vkBindImageMemory(g_device, g_sunShadowFarImage, g_sunShadowFarMemory, 0u),
        "sun shadow far image bind failed");

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = g_sunShadowFarImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R32_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1u;
    viewInfo.subresourceRange.layerCount = 1u;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_sunShadowFarView),
        "sun shadow far image view create failed");

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_sunShadowFarSampler),
        "sun shadow far sampler create failed");

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
        &g_sunShadowFarSetLayout), "sun shadow far set layout failed");

    VkDescriptorPoolSize poolSizes[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u },
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u }
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1u;
    poolInfo.poolSizeCount = 3u;
    poolInfo.pPoolSizes = poolSizes;
    check(vkCreateDescriptorPool(g_device, &poolInfo, nullptr,
        &g_sunShadowFarDescriptorPool), "sun shadow far descriptor pool failed");

    VkDescriptorSetAllocateInfo setAllocation{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
    };
    setAllocation.descriptorPool = g_sunShadowFarDescriptorPool;
    setAllocation.descriptorSetCount = 1u;
    setAllocation.pSetLayouts = &g_sunShadowFarSetLayout;
    check(vkAllocateDescriptorSets(g_device, &setAllocation, &g_sunShadowFarSet),
        "sun shadow far descriptor set allocation failed");

    VkDescriptorBufferInfo cameraInfo{};
    cameraInfo.buffer = g_cameraUboBuffer;
    cameraInfo.range = sizeof(CameraUBO);
    VkDescriptorImageInfo shadowStorageInfo{};
    shadowStorageInfo.imageView = g_sunShadowFarView;
    shadowStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet writes[2]{};
    writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_sunShadowFarSet, 0u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &cameraInfo, nullptr };
    writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_sunShadowFarSet, 2u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &shadowStorageInfo, nullptr, nullptr };
    vkUpdateDescriptorSets(g_device, 2u, writes, 0u, nullptr);

    if(g_tlas != VK_NULL_HANDLE)
    {
        VkWriteDescriptorSetAccelerationStructureKHR tlasInfo{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR
        };
        tlasInfo.accelerationStructureCount = 1u;
        tlasInfo.pAccelerationStructures = &g_tlas;
        VkWriteDescriptorSet tlasWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        tlasWrite.pNext = &tlasInfo;
        tlasWrite.dstSet = g_sunShadowFarSet;
        tlasWrite.dstBinding = 1u;
        tlasWrite.descriptorCount = 1u;
        tlasWrite.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        vkUpdateDescriptorSets(g_device, 1u, &tlasWrite, 0u, nullptr);
        g_sunShadowFarTlasHandle = g_tlas;
    }

    const std::vector<uint8_t> code = loadSpv("shaders/rt_sun_shadow_far.comp.spv");
    VkShaderModule shader = createShaderModule(g_device, code);
    try
    {
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        pipelineLayoutInfo.setLayoutCount = 1u;
        pipelineLayoutInfo.pSetLayouts = &g_sunShadowFarSetLayout;
        check(vkCreatePipelineLayout(g_device, &pipelineLayoutInfo, nullptr,
            &g_sunShadowFarPipelineLayout), "sun shadow far pipeline layout failed");
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
        pipelineInfo.layout = g_sunShadowFarPipelineLayout;
        check(vkCreateComputePipelines(g_device, getSharedPipelineCache(), 1u,
            &pipelineInfo, nullptr, &g_sunShadowFarPipeline),
            "sun shadow far pipeline failed");
    }
    catch(...)
    {
        vkDestroyShaderModule(g_device, shader, nullptr);
        destroySunShadowFarResources();
        throw;
    }
    vkDestroyShaderModule(g_device, shader, nullptr);
    g_sunShadowFarLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_sunShadowFarAvailable.store(true);
    RTLog("[SUNSHADOW-FAR] 1024x1024 far sun shadow map ready (extent 384, 1048576 rays/frame)");
}

// 每帧一次。必须在 **RT 派发之前**调用 —— rgen 的远场分支采样它。
// 这是与近场图（在体积 pass 之前）唯一的位置差异。
void recordSunShadowFarMap(VkCommandBuffer cmd)
{
    if(cmd == VK_NULL_HANDLE || !g_sunShadowFarAvailable.load() ||
       !sunShadowFarEnabled() ||
       g_sunShadowFarPipeline == VK_NULL_HANDLE || g_sunShadowFarSet == VK_NULL_HANDLE ||
       g_sunShadowFarImage == VK_NULL_HANDLE || g_tlas == VK_NULL_HANDLE ||
       g_sunShadowFarTlasHandle != g_tlas)
    {
        return;
    }

    if(!sunShadowSignatureChanged(
        2.0f * sunShadowFarExtent() /
            static_cast<float>(SUN_SHADOW_FAR_RESOLUTION),
        g_sunShadowFarBuiltPhase,
        g_sunShadowFarBuiltRevision,
        g_sunShadowFarBuiltCenter))
    {
        return;
    }

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = g_sunShadowFarLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.srcAccessMask = g_sunShadowFarLayout == VK_IMAGE_LAYOUT_UNDEFINED
        ? 0u : (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.image = g_sunShadowFarImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1u;
    barrier.subresourceRange.layerCount = 1u;
    vkCmdPipelineBarrier(cmd,
        g_sunShadowFarLayout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u,
        &barrier);
    g_sunShadowFarLayout = VK_IMAGE_LAYOUT_GENERAL;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_sunShadowFarPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_sunShadowFarPipelineLayout, 0u, 1u, &g_sunShadowFarSet, 0u, nullptr);
    const uint32_t groups =
        (SUN_SHADOW_FAR_RESOLUTION + SUN_SHADOW_FAR_WORKGROUP_SIZE - 1u) /
        SUN_SHADOW_FAR_WORKGROUP_SIZE;
    vkCmdDispatch(cmd, groups, groups, 1u);

    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
}
void destroySunShadowFarResources()
{
    if(g_sunShadowFarPipeline) vkDestroyPipeline(g_device, g_sunShadowFarPipeline, nullptr);
    if(g_sunShadowFarPipelineLayout) vkDestroyPipelineLayout(g_device, g_sunShadowFarPipelineLayout, nullptr);
    if(g_sunShadowFarDescriptorPool) vkDestroyDescriptorPool(g_device, g_sunShadowFarDescriptorPool, nullptr);
    if(g_sunShadowFarSetLayout) vkDestroyDescriptorSetLayout(g_device, g_sunShadowFarSetLayout, nullptr);
    if(g_sunShadowFarSampler) vkDestroySampler(g_device, g_sunShadowFarSampler, nullptr);
    if(g_sunShadowFarView) vkDestroyImageView(g_device, g_sunShadowFarView, nullptr);
    if(g_sunShadowFarImage) vkDestroyImage(g_device, g_sunShadowFarImage, nullptr);
    if(g_sunShadowFarMemory) vkFreeMemory(g_device, g_sunShadowFarMemory, nullptr);
    g_sunShadowFarPipeline = VK_NULL_HANDLE;
    g_sunShadowFarPipelineLayout = VK_NULL_HANDLE;
    g_sunShadowFarDescriptorPool = VK_NULL_HANDLE;
    g_sunShadowFarSet = VK_NULL_HANDLE;
    g_sunShadowFarSetLayout = VK_NULL_HANDLE;
    g_sunShadowFarSampler = VK_NULL_HANDLE;
    g_sunShadowFarView = VK_NULL_HANDLE;
    g_sunShadowFarImage = VK_NULL_HANDLE;
    g_sunShadowFarMemory = VK_NULL_HANDLE;
    g_sunShadowFarLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_sunShadowFarTlasHandle = VK_NULL_HANDLE;
    g_sunShadowFarAvailable.store(false);
}

// ---------------------------------------------------------------
// 地形高度场：资源、描述符、管线
//
// 与太阳阴影图逐条同构（见上面那段注释与 cpp/shaders/terrain_height.glsl）：
// 定尺、不进体积云的重建路径、需要 ray query 时静默降级。唯一多出来
// 的一步是首帧把整张图清成哨兵 —— 理由见 recordTerrainHeightMap。
// ---------------------------------------------------------------
void createTerrainHeightResources()
{
    if(g_terrainHeightPipeline != VK_NULL_HANDLE)
    {
        return;
    }
    if(g_physicalDevice == VK_NULL_HANDLE || g_device == VK_NULL_HANDLE ||
       g_instance == VK_NULL_HANDLE)
    {
        return;
    }

    auto getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceFeatures2"));
    if(getFeatures2 == nullptr)
    {
        getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceFeatures2KHR"));
    }
    if(getFeatures2 != nullptr)
    {
        VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR
        };
        VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features2.pNext = &rayQueryFeatures;
        getFeatures2(g_physicalDevice, &features2);
        if(rayQueryFeatures.rayQuery != VK_TRUE)
        {
            RTLog("[TERRAINHEIGHT] ray query unsupported on this device; "
                "volumetric fog has no ground height field");
            return;
        }
    }

    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(g_physicalDevice, VK_FORMAT_R32_SFLOAT,
        &formatProperties);
    const VkFormatFeatureFlags requiredFeatures =
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if((formatProperties.optimalTilingFeatures & requiredFeatures) != requiredFeatures)
    {
        RTLog("[TERRAINHEIGHT] R32_SFLOAT cannot be both stored and sampled; disabled");
        return;
    }

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R32_SFLOAT;
    imageInfo.extent = { TERRAIN_HEIGHT_RESOLUTION, TERRAIN_HEIGHT_RESOLUTION, 1u };
    imageInfo.mipLevels = 1u;
    imageInfo.arrayLayers = 1u;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateImage(g_device, &imageInfo, nullptr, &g_terrainHeightImage),
        "terrain height image create failed");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g_device, g_terrainHeightImage, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocation, nullptr, &g_terrainHeightMemory),
        "terrain height image allocate failed");
    check(vkBindImageMemory(g_device, g_terrainHeightImage, g_terrainHeightMemory, 0u),
        "terrain height image bind failed");

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = g_terrainHeightImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R32_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1u;
    viewInfo.subresourceRange.layerCount = 1u;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_terrainHeightView),
        "terrain height image view create failed");

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_terrainHeightSampler),
        "terrain height sampler create failed");

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
        &g_terrainHeightSetLayout), "terrain height set layout failed");

    VkDescriptorPoolSize poolSizes[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u },
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u }
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1u;
    poolInfo.poolSizeCount = 3u;
    poolInfo.pPoolSizes = poolSizes;
    check(vkCreateDescriptorPool(g_device, &poolInfo, nullptr,
        &g_terrainHeightDescriptorPool), "terrain height descriptor pool failed");

    VkDescriptorSetAllocateInfo setAllocation{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
    };
    setAllocation.descriptorPool = g_terrainHeightDescriptorPool;
    setAllocation.descriptorSetCount = 1u;
    setAllocation.pSetLayouts = &g_terrainHeightSetLayout;
    check(vkAllocateDescriptorSets(g_device, &setAllocation, &g_terrainHeightSet),
        "terrain height descriptor set allocation failed");

    VkDescriptorBufferInfo cameraInfo{};
    cameraInfo.buffer = g_cameraUboBuffer;
    cameraInfo.range = sizeof(CameraUBO);
    VkDescriptorImageInfo storageInfo{};
    storageInfo.imageView = g_terrainHeightView;
    storageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet writes[2]{};
    writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_terrainHeightSet, 0u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &cameraInfo, nullptr };
    writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_terrainHeightSet, 2u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &storageInfo, nullptr, nullptr };
    vkUpdateDescriptorSets(g_device, 2u, writes, 0u, nullptr);

    if(g_tlas != VK_NULL_HANDLE)
    {
        VkWriteDescriptorSetAccelerationStructureKHR tlasInfo{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR
        };
        tlasInfo.accelerationStructureCount = 1u;
        tlasInfo.pAccelerationStructures = &g_tlas;
        VkWriteDescriptorSet tlasWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        tlasWrite.pNext = &tlasInfo;
        tlasWrite.dstSet = g_terrainHeightSet;
        tlasWrite.dstBinding = 1u;
        tlasWrite.descriptorCount = 1u;
        tlasWrite.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        vkUpdateDescriptorSets(g_device, 1u, &tlasWrite, 0u, nullptr);
        g_terrainHeightTlasHandle = g_tlas;
    }

    const std::vector<uint8_t> code = loadSpv("shaders/rt_terrain_height.comp.spv");
    VkShaderModule shader = createShaderModule(g_device, code);
    try
    {
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        pipelineLayoutInfo.setLayoutCount = 1u;
        pipelineLayoutInfo.pSetLayouts = &g_terrainHeightSetLayout;
        check(vkCreatePipelineLayout(g_device, &pipelineLayoutInfo, nullptr,
            &g_terrainHeightPipelineLayout), "terrain height pipeline layout failed");
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
        pipelineInfo.layout = g_terrainHeightPipelineLayout;
        check(vkCreateComputePipelines(g_device, getSharedPipelineCache(), 1u,
            &pipelineInfo, nullptr, &g_terrainHeightPipeline),
            "terrain height pipeline failed");
    }
    catch(...)
    {
        vkDestroyShaderModule(g_device, shader, nullptr);
        destroyTerrainHeightResources();
        throw;
    }
    vkDestroyShaderModule(g_device, shader, nullptr);
    g_terrainHeightLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_terrainHeightAvailable.store(true);
    RTLog("[TERRAINHEIGHT] 512x512 ground height field ready (ray query, 262144 rays/frame)");
}
void recordTerrainHeightMap(VkCommandBuffer cmd)
{
    if(cmd == VK_NULL_HANDLE || g_terrainHeightImage == VK_NULL_HANDLE)
    {
        return;
    }

    if(g_terrainHeightLayout == VK_IMAGE_LAYOUT_UNDEFINED)
    {
        VkImageMemoryBarrier clearBarrier{};
        clearBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        clearBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        clearBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        clearBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        clearBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        clearBarrier.srcAccessMask = 0u;
        clearBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        clearBarrier.image = g_terrainHeightImage;
        clearBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clearBarrier.subresourceRange.levelCount = 1u;
        clearBarrier.subresourceRange.layerCount = 1u;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u,
            &clearBarrier);
        const VkClearColorValue emptyHeight = {{
            TERRAIN_HEIGHT_EMPTY_SENTINEL, 0.0f, 0.0f, 0.0f }};
        vkCmdClearColorImage(cmd, g_terrainHeightImage, VK_IMAGE_LAYOUT_GENERAL,
            &emptyHeight, 1u, &clearBarrier.subresourceRange);
        clearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        clearBarrier.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u,
            &clearBarrier);
        g_terrainHeightLayout = VK_IMAGE_LAYOUT_GENERAL;
    }

    if(!g_terrainHeightAvailable.load() ||
       g_terrainHeightPipeline == VK_NULL_HANDLE || g_terrainHeightSet == VK_NULL_HANDLE ||
       g_tlas == VK_NULL_HANDLE || g_terrainHeightTlasHandle != g_tlas)
    {
        return;
    }

    const uint64_t terrainHeightRevision =
        probeGeometryRevision();
    const bool terrainHeightForceAlways =
        std::getenv("ORION_TERRAIN_HEIGHT_ALWAYS") != nullptr;

    constexpr float kTerrainHeightCenterQuantum = 2.0f * 256.0f / 512.0f;

    float terrainHeightCenterX = 0.0f;
    float terrainHeightCenterZ = 0.0f;

    if(g_cameraUboMapped != nullptr)
    {
        const CameraUBO* heightUbo =
            static_cast<const CameraUBO*>(g_cameraUboMapped);

        terrainHeightCenterX =
            std::floor(heightUbo->viewInverse[12] / kTerrainHeightCenterQuantum) *
            kTerrainHeightCenterQuantum;
        terrainHeightCenterZ =
            std::floor(heightUbo->viewInverse[14] / kTerrainHeightCenterQuantum) *
            kTerrainHeightCenterQuantum;
    }

    if(!terrainHeightForceAlways &&
       g_terrainHeightBuilt &&
       g_terrainHeightBuiltRevision == terrainHeightRevision &&
       g_terrainHeightBuiltCenter[0] == terrainHeightCenterX &&
       g_terrainHeightBuiltCenter[1] == terrainHeightCenterZ)
    {
        return;
    }

    g_terrainHeightBuiltCenter[0] = terrainHeightCenterX;
    g_terrainHeightBuiltCenter[1] = terrainHeightCenterZ;

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = g_terrainHeightLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.image = g_terrainHeightImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1u;
    barrier.subresourceRange.layerCount = 1u;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u,
        &barrier);
    g_terrainHeightLayout = VK_IMAGE_LAYOUT_GENERAL;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_terrainHeightPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_terrainHeightPipelineLayout, 0u, 1u, &g_terrainHeightSet, 0u, nullptr);
    const uint32_t groups =
        (TERRAIN_HEIGHT_RESOLUTION + TERRAIN_HEIGHT_WORKGROUP_SIZE - 1u) /
        TERRAIN_HEIGHT_WORKGROUP_SIZE;
    vkCmdDispatch(cmd, groups, groups, 1u);
    g_terrainHeightBuilt = true;
    g_terrainHeightBuiltRevision = terrainHeightRevision;

    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u,
        &barrier);
}
void destroyTerrainHeightResources()
{
    g_terrainHeightBuilt = false;
    g_terrainHeightBuiltRevision = 0u;
    if(g_terrainHeightPipeline) vkDestroyPipeline(g_device, g_terrainHeightPipeline, nullptr);
    if(g_terrainHeightPipelineLayout) vkDestroyPipelineLayout(g_device, g_terrainHeightPipelineLayout, nullptr);
    if(g_terrainHeightDescriptorPool) vkDestroyDescriptorPool(g_device, g_terrainHeightDescriptorPool, nullptr);
    if(g_terrainHeightSetLayout) vkDestroyDescriptorSetLayout(g_device, g_terrainHeightSetLayout, nullptr);
    if(g_terrainHeightSampler) vkDestroySampler(g_device, g_terrainHeightSampler, nullptr);
    if(g_terrainHeightView) vkDestroyImageView(g_device, g_terrainHeightView, nullptr);
    if(g_terrainHeightImage) vkDestroyImage(g_device, g_terrainHeightImage, nullptr);
    if(g_terrainHeightMemory) vkFreeMemory(g_device, g_terrainHeightMemory, nullptr);
    g_terrainHeightPipeline = VK_NULL_HANDLE;
    g_terrainHeightPipelineLayout = VK_NULL_HANDLE;
    g_terrainHeightDescriptorPool = VK_NULL_HANDLE;
    g_terrainHeightSet = VK_NULL_HANDLE;
    g_terrainHeightSetLayout = VK_NULL_HANDLE;
    g_terrainHeightSampler = VK_NULL_HANDLE;
    g_terrainHeightView = VK_NULL_HANDLE;
    g_terrainHeightImage = VK_NULL_HANDLE;
    g_terrainHeightMemory = VK_NULL_HANDLE;
    g_terrainHeightLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_terrainHeightTlasHandle = VK_NULL_HANDLE;
    g_terrainHeightAvailable.store(false);
}
void ensureSunShadowFarDescriptor()
{
    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE)
    {
        return;
    }

    const bool farReady = g_sunShadowFarAvailable.load() &&
        g_sunShadowFarView != VK_NULL_HANDLE;

    VkImageView view = farReady ? g_sunShadowFarView : g_atlasImageView;
    VkSampler sampler = farReady ? g_sunShadowFarSampler : g_atlasSampler;

    if(view == VK_NULL_HANDLE)
    {
        g_sunShadowFarPublishedView = VK_NULL_HANDLE;
        g_sunShadowFarPublishedReady = false;
        return;
    }

    if(g_sunShadowFarPublishedView == view &&
       g_sunShadowFarPublishedReady == farReady)
    {
        return;
    }

    FGDeviceWaitIdle(g_device);

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView = view;
    imageInfo.sampler = sampler;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = g_rayTracingDescriptorSet;
    write.dstBinding = 58u;
    write.dstArrayElement = 0u;
    write.descriptorCount = 1u;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(g_device, 1u, &write, 0u, nullptr);

    g_sunShadowFarPublishedView = view;
    g_sunShadowFarPublishedReady = farReady;

    RTLog(std::string("[SUNSHADOW-FAR] 描述符已写 binding58=") +
        (farReady ? "1024² 远场图" : "回落图集（UBO 门 0）"));
}
void ensureSunShadowNearDescriptor()
{
    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE)
    {
        return;
    }

    const bool nearReady = g_sunShadowAvailable.load() &&
        g_sunShadowView != VK_NULL_HANDLE;

    VkImageView view = nearReady ? g_sunShadowView : g_atlasImageView;
    VkSampler sampler = nearReady ? g_sunShadowSampler : g_atlasSampler;

    if(view == VK_NULL_HANDLE)
    {
        g_sunShadowNearPublishedView = VK_NULL_HANDLE;
        g_sunShadowNearPublishedReady = false;
        return;
    }

    if(g_sunShadowNearPublishedView == view &&
       g_sunShadowNearPublishedReady == nearReady)
    {
        return;
    }

    FGDeviceWaitIdle(g_device);

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView = view;
    imageInfo.sampler = sampler;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = g_rayTracingDescriptorSet;
    write.dstBinding = 79u;
    write.dstArrayElement = 0u;
    write.descriptorCount = 1u;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(g_device, 1u, &write, 0u, nullptr);

    g_sunShadowNearPublishedView = view;
    g_sunShadowNearPublishedReady = nearReady;

    RTLog(std::string("[SUNSHADOW-NEAR] 描述符已写 binding79=") +
        (nearReady ? "1024² 近场图" : "回落图集"));
}


// ============================================================================
// 2026-11-11 太阳可见度屏幕空间滤波（软影 A 方案，形态 1）
//
// 动机见 cpp/shaders/rt_sunvis_filter.comp 的头部注释。三段式：
//   raygen 打 1 条阴影射线 → sunVisRaw（binding 90, 全分辨率）
//   本模块的 rt_sunvis_filter.comp 在**半分辨率**上按遮挡距离做变半径加权平均
//     → sunVisHistory[1-historyReadIndex]（binding 91, 两层 ping-pong）
//   下一帧 raygen 按 MV 重投影采样 sunVisHistory[historyReadIndex]
//
// 2026-11-12：**默认启用**（用户要求落实进主代码，不再靠环境变量开启）。
// 只有显式 `ORION_SUNVIS=0` 才退回旧的 N-tap 太阳盘采样路径 —— 留这个出口只为
// 出问题时能一次重启定位，正常游玩不需要任何环境变量。
// 可调量（改这些**不需要重编着色器**，重启客户端即可）：
//   ORION_SUNVIS_APERTURE     光源角半径(rad)，默认 0.030
//   ORION_SUNVIS_MAXRADIUS    半分辨率核半径上限(像素)，默认 16
//   ORION_SUNVIS_DEPTHTOL     深度拒绝比例，默认 0.05
// 布局口径跟 ss_hybrid/ss_hzb 一致：直接用 g_rayTracingDescriptorSet（set 0），
// 所以不需要新的 set layout / pool。
// ============================================================================
namespace
{
uint32_t g_sunVisRenderWidth = 0u;
uint32_t g_sunVisRenderHeight = 0u;
uint32_t g_sunVisHalfWidth = 0u;
uint32_t g_sunVisHalfHeight = 0u;

VkImage g_sunVisRawImage = VK_NULL_HANDLE;
VkDeviceMemory g_sunVisRawMemory = VK_NULL_HANDLE;
VkImageView g_sunVisRawView = VK_NULL_HANDLE;
VkImageLayout g_sunVisRawLayout = VK_IMAGE_LAYOUT_UNDEFINED;

VkImage g_sunVisHistoryImages[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
VkDeviceMemory g_sunVisHistoryMemories[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
VkImageView g_sunVisHistoryViews[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
VkImageLayout g_sunVisHistoryLayouts[2] = {
    VK_IMAGE_LAYOUT_UNDEFINED,
    VK_IMAGE_LAYOUT_UNDEFINED};

VkPipelineLayout g_sunVisPipelineLayout = VK_NULL_HANDLE;
VkPipeline g_sunVisPipeline = VK_NULL_HANDLE;
VkDescriptorSetLayout g_sunVisPipelineSetLayout = VK_NULL_HANDLE;
bool g_sunVisPipelineFailed = false;

VkDescriptorSet g_sunVisBoundSet = VK_NULL_HANDLE;

float sunVisEnvFloat(
    const char* name,
    float fallback)
{
    const char* raw = std::getenv(name);

    if(raw == nullptr || raw[0] == '\0')
    {
        return fallback;
    }

    const float value = static_cast<float>(std::atof(raw));

    return std::isfinite(value) ? value : fallback;
}

void destroySunVisResourcesLocked()
{
    for(int i = 0; i < 2; ++i)
    {
        destroyHistoryImage(
            g_sunVisHistoryImages[i],
            g_sunVisHistoryMemories[i],
            g_sunVisHistoryViews[i],
            g_sunVisHistoryLayouts[i]);
    }

    destroyHistoryImage(
        g_sunVisRawImage,
        g_sunVisRawMemory,
        g_sunVisRawView,
        g_sunVisRawLayout);

    g_sunVisRenderWidth = 0u;
    g_sunVisRenderHeight = 0u;
    g_sunVisHalfWidth = 0u;
    g_sunVisHalfHeight = 0u;
    g_sunVisBoundSet = VK_NULL_HANDLE;
}

// 建图之后立刻做一次性转换：两张 history 清成 1.0（全亮），raw 转到 GENERAL。
// 只在创建/分辨率变化时跑，用阻塞提交（与其它 create 函数同量级成本）。
void sunVisPrimeImages()
{
    if(g_rtCommandPool == VK_NULL_HANDLE || g_queue == VK_NULL_HANDLE)
    {
        return;
    }

    VkCommandBufferAllocateInfo allocateInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocateInfo.commandPool = g_rtCommandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1u;

    VkCommandBuffer cmd = VK_NULL_HANDLE;

    if(vkAllocateCommandBuffers(g_device, &allocateInfo, &cmd) != VK_SUCCESS ||
       cmd == VK_NULL_HANDLE)
    {
        return;
    }

    VkCommandBufferBeginInfo beginInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(cmd, &beginInfo);

    VkImageMemoryBarrier barriers[3]{};
    const uint32_t count =
        (g_sunVisRawImage != VK_NULL_HANDLE) ? 3u : 2u;

    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].srcAccessMask = 0u;
    barriers[0].dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].image = g_sunVisRawImage;
    barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[0].subresourceRange.levelCount = 1u;
    barriers[0].subresourceRange.layerCount = 1u;

    for(int i = 0; i < 2; ++i)
    {
        VkImageMemoryBarrier& barrier = barriers[i + 1u];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = 0u;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = g_sunVisHistoryImages[i];
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1u;
        barrier.subresourceRange.layerCount = 1u;
    }

    if(count == 3u)
    {
        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT |
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u,
            0u, nullptr,
            0u, nullptr,
            count,
            barriers);
    }
    else
    {
        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0u,
            0u, nullptr,
            0u, nullptr,
            2u,
            barriers + 1);
    }

    VkClearColorValue litValue{};
    litValue.float32[0] = 1.0f;
    litValue.float32[1] = 0.0f;
    litValue.float32[2] = 0.0f;
    litValue.float32[3] = 0.0f;

    for(int i = 0; i < 2; ++i)
    {
        if(g_sunVisHistoryImages[i] == VK_NULL_HANDLE)
        {
            continue;
        }

        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1u;
        range.layerCount = 1u;

        vkCmdClearColorImage(
            cmd,
            g_sunVisHistoryImages[i],
            VK_IMAGE_LAYOUT_GENERAL,
            &litValue,
            1u,
            &range);
    }

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1u;
    submitInfo.pCommandBuffers = &cmd;

    vkQueueWaitIdle(g_queue);
    vkQueueSubmit(g_queue, 1u, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_queue);

    vkFreeCommandBuffers(g_device, g_rtCommandPool, 1u, &cmd);

    g_sunVisRawLayout = VK_IMAGE_LAYOUT_GENERAL;

    for(int i = 0; i < 2; ++i)
    {
        g_sunVisHistoryLayouts[i] = VK_IMAGE_LAYOUT_GENERAL;
    }
}

void ensureSunVisPipeline()
{
    if(g_sunVisPipeline != VK_NULL_HANDLE &&
       g_sunVisPipelineSetLayout == g_rayTracingDescriptorSetLayout)
    {
        return;
    }

    if(g_sunVisPipelineFailed)
    {
        return;
    }

    if(g_device == VK_NULL_HANDLE ||
       g_rayTracingDescriptorSetLayout == VK_NULL_HANDLE)
    {
        return;
    }

    if(g_sunVisPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_sunVisPipeline, nullptr);
        g_sunVisPipeline = VK_NULL_HANDLE;
    }

    if(g_sunVisPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(g_device, g_sunVisPipelineLayout, nullptr);
        g_sunVisPipelineLayout = VK_NULL_HANDLE;
    }

    try
    {
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1u;
        pipelineLayoutInfo.pSetLayouts = &g_rayTracingDescriptorSetLayout;

        check(
            vkCreatePipelineLayout(
                g_device,
                &pipelineLayoutInfo,
                nullptr,
                &g_sunVisPipelineLayout),
            "sun vis pipeline layout failed");

        const std::vector<uint8_t> code =
            loadSpv("shaders/rt_sunvis_filter.comp.spv");

        VkShaderModule shader = createShaderModule(g_device, code);

        VkPipelineShaderStageCreateInfo stage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shader;
        stage.pName = "main";

        VkComputePipelineCreateInfo pipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = stage;
        pipelineInfo.layout = g_sunVisPipelineLayout;

        const VkResult result = vkCreateComputePipelines(
            g_device,
            getSharedPipelineCache(),
            1u,
            &pipelineInfo,
            nullptr,
            &g_sunVisPipeline);

        vkDestroyShaderModule(g_device, shader, nullptr);

        if(result != VK_SUCCESS)
        {
            throw std::runtime_error("sun vis compute pipeline failed");
        }

        g_sunVisPipelineSetLayout = g_rayTracingDescriptorSetLayout;
    }
    catch(...)
    {
        g_sunVisPipelineFailed = true;
        g_sunVisPipeline = VK_NULL_HANDLE;
        RTLog("[SUNVIS] 管线创建失败，本会话关闭该 pass");
    }
}
} // namespace


bool sunVisEnabled()
{
    // 2026-11-12 回退：默认**关**。
    //
    // 原因（用户实测"好多噪点"）：这条 A 路径是"1 条 **确定性** 射线 + 空域
    // 滤波"，而参考实现（Caustica 的 RR-only 路径，见 trace_transport.slang 的
    // traceDirect / retained_lights.slang 的 angularRadius）用的是
    // **1 spp 随机太阳盘采样 + DLSS-RR 时域收敛**。没有时域累积时，
    // 1 条确定性射线的方差是 N tap 平均的 N 倍 ⇒ 必然比原来更噪。
    // 要显式复测这条路径：ORION_SUNVIS=1。
    static const bool cached = []() -> bool
    {
        const char* raw = std::getenv("ORION_SUNVIS");

        if(raw == nullptr || raw[0] == '\0')
        {
            return false;
        }

        return raw[0] != '0';
    }();

    return cached;
}

float sunVisAperture()
{
    static const float cached =
        sunVisEnvFloat("ORION_SUNVIS_APERTURE", 0.030f);

    return cached;
}

float sunVisMaxRadius()
{
    static const float cached =
        sunVisEnvFloat("ORION_SUNVIS_MAXRADIUS", 16.0f);

    return cached;
}

float sunVisDepthTolerance()
{
    static const float cached =
        sunVisEnvFloat("ORION_SUNVIS_DEPTHTOL", 0.05f);

    return cached;
}

void createSunVisResources(
    uint32_t width,
    uint32_t height)
{
    if(!sunVisEnabled() || width == 0u || height == 0u)
    {
        return;
    }

    // 2026-11-12：默认启用后这条早退必须同时确认"描述符仍是当前那一份 set"。
    // RT 描述符集若被重建（管线/布局变化），旧的 binding 90/91 就悬空了 ——
    // 只要它变了就整组重建（分辨率不变也重建），比逐帧猜便宜。
    if(g_sunVisRawImage != VK_NULL_HANDLE &&
       g_sunVisRenderWidth == width &&
       g_sunVisRenderHeight == height &&
       g_sunVisBoundSet == g_rayTracingDescriptorSet &&
       g_rayTracingDescriptorSet != VK_NULL_HANDLE)
    {
        return;
    }

    destroySunVisResourcesLocked();

    const uint32_t halfWidth = std::max(1u, (width + 1u) / 2u);
    const uint32_t halfHeight = std::max(1u, (height + 1u) / 2u);

    createRRGuideImage(
        width,
        height,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        g_sunVisRawImage,
        g_sunVisRawMemory,
        g_sunVisRawView,
        g_sunVisRawLayout);

    g_sunVisRawLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    for(int i = 0; i < 2; ++i)
    {
        createRRGuideImage(
            halfWidth,
            halfHeight,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            g_sunVisHistoryImages[i],
            g_sunVisHistoryMemories[i],
            g_sunVisHistoryViews[i],
            g_sunVisHistoryLayouts[i]);

        g_sunVisHistoryLayouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    if(g_rayTracingDescriptorSet != VK_NULL_HANDLE)
    {
        VkDescriptorImageInfo infos[3]{};

        infos[0].imageView = g_sunVisRawView;
        infos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        infos[1].imageView = g_sunVisHistoryViews[0];
        infos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        infos[2].imageView = g_sunVisHistoryViews[1];
        infos[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2]{};

        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = g_rayTracingDescriptorSet;
        writes[0].dstBinding = 90u;
        writes[0].dstArrayElement = 0u;
        writes[0].descriptorCount = 1u;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &infos[0];

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = g_rayTracingDescriptorSet;
        writes[1].dstBinding = 91u;
        writes[1].dstArrayElement = 0u;
        writes[1].descriptorCount = 2u;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &infos[1];

        vkUpdateDescriptorSets(g_device, 2u, writes, 0u, nullptr);

        g_sunVisBoundSet = g_rayTracingDescriptorSet;
    }

    sunVisPrimeImages();

    ensureSunVisPipeline();

    g_sunVisRenderWidth = width;
    g_sunVisRenderHeight = height;
    g_sunVisHalfWidth = halfWidth;
    g_sunVisHalfHeight = halfHeight;

    RTLogFlushed(
        "[SUNVIS] enabled raw=" + std::to_string(width) + "x" +
        std::to_string(height) + " history=" + std::to_string(halfWidth) +
        "x" + std::to_string(halfHeight) +
        " aperture=" + std::to_string(sunVisAperture()) +
        " maxR=" + std::to_string(sunVisMaxRadius()) +
        " depthTol=" + std::to_string(sunVisDepthTolerance()));
}

void recordSunVisFilter(VkCommandBuffer cmd)
{
    if(!sunVisEnabled() || cmd == VK_NULL_HANDLE)
    {
        return;
    }

    if(g_sunVisPipeline == VK_NULL_HANDLE ||
       g_sunVisRawImage == VK_NULL_HANDLE ||
       g_sunVisBoundSet != g_rayTracingDescriptorSet ||
       g_sunVisHalfWidth == 0u ||
       g_sunVisHalfHeight == 0u)
    {
        return;
    }

    // raygen 刚写过 raw（binding 90）⇒ RT → COMPUTE 的可见性。
    // 两张 history 本帧只写一张，另一张留给 raygen 读，故两张都要把
    // 上一帧的写完成（SHADER_WRITE → SHADER_WRITE）。
    VkImageMemoryBarrier before[3]{};

    before[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    before[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    before[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    before[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    before[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    before[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    before[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    before[0].image = g_sunVisRawImage;
    before[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    before[0].subresourceRange.levelCount = 1u;
    before[0].subresourceRange.layerCount = 1u;

    for(int i = 0; i < 2; ++i)
    {
        VkImageMemoryBarrier& barrier = before[i + 1u];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = g_sunVisHistoryImages[i];
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1u;
        barrier.subresourceRange.layerCount = 1u;
    }

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0u,
        0u, nullptr,
        0u, nullptr,
        3u,
        before);

    vkCmdBindPipeline(
        cmd,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        g_sunVisPipeline);

    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        g_sunVisPipelineLayout,
        0u,
        1u,
        &g_rayTracingDescriptorSet,
        0u,
        nullptr);

    vkCmdDispatch(
        cmd,
        (g_sunVisHalfWidth + 7u) / 8u,
        (g_sunVisHalfHeight + 7u) / 8u,
        1u);

    // 下一帧 raygen 要读 history（binding 91）⇒ COMPUTE → RT 的可见性。
    VkImageMemoryBarrier after[2]{};

    for(int i = 0; i < 2; ++i)
    {
        after[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        after[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        after[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        after[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        after[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        after[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        after[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        after[i].image = g_sunVisHistoryImages[i];
        after[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        after[i].subresourceRange.levelCount = 1u;
        after[i].subresourceRange.layerCount = 1u;
    }

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0u,
        0u, nullptr,
        0u, nullptr,
        2u,
        after);
}
