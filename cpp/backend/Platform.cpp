// Orion backend — Platform 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"

#include <atomic>
#include <condition_variable>
#include <ctime>
#include <chrono>
#include <deque>
#include <thread>

#include <windows.h>
#include <dbghelp.h>

#pragma comment(lib, "dbghelp.lib")



std::string GetRTXBackendDirectory()
{
    static const std::string cachedDirectory = []() -> std::string
    {
        std::vector<char> buffer(
            32768,
            '\0'
        );


        const DWORD length =
            GetModuleFileNameA(
                reinterpret_cast<HMODULE>(
                    &__ImageBase
                ),
                buffer.data(),
                static_cast<DWORD>(
                    buffer.size()
                )
            );


        if(
            length == 0 ||
            length >= buffer.size()
        )
        {
            fprintf(
                stderr,
                "[RUNTIME PATH] GetModuleFileNameA failed error=%lu\n",
                GetLastError()
            );

            fflush(stderr);

            return {};
        }


        std::string modulePath(
            buffer.data(),
            length
        );


        const std::string::size_type slash =
            modulePath.find_last_of(
                "\\/"
            );


        if(slash == std::string::npos)
        {
            fprintf(
                stderr,
                "[RUNTIME PATH] invalid backend module path=%s\n",
                modulePath.c_str()
            );

            fflush(stderr);

            return {};
        }


        return modulePath.substr(
            0,
            slash
        );
    }();


    return cachedDirectory;
}


bool IsAbsoluteWindowsPath(
    const char* path
)
{
    if(
        !path ||
        path[0] == '\0'
    )
    {
        return false;
    }


    if(
        path[0] != '\0' &&
        path[1] == ':' &&
        (
            path[2] == '\\' ||
            path[2] == '/'
        )
    )
    {
        return true;
    }


    return
        path[0] == '\\' ||
        path[0] == '/';
}


std::string ResolveRTXRuntimePath(
    const char* path
)
{
    if(
        !path ||
        path[0] == '\0'
    )
    {
        return {};
    }


    if(IsAbsoluteWindowsPath(path))
    {
        return std::string(path);
    }


    const std::string baseDirectory =
        GetRTXBackendDirectory();


    if(baseDirectory.empty())
    {
        return std::string(path);
    }


    return
        baseDirectory +
        "\\" +
        path;
}


// =====================================================
// Optional MFG Ada unlock module (RTXMFG.dll)
// =====================================================
//
// Third-party module that makes 3x/4x/6x frame generation legal on Ada by
// detouring Streamline (slInit, slDLSSGSetOptions, slDLSSGGetState,
// slGetFeatureFunction) and the NGX DLSS-G provider entry points inside this
// process, and by correcting the provider's temporal program in mapped memory.
// It must be inside the process before slInit runs and before the DLSS-G
// pipeline is created, which is why the backend loads it rather than leaning on
// the game: the executable in a Fabric run is java.exe inside the JDK
// directory, so the mod's documented "rename it to a proxy DLL next to the exe"
// route would mean writing into the JDK.
//
// Entirely optional.  A missing file is not an error, nothing in this backend
// depends on it, and deleting RTXMFG.dll turns the unlock off again: the
// multiplier setting stays clamped to whatever the driver reports, and this
// module is what raises that ceiling.
void LoadMfgUnlockModuleIfPresent()
{
    static bool attempted = false;


    if(attempted)
    {
        return;
    }

    attempted = true;


    const std::string modulePath =
        ResolveRTXRuntimePath(
            "RTXMFG.dll"
        );


    if(
        modulePath.empty() ||
        GetFileAttributesA(
            modulePath.c_str()
        ) == INVALID_FILE_ATTRIBUTES
    )
    {
        fprintf(
            stderr,
            "[MFG UNLOCK] RTXMFG.dll not present, unlock inactive\n"
        );

        fflush(stderr);

        return;
    }


    HMODULE unlockModule =
        LoadLibraryExA(
            modulePath.c_str(),
            nullptr,
            0
        );


    fprintf(
        stderr,
        "[MFG UNLOCK] path=%s handle=%p error=%lu\n",
        modulePath.c_str(),
        reinterpret_cast<void*>(
            unlockModule
        ),
        unlockModule
            ? 0ul
            : GetLastError()
    );

    fflush(stderr);
}


// Writes one timestamped line.  Flushing is opt-in: the buffered path exists so
// filesystem latency never lands on the render thread.
namespace
{
    struct AsyncRtLogState
    {
        std::mutex mtx;
        std::condition_variable cv;
        std::condition_variable flushCv;
        std::deque<std::string> queue;
        uint64_t enqueuedSeq = 0u;
        uint64_t flushTargetSeq = 0u;
        uint64_t flushedSeq = 0u;
        bool started = false;
    };

    AsyncRtLogState* getAsyncRtLogState()
    {
        static AsyncRtLogState* s_asyncLog = new AsyncRtLogState();
        return s_asyncLog;
    }

    void asyncRtLogWorkerMain(AsyncRtLogState* state)
    {
        std::deque<std::string> localBatch;
        for(;;)
        {
            uint64_t batchEndSeq = 0u;
            bool needFlush = false;
            {
                std::unique_lock<std::mutex> lock(state->mtx);
                state->cv.wait(
                    lock,
                    [state]()
                    {
                        return !state->queue.empty() ||
                               state->flushTargetSeq > state->flushedSeq;
                    }
                );
                localBatch.swap(state->queue);
                batchEndSeq = state->enqueuedSeq;
                needFlush = state->flushTargetSeq > state->flushedSeq;
            }

            if(!localBatch.empty() || needFlush)
            {
                std::lock_guard<std::mutex> fileLock(g_rtLogMutex);
                for(const std::string& line : localBatch)
                {
                    rtLogFile << line;
                }
                if(needFlush)
                {
                    rtLogFile.flush();
                }
                localBatch.clear();
            }

            {
                std::lock_guard<std::mutex> lock(state->mtx);
                if(batchEndSeq > state->flushedSeq)
                {
                    state->flushedSeq = batchEndSeq;
                }
                if(needFlush)
                {
                    state->flushCv.notify_all();
                }
            }
        }
    }
}

void RTLogWrite(
    const std::string& msg,
    bool flush
)
{
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);

    AsyncRtLogState* state = getAsyncRtLogState();
    uint64_t targetSeq = 0u;

    {
        std::lock_guard<std::mutex> lock(state->mtx);

        if(!state->started)
        {
            state->started = true;
            std::thread(asyncRtLogWorkerMain, state).detach();
        }

        if(!flush && state->queue.size() >= 8192u)
        {
            return;
        }

        static std::time_t s_lastStampSecond = static_cast<std::time_t>(-1);
        static char s_stamp[16] = "00:00:00";

        if(t != s_lastStampSecond)
        {
            const std::tm* local = std::localtime(&t);

            if(local == nullptr ||
               std::strftime(s_stamp, sizeof(s_stamp), "%H:%M:%S", local) == 0u)
            {
                std::snprintf(s_stamp, sizeof(s_stamp), "--:--:--");
            }

            s_lastStampSecond = t;
        }

        std::string line;
        line.reserve(16u + msg.size());
        line.append("[CPP] ");
        line.append(s_stamp);
        line.push_back(' ');
        line.append(msg);
        line.push_back('\n');

        state->queue.push_back(std::move(line));
        targetSeq = ++state->enqueuedSeq;
        if(flush && targetSeq > state->flushTargetSeq)
        {
            state->flushTargetSeq = targetSeq;
        }
    }

    state->cv.notify_one();

    if(flush)
    {
        std::unique_lock<std::mutex> lock(state->mtx);
        state->flushCv.wait_for(
            lock,
            std::chrono::milliseconds(200),
            [state, targetSeq]()
            {
                return state->flushedSeq >= targetSeq;
            }
        );
    }
}


void RTLog(
    const std::string& msg
)
{
    RTLogWrite(msg, false);
}


