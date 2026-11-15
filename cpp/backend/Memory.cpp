// Orion backend — Memory 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"

uint32_t findMemoryType(VkPhysicalDevice pd,uint32_t filter,VkMemoryPropertyFlags props){
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd,&mp);


    for(uint32_t i=0;i<mp.memoryTypeCount;i++)
    {
        if((filter&(1<<i))&&(mp.memoryTypes[i].propertyFlags&props)==props)
        {
            return i;
        }
    }

    throw std::runtime_error("Failed to find suitable memory type");
}

// =====================================================
// 2026-10-09（内存/卡顿根因修复）：**上传类**缓冲的内存类型选择。
//
// 病灶：上面的 findMemoryType() 取的是**下标最小**的匹配项。而 Vulkan 的类型表
// 通常把 `DEVICE_LOCAL | HOST_VISIBLE`（BAR / window 堆 —— 没开 ReBAR 时只有
// 256MB）排在前面，把"系统内存里的 host-visible"排在后面。于是本工程**所有**
// HOST_VISIBLE 缓冲都被塞进那块小堆，其中两类是"主机写、设备只读一次"的上传缓冲：
//   · Section 的 OMM 输入（三角形数 × 64B，实测 ~1.1MB/section，每个 CUTOUT
//     Section 一份；[RT MEM] 的 omm= 峰值 835 ⇒ 量级约 1GB）
//   · 纹理上传 staging（`capacity = bytes*1.5 + 64KB`，实测单次 7~20MB）
//
// 现场证据（[PERF L10 ALLOC]，2026-10-09 那一场）：
//     usage=0x820000（OMM 输入）request=1186432  ms=41.26
//     usage=0x820000（OMM 输入）request=1038848  ms=54.77
//     usage=0x1（纹理 staging） request=7~20MB   ms=3.2~10.7（十几连发）
//     usage=0x1                request=36576     ms=9.02   ← 36KB 也要 9ms
// 最后一行是关键：36KB 的分配要 9ms，只可能是 BAR 堆被超额认购后驱动在整理/驱逐。
// 而溢出的部分落到系统内存 ⇒ [RT MEM] 里 rssMB/privateMB 涨，而原生各结构全都有界。
//
// 所以：上传缓冲应当放在**系统内存**里（这正是 staging 内存的用途）。这里显式
// 优先选"满足 props 且**不在 DEVICE_LOCAL 堆**"的类型；平台没有这种类型时
// 逐位退回原选择器，行为与改动前完全一致。
//
// 只给上传/构建输入用，**不要**给每帧被光线追踪反复读的缓冲（本地光、动态光、
// 地址表、TLAS 实例表）用 —— 那些走 PCIe 会变慢，保持现状。
uint32_t findHostUploadMemoryType(VkPhysicalDevice pd,uint32_t filter,VkMemoryPropertyFlags props){
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd,&mp);

    for(uint32_t i=0;i<mp.memoryTypeCount;i++)
    {
        if((filter&(1u<<i))==0u)
        {
            continue;
        }
        if((mp.memoryTypes[i].propertyFlags&props)!=props)
        {
            continue;
        }
        if((mp.memoryHeaps[mp.memoryTypes[i].heapIndex].flags&
            VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)!=0u)
        {
            continue;
        }
        return i;
    }

    return findMemoryType(pd,filter,props);
}
void check(VkResult r,const char* s){ if(r != VK_SUCCESS) { const std::string msg = std::string("[CHECK FAILED] ") + s + " VkResult=" + std::to_string((int)r); RTLogFlushed(msg); throw std::runtime_error(msg); } }
sl::float4x4 JOMLColumnMajorToSLRowMajor(
    const float m[16]
)
{
    sl::float4x4 out{};

    out[0] = sl::float4(
        m[0],
        m[1],
        m[2],
        m[3]
    );

    out[1] = sl::float4(
        m[4],
        m[5],
        m[6],
        m[7]
    );

    out[2] = sl::float4(
        m[8],
        m[9],
        m[10],
        m[11]
    );

    out[3] = sl::float4(
        m[12],
        m[13],
        m[14],
        m[15]
    );

    return out;
}
void createRTCommandPool()
{
    VkCommandPoolCreateInfo ci{
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO
    };

    ci.queueFamilyIndex = g_queueFamily;

    ci.flags =
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;


    check(
        vkCreateCommandPool(
            g_device,
            &ci,
            nullptr,
            &g_rtCommandPool
        ),
        "Failed to create RT command pool"
    );


}
void createBuffer(VkDevice d,VkPhysicalDevice pd,VkDeviceSize size,VkBufferUsageFlags usage,VkMemoryPropertyFlags props,VkBuffer& buffer,VkDeviceMemory& memory,bool preferHostUploadMemory){
    const std::chrono::steady_clock::time_point perfL10AllocStart = std::chrono::steady_clock::now();
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=size;bi.usage=usage;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(d,&bi,nullptr,&buffer),"Failed to create buffer");
    VkMemoryRequirements mr{};vkGetBufferMemoryRequirements(d,buffer,&mr);
    VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=mr.size;
    ai.memoryTypeIndex = preferHostUploadMemory
        ? findHostUploadMemoryType(pd,mr.memoryTypeBits,props)
        : findMemoryType(pd,mr.memoryTypeBits,props);
    if(usage&VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT){fi.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;ai.pNext=&fi;}
    VkResult r=vkAllocateMemory(d,&ai,nullptr,&memory);
    if(r!=VK_SUCCESS){
        vkDestroyBuffer(d,buffer,nullptr);buffer=VK_NULL_HANDLE;
        RTLogFlushed(
            std::string("[ALLOC FAIL] VkResult=") + std::to_string(static_cast<int>(r)) +
            " request=" + std::to_string(static_cast<unsigned long long>(size)) +
            " aligned=" + std::to_string(static_cast<unsigned long long>(mr.size)) +
            " usage=0x" + std::to_string(static_cast<unsigned long long>(usage)));
        check(r,"Failed to allocate buffer memory");
    }
    r=vkBindBufferMemory(d,buffer,memory,0);
    if(r!=VK_SUCCESS){vkFreeMemory(d,memory,nullptr);vkDestroyBuffer(d,buffer,nullptr);memory=VK_NULL_HANDLE;buffer=VK_NULL_HANDLE;check(r,"Failed to bind buffer memory");}

    rtxNoteBufferMemory(
        static_cast<uint64_t>(usage),
        static_cast<uint64_t>(mr.size),
        memory
    );

    {
        const double perfL10AllocMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - perfL10AllocStart).count();
        if(perfL10AllocMs >= 1.0)
        {
            VkPhysicalDeviceMemoryProperties allocMemoryProperties{};
            vkGetPhysicalDeviceMemoryProperties(pd,&allocMemoryProperties);
            const uint32_t allocHeapIndex =
                allocMemoryProperties.memoryTypes[ai.memoryTypeIndex].heapIndex;
            const bool allocHeapIsDeviceLocal =
                (allocMemoryProperties.memoryHeaps[allocHeapIndex].flags &
                 VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0u;

            std::ostringstream allocLog;
            allocLog << std::fixed << std::setprecision(2)
                << "[PERF L10 ALLOC] frame=" << g_perfL10FrameSerial
                << " ms=" << perfL10AllocMs
                << " request=" << static_cast<unsigned long long>(size)
                << " aligned=" << static_cast<unsigned long long>(mr.size)
                << " usage=0x" << std::hex
                << static_cast<unsigned long long>(usage) << std::dec
                << " type=" << ai.memoryTypeIndex
                << " heap=" << allocHeapIndex
                << " deviceLocal=" << (allocHeapIsDeviceLocal ? 1 : 0);
            RTLogFlushed(allocLog.str());
        }
    }
}

