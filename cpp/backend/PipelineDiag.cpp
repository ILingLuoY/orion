// Orion backend — PipelineDiag 模块。
//
// 2026-11-02 P0（冷编分解）：把管线创建耗时按 SPV 名逐条记录，用于把
// "改一个 SPV 就要等十几分钟"拆成 cache 加载 / compute 管线 / RT 管线三部分。
//
// 只做观测起步；2026-11-11 新增 boot 冷编并行路径：RT 管线创建先走
// VkDeferredOperationKHR + 多线程 vkDeferredOperationJoinKHR（驱动把编译
// 劈给所有 join 线程，官方缩短冷编的通道）。任何一步走不通都干净回退到
// 原同步创建（deferredOperation=null，逐位同旧路径）；ORION_RT_PARCOMP=0
// 整体关闭。日志统一前缀 [PERF PIPE]，与既有 [PERF PC.*] 走同一个
// stderr + RTLog 出口，便于把时间轴对齐。
//
// 为什么 compute 侧不逐个改调用点：TrackingHooks.h 已经把
// vkCreateComputePipelines 宏到 rtxTrackedCreateComputePipelines，改 Tracking.cpp
// 里那一处就覆盖了全部 40+ 个 compute 创建点。

#include "OrionInternal.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace
{

std::mutex g_orionPipelineDiagMutex;

// SPV 内容哈希 -> 请求路径（loadSpv 登记）。
std::unordered_map<uint64_t, std::string> g_orionSpvNameByHash;

// VkShaderModule -> SPV 内容哈希（rtxTrackedCreateShaderModule 登记）。
// module 被销毁后句柄可能复用，但这里只用于日志标注，标错也不影响功能。
std::unordered_map<VkShaderModule, uint64_t> g_orionModuleSpvHash;

std::atomic<uint64_t> g_orionPipelineCreateCount{0u};
std::atomic<uint64_t> g_orionPipelineCreateMicros{0u};

// FNV-1a 64：与 PipelineCache.cpp 的 ComputeRTPipelineCacheKey 同口径，
// 避免同一份 SPV 在两处算出不同身份。
uint64_t orionPipelineDiagFnv1a(const void* data, size_t size)
{
    const unsigned char* bytes = static_cast<const unsigned char*>(data);

    uint64_t hash = 1469598103934665603ull;

    for(size_t i = 0; i < size; ++i)
    {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= 1099511628211ull;
    }

    return hash;
}

void orionPipelineDiagEmit(const char* text)
{
    fprintf(stderr, "%s\n", text);
    fflush(stderr);

    // 2026-11-02：必须用 flush 版本。RTLog 走的是不 flush 的 ofstream
    // （Platform.cpp:233-263），冷编期间 rt_debug.log 会一直停在上一帧，
    // 只能盯 stderr。冷编正是"需要实时看进度"的唯一场景，这点落盘开销
    // （初始化期几十行）远小于失去可观测性。
    RTLogFlushed(text);
}

// 2026-11-14：vkGetPipelineCacheData 在驱动端是重量级序列化（11-13 实测
// main 落盘 73s），与刚结束的 JIT 峰值（cache-load 路径实测 23GB）撞在同一
// 时刻 ⇒ 当天 4 次冷编崩点全落在编译完成后 1s 内（persist 首秒）。延迟
// 10s 等 JIT 内存释放后再落盘；begin/end 打点用于把崩点钉在 persist 与
// OrionLogPipelineStats 之间。
std::atomic<int> g_orionPersistDeferredCount{0u};

void orionPersistPipelineCacheDeferred()
{
    g_orionPersistDeferredCount.fetch_add(1u);

    std::thread([]()
    {
        std::this_thread::sleep_for(std::chrono::seconds(10));

        orionPipelineDiagEmit("[PERF PIPE] kind=persist-begin");

        PersistSharedPipelineCacheNow();

        orionPipelineDiagEmit("[PERF PIPE] kind=persist-end");

        g_orionPersistDeferredCount.fetch_sub(1u);
    }).detach();
}

} // namespace