// Flushed variant for the rare diagnostics whose loss would invalidate a whole
// capture: the host can die before the buffered stream ever reaches disk, and a
// report that was never written is indistinguishable from a stall that never
// happened.
void RTLogFlushed(
    const std::string& msg
)
{
    RTLogWrite(msg, true);
}
void RTXTerminateHandler()
{
    if(auto e = std::current_exception())
    {
        try
        {
            std::rethrow_exception(e);
        }
        catch(const std::exception& ex)
        {
            RTLogFlushed(std::string("[FATAL] uncaught C++ exception: ") + ex.what());
        }
        catch(...)
        {
            RTLogFlushed("[FATAL] uncaught non-std C++ exception");
        }
    }
    else
    {
        RTLogFlushed("[FATAL] std::terminate with no active exception");
    }
    std::abort();
}
void RTXMarkPhase(const char* name)
{
    const uint64_t seq =
        g_rtxPhaseSeq.fetch_add(1, std::memory_order_relaxed) + 1u;
    const int slot = static_cast<int>(seq % RTX_PHASE_RING);

    g_rtxPhaseStamp[slot].store(seq, std::memory_order_relaxed);
    g_rtxPhaseName[slot].store(name, std::memory_order_relaxed);
    g_rtxPhaseCurrent.store(name, std::memory_order_relaxed);
}
std::string RTXHex(uint64_t value)
{
    static const char* digits = "0123456789ABCDEF";

    char buffer[20] = { '0', 'x' };
    int pos = 2;
    bool started = false;

    for(int shift = 60; shift >= 0; shift -= 4)
    {
        const int nibble = static_cast<int>((value >> shift) & 0xFull);

        if(nibble != 0)
        {
            started = true;
        }

        if(started || shift == 0)
        {
            buffer[pos] = digits[nibble];
            pos += 1;
        }
    }

    buffer[pos] = '\0';

    return std::string(buffer);
}

// 故障地址 → "模块名+偏移"（与 hs_err 的 Problematic frame 同口径）。
std::string RTXModuleOfPc(uintptr_t pc)
{
    HMODULE module = nullptr;

    if(GetModuleHandleExA(
           GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
           reinterpret_cast<LPCSTR>(pc),
           &module) &&
       module != nullptr)
    {
        char path[MAX_PATH] = {};

        if(GetModuleFileNameA(module, path, MAX_PATH) > 0)
        {
            const char* leaf = std::strrchr(path, '\\');
            leaf = (leaf != nullptr) ? (leaf + 1) : path;

            return std::string(leaf) +
                   "+" + RTXHex(pc - reinterpret_cast<uintptr_t>(module));
        }

        return std::string("<module>+") +
               RTXHex(pc - reinterpret_cast<uintptr_t>(module));
    }

    return std::string("<unknown module>@") + RTXHex(pc);
}

