#include "OrionInternal.h"

#include <wchar.h>

// ============================================================================
// DisplayHdr.cpp —— Windows「高级颜色 / 系统 HDR」状态查询（2026-11-23）
//
// 动机
// ----
// 既有的 HDR 判定只有两条输入：
//   1) 用户在 mod 设置里勾了「HDR」                    → g_hdrRequested
//   2) 驱动的 surface format 列表里有 HDR10_ST2084      → g_hdrSurfaceSupported
// 两者满足就把 swapchain 强制成 A2B10G10R10 + HDR10_ST2084，并在显示管线里
// 输出 PQ（rr_hdr_encode.comp）+ BT.2020 静态元数据。
//
// 但「驱动列出了 ST2084」≠「Windows 现在会用 HDR 合成这层 swapchain」。
// 实测（run/*.err）里 ST2084 一直是 SUPPORTED=1，若此时 Windows 的
// 「使用 HDR」是关的，系统只能把 HDR10/PQ 内容按 SDR 桌面处理 ⇒ 画面发灰、
// 发白、饱和度下降。也就是用户说的「只有电脑同时开 HDR 才正常」。
//
// 本文件用 DisplayConfig API 读显示器的真实 advanced color 状态：
//   GetDisplayConfigBufferSizes + QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS)
//   → 由 HWND 拿到所在显示器（GetMonitorInfoW 的 szDevice）
//   → 匹配 path 的 DISPLAYCONFIG_SOURCE_DEVICE_NAME
//   → DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO（Win10 1803+）
// 结果参与 hdrOutputActive() 的门控：系统 HDR 没开 ⇒ 回退 SDR 显示管线，
// 画面保持正常（只是没有 HDR 效果），而不是输出一个没人能正确解释的 PQ 信号。
//
// 语义约定（重要）
// ----------------
//   * 查询失败 / API 不可用 / 窗口显示器匹配不到 ⇒ 视为「未知」，**不门控**，
//     完全保持改动前的行为。宁可少管，不要因为一次 API 失败把用户的 HDR 关掉。
//   * 只认「显示器报告 advancedColorSupported=1 且 advancedColorEnabled=1」为
//     可输出 HDR。supported=0（显示器不支持）同样回退 SDR。
//   * 环境变量 ORION_HDR_SYSTEM_GATE=0 可整体关闭门控（A/B 与排障用）。
//
// 刷新时机
// --------
//   1) slCreateWin32Surface 成功后（拿到真实 HWND，日志能尽早出现）；
//   2) 每次 slCreateSwapchain 覆盖格式之前（切 HDR 开关、窗口重排、系统切换
//      显示模式都会走到 swapchain 重建）。
//   这两处都在渲染线程/创建路径上，QueryDisplayConfig 是普通 Win32 调用，无
//   Vulkan 状态依赖。
// ============================================================================

#if defined(_WIN32)

// 老 SDK 兼容：GET_ADVANCED_COLOR_INFO 从 Windows 10 1803 SDK 起提供。这里按
// wingdi.h 的原样布局补一份，保证 sizeof 与驱动期望的 32 字节一致。
#if !defined(DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO)
#define DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO 9

struct OrionDisplayConfigAdvancedColorInfo
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;

    union
    {
        struct
        {
            UINT32 advancedColorSupported : 1;
            UINT32 advancedColorEnabled : 1;
            UINT32 wideColorEnforced : 1;
            UINT32 advancedColorForceDisabled : 1;
            UINT32 reserved : 28;
        };

        UINT32 value;
    };

    UINT32 colorEncoding;
    UINT32 bitsPerColorChannel;
};
#else
using OrionDisplayConfigAdvancedColorInfo =
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO;
#endif

// GET_ADVANCED_COLOR_INFO_2（Win11 22H2+）才区分 SDR / WCG / HDR 三种模式。
// v1 的 advancedColorEnabled 在 Win11 上对「自动管理颜色（ACM，WCG）」同样为 1，
// 只看 v1 会把 WCG 误判成 HDR ⇒ 门控失效（照样输出 PQ、画面依旧发灰）。
// 因此 v2 可用时优先用 v2，失败（Win10 返回 ERROR_INVALID_PARAMETER）再回退 v1。
#if defined(NTDDI_WIN11_GA) && defined(NTDDI_VERSION) && \
    (NTDDI_VERSION >= NTDDI_WIN11_GA)
#define ORION_HAS_ADVANCED_COLOR_INFO_2 1
#pragma message("[HDR-SYS] DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 available")
#else
#define ORION_HAS_ADVANCED_COLOR_INFO_2 0
#endif


std::atomic<bool> g_winHdrQueryValid{false};
std::atomic<bool> g_winHdrSupported{false};
std::atomic<bool> g_winHdrEnabled{false};
std::atomic<void*> g_gameHwnd{nullptr};


