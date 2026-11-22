// ============================================================
// orion_precache.exe — RT 管线无头预编译工具（2026-11-22 冷编治理 批B）
//
// 动机：k2sun-direct 等 4 条 K1/K2 腿管线的驱动冷编要 20GB 级 commit 与
// 9.5~50+ 分钟，游戏内编译会被内存挤兑 / 玩家退出世界 / 进程退出打断，
// 且失败只写 std::cerr（rt_debug.log 里静默）。本工具把同一份编译搬到
// 独立进程：机器资源全部可用、可挂机跑完、成功即落盘管线缓存，
// 游戏下一次启动四条腿全部 cache hit（~100-200ms/条）。
//
// 用法：把本 exe 放在与 rtx_backend.dll 相同的目录（run\ 或玩家游戏目录）
// 运行，缓存与 spv 都按 GetRTXBackendDirectory()（= exe 所在目录）解析：
//   orion_precache.exe [--adapter N]
//
// 设备/扩展/feature 配置与 VulkanBackendMixin.java 注入的游戏设备逐位对齐
// （扩展探测、feature 链、OMM/SER 探测顺序照抄）——对齐失败只会导致游戏侧
// 缓存 miss（安全回落游戏内编译），不会崩溃。
// 复用 backend 的 ensureK1K2Pipelines()（Sharc.cpp），它只依赖
// g_device/g_physicalDevice/g_instance + 管线 layout + 缓存句柄，
// 设备无关、不碰游戏状态（见 ensureK1K2Pipelines 头部 guard）。
// ============================================================

#include "../../backend/OrionInternal.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

VkInstance g_pcInstance = VK_NULL_HANDLE;

[[noreturn]] void fail(const std::string& message)
{
    fprintf(stderr, "[PRECACHE] FATAL %s\n", message.c_str());
    fflush(stderr);
    RTLogFlushed(std::string("[PRECACHE] FATAL ") + message);
    std::exit(1);
}

bool deviceHasExtension(
    VkPhysicalDevice physicalDevice,
    const char* extensionName)
{
    uint32_t count = 0u;
    if(vkEnumerateDeviceExtensionProperties(
           physicalDevice, nullptr, &count, nullptr) != VK_SUCCESS ||
       count == 0u)
    {
        return false;
    }

    std::vector<VkExtensionProperties> props(count);
    if(vkEnumerateDeviceExtensionProperties(
           physicalDevice, nullptr, &count, props.data()) != VK_SUCCESS)
    {
        return false;
    }

    for(const VkExtensionProperties& p : props)
    {
        if(std::strcmp(p.extensionName, extensionName) == 0)
        {
            return true;
        }
    }
    return false;
}

// 与 VulkanBackendMixin.java 的注入清单一致（2026-11-22 读源核对）：
// 必需 3 项 + 支持才加的 5 项。FG/NVX/opticalflow 与管线缓存无关，不请求。
struct WantedExtension
{
    const char* name;
    bool required;
};

constexpr WantedExtension WANTED_EXTENSIONS[] = {
    { VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, true },
    { VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, true },
    { VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, true },
    { VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, false },
    { VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, false },
    { VK_KHR_RAY_QUERY_EXTENSION_NAME, false },
    { VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME, false },
    { VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME, false },
};

} // namespace