// 阶段面包屑倒序打印（最新在前）。崩溃时也写一份到独立文件，
// 避免 rt_debug.log 的流在崩溃现场不可用。
void RTXDumpCrashContext(const std::string& header)
{
    const uint64_t seq = g_rtxPhaseSeq.load(std::memory_order_relaxed);
    const uint64_t frame = g_rtxLastFrameSerial.load(std::memory_order_relaxed);
    const char* current = g_rtxPhaseCurrent.load(std::memory_order_relaxed);

    RTLogFlushed(header);

    const uint64_t detailKey =
        g_rtxDetailSectionKey.load(std::memory_order_relaxed);
    const uint64_t detailVerts =
        g_rtxDetailSectionVerts.load(std::memory_order_relaxed);
    const uint64_t detailInstances =
        g_rtxDetailInstances.load(std::memory_order_relaxed);

    RTLogFlushed(
        std::string("[CRASH] frame=") + std::to_string(frame) +
        " phase=" + (current != nullptr ? current : "?") +
        " sectionKey=" + std::to_string(detailKey) +
        " sectionVerts=" + std::to_string(detailVerts) +
        " tlasInstances=" + std::to_string(detailInstances)
    );

    for(uint64_t back = 1; back <= 8u && back <= seq; ++back)
    {
        const uint64_t stamp = seq - back + 1u;
        const int slot = static_cast<int>(stamp % RTX_PHASE_RING);

        if(g_rtxPhaseStamp[slot].load(std::memory_order_relaxed) != stamp)
        {
            continue;
        }

        const char* name = g_rtxPhaseName[slot].load(std::memory_order_relaxed);

        RTLogFlushed(
            std::string("[CRASH PATH] #") + std::to_string(stamp) +
            " " + (name != nullptr ? name : "?")
        );
    }

    std::ofstream crashFile("rt_crash.log", std::ios::app);

    if(crashFile.is_open())
    {
        crashFile << header << "\n";
        crashFile << "[CRASH] frame=" << frame
                  << " phase=" << (current != nullptr ? current : "?")
                  << " sectionKey=" << detailKey
                  << " sectionVerts=" << detailVerts
                  << " tlasInstances=" << detailInstances << "\n";

        for(uint64_t back = 1; back <= 8u && back <= seq; ++back)
        {
            const uint64_t stamp = seq - back + 1u;
            const int slot = static_cast<int>(stamp % RTX_PHASE_RING);

            if(g_rtxPhaseStamp[slot].load(std::memory_order_relaxed) != stamp)
            {
                continue;
            }

            const char* name = g_rtxPhaseName[slot].load(std::memory_order_relaxed);

            crashFile << "[CRASH PATH] #" << stamp << " "
                      << (name != nullptr ? name : "?") << "\n";
        }

        crashFile.flush();
    }
}
LONG WINAPI RTXUnhandledExceptionFilter(EXCEPTION_POINTERS* info)
{
    DWORD exceptionCode = 0u;
    uintptr_t exceptionAddress = 0u;
    uintptr_t instructionPointer = 0u;
    uintptr_t stackPointer = 0u;

    if(info != nullptr)
    {
        if(info->ExceptionRecord != nullptr)
        {
            exceptionCode = info->ExceptionRecord->ExceptionCode;
            exceptionAddress =
                reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
        }

        if(info->ContextRecord != nullptr)
        {
#if defined(_M_X64)
            instructionPointer =
                static_cast<uintptr_t>(info->ContextRecord->Rip);
            stackPointer =
                static_cast<uintptr_t>(info->ContextRecord->Rsp);
#elif defined(_M_ARM64)
            instructionPointer =
                static_cast<uintptr_t>(info->ContextRecord->Pc);
            stackPointer =
                static_cast<uintptr_t>(info->ContextRecord->Sp);
#else
            instructionPointer = exceptionAddress;
#endif
        }
    }

    const uintptr_t faultPc =
        (exceptionAddress != 0u) ? exceptionAddress : instructionPointer;

    RTXDumpCrashContext(
        std::string("[CRASH] unhandled native exception code=") +
        RTXHex(exceptionCode) +
        " at=" + RTXModuleOfPc(faultPc) +
        " rip=" + RTXHex(instructionPointer) +
        " rsp=" + RTXHex(stackPointer) +
        " thread=" + std::to_string(GetCurrentThreadId())
    );

    if(g_rtxPreviousExceptionFilter != nullptr)
    {
        return g_rtxPreviousExceptionFilter(info);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}



// ============================================================
// 2026-10-29 OPT-EXT①：GPU 挂死诊断
//
// VK_EXT_device_fault + VK_NV_device_diagnostic_checkpoints。
//
// 背景：2026-10-21 那次"进世界必崩 / 5s semaphore 超时"排查了四轮才落到
// "rchit 的 #else 分支少写一条 ray payload ⇒ material_resolve 拿上一条射线的
// identity 索引 ⇒ buffer_reference 解引用任意地址 ⇒ GPU 挂死"。当时没有任何
// 设备侧读数可用，只能二分 + 读源码。本函数唯一目的：设备真的丢掉时，把驱动
// 给的描述/地址与队列最后一个 checkpoint 写进 rt_debug.log，下一次先看日志。
//
// 两者都是只读查询，不改变任何渲染行为。扩展未启用时只打一行 "not enabled"。
// ============================================================
// 2026-11-14：GPU 卡死探针实现（声明与背景见 OrionInternal.h）。
namespace
{
    struct RtFreedRange
    {
        uint64_t address;
        uint64_t bytes;
        const char* tag;
        uint64_t frame;
    };

    constexpr uint32_t RT_FREED_RANGE_SLOTS = 512u;

    RtFreedRange g_rtFreedRanges[RT_FREED_RANGE_SLOTS]{};
    std::atomic<uint32_t> g_rtFreedRangeWrite{0u};
    std::atomic<uint64_t> g_rtFrameHeartbeat{0u};
    std::atomic<uint32_t> g_rtRenderThreadId{0u};
    std::atomic<bool> g_rtHangWatchdogStarted{false};
    std::atomic<bool> g_rtHangReported{false};
}

// ============================================================
// 2026-11-14 晚：VK_AMD_buffer_marker 精确打点
//
// 为什么再加一层（checkpoint 已经有了）：
//   21:34 局的 dump 是
//     checkpoints=2 | cp[0] marker=frame:sharc-resolve | cp[1] marker=frame:sharc-resolve
//   但 vkGetQueueCheckpointData2NV 只回"每个命令缓冲最后一跳"，本工程是双缓冲
//   ⇒ 已经完整跑完的旧帧，它的最后一跳也是 frame:sharc-resolve（那是我们录的
//   最后一个 tag）⇒ "两跳都是 sharc-resolve" 无法区分
//     (a) GPU 卡在 sharc-resolve 之后的 pass，还是
//     (b) 两个 cb 都在更早的位置卡住、这个 tag 只是历史残留。
//
// buffer marker 解决歧义：把 (frameSerial<<20 | tagIndex+1) 直接写进一块
// host-visible 缓冲，GPU 走一个 pass 就写一次。挂死后读回来的值就是
// "GPU 实际执行到的最后一跳 + 是哪一帧"，与 CPU 侧的"已录到哪一跳"对比即可
// 分出「GPU 卡住」与「GPU 压根没开始这一帧」。
//
// 只用已启用的 VK_AMD_buffer_marker（渲染后端创建设备时就开了，见启动日志
// "VK_AMD_buffer_marker (D)"），不需要新扩展。ORION_HANG_MARKERS=0 可关。
// ============================================================
namespace
{
    constexpr uint32_t RT_HANG_TAG_SLOTS = 128u;

    const char* g_rtHangTagNames[RT_HANG_TAG_SLOTS]{};
    uint32_t g_rtHangTagCount = 0u;

    VkBuffer g_rtHangMarkerBuffer = VK_NULL_HANDLE;
    VkDeviceMemory g_rtHangMarkerMemory = VK_NULL_HANDLE;
    volatile uint64_t* g_rtHangMarkerMapped = nullptr;
    PFN_vkCmdWriteBufferMarkerAMD g_rtHangWriteMarker = nullptr;

    std::atomic<uint32_t> g_rtHangLastRecordedTag{~0u};
    std::atomic<uint64_t> g_rtHangLastRecordedFrame{0u};

    const char* rtHangTagName(uint32_t index)
    {
        if(index >= RT_HANG_TAG_SLOTS || g_rtHangTagNames[index] == nullptr)
        {
            return "?";
        }

        return g_rtHangTagNames[index];
    }

    uint32_t rtHangTagIndex(const char* tag)
    {
        if(tag == nullptr)
        {
            return 0u;
        }

        for(uint32_t i = 0u; i < g_rtHangTagCount; ++i)
        {
            if(g_rtHangTagNames[i] != nullptr &&
               std::strcmp(g_rtHangTagNames[i], tag) == 0)
            {
                return i;
            }
        }

        if(g_rtHangTagCount >= RT_HANG_TAG_SLOTS)
        {
            return RT_HANG_TAG_SLOTS - 1u;
        }

        const uint32_t index = g_rtHangTagCount++;
        g_rtHangTagNames[index] = tag;

        return index;
    }

    bool rtHangMarkersEnabled()
    {
        static const bool enabled = []() -> bool
        {
            const char* raw = std::getenv("ORION_HANG_MARKERS");

            if(raw != nullptr &&
               (raw[0] == '0' || raw[0] == 'n' || raw[0] == 'N'))
            {
                return false;
            }

            return true;
        }();

        return enabled;
    }

    void rtHangMarkerEnsure()
    {
        if(g_rtHangMarkerBuffer != VK_NULL_HANDLE || g_device == VK_NULL_HANDLE)
        {
            return;
        }

        g_rtHangWriteMarker = reinterpret_cast<PFN_vkCmdWriteBufferMarkerAMD>(
            vkGetDeviceProcAddr(g_device, "vkCmdWriteBufferMarkerAMD"));

        if(g_rtHangWriteMarker == nullptr)
        {
            return;
        }

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = static_cast<VkDeviceSize>(RT_HANG_TAG_SLOTS) * 8u;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if(vkCreateBuffer(g_device, &bufferInfo, nullptr, &g_rtHangMarkerBuffer)
           != VK_SUCCESS)
        {
            g_rtHangMarkerBuffer = VK_NULL_HANDLE;
            return;
        }

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(
            g_device,
            g_rtHangMarkerBuffer,
            &requirements);

        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(g_physicalDevice, &properties);

        const VkMemoryPropertyFlags want =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        uint32_t typeIndex = ~0u;

        for(uint32_t i = 0u; i < properties.memoryTypeCount; ++i)
        {
            if((requirements.memoryTypeBits & (1u << i)) != 0u &&
               (properties.memoryTypes[i].propertyFlags & want) == want)
            {
                typeIndex = i;
                break;
            }
        }

        if(typeIndex == ~0u)
        {
            vkDestroyBuffer(g_device, g_rtHangMarkerBuffer, nullptr);
            g_rtHangMarkerBuffer = VK_NULL_HANDLE;
            return;
        }

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = typeIndex;

        if(vkAllocateMemory(g_device, &allocInfo, nullptr, &g_rtHangMarkerMemory)
           != VK_SUCCESS)
        {
            vkDestroyBuffer(g_device, g_rtHangMarkerBuffer, nullptr);
            g_rtHangMarkerBuffer = VK_NULL_HANDLE;
            g_rtHangMarkerMemory = VK_NULL_HANDLE;
            return;
        }

        if(vkBindBufferMemory(
               g_device,
               g_rtHangMarkerBuffer,
               g_rtHangMarkerMemory,
               0u) != VK_SUCCESS)
        {
            vkFreeMemory(g_device, g_rtHangMarkerMemory, nullptr);
            vkDestroyBuffer(g_device, g_rtHangMarkerBuffer, nullptr);
            g_rtHangMarkerBuffer = VK_NULL_HANDLE;
            g_rtHangMarkerMemory = VK_NULL_HANDLE;
            return;
        }

        void* mapped = nullptr;

        if(vkMapMemory(
               g_device,
               g_rtHangMarkerMemory,
               0u,
               VK_WHOLE_SIZE,
               0u,
               &mapped) != VK_SUCCESS)
        {
            vkFreeMemory(g_device, g_rtHangMarkerMemory, nullptr);
            vkDestroyBuffer(g_device, g_rtHangMarkerBuffer, nullptr);
            g_rtHangMarkerBuffer = VK_NULL_HANDLE;
            g_rtHangMarkerMemory = VK_NULL_HANDLE;
            return;
        }

        g_rtHangMarkerMapped = static_cast<volatile uint64_t*>(mapped);
        *g_rtHangMarkerMapped = 0ull;
    }
}


void rtGpuFreedRangeNote(
    uint64_t address,
    uint64_t bytes,
    const char* tag
)
{
    if(address == 0u)
    {
        return;
    }

    const uint32_t slot =
        g_rtFreedRangeWrite.fetch_add(1u, std::memory_order_relaxed) %
        RT_FREED_RANGE_SLOTS;

    g_rtFreedRanges[slot].address = address;
    g_rtFreedRanges[slot].bytes = bytes;
    g_rtFreedRanges[slot].tag = tag;
    g_rtFreedRanges[slot].frame = g_perfL10FrameSerial;
}

// 2026-11-14：卡死线程的 native 栈回溯。打印 `模块+RVA`，离线用
// llvm-symbolizer 对着 rtx_backend.pdb 解出函数名 —— 这样能直接看清
// 渲染线程卡在哪个调用里（此前只能靠 [GPU HANG] 的粗阶段 0~5 猜）。
static void rtDumpThreadStack(uint32_t threadId)
{
    if(threadId == 0u)
    {
        return;
    }

    HANDLE thread = OpenThread(
        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
        FALSE,
        static_cast<DWORD>(threadId));

    if(thread == nullptr)
    {
        RTLogFlushed("[GPU HANG] stack: OpenThread failed");
        return;
    }

    if(SuspendThread(thread) == static_cast<DWORD>(-1))
    {
        RTLogFlushed("[GPU HANG] stack: SuspendThread failed");
        CloseHandle(thread);
        return;
    }

    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;

    if(GetThreadContext(thread, &context) != 0)
    {
        // 2026-11-14：SymInitialize 不接受 GetCurrentProcess() 的伪句柄
        // （实测返回 ERROR_INVALID_PARAMETER=87），必须用 OpenProcess 真句柄。
        HANDLE process = OpenProcess(
            PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
            FALSE,
            GetCurrentProcessId());

        if(process == nullptr)
        {
            RTLogFlushed("[GPU HANG] stack: OpenProcess failed");
            ResumeThread(thread);
            CloseHandle(thread);
            return;
        }

        static bool symbolsReady = false;

        if(!symbolsReady)
        {
            // 2026-11-14：之前 SymInitialize(.., nullptr, TRUE) 直接失败。
            // 给出明确的符号搜索路径（游戏目录，rtx_backend.pdb 就在这里），
            // 并关掉「critical error 弹框」；失败时把 GetLastError 打出来。
            char modulePath[MAX_PATH]{};
            GetModuleFileNameA(nullptr, modulePath, MAX_PATH);

            std::string searchPath = modulePath;

            const size_t slash = searchPath.find_last_of("\\/");

            if(slash != std::string::npos)
            {
                searchPath.resize(slash);
            }

            SymSetOptions(
                SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS |
                SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_LOAD_LINES);

            symbolsReady = SymInitialize(process, searchPath.c_str(), TRUE) != 0;

            if(!symbolsReady)
            {
                RTLogFlushed(
                    "[GPU HANG] stack: SymInitialize failed err=" +
                    std::to_string(static_cast<unsigned long>(GetLastError())) +
                    " path=" + searchPath);
            }
        }

        if(symbolsReady)
        {
            STACKFRAME64 frame{};
            frame.AddrPC.Offset = context.Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrStack.Offset = context.Rsp;
            frame.AddrStack.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = context.Rbp;
            frame.AddrFrame.Mode = AddrModeFlat;

            for(int depth = 0; depth < 24; ++depth)
            {
                if(StackWalk64(
                       IMAGE_FILE_MACHINE_AMD64,
                       process,
                       thread,
                       &frame,
                       &context,
                       nullptr,
                       SymFunctionTableAccess64,
                       SymGetModuleBase64,
                       nullptr) == FALSE)
                {
                    break;
                }

                const DWORD64 pc = frame.AddrPC.Offset;

                if(pc == 0u)
                {
                    break;
                }

                IMAGEHLP_MODULE64 moduleInfo{};
                moduleInfo.SizeOfStruct = sizeof(moduleInfo);

                std::string where = "?";

                const DWORD64 moduleBase =
                    SymGetModuleBase64(process, pc);

                if(moduleBase != 0u &&
                   SymGetModuleInfo64(process, pc, &moduleInfo) != FALSE)
                {
                    where = std::string(
                        moduleInfo.ImageName != nullptr
                            ? moduleInfo.ImageName
                            : "?") +
                        "+0x" +
                        std::to_string(
                            static_cast<unsigned long long>(
                                pc - moduleBase));
                }
                else
                {
                    where = "pc=0x" + std::to_string(
                        static_cast<unsigned long long>(pc));
                }

                RTLogFlushed(
                    "[GPU HANG] stack[" + std::to_string(depth) + "] " + where);
            }
        }
        else
        {
            RTLogFlushed("[GPU HANG] stack: SymInitialize failed");
        }
    }

    ResumeThread(thread);
    CloseHandle(thread);
}

// 2026-11-14：TDR 后 host-visible 实例表的映射可能已死（buffer 被释放/unmap，
// 或设备丢失后驱动作废映射），透过 g_tlasInstancesAddress 裸读会 AV 且把看门狗
// 线程带崩 —— 实测 0x76DF4 三连崩，后面 phase/VLI/设备诊断一行都打不出来。
// 把裸扫描隔离进 POD-only 函数用 SEH 兜底：崩了就放弃扫描继续走完 dump。
// MSVC 限制：__try 里不能有带析构的对象，所以日志全挪回调用方。
struct RtDanglingPairRecord
{
    size_t index;
    uint64_t ref;
    uint64_t freedAddr;
    uint64_t freedBytes;
    uint64_t freedFrame;
    const char* freedTag;
};

static bool rtScanTlasDanglingSeh(
    const void* instancesBase,
    size_t count,
    const RtFreedRange* ranges,
    uint32_t rangeCount,
    RtDanglingPairRecord* outPairs,
    uint32_t maxPairs,
    uint64_t* outDangling,
    uint32_t* outPairCount
)
{
    __try
    {
        const auto* tlasInstances =
            static_cast<const VkAccelerationStructureInstanceKHR*>(
                instancesBase);

        uint64_t dangling = 0u;
        uint32_t pairCount = 0u;

        for(size_t i = 0u; i < count; ++i)
        {
            const uint64_t ref =
                tlasInstances[i].accelerationStructureReference;

            if(ref == 0u)
            {
                continue;
            }

            for(uint32_t s = 0u; s < rangeCount; ++s)
            {
                const RtFreedRange& range = ranges[s];

                if(range.address == 0u)
                {
                    continue;
                }

                const bool inside =
                    ref >= range.address &&
                    (range.bytes == 0u || ref < range.address + range.bytes);

                if(!inside)
                {
                    continue;
                }

                ++dangling;

                if(pairCount < maxPairs)
                {
                    RtDanglingPairRecord& record = outPairs[pairCount++];
                    record.index = i;
                    record.ref = ref;
                    record.freedAddr = range.address;
                    record.freedBytes = range.bytes;
                    record.freedFrame = range.frame;
                    record.freedTag = range.tag;
                }

                break;
            }
        }

        *outDangling = dangling;
        *outPairCount = pairCount;
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}


// 2026-11-14 晚：把"GPU 实际走到哪"与"CPU 录到哪"一起打出来。
static void rtLogHangMarkerForHang()
{
    const uint32_t recordedTag =
        g_rtHangLastRecordedTag.load(std::memory_order_relaxed);

    const uint64_t recordedFrame =
        g_rtHangLastRecordedFrame.load(std::memory_order_relaxed);

    RTLogFlushed(
        "[GPU HANG] record: CPU last recorded tag=" +
        std::string(rtHangTagName(recordedTag)) +
        " (#" + std::to_string(recordedTag) + ")" +
        " frame=" + std::to_string(recordedFrame));

    if(g_rtHangMarkerMapped == nullptr)
    {
        RTLogFlushed(
            "[GPU HANG] marker: unavailable "
            "(VK_AMD_buffer_marker 未启用或未初始化)");
        return;
    }

    const uint32_t tagCount = g_rtHangTagCount;

    uint64_t newestFramePlusOne = 0ull;

    for(uint32_t i = 0u; i < tagCount; ++i)
    {
        const uint64_t slotValue = g_rtHangMarkerMapped[i];

        if(slotValue > newestFramePlusOne)
        {
            newestFramePlusOne = slotValue;
        }
    }

    if(newestFramePlusOne == 0ull)
    {
        RTLogFlushed("[GPU HANG] marker: never written");
        return;
    }

    const uint64_t newestFrame = newestFramePlusOne - 1ull;

    std::string reached;
    std::string stale;

    for(uint32_t i = 0u; i < tagCount; ++i)
    {
        const uint64_t slotValue = g_rtHangMarkerMapped[i];

        std::string& target =
            (slotValue == newestFramePlusOne) ? reached : stale;

        if(!target.empty())
        {
            target += " | ";
        }

        target += "#" + std::to_string(i) + " " + rtHangTagName(i);

        if(slotValue != newestFramePlusOne)
        {
            target += "(f=" + std::to_string(
                slotValue == 0ull ? 0ull : slotValue - 1ull) + ")";
        }
    }

    RTLogFlushed(
        "[GPU HANG] marker: newest executed frame=" +
        std::to_string(newestFrame) +
        " (该帧已执行的 tag) " +
        (reached.empty() ? std::string("(none)") : reached));

    RTLogFlushed(
        "[GPU HANG] marker: (没到最新帧的 tag) " +
        (stale.empty() ? std::string("(none)") : stale));

    RTLogFlushed(
        "[GPU HANG] dispatch: cloud=" +
        std::to_string(g_volumetricCloudWidth) + "x" +
        std::to_string(g_volumetricCloudHeight) +
        " groups=" + std::to_string((g_volumetricCloudWidth + 7u) / 8u) + "x" +
        std::to_string((g_volumetricCloudHeight + 7u) / 8u) +
        " | rt=" + std::to_string(g_rtOutputWidth) + "x" +
        std::to_string(g_rtOutputHeight) +
        " | godray=" + std::to_string(g_godRayWidth) + "x" +
        std::to_string(g_godRayHeight));
}

static void rtDumpHangState()
{
    std::string head = "[GPU HANG] heartbeat stalled frame=" +
        std::to_string(g_perfL10FrameSerial) +
        " vramUsedMB=" + std::to_string(g_vramProbe.usedMb) +
        " vramBudgetMB=" + std::to_string(g_vramProbe.budgetMb) +
        " vramRatio=" + std::to_string(g_vramProbe.ratio);

    RTLogFlushed(head);

    // 最近 12 条已释放显存（新 → 旧），这是"命令引用已释放内存"的直接线索。
    const uint32_t write =
        g_rtFreedRangeWrite.load(std::memory_order_relaxed);

    for(uint32_t back = 1u; back <= 12u; ++back)
    {
        const uint32_t slot =
            (write + RT_FREED_RANGE_SLOTS - back) % RT_FREED_RANGE_SLOTS;
        const RtFreedRange& entry = g_rtFreedRanges[slot];

        if(entry.address == 0u)
        {
            continue;
        }

        RTLogFlushed(
            "[GPU HANG] freed[-" + std::to_string(back) + "] addr=0x" +
            std::to_string(entry.address) +
            " bytes=" + std::to_string(entry.bytes) +
            " tag=" + std::string(entry.tag != nullptr ? entry.tag : "?") +
            " frame=" + std::to_string(entry.frame)
        );
    }

    rtDumpThreadStack(
        g_rtRenderThreadId.load(std::memory_order_relaxed));

    RTLogFlushed(
        "[GPU HANG] framePhase=" + std::to_string(
            g_rtFramePhase.load(std::memory_order_relaxed)) +
        " (0=outside 1=enter 2=preRebuild 3=postRebuild 4=postTlas 5=postSubmit)");

    rtLogVliStateForHang();

    rtLogHangMarkerForHang();

    // 2026-11-14：直接查 TLAS 里的实例是否还指向「已释放」的显存区间 ——
    // 这就是 AS / buffer_reference 悬垂的实证（实例表是 host-visible 的）。
    // 扫描本体在 rtScanTlasDanglingSeh 里带 SEH 兜底（映射可能已死）。
    if(g_tlasInstancesAddress != 0u && g_tlasInstanceCount > 0u)
    {
        // 2026-11-14：上限保护 —— 这里读的是 host-visible 实例表，指针可能
        // 已被 park/释放，读到未映射内存会让看门狗线程自己挂掉，导致后面的
        // 阶段/VLI 信息一行都打不出来（实测就是这样丢的）。
        const size_t tlasScanCount = std::min<size_t>(
            g_tlasInstanceCount,
            8192u);

        uint64_t dangling = 0u;
        uint32_t pairCount = 0u;
        RtDanglingPairRecord pairs[4];

        const bool scanOk = rtScanTlasDanglingSeh(
            reinterpret_cast<const void*>(g_tlasInstancesAddress),
            tlasScanCount,
            g_rtFreedRanges,
            RT_FREED_RANGE_SLOTS,
            pairs,
            4u,
            &dangling,
            &pairCount
        );

        if(scanOk)
        {
            for(uint32_t p = 0u; p < pairCount; ++p)
            {
                const RtDanglingPairRecord& record = pairs[p];

                RTLogFlushed(
                    "[GPU HANG] DANGLING instance=" + std::to_string(record.index) +
                    " ref=0x" + std::to_string(record.ref) +
                    " freedAddr=0x" + std::to_string(record.freedAddr) +
                    " bytes=" + std::to_string(record.freedBytes) +
                    " tag=" + std::string(record.freedTag != nullptr ? record.freedTag : "?") +
                    " freedFrame=" + std::to_string(record.freedFrame));
            }

            RTLogFlushed(
                "[GPU HANG] instances=" + std::to_string(g_tlasInstanceCount) +
                " danglingRefs=" + std::to_string(dangling));
        }
        else
        {
            RTLogFlushed(
                "[GPU HANG] instances=" + std::to_string(g_tlasInstanceCount) +
                " danglingRefs=unknown (scan faulted: mapping dead)");
        }
    }

    logDeviceFaultDiagnostics("hangWatchdog");
}

void rtHangHeartbeatTick()
{
    g_rtFrameHeartbeat.fetch_add(1u, std::memory_order_relaxed);

    // 记住录制线程（卡死时回搠它的调用栈）。
    g_rtRenderThreadId.store(
        static_cast<uint32_t>(GetCurrentThreadId()),
        std::memory_order_relaxed);

    bool expected = false;

    if(!g_rtHangWatchdogStarted.compare_exchange_strong(
           expected, true, std::memory_order_acq_rel))
    {
        return;
    }

    std::thread([]()
    {
        uint64_t lastBeat = g_rtFrameHeartbeat.load(std::memory_order_relaxed);
        std::chrono::steady_clock::time_point lastProgress =
            std::chrono::steady_clock::now();

        for(;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));

            const uint64_t beat =
                g_rtFrameHeartbeat.load(std::memory_order_relaxed);

            if(beat != lastBeat)
            {
                lastBeat = beat;
                lastProgress = std::chrono::steady_clock::now();
                continue;
            }

            const double stalledMs =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - lastProgress).count();

            if(stalledMs < 3000.0)
            {
                continue;
            }

            if(!g_rtHangReported.exchange(true))
            {
                RTLogFlushed(
                    "[GPU HANG] watchdog stalledMs=" + std::to_string(stalledMs)
                );

                rtDumpHangState();
            }
        }
    }).detach();
}