// ---------------------------------------------------------------------------
// 2026-10-04 TLAS 视锥剔除的数学件。viewInverse / projInverse 是 JOML 列主序
// （m[col*4+row]，CAMPROBE 处 m[12..14]=相机位置的判读同口径）。
// ---------------------------------------------------------------------------
void mat4Mul(const float a[16], const float b[16], float out[16])
{
    for(int col = 0; col < 4; ++col)
    {
        for(int row = 0; row < 4; ++row)
        {
            float sum = 0.0f;
            for(int k = 0; k < 4; ++k)
            {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            out[col * 4 + row] = sum;
        }
    }
}
bool mat4Invert(const float in[16], float out[16])
{
    float inv[16];
    float det;

    inv[0]  =  in[5] * in[10] * in[15] - in[5] * in[11] * in[14] - in[9] * in[6] * in[15] + in[9] * in[7] * in[14] + in[13] * in[6] * in[11] - in[13] * in[7] * in[10];
    inv[4]  = -in[4] * in[10] * in[15] + in[4] * in[11] * in[14] + in[8] * in[6] * in[15] - in[8] * in[7] * in[14] - in[12] * in[6] * in[11] + in[12] * in[7] * in[10];
    inv[8]  =  in[4] * in[9] * in[15] - in[4] * in[11] * in[13] - in[8] * in[5] * in[15] + in[8] * in[7] * in[13] + in[12] * in[5] * in[11] - in[12] * in[7] * in[9];
    inv[12] = -in[4] * in[9] * in[14] + in[4] * in[10] * in[13] + in[8] * in[5] * in[14] - in[8] * in[6] * in[13] - in[12] * in[5] * in[10] + in[12] * in[6] * in[9];
    inv[1]  = -in[1] * in[10] * in[15] + in[1] * in[11] * in[14] + in[9] * in[2] * in[15] - in[9] * in[3] * in[14] - in[13] * in[2] * in[11] + in[13] * in[3] * in[10];
    inv[5]  =  in[0] * in[10] * in[15] - in[0] * in[11] * in[14] - in[8] * in[2] * in[15] + in[8] * in[3] * in[14] + in[12] * in[2] * in[11] - in[12] * in[3] * in[10];
    inv[9]  = -in[0] * in[9] * in[15] + in[0] * in[11] * in[13] + in[8] * in[1] * in[15] - in[8] * in[3] * in[13] - in[12] * in[1] * in[11] + in[12] * in[3] * in[9];
    inv[13] =  in[0] * in[9] * in[14] - in[0] * in[10] * in[13] - in[8] * in[1] * in[14] + in[8] * in[2] * in[13] + in[12] * in[1] * in[10] - in[12] * in[2] * in[9];
    inv[2]  =  in[1] * in[6] * in[15] - in[1] * in[7] * in[14] - in[5] * in[2] * in[15] + in[5] * in[3] * in[14] + in[13] * in[2] * in[7] - in[13] * in[3] * in[6];
    inv[6]  = -in[0] * in[6] * in[15] + in[0] * in[7] * in[14] + in[4] * in[2] * in[15] - in[4] * in[3] * in[14] - in[12] * in[2] * in[7] + in[12] * in[3] * in[6];
    inv[10] =  in[0] * in[5] * in[15] - in[0] * in[7] * in[13] - in[4] * in[1] * in[15] + in[4] * in[3] * in[13] + in[12] * in[1] * in[7] - in[12] * in[3] * in[5];
    inv[14] = -in[0] * in[5] * in[14] + in[0] * in[6] * in[13] + in[4] * in[1] * in[14] - in[4] * in[2] * in[13] - in[12] * in[1] * in[6] + in[12] * in[2] * in[5];
    inv[3]  = -in[1] * in[6] * in[11] + in[1] * in[7] * in[10] + in[5] * in[2] * in[11] - in[5] * in[3] * in[10] - in[9] * in[2] * in[7] + in[9] * in[3] * in[6];
    inv[7]  =  in[0] * in[6] * in[11] - in[0] * in[7] * in[10] - in[4] * in[2] * in[11] + in[4] * in[3] * in[10] + in[8] * in[2] * in[7] - in[8] * in[3] * in[6];
    inv[11] = -in[0] * in[5] * in[11] + in[0] * in[7] * in[9] + in[4] * in[1] * in[11] - in[4] * in[3] * in[9] - in[8] * in[1] * in[7] + in[8] * in[3] * in[5];
    inv[15] =  in[0] * in[5] * in[10] - in[0] * in[6] * in[9] - in[4] * in[1] * in[10] + in[4] * in[2] * in[9] + in[8] * in[1] * in[6] - in[8] * in[2] * in[5];

    det = in[0] * inv[0] + in[1] * inv[4] + in[2] * inv[8] + in[3] * inv[12];

    if(det == 0.0f || !std::isfinite(det))
    {
        return false;
    }

    const float invDet = 1.0f / det;
    for(int i = 0; i < 16; ++i)
    {
        out[i] = inv[i] * invDet;
        if(!std::isfinite(out[i]))
        {
            return false;
        }
    }
    return true;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setPostProcessingParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    const jsize paramCount = params != nullptr ? env->GetArrayLength(params) : 0;
    if(paramCount < 7) return JNI_FALSE;
    jfloat values[12] = {};
    env->GetFloatArrayRegion(params, 0, std::min<jsize>(paramCount, 12), values);
    for(int i = 0; i < std::min<jsize>(paramCount, 12); ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_postTonemapMode.store(
        std::min(std::max(static_cast<int>(values[0] + 0.5f), 0), 6));
    g_postExposureStrength.store(
        std::min(std::max(static_cast<float>(values[1]), 0.0f), 1.0f));
    g_postContrast.store(
        std::min(std::max(static_cast<float>(values[2]), 0.5f), 2.0f));
    g_postSaturation.store(
        std::min(std::max(static_cast<float>(values[3]), 0.0f), 2.0f));
    g_postGamma.store(
        std::min(std::max(static_cast<float>(values[4]), 1.6f), 2.8f));
    g_mirrorBounceCount.store(
        std::min(std::max(static_cast<int>(values[5] + 0.5f), 1), 8));
    g_tlasCullDistance.store(
        std::min(std::max(static_cast<int>(values[6] + 0.5f), 0), 512));
    g_tlasFrustumCullMargin.store(
        paramCount >= 8
            ? std::min(std::max(static_cast<int>(values[7] + 0.5f), 0), 256)
            : 0);
    g_postBloomStrength.store(
        paramCount >= 9
            ? std::min(std::max(static_cast<float>(values[8]), 0.0f), 2.0f)
            : 0.0f);
    g_postGainR.store(
        paramCount >= 12
            ? std::min(std::max(static_cast<float>(values[9]), 0.25f), 2.5f)
            : 1.0f);
    g_postGainG.store(
        paramCount >= 12
            ? std::min(std::max(static_cast<float>(values[10]), 0.25f), 2.5f)
            : 1.0f);
    g_postGainB.store(
        paramCount >= 12
            ? std::min(std::max(static_cast<float>(values[11]), 0.25f), 2.5f)
            : 1.0f);
    return JNI_TRUE;
}

// === 新增：创建 Camera UBO ===
// 2026-10-26：28 个 HP 参数（CameraUBO 尾块 hpShapeA..hpMiscA）的**唯一**写入点。
// 必须逐帧调用 —— 原来这段只写在 createCameraUBO() 里（启动时执行一次），
// 于是面板里 28 条 HP 滑条全部失效（改的只是原子量，UBO 永远停在初值）。
void populateHpParams(CameraUBO* ubo)
{
    ubo->hpShapeA[0] = g_hpDetailStrengthCu.load();
    ubo->hpShapeA[1] = g_hpDetailStrengthTcu.load();
    ubo->hpShapeA[2] = g_hpDetailStrengthCb.load();
    ubo->hpShapeA[3] = g_hpBottomSmoothHeight.load();
    ubo->hpShapeB[0] = g_hpWispyEdgeWidth.load();
    ubo->hpShapeB[1] = g_hpWispyReach.load();
    ubo->hpShapeB[2] = g_hpWispyTopHeight.load();
    ubo->hpShapeB[3] = g_hpWispyTopHardness.load();
    ubo->hpShapeC[0] = g_hpWispyWeight.load();
    ubo->hpShapeC[1] = g_hpBillowyWeight.load();
    ubo->hpShapeC[2] = g_hpLoCoverTopMax.load();
    ubo->hpShapeC[3] = g_hpLoCoverTopStr.load();
    ubo->hpShapeD[0] = g_hpCovMapScale.load();
    ubo->hpShapeD[1] = g_hpScWorleyStrength.load();
    ubo->hpShapeD[2] = g_hpScHeightScale.load();
    ubo->hpShapeD[3] = g_hpScCellThickStr.load();
    ubo->hpLightA[0] = g_hpForwardEcc.load();
    ubo->hpLightA[1] = g_hpBackwardEcc.load();
    ubo->hpLightA[2] = g_hpPhiFwdIntensity.load();
    ubo->hpLightA[3] = g_hpPhiFwdDepthPow.load();
    ubo->hpLightB[0] = g_hpPhiFwdDepthBias.load();
    ubo->hpLightB[1] = g_hpPhiFwdCompress.load();
    ubo->hpLightB[2] = g_hpScatterOdScale.load();
    ubo->hpLightB[3] = g_hpScatterCurvePow.load();
    ubo->hpMiscA[0] = g_hpMsEccentricity.load();
    ubo->hpMiscA[1] = g_hpAoUpwardScale.load();
    ubo->hpMiscA[2] = g_hpFadeStart.load();
    ubo->hpMiscA[3] = g_hpPowderIntensity.load();
}
void createCameraUBO() {
    createBuffer(
        g_device,
        g_physicalDevice,
        sizeof(CameraUBO),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        g_cameraUboBuffer,
        g_cameraUboMemory
    );
    g_cameraUboMapped = &g_cameraUboHost;
    CameraUBO* ubo = static_cast<CameraUBO*>(g_cameraUboMapped);
    ubo->vertexBufferAddrLo = static_cast<uint32_t>(g_testVertexDeviceAddress & 0xFFFFFFFF);
    ubo->vertexBufferAddrHi = static_cast<uint32_t>(g_testVertexDeviceAddress >> 32);
    ubo->padding = 0u;

    ubo->dhFarFieldParams[0] = 0.0f;
    ubo->dhFarFieldParams[1] = 0.0f;
    ubo->dhFarFieldParams[2] = 0.0f;
    ubo->dhFarFieldParams[3] = 0.0f;

    for(int dhMatrixInitIndex = 0; dhMatrixInitIndex < 16; ++dhMatrixInitIndex)
    {
        ubo->dhFarFieldInvMvp[dhMatrixInitIndex] = 0.0f;
        ubo->dhFarFieldMvp[dhMatrixInitIndex] = 0.0f;
    }

    ubo->dhFarFieldLight[0] = 0.0f;
    ubo->dhFarFieldLight[1] = 0.0f;
    ubo->dhFarFieldLight[2] = 1.0f;
    ubo->dhFarFieldLight[3] = 0.0f;

    // 2026-11-09：派生矩阵（viewMatrix / projectionMatrix / viewProj /
    // prevViewProj）初值取单位阵。首帧 updateCamera 会立刻用真实值覆盖；
    // 这里只是保证"万一首帧求逆失败"时读到的不是未初始化内容。
    for(int derivedInitIndex = 0; derivedInitIndex < 16; ++derivedInitIndex)
    {
        const float identityEntry = ((derivedInitIndex % 5) == 0) ? 1.0f : 0.0f;
        ubo->viewMatrix[derivedInitIndex] = identityEntry;
        ubo->projectionMatrix[derivedInitIndex] = identityEntry;
        ubo->viewProj[derivedInitIndex] = identityEntry;
        ubo->prevViewProj[derivedInitIndex] = identityEntry;
    }

    // ── 本地光源档案表（32 档）──
    // 2026-11-09：原本是 cpp/shaders/local_light_profile.glsl 里的四张 const 表。
    // glslang 对"const 数组 + 运行时索引"会生成 indexable 局部数组 ⇒ rgen 里
    // 768 B/线程的 local memory（vec3[32] + 3 x float[32]，SPIR-V 普查可复现）。
    // 搬到 UBO 后动态索引变成 OpAccessChain，不再有局部副本。
    // 顺序必须与 SectionCompilerMixin.classifyLocalLightType() 的映射逐项一致。
    struct LocalLightProfileEntry
    {
        float r, g, b, lumens, range, pointLike;
    };
    static const LocalLightProfileEntry kLocalLightProfile[32] = {
        { 1.0000f, 0.7193f, 0.4766f, 1.0000f, 12.0000f, 0.0f },  //  0  fallback/未分类发光块        3000K
        { 1.0000f, 0.6387f, 0.2625f, 1.0000f, 11.0000f, 1.0f },  //  1  torch 火把               2000K
        { 1.0000f, 0.6421f, 0.2890f, 1.1200f, 13.0000f, 1.0f },  //  2  lantern 灯笼             2200K
        { 1.0000f, 0.6230f, 0.2200f, 1.3000f, 15.0000f, 1.0f },  //  3  campfire 营火            1900K
        { 1.0000f, 0.6149f, 0.2200f, 1.2000f, 13.0000f, 1.0f },  //  4  fire 火焰                1850K
        { 0.2200f, 0.7200f, 1.0000f, 0.7800f, 8.0000f, 1.0f },  //  5  soul_torch 灵魂火把        tint
        { 0.2400f, 0.7600f, 1.0000f, 1.0000f, 10.0000f, 1.0f },  //  6  soul_lantern 灵魂灯笼      tint
        { 0.2600f, 0.7900f, 1.0000f, 1.1800f, 12.0000f, 1.0f },  //  7  soul_campfire 灵魂营火     tint
        { 0.2000f, 0.7000f, 1.0000f, 0.9500f, 10.0000f, 1.0f },  //  8  soul_fire 灵魂火          tint
        { 1.0000f, 0.1600f, 0.0600f, 0.5200f, 6.0000f, 1.0f },  //  9  redstone_torch 红石火把    tint
        { 1.0000f, 0.3300f, 0.1300f, 1.0500f, 13.0000f, 0.0f },  // 10  redstone_lamp 红石灯      tint
        { 1.0000f, 0.5708f, 0.2200f, 1.9000f, 26.0000f, 0.0f },  // 11  lava 岩浆                1600K
        { 1.0000f, 0.5408f, 0.2200f, 0.5000f, 7.0000f, 0.0f },  // 12  magma_block 岩浆块        1450K
        { 1.0000f, 0.7642f, 0.5677f, 1.4500f, 18.0000f, 0.0f },  // 13  glowstone 荧石           3400K
        { 1.0000f, 0.9798f, 0.9541f, 1.3500f, 18.0000f, 0.0f },  // 14  sea_lantern 海晶灯        6200K
        { 1.0000f, 0.6840f, 0.3908f, 1.3500f, 17.0000f, 0.0f },  // 15  shroomlight 菌光体        2500K
        { 1.0000f, 0.6387f, 0.2625f, 1.1000f, 14.0000f, 0.0f },  // 16  jack_o_lantern 南瓜灯     2000K
        { 0.8800f, 0.9200f, 1.0000f, 1.2000f, 14.0000f, 1.0f },  // 17  end_rod 末地烛            tint
        { 0.9000f, 0.9500f, 1.0000f, 2.2000f, 28.0000f, 0.0f },  // 18  beacon 信标              tint
        { 0.3000f, 0.8600f, 1.0000f, 1.5500f, 20.0000f, 0.0f },  // 19  conduit 潮涌核心           tint
        { 0.7200f, 0.3000f, 1.0000f, 1.5000f, 18.0000f, 0.0f },  // 20  nether_portal 地狱门      tint
        { 0.4600f, 0.8600f, 0.5200f, 0.3600f, 5.0000f, 1.0f },  // 21  glow_lichen 发光地衣       tint
        { 0.6400f, 0.4400f, 1.0000f, 0.4000f, 6.0000f, 1.0f },  // 22  amethyst 紫水晶簇          tint
        { 1.0000f, 0.6149f, 0.2200f, 0.5500f, 7.0000f, 1.0f },  // 23  candle 蜡烛              1850K
        { 0.5400f, 0.3200f, 1.0000f, 0.5800f, 8.0000f, 0.0f },  // 24  crying_obsidian 哭泣黑曜石  tint
        { 0.7400f, 0.4200f, 1.0000f, 1.1000f, 11.0000f, 0.0f },  // 25  respawn_anchor 重生锚     tint
        { 1.0000f, 0.6230f, 0.2200f, 0.8500f, 9.0000f, 0.0f },  // 26  furnace_lit 熔炉点燃       1900K
        { 1.0000f, 0.7071f, 0.4509f, 1.3500f, 16.0000f, 0.0f },  // 27  froglight 蛙明灯          2900K
        { 1.0000f, 0.6230f, 0.2200f, 0.2800f, 4.0000f, 1.0f },  // 28  brewing_stand 酿造台      1900K
        { 0.7800f, 0.8400f, 0.5000f, 0.3200f, 5.0000f, 1.0f },  // 29  cave_vines 洞穴藤蔓        tint
        { 0.8600f, 0.8000f, 0.6200f, 0.0600f, 2.0000f, 1.0f },  // 30  brown_mushroom 棕色蘑菇    tint
        { 0.6000f, 0.4600f, 1.0000f, 0.3600f, 6.0000f, 0.0f },  // 31  enchanting_table 附魔台   tint
    };

    for(int profileIndex = 0; profileIndex < 32; ++profileIndex)
    {
        const LocalLightProfileEntry& entry = kLocalLightProfile[profileIndex];
        ubo->localLightProfileA[profileIndex * 4 + 0] = entry.r;
        ubo->localLightProfileA[profileIndex * 4 + 1] = entry.g;
        ubo->localLightProfileA[profileIndex * 4 + 2] = entry.b;
        ubo->localLightProfileA[profileIndex * 4 + 3] = entry.lumens;
        ubo->localLightProfileB[profileIndex * 4 + 0] = entry.range;
        ubo->localLightProfileB[profileIndex * 4 + 1] = entry.pointLike;
        ubo->localLightProfileB[profileIndex * 4 + 2] = 0.0f;
        ubo->localLightProfileB[profileIndex * 4 + 3] = 0.0f;
    }

    ubo->historyParamsPadding0 =
        g_pomDepth.load();

    ubo->historyParamsPadding1 =
        0.0f;

    ubo->pomParams[0] =
        g_pomSharp.load();
    ubo->pomParams[1] = 0.0f;
    ubo->pomParams[2] = 0.0f;
    ubo->pomParams[3] = 0.0f;

    ubo->sssParams[0] =
        g_sssMode.load();
    ubo->sssParams[1] = 0.0f;
    ubo->sssParams[2] = 0.0f;
    ubo->sssParams[3] = 0.0f;

    ubo->giHalfParams[0] = 0.0f;
    ubo->giHalfParams[1] = 0.0f;
    ubo->giHalfParams[2] = 0.0f;
    ubo->giHalfParams[3] = 0.0f;

    // 2026-11-09：次级射线跳过 POM的出厂值 = 开（见 pomSecondarySkipEnabled）。
    ubo->pomSecondaryParams[0] = 1.0f;

    // 2026-11-13：雾光学默认值（1,1,1,1 = 默认涡流、不偏色）
    ubo->fogOpticsParams[0] = 1.0f;
    ubo->fogOpticsParams[1] = 1.0f;
    ubo->fogOpticsParams[2] = 1.0f;
    ubo->fogOpticsParams[3] = 1.0f;

    // 2026-11-13：玻璃光学焦散默认值写入已删除（设置整体回退）；
    // glassCausticParams 字段保留占位（无读者）。
    ubo->pomSecondaryParams[1] = 0.0f;
    ubo->pomSecondaryParams[2] = 0.0f;
    ubo->pomSecondaryParams[3] = 0.0f;

    // 2026-11-02 屏幕空间降级链（SSR + SSPT）：出厂关闭
    ubo->ssHybridParams[0] = 0.0f;
    ubo->ssHybridParams[1] = 0.0f;
    // z = RT 光泽射线开关，出厂**开**（1），与 g_ssRtGlossy 的初值一致
    ubo->ssHybridParams[2] = 1.0f;
    ubo->ssHybridParams[3] = 0.0f;

    ubo->giRayParams[0] = 0.0f;
    ubo->giRayParams[1] = 0.0f;
    ubo->giRayParams[2] = 0.0f;
    ubo->giRayParams[3] = 0.0f;

    ubo->sparseParams[0] = 0.0f;
    ubo->sparseParams[1] = 0.0f;
    ubo->sparseParams[2] = 1.0f;
    ubo->sparseParams[3] = 0.0f;

    populateHpParams(ubo);

    ubo->playerX = 0.0f;
    ubo->playerY = 0.0f;
    ubo->playerZ = 0.0f;
    ubo->playerInWater = 0.0f;
    ubo->playerVelocityX = 0.0f;
    ubo->playerVelocityY = 0.0f;
    ubo->playerVelocityZ = 0.0f;
    ubo->playerHorizontalSpeed = 0.0f;
    ubo->waterSurfaceY = 0.0f;
    ubo->waterSimOriginX = 0.0f;
    ubo->waterSimOriginZ = 0.0f;
    ubo->waterSimTexelWorldSize = WATER_SIM_TEXEL_WORLD_SIZE;
    ubo->waterSimReadLayer = 0u;
    ubo->waterSimEnabled = 0u;
    ubo->waterSimImpulsePending = 0u;
    ubo->cameraInWater = 0u;
    ubo->waterSimImpulseX = 0.0f;
    ubo->waterSimImpulseZ = 0.0f;
    ubo->waterSimImpulseStrength = 0.0f;
    ubo->waterSimImpulseSurfaceY = 0.0f;

    ubo->weatherParams[0] = 0.0f;
    ubo->weatherParams[1] = 0.0f;
    ubo->weatherParams[2] = 1.0f;
    ubo->weatherParams[3] = 1.0f;
    ubo->auroraParams[0] = 0.0f;
    ubo->auroraParams[1] = 1.0f;
    ubo->auroraParams[2] = 1.0f;
    ubo->auroraParams[3] = 0.0f;
    ubo->rtSampleParams[0] = 1.0f;
    ubo->rtSampleParams[1] = 1.0f;
    ubo->rtSampleParams[2] = 32.0f;
    ubo->rtSampleParams[3] = 0.0f;
    populateCloudParams(ubo->cloudParams);
    populateSkyParams(ubo->skyParams);
    populateSkyModelParams(ubo->skyModelParams);
    populateGalaxyParams(ubo->galaxyParams);
    populateGodRayParams(ubo->godRayParams);
    populateGroundMistParams(ubo->godRayParams + 4);
    populateRainWallParams(ubo->rainWallParams);
    populateWaterStyleParams(ubo->waterStyleParams);
    populateHighCloudParams(ubo->highCloudParams);
    populateCloudMsParams(ubo->cloudMsParams);
    populateCloudMarchParams(ubo->cloudMarchParams);
    populateWaterParams(ubo->waterParams);
    populateCloudPhiParams(ubo->cloudPhiParams);
    populateCloudLightParams(ubo->cloudLightParams);
    populateCloudShapeParams(ubo->cloudShapeParams);
    populateCloudMoonParams(ubo->cloudMoonParams);
    populateCloudShape2Params(ubo->cloudShape2Params);
    populateCloudAmbientParams(ubo->cloudAmbientParams);
    populateCloudMsParams2(ubo->cloudMsParams2);
    populateCloudLightParams2(ubo->cloudLightParams2);
    populateCloudProfileParams(ubo->cloudProfileParams);
    populateCloudDetailParams(ubo->cloudDetailParams);
    populateFogExtrasParams(ubo->fogExtrasParams);
    populateCloudTemporalParams(ubo->cloudTemporalParams);
    populateCloudAerialParams(ubo->cloudAerialParams);

    populateCloudSkyHueParams(ubo->cloudSkyHueParams);
    populateCloudPhysicsParams(ubo->cloudPhysicsParams);
    populateUeCloudParams(ubo->ueCloudA, ubo->ueCloudB);

    {
        const uint64_t sharcAddresses[4] = {
            g_sharcHashEntriesAddress,
            g_sharcAccumulationAddress,
            g_sharcResolvedAddress,
            g_sharcLockAddress
        };

        for(uint32_t sharcIndex = 0u; sharcIndex < 4u; ++sharcIndex)
        {
            ubo->sharcBuffersLo[sharcIndex] =
                static_cast<uint32_t>(sharcAddresses[sharcIndex] & 0xFFFFFFFFull);
            ubo->sharcBuffersHi[sharcIndex] =
                static_cast<uint32_t>(sharcAddresses[sharcIndex] >> 32u);
        }

        const float sharcSceneScaleValue = sharcSceneScale();

        const float sharcRadianceScaleValue = 1000.0f;
        uint32_t sharcSceneScaleBits = 0u;
        uint32_t sharcRadianceScaleBits = 0u;
        std::memcpy(&sharcSceneScaleBits, &sharcSceneScaleValue, sizeof(uint32_t));
        std::memcpy(&sharcRadianceScaleBits, &sharcRadianceScaleValue, sizeof(uint32_t));

        ubo->sharcCacheParams[0] =
            g_sharcBuffersReady ? sharcSceneScaleBits : 0u;
        ubo->sharcCacheParams[1] =
            g_sharcBuffersReady ? g_sharcEntryCountResolved : 0u;
        ubo->sharcCacheParams[2] =
            g_sharcBuffersReady ? sharcRadianceScaleBits : 0u;
        ubo->sharcCacheParams[3] = sharcModeLane();
    }
    populateWaterWaveParams(ubo->waterWaveParams);
    populateWaterWaveShapeParams(ubo->waterWaveShapeParams);
    populateWaterSssParams(ubo->waterSssParams);
    populateWaterFftParams(ubo->waterFftParams);
}