int main(int argc, char** argv)
{
    const auto start = std::chrono::steady_clock::now();

    int adapterIndex = 0;
    for(int i = 1; i < argc; ++i)
    {
        if(std::strcmp(argv[i], "--adapter") == 0 && i + 1 < argc)
        {
            adapterIndex = std::atoi(argv[++i]);
        }
    }

    RTLogFlushed("[PRECACHE] start");

    // ---- instance ------------------------------------------------------
    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "orion_precache";
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &appInfo;

    if(vkCreateInstance(&ici, nullptr, &g_pcInstance) != VK_SUCCESS)
    {
        fail("vkCreateInstance failed");
    }

    // ---- physical device ------------------------------------------------
    uint32_t gpuCount = 0u;
    if(vkEnumeratePhysicalDevices(g_pcInstance, &gpuCount, nullptr) !=
           VK_SUCCESS ||
       gpuCount == 0u)
    {
        fail("no physical devices");
    }

    std::vector<VkPhysicalDevice> gpus(gpuCount);
    if(vkEnumeratePhysicalDevices(g_pcInstance, &gpuCount, gpus.data()) !=
       VK_SUCCESS)
    {
        fail("vkEnumeratePhysicalDevices failed");
    }

    if(adapterIndex < 0 || static_cast<uint32_t>(adapterIndex) >= gpuCount)
    {
        fail("adapter index out of range");
    }

    VkPhysicalDevice physicalDevice = gpus[static_cast<size_t>(adapterIndex)];

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice, &props);

    std::vector<std::string> enabledExtensions;
    for(const WantedExtension& want : WANTED_EXTENSIONS)
    {
        const bool supported =
            deviceHasExtension(physicalDevice, want.name);
        if(supported)
        {
            enabledExtensions.push_back(want.name);
        }
        else if(want.required)
        {
            fail(std::string("required extension missing: ") + want.name);
        }
        fprintf(
            stderr,
            "[PRECACHE] ext %-48s supported=%d\n",
            want.name,
            supported ? 1 : 0);
    }
    fflush(stderr);

    // ---- queue ----------------------------------------------------------
    uint32_t familyCount = 0u;
    vkGetPhysicalDeviceQueueFamilyProperties(
        physicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(
        physicalDevice, &familyCount, families.data());

    int queueFamily = -1;
    for(uint32_t i = 0; i < familyCount; ++i)
    {
        const VkQueueFlags flags = families[i].queueFlags;
        if((flags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
           (flags & VK_QUEUE_COMPUTE_BIT) != 0)
        {
            queueFamily = static_cast<int>(i);
            break;
        }
    }
    if(queueFamily < 0)
    {
        fail("no graphics+compute queue family");
    }

    // ---- feature chain（逐位对齐 VulkanBackendMixin.java 的注入清单）----
    // 链序：core Features2 → VK11 → VK12 → KHR/EXT 结构体。
    // 先整链 query，再按支持位使能——与游戏侧 rtxAddExtensionIfSupported
    // / supportsSerFeature 的"缺就降级并打日志"语义一致。
    VkPhysicalDeviceFeatures2 f2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features f11{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR fAS{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR fRTP{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR fRQ{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceOpacityMicromapFeaturesEXT fOMM{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT};
    VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT fSER{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_EXT};

    f2.pNext = &f11;
    f11.pNext = &f12;
    f12.pNext = &fAS;
    fAS.pNext = &fRTP;
    fRTP.pNext = &fRQ;
    fRQ.pNext = &fOMM;
    fOMM.pNext = &fSER;

    auto queryFn = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        vkGetInstanceProcAddr(g_pcInstance, "vkGetPhysicalDeviceFeatures2"));
    if(!queryFn)
    {
        fail("vkGetPhysicalDeviceFeatures2 unavailable");
    }
    queryFn(physicalDevice, &f2);

    // 按支持使能；缺哪位打一行（与游戏注入日志同风格）。
    struct FeatureRequest
    {
        const char* name;
        VkBool32 supported;
        VkBool32* enable;
    };
    const FeatureRequest requests[] = {
        { "shaderInt64", f2.features.shaderInt64,
          &f2.features.shaderInt64 },
        { "shaderStorageImageReadWithoutFormat",
          f2.features.shaderStorageImageReadWithoutFormat,
          &f2.features.shaderStorageImageReadWithoutFormat },
        { "shaderStorageImageWriteWithoutFormat",
          f2.features.shaderStorageImageWriteWithoutFormat,
          &f2.features.shaderStorageImageWriteWithoutFormat },
        { "storageBuffer16BitAccess(VK11)", f11.storageBuffer16BitAccess,
          &f11.storageBuffer16BitAccess },
        { "bufferDeviceAddress(VK12)", f12.bufferDeviceAddress,
          &f12.bufferDeviceAddress },
        { "descriptorBindingPartiallyBound(VK12)",
          f12.descriptorBindingPartiallyBound,
          &f12.descriptorBindingPartiallyBound },
        { "runtimeDescriptorArray(VK12)", f12.runtimeDescriptorArray,
          &f12.runtimeDescriptorArray },
        { "shaderFloat16(VK12)", f12.shaderFloat16,
          &f12.shaderFloat16 },
        { "shaderBufferInt64Atomics(VK12)", f12.shaderBufferInt64Atomics,
          &f12.shaderBufferInt64Atomics },
        { "accelerationStructure", fAS.accelerationStructure,
          &fAS.accelerationStructure },
        { "rayTracingPipeline", fRTP.rayTracingPipeline,
          &fRTP.rayTracingPipeline },
        { "rayQuery", fRQ.rayQuery, &fRQ.rayQuery },
        { "micromap(OMM)", fOMM.micromap, &fOMM.micromap },
        { "reordering(SER)", fSER.rayTracingInvocationReorder,
          &fSER.rayTracingInvocationReorder },
    };

    for(const FeatureRequest& req : requests)
    {
        fprintf(
            stderr,
            "[PRECACHE] feature %-42s supported=%d\n",
            req.name,
            req.supported ? 1 : 0);
        *(req.enable) =
            req.supported ? VK_TRUE : VK_FALSE;
    }
    fflush(stderr);

    // ---- device ---------------------------------------------------------
    const float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo qci{
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = static_cast<uint32_t>(queueFamily);
    qci.queueCount = 1u;
    qci.pQueuePriorities = &queuePriority;

    std::vector<const char*> extPtrs;
    extPtrs.reserve(enabledExtensions.size());
    for(const std::string& e : enabledExtensions)
    {
        extPtrs.push_back(e.c_str());
    }

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1u;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount =
        static_cast<uint32_t>(extPtrs.size());
    dci.ppEnabledExtensionNames = extPtrs.data();

    VkDevice device = VK_NULL_HANDLE;
    if(vkCreateDevice(physicalDevice, &dci, nullptr, &device) != VK_SUCCESS)
    {
        fail("vkCreateDevice failed");
    }

    // ---- backend 全局量接线（ensureK1K2Pipelines 的全部依赖）------------
    g_instance = g_pcInstance;
    g_physicalDevice = physicalDevice;
    g_device = device;

    // OMM：照游戏顺序 probe（As.cpp probeOpacityMicromapSupport），
    // 它读扩展入口点 + maxOpacity4StateSubdivisionLevel，写 g_ommNativeEnabled。
    probeOpacityMicromapSupport();

    // SER：spv 变体选择要 serShaderVariantWanted()==true ⇒
    // g_serEnabled（默认 true）+ g_serFeatureSupported。
    // supported 判据与游戏 supportsSerFeature 一致：扩展在 + feature 位在。
    const bool serExtensionEnabled =
        std::find(
            enabledExtensions.begin(),
            enabledExtensions.end(),
            std::string(VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME)) !=
        enabledExtensions.end();
    g_serFeatureSupported =
        serExtensionEnabled && fSER.rayTracingInvocationReorder == VK_TRUE;
    RTLogFlushed(
        std::string("[PRECACHE] serFeatureSupported=") +
        (g_serFeatureSupported ? "1" : "0"));

    // RT 管线 layout：与 Device.cpp:124-128 逐位一致。
    g_rayTracingDescriptorSetLayout = createRTDescriptorSetLayout();
    VkPipelineLayoutCreateInfo pli{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1u;
    pli.pSetLayouts = &g_rayTracingDescriptorSetLayout;
    if(vkCreatePipelineLayout(
           g_device, &pli, nullptr, &g_rayTracingPipelineLayout) != VK_SUCCESS)
    {
        fail("vkCreatePipelineLayout failed");
    }

    ensureSbtHandleMetrics();
    getSharedPipelineCache();

    // ---- 编译 4 条腿（顺序，与游戏 boot worker 同序）--------------------
    RTLogFlushed("[PRECACHE] compiling K1/K2 legs...");
    const auto legsStart = std::chrono::steady_clock::now();
    ensureK1K2Pipelines();
    const double legsMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - legsStart).count();

    const bool failed = g_k1K2PipelineFailed;
    const bool k1Ok = g_k1Pipeline != VK_NULL_HANDLE;
    const bool sunOk = g_k2sunPipeline != VK_NULL_HANDLE;
    const bool locOk = g_k2locPipeline != VK_NULL_HANDLE;
    const bool iOk = g_k2iPipeline != VK_NULL_HANDLE;

    // ---- 落盘 ------------------------------------------------------------
    if(!failed)
    {
        PersistSharedPipelineCacheNow();
    }

    char summary[512]{};
    std::snprintf(
        summary,
        sizeof(summary),
        "[PRECACHE] done legsMs=%.0f k1=%d k2sun=%d k2loc=%d k2i=%d failed=%d "
        "cache=%s",
        legsMs,
        k1Ok ? 1 : 0,
        sunOk ? 1 : 0,
        locOk ? 1 : 0,
        iOk ? 1 : 0,
        failed ? 1 : 0,
        GetPipelineCacheFilePath().c_str());
    fprintf(stderr, "%s\n", summary);
    fflush(stderr);
    RTLogFlushed(summary);

    const double totalMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    fprintf(stderr, "[PRECACHE] totalMs=%.0f\n", totalMs);
    fflush(stderr);

    return failed ? 2 : 0;
}