void logDeviceFaultDiagnostics(const char* where)
{
    // 2026-11-14：全线 RTLogFlushed。本函数只在设备已死/挂死路径上跑，
    // 进程随后必崩 —— RTLog 的 ofstream 缓冲会被一起带走（实测 21:00 局
    // VK_EXT_device_fault 已在 mixin 启用，却因没 flush 一行都没落到盘），
    // 而 NVIDIA 的 fault 报告（出错虚拟地址）正是 TDR 归因的关键证据。
    if(g_device == VK_NULL_HANDLE)
    {
        return;
    }

    std::string line = "[GPU FAULT] ";
    line += (where != nullptr) ? where : "?";

    auto getFaultInfo = reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(
        vkGetDeviceProcAddr(g_device, "vkGetDeviceFaultInfoEXT"));

    if(getFaultInfo == nullptr)
    {
        RTLogFlushed(line + " : VK_EXT_device_fault not enabled");
    }
    else
    {
        VkDeviceFaultCountsEXT counts{};
        counts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;

        const VkResult countsResult = getFaultInfo(g_device, &counts, nullptr);

        if(countsResult != VK_SUCCESS)
        {
            RTLogFlushed(line + " : vkGetDeviceFaultInfoEXT(counts) returned " +
                std::to_string(static_cast<int>(countsResult)));
        }
        else
        {
            std::vector<VkDeviceFaultAddressInfoEXT> addressInfos(counts.addressInfoCount);
            std::vector<VkDeviceFaultVendorInfoEXT> vendorInfos(counts.vendorInfoCount);
            std::vector<char> vendorBinary(static_cast<size_t>(counts.vendorBinarySize));

            VkDeviceFaultInfoEXT info{};
            info.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
            info.pAddressInfos = addressInfos.empty() ? nullptr : addressInfos.data();
            info.pVendorInfos = vendorInfos.empty() ? nullptr : vendorInfos.data();
            info.pVendorBinaryData = vendorBinary.empty() ? nullptr : vendorBinary.data();

            const VkResult infoResult = getFaultInfo(g_device, nullptr, &info);

            if(infoResult != VK_SUCCESS)
            {
                RTLogFlushed(line + " : vkGetDeviceFaultInfoEXT(info) returned " +
                    std::to_string(static_cast<int>(infoResult)));
            }
            else
            {
                std::stringstream fault;
                fault << line
                      << " : description=\"" << info.description << "\""
                      << " addressInfos=" << counts.addressInfoCount
                      << " vendorInfos=" << counts.vendorInfoCount
                      << " vendorBinaryBytes=" << counts.vendorBinarySize;

                for(uint32_t i = 0u; i < counts.addressInfoCount; ++i)
                {
                    fault << " | addr[" << i << "]"
                          << " type=" << static_cast<int>(addressInfos[i].addressType)
                          << " reported=0x" << std::hex << addressInfos[i].reportedAddress
                          << " precision=" << addressInfos[i].addressPrecision
                          << std::dec;
                }

                for(uint32_t i = 0u; i < counts.vendorInfoCount; ++i)
                {
                    fault << " | vendor[" << i << "]=" << vendorInfos[i].description
                          << " code=0x" << std::hex << vendorInfos[i].vendorFaultCode
                          << std::dec;
                }

                RTLogFlushed(fault.str());

                // 2026-11-14：fault 地址对照 freed 环 —— 命中即「GPU 访问的
                // 正是我们刚释放的 section 显存」的实证，TDR 归因到此为止。
                for(uint32_t i = 0u; i < counts.addressInfoCount; ++i)
                {
                    const uint64_t reported = addressInfos[i].reportedAddress;

                    if(reported == 0u)
                    {
                        continue;
                    }

                    for(uint32_t s = 0u; s < RT_FREED_RANGE_SLOTS; ++s)
                    {
                        const RtFreedRange& range = g_rtFreedRanges[s];

                        if(range.address == 0u || range.bytes == 0u)
                        {
                            continue;
                        }

                        if(reported >= range.address &&
                           reported < range.address + range.bytes)
                        {
                            RTLogFlushed(
                                "[GPU FAULT] addr[" + std::to_string(i) +
                                "] 0x" + std::to_string(reported) +
                                " falls INSIDE freed tag=" +
                                std::string(range.tag != nullptr ? range.tag : "?") +
                                " freedAddr=0x" + std::to_string(range.address) +
                                " bytes=" + std::to_string(range.bytes) +
                                " freedFrame=" + std::to_string(range.frame));
                            break;
                        }
                    }
                }

                if(counts.vendorBinarySize > 0u)
                {
                    RTLogFlushed(line + " vendorBinary head=" +
                        std::to_string(vendorBinary[0]));
                }
            }
        }
    }

    auto getCheckpointData = reinterpret_cast<PFN_vkGetQueueCheckpointData2NV>(
        vkGetDeviceProcAddr(g_device, "vkGetQueueCheckpointData2NV"));

    if(getCheckpointData == nullptr || g_queue == VK_NULL_HANDLE)
    {
        return;
    }

    uint32_t checkpointCount = 0u;
    getCheckpointData(g_queue, &checkpointCount, nullptr);

    if(checkpointCount == 0u)
    {
        RTLogFlushed(line + " : queue has no checkpoint (vkCmdSetCheckpointNV never hit)");
        return;
    }

    std::vector<VkCheckpointData2NV> checkpoints(checkpointCount);

    for(uint32_t i = 0u; i < checkpointCount; ++i)
    {
        checkpoints[i].sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_2_NV;
        checkpoints[i].pNext = nullptr;
    }

    getCheckpointData(g_queue, &checkpointCount, checkpoints.data());

    std::stringstream cp;
    cp << line << " : checkpoints=" << checkpointCount;

    for(uint32_t i = 0u; i < checkpointCount; ++i)
    {
        cp << " | cp[" << i << "] stage=0x" << std::hex << checkpoints[i].stage
           << " marker=" << std::dec;

        if(checkpoints[i].pCheckpointMarker != nullptr)
        {
            cp << static_cast<const char*>(checkpoints[i].pCheckpointMarker);
        }
        else
        {
            cp << "(null)";
        }
    }

    RTLogFlushed(cp.str());
}

