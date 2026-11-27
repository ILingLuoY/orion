package com.example.client;

import com.example.vulkan.NativeVulkan;

/**
 * 2026-11-11：接管式渲染第一层 —— **让原版地形光栅不进入命令缓冲**。
 *
 * <h2>为什么这么做</h2>
 * 现在整条链路是「旁挂式」：原版先光栅 766 个 section（CPU 十几毫秒 + 每帧数 MB 分配，
 * 还连带 GC），我们再追一条全屏 RT，最后把 main render target 的颜色整张盖掉。
 * 那份光栅的颜色是纯浪费；深度也不会被 RT 用到 —— RT 的深度是 rgen 自己写进
 * {@code g_dlssDepthImage}（{@code PostFx.cpp:createDLSSDepthImage}，R32_SFLOAT，
 * 以 STORAGE_IMAGE 绑在 ray tracing 描述符 set 的 binding 7），Java 侧从不把原版
 * 深度图传给 native（{@code recordFrameCommand} 只有 color）。
 *
 * <h2>掐在哪一层</h2>
 * 不拦 draw，而是拦**draw group 的构建**：
 * {@code LevelRenderer.extractSectionDrawGroups(boolean, List, Map)} 是 DrawIndirect
 * 与 DrawSeparate **两条路径的共同上游**，它填充每个 layer 的 {@code ChunkDrawGroup}
 * 列表并追加 {@code ChunkSectionInfo}。返回 0 后：
 * <ul>
 *   <li>{@code sectionInfos} 为空 ⇒ 两个路径的动态 UBO 都走 empty 分支
 *       （{@code writeDataBatched} 对空列表返回 {@code GpuBufferSlice(null,0,0)}，
 *       {@code writeData(T[])} 对空数组返回 {@code new GpuBufferSlice[0]}）；</li>
 *   <li>每个 layer 的 draw group 为空 ⇒ {@code renderGroup} 里
 *       {@code if (!drawGroups.isEmpty())} 与 {@code for (… : drawGroups)} 全跳过，
 *       一个 {@code drawIndexedIndirect} / {@code drawMultipleIndexed} 都不录；</li>
 *   <li>{@code largestIndexCount = 0} ⇒ 不申请 auto storage index buffer。</li>
 * </ul>
 * 于是地形的**遍历 + 分配 + 命令录制**三笔一起消失，且不依赖任何 renderpearl 私有 API
 * （那些 draw 级入口存在，但拦在这里更省）。
 *
 * <h2>为什么是安全的</h2>
 * 1. 只在 **RT 上一帧真的把画面盖进 main render target** 时才关光栅
 *    （{@link #active()} 要求上一帧注入成功），RT 掉线当帧地形自动回来；
 * 2. section 网格构建（{@code SectionCompiler} / {@code SectionMesh}）完全不动 ——
 *    RT 的几何捕获靠它；
 * 3. {@code visibleSections} 不动 —— RT 侧剔除表、{@code setVisibleSections} 照旧。
 *
 * <h2>开关</h2>
 * {@code ORION_TAKEOVER} = {@code 0} 全关 / {@code 1} 只关地形 / {@code 2}（默认）再关主 pass
 * 的原版要素光栅。设为 0 时逐帧行为与改造前完全一致。
 */
public final class TerrainTakeover {

    /**
     * 接管档位，{@code ORION_TAKEOVER}：
     * <ul>
     *   <li>{@code 0} —— 完全关闭，逐帧行为与改造前一致；</li>
     *   <li>{@code 1} —— 只关地形光栅（draw group 构建）；</li>
     *   <li>{@code 2} —— 再关主 pass 里的原版要素光栅（实体 / 粒子 / gizmo / OIT /
     *       描边 / see-through / water mask）。被关掉的这些本来就被 RT 全屏覆盖，
     *       视觉中性；手持物与屏幕特效走的是 {@code renderAllFeatures}，不在其中。</li>
     * </ul>
     *
     * <p><b>默认 0（关闭）</b>：2026-11-11 实测档位 1/2 都会让 {@code rebuildWorldGeometry}
     * 的 {@code pCap}（等区块锁）从 2 ms 一路涨到 65~200 ms，帧率掉到 2.5 fps。
     * 细节与实测数据见 {@code docs/handoff-20261111.md} §12.7。在搞清机理之前不要默认开。
     */
    private static final int LEVEL = parseLevel(System.getenv("ORION_TAKEOVER"));