namespace
{

struct OrionHdrQueryResult
{
    bool valid = false;
    bool supported = false;
    bool enabled = false;
    bool userEnabled = false;
    bool wideColorEnforced = false;
    bool forceDisabled = false;
    // DISPLAYCONFIG_ADVANCED_COLOR_MODE；-1 = 走的是 v1，无法区分 WCG/HDR。
    int activeColorMode = -1;
    int infoVersion = 1;
    uint32_t bitsPerColorChannel = 0;
    wchar_t device[CCHDEVICENAME] = L"(unknown)";
};


bool windowsHdrSystemGateEnabled()
{
    // 静态局部初始化线程安全；进程内只读一次环境变量。
    static const bool enabled = []
    {
        const char* raw = std::getenv("ORION_HDR_SYSTEM_GATE");

        return !(raw != nullptr && raw[0] == '0');
    }();

    return enabled;
}


HMONITOR orionPrimaryMonitor()
{
    struct Context
    {
        HMONITOR primary = nullptr;
    } context;

    EnumDisplayMonitors(
        nullptr,
        nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM userData) -> BOOL
        {
            auto* ctx =
                reinterpret_cast<Context*>(userData);

            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);

            if(!GetMonitorInfoW(monitor, &info))
            {
                return TRUE;
            }

            if((info.dwFlags & MONITORINFOF_PRIMARY) != 0)
            {
                ctx->primary = monitor;
                return FALSE;
            }

            if(ctx->primary == nullptr)
            {
                ctx->primary = monitor;
            }

            return TRUE;
        },
        reinterpret_cast<LPARAM>(&context));

    return context.primary;
}


OrionHdrQueryResult queryWindowsAdvancedColor(void* hwndRaw)
{
    OrionHdrQueryResult result;

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;

    if(GetDisplayConfigBufferSizes(
           QDC_ONLY_ACTIVE_PATHS,
           &pathCount,
           &modeCount) != ERROR_SUCCESS ||
       pathCount == 0)
    {
        return result;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(
        modeCount != 0 ? modeCount : 1u);

    if(QueryDisplayConfig(
           QDC_ONLY_ACTIVE_PATHS,
           &pathCount,
           paths.data(),
           &modeCount,
           modes.data(),
           nullptr) != ERROR_SUCCESS ||
       pathCount == 0)
    {
        return result;
    }

    paths.resize(pathCount);


    // 目标显示器：游戏窗口所在的那一台；没有窗口信息时退回主显示器。
    HWND hwnd =
        reinterpret_cast<HWND>(hwndRaw);

    HMONITOR monitor =
        hwnd != nullptr
            ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY)
            : orionPrimaryMonitor();

    wchar_t wantedDevice[CCHDEVICENAME] = L"";

    if(monitor != nullptr)
    {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);

        if(GetMonitorInfoW(monitor, &info))
        {
            wcscpy_s(wantedDevice, info.szDevice);
        }
    }


    int chosenPath = -1;
    wchar_t chosenSource[CCHDEVICENAME] = L"";

    for(UINT32 i = 0; i < pathCount; ++i)
    {
        if((paths[i].flags & DISPLAYCONFIG_PATH_ACTIVE) == 0)
        {
            continue;
        }

        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
        sourceName.header.type =
            DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size =
            sizeof(sourceName);
        sourceName.header.adapterId =
            paths[i].sourceInfo.adapterId;
        sourceName.header.id =
            paths[i].sourceInfo.id;

        if(DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS)
        {
            continue;
        }

        if(wantedDevice[0] != L'\0' &&
           _wcsicmp(sourceName.viewGdiDeviceName, wantedDevice) == 0)
        {
            chosenPath = static_cast<int>(i);
            wcscpy_s(chosenSource, sourceName.viewGdiDeviceName);
            break;
        }

        if(wantedDevice[0] == L'\0' && chosenPath < 0)
        {
            chosenPath = static_cast<int>(i);
            wcscpy_s(chosenSource, sourceName.viewGdiDeviceName);
        }
    }

    // 窗口显示器存在、但在 active path 里找不到对应 source（克隆/远程桌面/
    // 路径刚重建）时，不猜：保持「未知」，继续沿用改动前的行为。
    if(chosenPath < 0)
    {
        return result;
    }


    const LUID targetAdapterId =
        paths[chosenPath].targetInfo.adapterId;
    const UINT32 targetId =
        paths[chosenPath].targetInfo.id;