// 2026-11-14：扩展在 mixin 里已启用，但 vkCmdSetCheckpointNV 从未调用过 ⇒
// 挂死取证时 "queue has no checkpoint"。埋字符串标记后，设备死时
// logDeviceFaultDiagnostics 能打回 GPU 最后走到的标记，把 TDR 钉到具体 pass。
PFN_vkCmdSetCheckpointNV rtGetCmdSetCheckpoint()
{
    static PFN_vkCmdSetCheckpointNV fn = nullptr;
    static bool fetched = false;

    if(!fetched)
    {
        fetched = true;

        if(g_device != VK_NULL_HANDLE)
        {
            fn = reinterpret_cast<PFN_vkCmdSetCheckpointNV>(
                vkGetDeviceProcAddr(g_device, "vkCmdSetCheckpointNV"));
        }
    }

    return fn;
}

void rtCheckpoint(VkCommandBuffer cmd, const char* tag)
{
    const uint32_t tagIndex = rtHangTagIndex(tag);

    g_rtHangLastRecordedTag.store(tagIndex, std::memory_order_relaxed);
    g_rtHangLastRecordedFrame.store(g_perfL10FrameSerial, std::memory_order_relaxed);

    PFN_vkCmdSetCheckpointNV fn = rtGetCmdSetCheckpoint();

    if(fn != nullptr)
    {
        fn(cmd, tag);
    }

    if(cmd == VK_NULL_HANDLE || !rtHangMarkersEnabled())
    {
        return;
    }

    rtHangMarkerEnsure();

    if(g_rtHangWriteMarker == nullptr || g_rtHangMarkerBuffer == VK_NULL_HANDLE)
    {
        return;
    }

    // 每个 tag 独占一个槽，值 = frameSerial+1（0 = 从未写过）。
    // 这样即便驱动对 marker 写入时机做了重排，也不会出现"跨 tag 互相覆盖"
    // —— 单槽版本正是被这件事坑过一次（cloud:shadow / cloud:march 打架）。
    g_rtHangWriteMarker(
        cmd,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        g_rtHangMarkerBuffer,
        static_cast<VkDeviceSize>(tagIndex) * 8u,
        g_perfL10FrameSerial + 1ull);
}