    private static final boolean ENABLED = LEVEL > 0;

    /**
     * 地形接管的第一层有两种掐法，靠 {@code ORION_TAKEOVER_EXTRACT} 选：
     * <ul>
     *   <li><b>变体 B（默认，{@code =0}）</b> —— 保留 {@code extractSectionDrawGroups}
     *       的全部簿记（{@code sectionRenderDispatcher.lock()} 窗口、
     *       {@code getRenderSectionSlice}、动态 UBO 写入），也在
     *       {@code renderLayers} 里照旧绑 {@code TerrainUniform} / {@code Sampler0}
     *       （native 侧图集句柄的唯一上报点）/ 管线，只在
     *       {@code DrawIndirect.render} / {@code DrawSeparate.render} 里取消 draw。
     *       环境语义与 vanilla 逐位相同，只省 GPU 与命令录制。</li>
     *   <li><b>变体 A（{@code =1}）</b> —— 直接跳过 draw group 构建，连段遍历和逐段分配
     *       一起省掉。2026-11-11 实测会触发 {@code pCap} 劣化（见 handoff §12.7），
     *       留作对照用，不要默认开。</li>
     * </ul>
     */
    private static final boolean EXTRACT_SKIP =
            "1".equals(System.getenv("ORION_TAKEOVER_EXTRACT"));

    private static int parseLevel(final String raw) {
        if (raw == null || raw.isBlank()) {
            // Batch Y: default to Variant B (LEVEL=1, EXTRACT_SKIP=false) which preserves
            // vanilla CPU bookkeeping/uniforms/locks while cancelling wasted GPU terrain draws.
            return 1;
        }
        try {
            return Math.max(0, Math.min(2, Integer.parseInt(raw.trim())));
        } catch (final NumberFormatException ignored) {
            return 1;
        }
    }

    /** 上一帧 RT 是否真的把画面写进了 main render target（由 resetGuiRtState 结算）。 */
    private static volatile boolean lastFrameInjected;

    private static volatile boolean lastActive;

    private TerrainTakeover() {}

    /**
     * 每帧交换链 acquire 时（{@code NativeVulkan.resetGuiRtState} 之前）结算上一帧结果。
     * 用"上一帧"而不是"本帧"，是因为地形提交发生在 RT 注入之前，本帧结果当时还不可知。
     */
    public static void onFrameBoundary(final boolean injectedThisFrame) {
        lastFrameInjected = injectedThisFrame;
    }

    /** 地形光栅是否应当被跳过。 */
    public static boolean active() {
        if (!ENABLED) {
            return false;
        }

        final boolean active =
                NativeVulkan.rtActive && lastFrameInjected;

        if (active != lastActive) {
            lastActive = active;
            if (active) {
                System.out.println("[TAKEOVER] terrain raster OFF"
                        + " variant=" + (EXTRACT_SKIP ? "A(extract-skip)" : "B(cancel-recording)")
                        + " level=" + LEVEL
                        + " (RT 已接管画面；section 网格构建与 visibleSections 不变)");
            } else {
                System.out.println("[TAKEOVER] terrain raster ON"
                        + " (RT 未出图 / 已被关闭，恢复原版光栅)");
            }
        }

        return active;
    }

    /**
     * 主 pass 里的原版要素光栅是否应当跳过（档位 ≥ 2）。
     *
     * <p>与 {@link #active()} 同一个安全阀：只在 RT 上一帧真的出图时才跳过。
     * 被跳过的 {@code PreparedFrame.execute*} 全部是纯光栅录制，它们的结果都落在
     * main render target 的颜色上，而那张图随后会被 RT 整屏覆盖。
     */
    public static boolean skipFeatureRaster() {
        return LEVEL >= 2 && active();
    }

    /**
     * 是否跳过 draw group 构建（变体 A）。默认 false ⇒ 走变体 B。
     * 与 {@link #active()} 同一个安全阀。
     */
    public static boolean skipTerrainExtraction() {
        return EXTRACT_SKIP && active();
    }

    /** 供日志/诊断读取，不改变行为。 */
    public static boolean enabled() {
        return ENABLED;
    }

    public static int level() {
        return LEVEL;
    }
}