uint64_t OrionPipelineSpvHash(const void* data, size_t size)
{
    if(data == nullptr || size == 0u)
    {
        return 0ull;
    }

    return orionPipelineDiagFnv1a(data, size);
}

// 2026-11-08：executable 枚举要求管线带 CAPTURE_STATISTICS 创建（否则
// vkGetPipelineExecutablePropertiesKHR 返回 0 个 executable）。flag 进缓存键
// ⇒ 会触发一轮冷编，故只挂在 ORION_PSTAT=1 采数启动上，平时不付代价。
bool OrionPipelineStatsCaptureEnabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_PSTAT");
        return raw != nullptr && raw[0] == '1';
    }();
    return cached;
}

void OrionPipelineRegisterSpvName(uint64_t hash, const char* name)
{
    if(hash == 0ull)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_orionPipelineDiagMutex);

    g_orionSpvNameByHash[hash] = (name != nullptr) ? name : "<null>";
}

void OrionPipelineRegisterShaderModule(VkShaderModule module, uint64_t spvHash)
{
    if(module == VK_NULL_HANDLE || spvHash == 0ull)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_orionPipelineDiagMutex);

    g_orionModuleSpvHash[module] = spvHash;
}

const char* OrionPipelineSpvNameOfModule(VkShaderModule module)
{
    // 调用方（rtxTrackedCreateComputePipelines）是同步使用返回值，thread_local
    // 避免持锁返回 map 内 string 的指针（rehash 会让引用失效）。
    static thread_local std::string name;

    name = "<unknown-spv>";

    if(module == VK_NULL_HANDLE)
    {
        return name.c_str();
    }

    std::lock_guard<std::mutex> lock(g_orionPipelineDiagMutex);

    const auto moduleIt = g_orionModuleSpvHash.find(module);

    if(moduleIt == g_orionModuleSpvHash.end())
    {
        return name.c_str();
    }

    const auto nameIt = g_orionSpvNameByHash.find(moduleIt->second);

    if(nameIt != g_orionSpvNameByHash.end())
    {
        name = nameIt->second;
    }

    return name.c_str();
}

void OrionPipelineLogCreate(
    const char* kind,
    const char* spvName,
    bool cacheUsed,
    double milliseconds,
    VkResult result)
{
    const uint64_t creates =
        g_orionPipelineCreateCount.fetch_add(1u, std::memory_order_relaxed) + 1u;

    const uint64_t micros =
        g_orionPipelineCreateMicros.fetch_add(
            static_cast<uint64_t>(milliseconds * 1000.0),
            std::memory_order_relaxed
        ) + static_cast<uint64_t>(milliseconds * 1000.0);

    char text[768] = {};

    std::snprintf(
        text,
        sizeof(text),
        "[PERF PIPE] kind=%s spv=%s cache=%d ms=%.1f result=%d creates=%llu totalMs=%.1f",
        (kind != nullptr) ? kind : "?",
        (spvName != nullptr) ? spvName : "?",
        cacheUsed ? 1 : 0,
        milliseconds,
        static_cast<int>(result),
        static_cast<unsigned long long>(creates),
        static_cast<double>(micros) / 1000.0
    );

    orionPipelineDiagEmit(text);
}

void OrionPipelineLogCachePhase(
    const char* phase,
    double milliseconds,
    size_t bytes,
    const char* detail)
{
    char text[768] = {};

    std::snprintf(
        text,
        sizeof(text),
        "[PERF PIPE] kind=cache-%s ms=%.1f bytes=%zu detail=%s",
        (phase != nullptr) ? phase : "?",
        milliseconds,
        bytes,
        (detail != nullptr) ? detail : ""
    );

    orionPipelineDiagEmit(text);
}