namespace
{
    VkDevice s_debugLabelDevice = VK_NULL_HANDLE;
    PFN_vkCmdBeginDebugUtilsLabelEXT s_cmdBeginDebugLabel = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT s_cmdEndDebugLabel = nullptr;

    void ensureDebugLabelFunctions()
    {
        if(g_device == VK_NULL_HANDLE)
        {
            return;
        }
        if(s_debugLabelDevice != g_device)
        {
            s_debugLabelDevice = g_device;
            s_cmdBeginDebugLabel = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
                vkGetDeviceProcAddr(g_device, "vkCmdBeginDebugUtilsLabelEXT"));
            s_cmdEndDebugLabel = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
                vkGetDeviceProcAddr(g_device, "vkCmdEndDebugUtilsLabelEXT"));
        }
    }
}

void vkDebugLabelBegin(VkCommandBuffer cmd, const char* name, float r, float g, float b)
{
    if(cmd == VK_NULL_HANDLE || name == nullptr)
    {
        return;
    }
    ensureDebugLabelFunctions();
    if(s_cmdBeginDebugLabel == nullptr)
    {
        return;
    }
    VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    label.pLabelName = name;
    label.color[0] = r;
    label.color[1] = g;
    label.color[2] = b;
    label.color[3] = 1.0f;
    s_cmdBeginDebugLabel(cmd, &label);
}

void vkDebugLabelEnd(VkCommandBuffer cmd)
{
    if(cmd == VK_NULL_HANDLE)
    {
        return;
    }
    ensureDebugLabelFunctions();
    if(s_cmdEndDebugLabel == nullptr)
    {
        return;
    }
    s_cmdEndDebugLabel(cmd);
}