#if ORION_HAS_ADVANCED_COLOR_INFO_2
    {
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 info{};
        info.header.type =
            DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
        info.header.size =
            sizeof(info);
        info.header.adapterId = targetAdapterId;
        info.header.id = targetId;

        if(DisplayConfigGetDeviceInfo(&info.header) == ERROR_SUCCESS)
        {
            result.valid = true;
            result.infoVersion = 2;
            result.supported =
                info.highDynamicRangeSupported != 0u;
            result.userEnabled =
                info.highDynamicRangeUserEnabled != 0u;

            // 只认「当前真的以 HDR 模式合成」。Win11 的自动管理颜色（ACM，
            // 模式 = WCG）会让 v1 的 advancedColorEnabled 也是 1，但桌面仍在
            // SDR/WCG 亮度域，输出 PQ 一样会发灰。
            result.enabled =
                info.activeColorMode ==
                DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR;

            result.wideColorEnforced =
                info.wideColorUserEnabled != 0u;
            result.forceDisabled =
                info.advancedColorLimitedByPolicy != 0u;
            result.activeColorMode =
                static_cast<int>(info.activeColorMode);
            result.bitsPerColorChannel =
                info.bitsPerColorChannel;

            wcscpy_s(result.device, chosenSource);

            return result;
        }
    }
#endif

    // v2 不可用（Win10）或调用失败：退回 v1 口径。Win10 没有 ACM，
    // advancedColorEnabled 本身就等价于「HDR 已开启」。
    OrionDisplayConfigAdvancedColorInfo colorInfo{};
    colorInfo.header.type =
        static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(
            DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO);
    colorInfo.header.size =
        sizeof(colorInfo);
    colorInfo.header.adapterId = targetAdapterId;
    colorInfo.header.id = targetId;

    if(DisplayConfigGetDeviceInfo(&colorInfo.header) != ERROR_SUCCESS)
    {
        return result;
    }

    result.valid = true;
    result.supported =
        colorInfo.advancedColorSupported != 0u;
    result.enabled =
        colorInfo.advancedColorEnabled != 0u;
    result.userEnabled =
        colorInfo.advancedColorEnabled != 0u;
    result.wideColorEnforced =
        colorInfo.wideColorEnforced != 0u;
    result.forceDisabled =
        colorInfo.advancedColorForceDisabled != 0u;
    result.bitsPerColorChannel =
        colorInfo.bitsPerColorChannel;

    wcscpy_s(result.device, chosenSource);

    return result;
}

} // namespace


bool windowsHdrAllowsHdrOutput()
{
    if(!windowsHdrSystemGateEnabled())
    {
        return true;
    }

    // 查询失败 ⇒ 不门控（保持旧行为）。
    if(!g_winHdrQueryValid.load())
    {
        return true;
    }

    return g_winHdrSupported.load() &&
           g_winHdrEnabled.load();
}


void refreshWindowsDisplayHdrState(void* hwnd)
{
    const OrionHdrQueryResult result =
        queryWindowsAdvancedColor(hwnd);

    g_winHdrQueryValid.store(result.valid);
    g_winHdrSupported.store(result.valid && result.supported);
    g_winHdrEnabled.store(result.valid && result.enabled);

    // 去重键一起包含 requested / surface 两个开关：surface 探测与 HDR 开关
    // 往往在 surface 创建之后才落值，只按显示器状态去重会把「真正生效的那次」
    // 吞掉，日志就会停在 requested=0 上。
    const int64_t state =
        (static_cast<int64_t>(result.valid ? 1 : 0) << 4) |
        (static_cast<int64_t>(result.supported ? 1 : 0) << 3) |
        (static_cast<int64_t>(result.enabled ? 1 : 0) << 2) |
        (static_cast<int64_t>(g_hdrRequested.load() ? 1 : 0) << 1) |
        static_cast<int64_t>(g_hdrSurfaceSupported.load() ? 1 : 0);

    // 窗口 resize 会反复重建 swapchain，状态没变就不重复刷日志。
    static std::atomic<int64_t> lastLoggedState{-1};

    if(lastLoggedState.exchange(state) == state)
    {
        return;
    }

    fprintf(
        stderr,
        "[HDR-SYS] query=%s info=v%d device=%ls supported=%d enabled=%d"
        " userEnabled=%d mode=%d wideEnforced=%d forceDisabled=%d bpc=%u gate=%d"
        " requested=%d surfaceHDR10=%d => hdrOutput=%d%s\n",
        result.valid ? "ok" : "failed",
        result.infoVersion,
        result.device,
        result.supported ? 1 : 0,
        result.enabled ? 1 : 0,
        result.userEnabled ? 1 : 0,
        result.activeColorMode,
        result.wideColorEnforced ? 1 : 0,
        result.forceDisabled ? 1 : 0,
        static_cast<unsigned>(result.bitsPerColorChannel),
        windowsHdrSystemGateEnabled() ? 1 : 0,
        g_hdrRequested.load() ? 1 : 0,
        g_hdrSurfaceSupported.load() ? 1 : 0,
        (g_hdrRequested.load() &&
         g_hdrSurfaceSupported.load() &&
         windowsHdrAllowsHdrOutput())
            ? 1
            : 0,
        (result.valid &&
         g_hdrRequested.load() &&
         g_hdrSurfaceSupported.load() &&
         !windowsHdrAllowsHdrOutput())
            ? " (blocked: Windows advanced color off)"
            : "");

    fflush(stderr);
}

#endif // _WIN32