namespace
{

// ---------------------------------------------------------------------------
// 2026-11-11 boot 冷编并行化：deferred op + N 线程 join。
//
// 动机：boot 冷编 15~30 分钟是单线程同步创建（deferredOperation=null），驱动
// 只用一个编译线程；VK_KHR_deferred_host_operations 官方语义 = 多个宿主线程
// join 同一个 op 时驱动把编译劈给它们（vkGetDeferredOperationMaxConcurrencyKHR
// 给出有效线程数上限）。运行时热重建路径（RtPipelineAsync.cpp）已经实证本设备
// 支持该扩展，这里把同一通道用到 boot。
//
// 关键规范语义：
//  - createPipe 返回 VK_OPERATION_DEFERRED_KHR ⇒ op 挂起，完成结果**不在返回码
//    里**；所有 join 返回 VK_SUCCESS 后逐条查 pPipelines，VK_NULL_HANDLE = 失败。
//  - VK_OPERATION_NOT_DEFERRED_KHR ⇒ 驱动同步完成了，句柄已写好（等效成功）。
//  - 其他 VkResult ⇒ 直接报错（OUT_OF_*_MEMORY 等），落到原有同步路径与 -3 重试。
//  - 挂起中的 op 不能销毁；join 拿到 THREAD_IDLE/THREAD_DONE 只能睡眠重试。
// ---------------------------------------------------------------------------

bool orionParcompEnabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_RT_PARCOMP");

        // 2026-11-15：默认**改为关闭**（原默认开）。依据当天实测：
        //   parcomp 走 6~8 个 join 线程，驱动后端编译器每线程一套独立编译
        //   上下文 ⇒ 峰值主机内存 ≈ 线程数倍。当天 4 次 parcomp 全量冷编里
        //   3 次驱动把编译失败（OOM）吞掉后返回"成功"管线，首次 dispatch 即
        //   在 nvoglv64+0xF5B5DC AV；另 1 次编译线程 ucrtbase fail-fast。
        //   同步路径同一份 SPV 编到 669 s 才明确报 VK_ERROR_OUT_OF_HOST_MEMORY
        //   （result=-3）—— 明确失败好过"坏管线 + 驱动里崩"。
        //   代价：冷编时间回到 8~17 分钟（16 GB 机器上 6 线程本就更早撞内存墙，
        //   并行并没有带来实际好处）。
        // 打开方式：ORION_RT_PARCOMP=1（大内存机器自选）。
        return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
    }();
    return cached;
}

// 0 = 不覆盖（默认 1 个 join 线程）；>0 = 强制线程数（2026-11-16 起多线程
// 只能靠它显式打开）。
uint32_t orionParcompThreadOverride()
{
    static const uint32_t cached = []()
    {
        const char* raw = std::getenv("ORION_RT_PARCOMP_THREADS");
        if(raw == nullptr || raw[0] == '\0')
        {
            return 0u;
        }
        const int v = std::atoi(raw);
        return v > 0 ? static_cast<uint32_t>(v) : 0u;
    }();
    return cached;
}

struct OrionParcompResult
{
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    bool attempted = false;   // 真的发起了 deferred 尝试（PFN 齐全 + op 建出来了）
    bool usable = false;      // pPipelines 全部非空，可直接走成功路径
    bool deferred = false;    // 驱动确实延迟了（而非同步完成）
    double milliseconds = 0.0;
    uint32_t threads = 0u;
    uint32_t maxJoin = 0u;
};