// =====================================================
// 2026-10-16 泄漏归因探针（只读时钟+只读计数器，零行为变更）
//
// 现场：Vulkan 后端下进程提交量以约 38MB/s 持续增长（105 秒 +4.0GB）；
// 已用闸门排除 Streamline/NGX 整块（`ORION_SL_FEATURES=none` 照涨 38.3MB/s），
// 并证明只要跳过我们的设备级初始化（`orion.native.deviceInit=false`）就归零
// （−0.3MB/s）。本探针回答"这 38MB/s 具体花在哪一段时间里"：
//   · inside  = 在 beginStreamlineFrame **进入→退出**之间涨的 ⇒ 落在我们自己的每帧路径；
//   · outside = 在**退出→下一次进入**之间涨的 ⇒ 不在我们的调用里（驱动/present 线程/其它模块）；
// 每 60 帧一行，同时给墙钟秒数 ⇒ 按帧 还是 按秒 一眼可判。
// 口径与 [RT MEM] 的 privateMB 完全一致（GetProcessMemoryInfo().PagefileUsage）。
// =====================================================
uint64_t leakProbeCommitBytes()
{
    PROCESS_MEMORY_COUNTERS counters{};

    if(!GetProcessMemoryInfo(
           GetCurrentProcess(),
           &counters,
           sizeof(counters)))
    {
        return 0u;
    }

    return static_cast<uint64_t>(
        counters.PagefileUsage);
}

// 嵌套安全：只有最外层的进入/退出才记账，这样把多个每帧 JNI 入口逐个包上也不会重复计数。
// 2026-10-17：每帧探针**默认关闭**（ORION_LEAK_PROBE=1 才开）。
// 之前部署版里它常开：3 个每帧 JNI 入口 × 进/出各一次 GetProcessMemoryInfo，
// 180fps 下约 1000 次/秒；内存吃紧时这个调用本身会变慢，是"一卡一卡"的可疑来源之一。
// 它只是测量工具，不该出现在给人玩的构建里。
bool leakProbeEnabled()
{
    static const bool enabled = []() -> bool
    {
        const char* raw =
            std::getenv("ORION_LEAK_PROBE");

        return raw != nullptr
               && (std::strcmp(raw, "1") == 0 || std::strcmp(raw, "true") == 0);
    }();

    return enabled;
}
void leakProbeFrameBegin()
{
    if(!leakProbeEnabled())
    {
        return;
    }

    if(g_leakProbeDepth++ != 0)
    {
        return;
    }

    const uint64_t now = leakProbeCommitBytes();

    if(g_leakProbeLastExitBytes != 0u)
    {
        g_leakProbeOutsideBytes +=
            static_cast<int64_t>(now) -
            static_cast<int64_t>(g_leakProbeLastExitBytes);
    }

    if(g_leakProbeFrameCount == 0u)
    {
        g_leakProbeWindowStart =
            std::chrono::steady_clock::now();
    }

    g_leakProbeEnterBytes = now;
}
void leakProbeFrameEnd()
{
    if(!leakProbeEnabled())
    {
        return;
    }

    if(--g_leakProbeDepth != 0)
    {
        return;
    }

    const uint64_t now = leakProbeCommitBytes();

    g_leakProbeInsideBytes +=
        static_cast<int64_t>(now) -
        static_cast<int64_t>(g_leakProbeEnterBytes);

    g_leakProbeLastExitBytes = now;

    ++g_leakProbeFrameCount;

    if(g_leakProbeFrameCount < 60u)
    {
        return;
    }

    const double seconds =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() -
                g_leakProbeWindowStart
        ).count();

    const double insideMb =
        static_cast<double>(g_leakProbeInsideBytes) / 1048576.0;

    const double outsideMb =
        static_cast<double>(g_leakProbeOutsideBytes) / 1048576.0;

    fprintf(
        stderr,
        "[LEAK PROBE] frames=%u sec=%.2f insideMB=%.1f outsideMB=%.1f "
        "insideMBps=%.2f outsideMBps=%.2f\n",
        g_leakProbeFrameCount,
        seconds,
        insideMb,
        outsideMb,
        seconds > 0.0 ? insideMb / seconds : 0.0,
        seconds > 0.0 ? outsideMb / seconds : 0.0
    );

    fflush(stderr);

    g_leakProbeInsideBytes = 0;
    g_leakProbeOutsideBytes = 0;
    g_leakProbeFrameCount = 0u;
}

// =====================================================
// 2026-10-16 诊断闸门 ORION_DEVICE_INIT_STOP=N（默认 0 = 不停 = 现状）
//
// 用途：设备级初始化（initDeviceHandlesNative）里逐里程碑二分，找出哪一步打开
// 了"宿主提交量以 ~36MB/s 按秒增长 + 一个 nvoglv64.dll 线程占满一个核"。
// 现状证据：跳过整个设备级初始化 ⇒ −0.3MB/s；零 Streamline feature 也照涨；
// 流失不在任何每帧 JNI 里（[LEAK PROBE] inside≈0）；按秒不按帧。
// 每个里程碑打一行 [DEV STEP]，达到 N 就 return（后面的初始化不做，只用于测量）。
// =====================================================
uint32_t deviceInitStopStep()
{
    static const uint32_t stopStep = []() -> uint32_t
    {
        const char* raw =
            std::getenv("ORION_DEVICE_INIT_STOP");

        return raw == nullptr
                   ? 0u
                   : static_cast<uint32_t>(std::strtoul(raw, nullptr, 10));
    }();

    return stopStep;
}

// 2026-10-31：渲染线程提优先级，默认关闭。
// 理由：C2ME 在把 worker 压到 THREAD_PRIORITY_LOWEST 的同时，把渲染主线程提到
// maximum-performance。这里默认只做"worker 降级"这一半（无副作用），主线程提级
// 会影响 Reflex / Streamline 自己的线程观感，需要实测 ⇒ 留在
// ORION_RENDER_THREAD_PRIORITY=1 后面。
void applyRenderThreadPriorityOnce()
{
    static bool applied = false;

    if(applied)
    {
        return;
    }

    applied = true;

    const char* raw = std::getenv("ORION_RENDER_THREAD_PRIORITY");

    if(raw == nullptr || raw[0] == '\0' || raw[0] == '0')
    {
        return;
    }

    const BOOL ok =
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    RTLog(
        std::string("[RT THREAD] render thread priority=HIGHEST ok=") +
        (ok ? "1" : "0"));
}