OrionParcompResult orionParcompAttempt(
    PFN_vkCreateRayTracingPipelinesKHR createPipe,
    uint32_t createInfoCount,
    const VkRayTracingPipelineCreateInfoKHR* pCreateInfos,
    VkPipelineCache pipelineCache,
    VkPipeline* pPipelines)
{
    OrionParcompResult out;

    auto createOp =
        reinterpret_cast<PFN_vkCreateDeferredOperationKHR>(
            vkGetDeviceProcAddr(g_device, "vkCreateDeferredOperationKHR"));
    auto destroyOp =
        reinterpret_cast<PFN_vkDestroyDeferredOperationKHR>(
            vkGetDeviceProcAddr(g_device, "vkDestroyDeferredOperationKHR"));
    auto joinOp =
        reinterpret_cast<PFN_vkDeferredOperationJoinKHR>(
            vkGetDeviceProcAddr(g_device, "vkDeferredOperationJoinKHR"));
    auto maxConcurrency =
        reinterpret_cast<PFN_vkGetDeferredOperationMaxConcurrencyKHR>(
            vkGetDeviceProcAddr(
                g_device, "vkGetDeferredOperationMaxConcurrencyKHR"));

    if(createOp == nullptr || destroyOp == nullptr || joinOp == nullptr ||
       maxConcurrency == nullptr)
    {
        return out;
    }

    VkDeferredOperationKHR op = VK_NULL_HANDLE;

    if(createOp(g_device, nullptr, &op) != VK_SUCCESS || op == VK_NULL_HANDLE)
    {
        return out;
    }

    out.attempted = true;

    const auto start = std::chrono::steady_clock::now();

    const VkResult createResult = createPipe(
        g_device,
        op,
        pipelineCache,
        createInfoCount,
        pCreateInfos,
        nullptr,
        pPipelines
    );

    if(createResult == VK_OPERATION_DEFERRED_KHR)
    {
        out.deferred = true;

        uint32_t maxJoin = maxConcurrency(g_device, op);

        if(maxJoin == 0u)
        {
            maxJoin = 1u;
        }

        out.maxJoin = maxJoin;

        // 2026-11-16：默认单线程。此前 parcomp 一开就 min(maxJoin, 硬件, 8) 个
        // join 线程，16GB 机器上并行 LLVM 正是冷编 OOM/驱动 AV 的推手；现在
        // 要 1 个以上 join 线程必须显式 ORION_RT_PARCOMP_THREADS>1。
        uint32_t threads = 1u;

        const uint32_t overrideThreads = orionParcompThreadOverride();

        if(overrideThreads > 0u)
        {
            threads = std::min<uint32_t>(overrideThreads, maxJoin);
        }

        if(threads == 0u)
        {
            threads = 1u;
        }

        out.threads = threads;

        // join 循环：SUCCESS=完事；IDLE/DONE=这片没活/干完了等别人，睡眠重试；
        // 其他返回码继续循环——挂起的 op 不能销毁也不能放弃，必须有人 join 到
        // SUCCESS 为止（调用者自己也参与，保证一定有 join 线程存活）。
        auto joinLoop = [joinOp, op]() -> VkResult
        {
            for(;;)
            {
                const VkResult j = joinOp(g_device, op);

                if(j == VK_SUCCESS)
                {
                    return VK_SUCCESS;
                }

                if(j == VK_THREAD_IDLE_KHR)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                else if(j == VK_THREAD_DONE_KHR)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(4));
                }
                else
                {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(10));
                }
            }
        };

        std::vector<std::thread> joiners;

        for(uint32_t i = 1u; i < threads; ++i)
        {
            joiners.emplace_back(joinLoop);
        }

        const VkResult callerJoin = joinLoop();

        for(auto& t : joiners)
        {
            t.join();
        }

        out.milliseconds =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start
            ).count();

        // 完成结果不在 join 返回码里：逐条查句柄。
        bool allOk = (callerJoin == VK_SUCCESS);

        if(allOk)
        {
            for(uint32_t i = 0u; i < createInfoCount; ++i)
            {
                if(pPipelines[i] == VK_NULL_HANDLE)
                {
                    allOk = false;
                    break;
                }
            }
        }

        if(allOk)
        {
            out.result = VK_SUCCESS;
            out.usable = true;
        }
        else
        {
            // 与既有 -3 重试语义对齐：INITIALIZATION_FAILED 触发 3s 后重试。
            out.result = VK_ERROR_INITIALIZATION_FAILED;
        }

        destroyOp(g_device, op, nullptr);

        return out;
    }
    else if(createResult == VK_OPERATION_NOT_DEFERRED_KHR)
    {
        // 驱动同步完成了：句柄已写好（等效旧同步路径的成功）。
        out.milliseconds =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start
            ).count();

        bool allOk = true;

        for(uint32_t i = 0u; i < createInfoCount; ++i)
        {
            if(pPipelines[i] == VK_NULL_HANDLE)
            {
                allOk = false;
                break;
            }
        }

        if(allOk)
        {
            out.result = VK_SUCCESS;
            out.usable = true;
        }
        else
        {
            out.result = VK_ERROR_INITIALIZATION_FAILED;
        }

        destroyOp(g_device, op, nullptr);

        return out;
    }
    else
    {
        // 直接报错（OUT_OF_*_MEMORY 等）：未产出管线，错误码原样上报，
        // 走下方同步回退与既有重试。
        out.milliseconds =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start
            ).count();

        out.result = createResult;

        destroyOp(g_device, op, nullptr);

        return out;
    }
}

} // namespace