// =====================================================
// 2026-10-09：[RT MEM] 内存记账（诊断行）。
//
// 为什么需要它：用户报"玩久了会直接占满内存"。此前本文件**没有任何**内存口径
// （`[ALLOC FAIL]` 从未出现过 ⇒ 显存分配没失败过 ⇒ 不是显存爆），所以既不能说
// 是哪个结构在涨，也不能说是涨在原生侧还是 Java 堆 / 驱动里。
//
// 判读（这一行给的就是这三种切分）：
//   · rssMB 随时间单调上升，而下列各项之和基本不变
//       ⇒ 涨在**本行没记到的地方**（Java 堆、Vulkan 驱动、或本文件的漏记结构）；
//   · 某一项与 rssMB 同步单调上升（最可能是 meshMB / rawMB / queued）
//       ⇒ 就是它，按持有者去查删除路径；
//   · 全部平稳、只有 sysAvailMB 在掉
//       ⇒ 是别的进程/别的消费者，不是本模组。
//
// 语义说明：
//   · meshMB = g_chunkMeshes 的顶点字节（CPU 侧权威镜像）
//   · rawMB  = g_chunkRawPayloads 的**原始**字节：每层再存一份未打包副本，用于
//              上传前 memcmp 去重 ⇒ 与 meshMB 同量级是**设计如此**，不是泄漏。
//   · 这些量都是世界规模的函数，探索得越远越大属正常；**要看的是它是否收敛**。
//   · 每 600 帧（≈10s）一行，与 [RT GEOMETRY] counters 同频，方便两条时间线对齐。
// =====================================================
void logNativeMemoryReport()
{
    PROCESS_MEMORY_COUNTERS memCounters{};
    const bool haveProcessMemory =
        GetProcessMemoryInfo(
            GetCurrentProcess(),
            &memCounters,
            sizeof(memCounters)
        ) != FALSE;

    MEMORYSTATUSEX systemMemory{};
    systemMemory.dwLength = sizeof(systemMemory);
    const bool haveSystemMemory =
        GlobalMemoryStatusEx(&systemMemory) != FALSE;

    VramBudgetSample vramSample;
    queryVramBudget(vramSample);

    const double vramUsedMb = vramSample.usedMb;
    const double vramBudgetMb = vramSample.budgetMb;

    size_t meshSectionCount = 0u;
    size_t meshBytes = 0u;
    size_t rawBytes = 0u;
    size_t digestCount = 0u;
    size_t ommInputCount = 0u;
    size_t ommBytes = 0u;
    size_t dirtyKeyCount = 0u;

    {
        std::lock_guard<std::timed_mutex> lock(g_chunkMutex);

        meshSectionCount = g_chunkMeshes.size();

        for(const auto& entry : g_chunkMeshes)
        {
            meshBytes += entry.second.size() * sizeof(Vertex);
        }

        for(const auto& entry : g_chunkRawPayloads)
        {
            rawBytes += entry.second.size();
        }

        digestCount = g_chunkUploadDigests.size();

        ommInputCount = g_cutoutOmmInputs.size();

        for(const auto& entry : g_cutoutOmmInputs)
        {
            ommBytes += entry.second.data.size();
        }

        dirtyKeyCount = g_dirtySectionKeys.size();
    }

    size_t lightSectionCount = 0u;
    size_t lightCount = 0u;
    size_t lightBytes = 0u;

    {
        std::lock_guard<std::mutex> lock(g_lightMutex);

        lightSectionCount = g_lightSections.size();

        for(const auto& entry : g_lightSections)
        {
            lightCount += entry.second.size();
            lightBytes += entry.second.size() * sizeof(LocalLightRecord);
        }
    }

    size_t capturedCount = 0u;

    {
        std::lock_guard<std::mutex> lock(g_capturedSectionsMutex);
        capturedCount =
            g_capturedSections.size() - g_capturedSectionsHead;
    }

    const size_t blasCount = g_sectionBLAS.size();
    const size_t voxySourceCount = g_voxyLodSources.size();

    const auto toMb =
        [](size_t bytes)
        {
            return static_cast<double>(bytes) / 1048576.0;
        };

    const RtxMemAccountSnapshot vkMem = rtxMemAccountSnapshot();

    VkDeviceSize asBlasBytes = 0u;
    VkDeviceSize asTrueBytes = 0u;
    VkDeviceSize asChunkBytes = 0u;

    for(const auto& asEntry : g_sectionBLAS)
    {
        const RTSectionAS& asSection = asEntry.second;

        asBlasBytes += asSection.blasBytes;
        asTrueBytes += asSection.vertexBytes + asSection.blasBytes;
        asChunkBytes += asSection.sectionMemoryChunkBytes;
    }

    size_t vtxStageCount = 0u;
    VkDeviceSize vtxStageBytes = 0u;
    size_t vtxStageInFlight = 0u;
    g_vertexStagingPool.stats(vtxStageCount, vtxStageBytes, vtxStageInFlight);

    size_t blasScratchCount = 0u;
    VkDeviceSize blasScratchBytes = 0u;
    size_t blasScratchInFlight = 0u;
    g_blasScratchPool.stats(
        blasScratchCount,
        blasScratchBytes,
        blasScratchInFlight
    );

    const std::string vkChurn = rtxBufferChurnTopReport(12u);

    std::ostringstream report;
    report << std::fixed << std::setprecision(1)
        << "[RT MEM]"
        << " rssMB="
        << (haveProcessMemory ? toMb(memCounters.WorkingSetSize) : -1.0)
        << " peakRssMB="
        << (haveProcessMemory ? toMb(memCounters.PeakWorkingSetSize) : -1.0)
        << " privateMB="
        << (haveProcessMemory ? toMb(memCounters.PagefileUsage) : -1.0)
        << " sysAvailMB="
        << (haveSystemMemory
                ? static_cast<double>(systemMemory.ullAvailPhys) / 1048576.0
                : -1.0)
        << " sysTotalMB="
        << (haveSystemMemory
                ? static_cast<double>(systemMemory.ullTotalPhys) / 1048576.0
                : -1.0)
        << " vramUsedMB=" << vramUsedMb
        << " vramBudgetMB=" << vramBudgetMb
        << " slDlssMB=" << toMb(static_cast<size_t>(g_slVramDlssBytes.load()))
        << " slRrdMB=" << toMb(static_cast<size_t>(g_slVramDlssdBytes.load()))
        << " slFgMB=" << toMb(static_cast<size_t>(g_slVramDlssgBytes.load()))
        << " slNgxMB=" << toMb(static_cast<size_t>(
               g_slVramDlssBytes.load() +
               g_slVramDlssdBytes.load() +
               g_slVramDlssgBytes.load()))
        << " | meshSec=" << meshSectionCount
        << " meshMB=" << toMb(meshBytes)
        << " rawMB=" << toMb(rawBytes)
        << " digests=" << digestCount
        << " omm=" << ommInputCount
        << " ommMB=" << toMb(ommBytes)
        << " dirty=" << dirtyKeyCount
        << " lightSec=" << lightSectionCount
        << " lights=" << lightCount
        << " lightMB=" << toMb(lightBytes)
        << " queued=" << capturedCount
        << " blas=" << blasCount
        << " asBlasMB=" << toMb(asBlasBytes)
        << " asTrueMB=" << toMb(asTrueBytes)
        << " asChunkMB=" << toMb(asChunkBytes)
        << " asWasteMB="
        << toMb(
               asChunkBytes > asTrueBytes
                   ? asChunkBytes - asTrueBytes
                   : 0u
           )
        << " voxy=" << voxySourceCount
        << " slabKeptMB=" << toMb(g_sectionMemoryPoolKeptBytes)
        << " slabAllocMB=" << toMb(g_sectionMemoryAllocateBytes)
        << " slabFreeMB=" << toMb(g_sectionMemoryFreeBytes)
        << " vtxStage=" << vtxStageCount << "/" << toMb(vtxStageBytes)
        << "MB(inflight=" << vtxStageInFlight << ")"
        << " blasScratch=" << blasScratchCount << "/" << toMb(blasScratchBytes)
        << "MB(inflight=" << blasScratchInFlight << ")"
        << " | vkAllocMB=" << toMb(vkMem.allocBytes)
        << " vkFreeMB=" << toMb(vkMem.freeBytes)
        << " vkLiveMB=" << toMb(vkMem.liveBytes)
        << " vkLiveDeviceMB=" << toMb(vkMem.liveDeviceLocalBytes)
        << " vkLiveHostMB=" << toMb(vkMem.liveHostBytes)
        << " vkLivePeakMB=" << toMb(vkMem.livePeakBytes)
        << " vkLiveN=" << vkMem.liveCount
        << " vkAllocN=" << vkMem.allocCount
        << " vkFreeN=" << vkMem.freeCount
        << " vkFreeUnknownN=" << vkMem.freeUnknownCount
        << " | obj: descPool="
        << g_rtxMemObj.descPool.load(std::memory_order_relaxed)
        << " descSet="
        << g_rtxMemObj.descSet.load(std::memory_order_relaxed)
        << " cmdPool="
        << g_rtxMemObj.cmdPool.load(std::memory_order_relaxed)
        << " cmdBuf="
        << g_rtxMemObj.cmdBuf.load(std::memory_order_relaxed)
        << " layout="
        << g_rtxMemObj.layout.load(std::memory_order_relaxed)
        << " shader="
        << g_rtxMemObj.shader.load(std::memory_order_relaxed)
        << " pipe="
        << g_rtxMemObj.pipe.load(std::memory_order_relaxed)
        << " view="
        << g_rtxMemObj.view.load(std::memory_order_relaxed)
        << " sampler="
        << g_rtxMemObj.sampler.load(std::memory_order_relaxed)
        << " cmdBufFree="
        << g_rtxMemObj.cmdBufFree.load(std::memory_order_relaxed)
        << " fence="
        << g_rtxMemObj.fence.load(std::memory_order_relaxed)
        << " semaphore="
        << g_rtxMemObj.semaphore.load(std::memory_order_relaxed)
        << " queryPool="
        << g_rtxMemObj.queryPool.load(std::memory_order_relaxed)
        << " asCreate="
        << g_rtxMemObj.asCreate.load(std::memory_order_relaxed)
        << " asDestroy="
        << g_rtxMemObj.asDestroy.load(std::memory_order_relaxed)
        << " asBuildCmd="
        << g_rtxMemObj.asBuildCmd.load(std::memory_order_relaxed)
        << " mmCreate="
        << g_rtxMemObj.mmCreate.load(std::memory_order_relaxed)
        << " mmDestroy="
        << g_rtxMemObj.mmDestroy.load(std::memory_order_relaxed)
        << " | churnTop:" << vkChurn;

    RTLog(report.str());

    rtLogSectionBucketStats();
}