// 2026-11-15：RTPSO 冷编峰值主机内存 20~30 GB（NV 后端编译器），16 GB 机器
// 极易撞 commit 上限 —— 当天实测同步路径编到 669 s 后 result=-3
// （VK_ERROR_OUT_OF_HOST_MEMORY）。创建前先把我们自己手里可让的缓冲还回去：
//   · PBR 纹理上传 staging（> 64 MB 的大块；函数内部先等 fence 再销毁，安全）
//   · 其它大块 staging 走同一入口，将来加新缓冲只需在这里追加
// force=false：每进程只在第一次 RT 管线创建前做一次，避免逐条管线反复重建
// staging；force=true：OOM 重试前再收一次。
double orionReleaseBuffersForRtCompile(bool force)
{
    static bool sReleased = false;

    if(sReleased && !force)
    {
        return 0.0;
    }

    sReleased = true;

    const double freedMb = releaseHugeTextureUploadStaging();

    if(freedMb > 0.0)
    {
        char text[224] = {};

        std::snprintf(
            text,
            sizeof(text),
            "[PERF PIPE] kind=rt-mem-release freedStagingMB=%.1f force=%d",
            freedMb,
            force ? 1 : 0
        );

        orionPipelineDiagEmit(text);
    }

    return freedMb;
}

VkResult OrionCreateRayTracingPipelineTimed(
    const char* label,
    uint32_t createInfoCount,
    const VkRayTracingPipelineCreateInfoKHR* pCreateInfos,
    VkPipelineCache pipelineCache,
    VkPipeline* pPipelines)
{
    static std::mutex s_rtPipelineCompileMutex;
    std::lock_guard<std::mutex> compileLock(s_rtPipelineCompileMutex);

    auto createPipe =
        reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
            vkGetDeviceProcAddr(g_device, "vkCreateRayTracingPipelinesKHR"));

    if(createPipe == nullptr)
    {
        throw std::runtime_error("vkCreateRayTracingPipelinesKHR unavailable");
    }

    // 标签里要的是**参与编译的 shader stage 数**（RTPSO 的编译量由它决定），
    // 不是 createInfoCount。主 RT 管线是 1 条 createInfo / 6 个 stage，
    // 之前打 "1stage" 会误导"这条管线很小"。提前算好，rt-parcomp 日志同用。
    uint32_t stageCount = 0u;

    if(pCreateInfos != nullptr && createInfoCount > 0u)
    {
        stageCount = pCreateInfos[0].stageCount;
    }

    char name[192] = {};

    std::snprintf(
        name,
        sizeof(name),
        "rt:%s(%ustage)",
        (label != nullptr) ? label : "?",
        stageCount
    );

    // 2026-11-15：冷编让路 —— 本进程第一次 RT 管线创建前先把可让的缓冲还回去
    // （依据见 orionReleaseBuffersForRtCompile 的注释）。
    orionReleaseBuffersForRtCompile(false);

    // 2026-11-22 批A：编译前打 commit 水位。RT 冷编的 OOM（result=-3）前兆
    // 就是 commit 逼近上限（2026-11-21 夜实测：空闲 4GB 时磨 45 分钟后 -3；
    // 空闲 28GB 时 3.5 分钟完成 k1）。>=90% 附提示，照现有 hint 风格。
    {
        MEMORYSTATUSEX commitMem{};
        commitMem.dwLength = sizeof(commitMem);
        if(GlobalMemoryStatusEx(&commitMem))
        {
            char commitBuf[192]{};
            std::snprintf(
                commitBuf,
                sizeof(commitBuf),
                "[PERF PIPE] preCompile commitLoad=%lu%% availCommitMB=%.0f%s",
                static_cast<unsigned long>(commitMem.dwMemoryLoad),
                static_cast<double>(commitMem.ullAvailPageFile) /
                    (1024.0 * 1024.0),
                commitMem.dwMemoryLoad >= 90u
                    ? " hint=closeAppsOrRaisePagefile"
                    : "");
            RTLogFlushed(commitBuf);
        }
    }

    // 2026-11-11 并行冷编尝试：deferred op + N 线程 join；任何失败回退同步。
    OrionParcompResult par;

    if(orionParcompEnabled())
    {
        par = orionParcompAttempt(
            createPipe,
            createInfoCount,
            pCreateInfos,
            pipelineCache,
            pPipelines
        );

        if(par.attempted)
        {
            char ptext[384] = {};

            if(par.usable)
            {
                std::snprintf(
                    ptext,
                    sizeof(ptext),
                    "[PERF PIPE] kind=rt-parcomp spv=%s threads=%u maxJoin=%u deferred=%d ms=%.1f",
                    name,
                    par.threads,
                    par.maxJoin,
                    par.deferred ? 1 : 0,
                    par.milliseconds
                );
            }
            else
            {
                std::snprintf(
                    ptext,
                    sizeof(ptext),
                    "[PERF PIPE] kind=rt-parcomp-fail spv=%s result=%d threads=%u maxJoin=%u deferred=%d ms=%.1f",
                    name,
                    static_cast<int>(par.result),
                    par.threads,
                    par.maxJoin,
                    par.deferred ? 1 : 0,
                    par.milliseconds
                );
            }

            orionPipelineDiagEmit(ptext);
        }
    }

    VkResult result;
    double milliseconds;

    if(par.usable)
    {
        result = par.result;
        milliseconds = par.milliseconds;
    }
    else
    {
        const auto start = std::chrono::steady_clock::now();

        // deferredOperation 仍为 VK_NULL_HANDLE，其余参数与各调用点原样一致。
        result = createPipe(
            g_device,
            VK_NULL_HANDLE,
            pipelineCache,
            createInfoCount,
            pCreateInfos,
            nullptr,
            pPipelines
        );

        milliseconds =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start
            ).count();
    }

    OrionPipelineLogCreate(
        "rt",
        name,
        pipelineCache != VK_NULL_HANDLE,
        milliseconds,
        result
    );

    // 2026-11-08：成功即落盘。巨型管线一轮冷编 15~20 分钟，O(分钟) 的编译刚
    // 完成，用 O(100ms) 的同步写把它保下来 —— 否则后续管线 OOM/abort 崩进程时
    // 这份成果随缓存对象一起消失（落盘点原本只有帧 60 去抖与正常退出）。
    // 2026-11-14：改为延迟落盘，见 orionPersistPipelineCacheDeferred。
    if(result == VK_SUCCESS && pipelineCache != VK_NULL_HANDLE)
    {
        orionPersistPipelineCacheDeferred();
    }

    // 2026-11-15：OOM（result=-3 / VK_ERROR_OUT_OF_HOST_MEMORY）单独处理。
    // 旧行为：直接返回错误，不释放任何东西，重试也排除 -3（内存不会自己回来）。
    // 但 deferred/parcomp 路径会把同样的 OOM **吞掉**后返回"成功"管线（句柄非空
    // 且 result=0），首次 dispatch 即在 nvoglv64+0xF5B5DC AV —— 当天复现三次。
    // 新行为：明确失败 → 先释放我们自己的缓冲 → 重试一次 → 仍失败则把错误交回
    // 调用方（走"无 RT"降级），绝不让可疑管线进入渲染路径。
    if(result == VK_ERROR_OUT_OF_HOST_MEMORY ||
       result == VK_ERROR_OUT_OF_DEVICE_MEMORY)
    {
        const double freedMb = orionReleaseBuffersForRtCompile(true);

        char oomText[320] = {};

        std::snprintf(
            oomText,
            sizeof(oomText),
            "[PERF PIPE] kind=rt-oom spv=%s result=%d freedStagingMB=%.1f "
            "retrying=1 hint=raisePagefileOrCloseApps",
            name,
            static_cast<int>(result),
            freedMb
        );

        orionPipelineDiagEmit(oomText);

        std::this_thread::sleep_for(std::chrono::seconds(2));

        const auto oomRetryStart = std::chrono::steady_clock::now();
        const VkResult oomRetry = createPipe(
            g_device,
            VK_NULL_HANDLE,
            pipelineCache,
            createInfoCount,
            pCreateInfos,
            nullptr,
            pPipelines
        );
        const double oomRetryMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - oomRetryStart
            ).count();

        char oomRetryText[256] = {};

        std::snprintf(
            oomRetryText,
            sizeof(oomRetryText),
            "[PERF PIPE] kind=rt-oom-retry spv=%s retryResult=%d ms=%.1f",
            name,
            static_cast<int>(oomRetry),
            oomRetryMs
        );

        orionPipelineDiagEmit(oomRetryText);

        if(oomRetry == VK_SUCCESS && pipelineCache != VK_NULL_HANDLE)
        {
            orionPersistPipelineCacheDeferred();
        }

        return oomRetry;
    }

    // 2026-11-06 SER -3 定案：INITIALIZATION_FAILED 不是 shader/SER 本身，而是
    // 同会话背靠背多次十分钟级冷编译时驱动编译器资源耗尽（对照实验：同一
    // spv/缓存，main+k1 冷编后 k2d 失败；main+k1 缓存命中时 k2d 冷编成功）。
    // 失败即释放了本次编译的内存 ⇒ 稍候重试一次通常能过。
    if(result != VK_SUCCESS &&
       result != VK_PIPELINE_COMPILE_REQUIRED &&
       result != VK_ERROR_OUT_OF_DEVICE_MEMORY &&
       result != VK_ERROR_OUT_OF_HOST_MEMORY)
    {
        std::this_thread::sleep_for(std::chrono::seconds(3));

        const auto retryStart = std::chrono::steady_clock::now();
        const VkResult retry = createPipe(
            g_device,
            VK_NULL_HANDLE,
            pipelineCache,
            createInfoCount,
            pCreateInfos,
            nullptr,
            pPipelines
        );
        const double retryMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - retryStart
            ).count();

        char retryText[256] = {};

        std::snprintf(
            retryText,
            sizeof(retryText),
            "[PERF PIPE] kind=rt-retry spv=rt:%s firstResult=%d retryResult=%d "
            "ms=%.1f",
            (label != nullptr) ? label : "?",
            static_cast<int>(result),
            static_cast<int>(retry),
            retryMs
        );

        orionPipelineDiagEmit(retryText);

        if(retry == VK_SUCCESS && pipelineCache != VK_NULL_HANDLE)
        {
            orionPersistPipelineCacheDeferred();
        }

        return retry;
    }

    return result;
}

// 2026-11-08：VK_KHR_pipeline_executable_properties 寄存器/spill 基线。
// 每条管线逐 executable 打 [PERF PSTAT] 行；统计键值由驱动任意命名
// （NVIDIA 侧含寄存器/local memory 等），原样透传不挑字段。
void OrionLogPipelineStats(const char* label, VkPipeline pipeline)
{
    if(g_device == VK_NULL_HANDLE || pipeline == VK_NULL_HANDLE)
    {
        return;
    }

    auto getProps =
        reinterpret_cast<PFN_vkGetPipelineExecutablePropertiesKHR>(
            vkGetDeviceProcAddr(g_device,
                                "vkGetPipelineExecutablePropertiesKHR"));
    auto getStats =
        reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(
            vkGetDeviceProcAddr(g_device,
                                "vkGetPipelineExecutableStatisticsKHR"));

    if(getProps == nullptr || getStats == nullptr)
    {
        char text[256] = {};
        std::snprintf(
            text, sizeof(text),
            "[PERF PSTAT] pipe=%s unavailable entrypoint=0", label);
        orionPipelineDiagEmit(text);
        return;
    }

    VkPipelineInfoKHR pInfo{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
    pInfo.pipeline = pipeline;

    uint32_t exeCount = 0u;

    if(getProps(g_device, &pInfo, &exeCount, nullptr) != VK_SUCCESS ||
       exeCount == 0u)
    {
        char text[256] = {};
        std::snprintf(
            text, sizeof(text), "[PERF PSTAT] pipe=%s executables=0", label);
        orionPipelineDiagEmit(text);
        return;
    }

    std::vector<VkPipelineExecutablePropertiesKHR> props(exeCount);

    for(auto& p : props)
    {
        p = {};
        p.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR;
    }

    if(getProps(g_device, &pInfo, &exeCount, props.data()) != VK_SUCCESS)
    {
        return;
    }

    for(uint32_t i = 0u; i < exeCount; ++i)
    {
        VkPipelineExecutableInfoKHR eInfo{
            VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
        eInfo.pipeline = pipeline;
        eInfo.executableIndex = i;

        uint32_t statCount = 0u;
        getStats(g_device, &eInfo, &statCount, nullptr);

        std::vector<VkPipelineExecutableStatisticKHR> stats(statCount);

        for(auto& s : stats)
        {
            s = {};
            s.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR;
        }

        if(statCount > 0u)
        {
            getStats(g_device, &eInfo, &statCount, stats.data());
        }

        char text[512] = {};

        std::snprintf(
            text,
            sizeof(text),
            "[PERF PSTAT] pipe=%s exe=%u/%u stages=0x%x subgrp=%u name=%.96s",
            (label != nullptr) ? label : "?",
            i,
            exeCount,
            props[i].stages,
            props[i].subgroupSize,
            props[i].name
        );

        orionPipelineDiagEmit(text);

        for(uint32_t s = 0u; s < statCount; ++s)
        {
            const VkPipelineExecutableStatisticKHR& st = stats[s];
            char line[640] = {};

            switch(st.format)
            {
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
                    std::snprintf(
                        line, sizeof(line), "[PERF PSTAT]   %s=%d (%.96s)",
                        st.name, static_cast<int>(st.value.b32),
                        st.description);
                    break;
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
                    std::snprintf(
                        line, sizeof(line), "[PERF PSTAT]   %s=%lld (%.96s)",
                        st.name,
                        static_cast<long long>(st.value.i64),
                        st.description);
                    break;
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
                    std::snprintf(
                        line, sizeof(line), "[PERF PSTAT]   %s=%llu (%.96s)",
                        st.name,
                        static_cast<unsigned long long>(st.value.u64),
                        st.description);
                    break;
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR:
                    std::snprintf(
                        line, sizeof(line), "[PERF PSTAT]   %s=%.3f (%.96s)",
                        st.name, st.value.f64, st.description);
                    break;
                default:
                    std::snprintf(
                        line, sizeof(line), "[PERF PSTAT]   %s=<fmt%d>",
                        st.name, static_cast<int>(st.format));
                    break;
            }

            orionPipelineDiagEmit(line);
        }
    }
}
