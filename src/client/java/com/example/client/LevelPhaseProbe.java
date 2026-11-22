package com.example.client;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * 2026-11-11：把 {@code LevelRenderer.render} 的整段墙钟（此前统称「level 段
 * 15.5 ms / 3.49 MB」）切成可归因的几块。
 *
 * <h2>第一层（2026-11-11 已定论）</h2>
 * rd=8 实测：`level 8.9~15.2` 里 `submit 0.3` / `prepChunk 0.3` / `terrainDraw 0.01` /
 * `unaccounted 0.6`，剩下 **`featurePrep` 7.6~13.9 ms**。⇒ 「接管式渲染」能省的
 * 只有 0.3 ms，已放弃；真正的 CPU 大头是 `FeatureRenderDispatcher.prepareFrame`。
 *
 * <h2>第二层（本文件当前在拆的）</h2>
 * {@code prepareFrameWithContext} 内部是四段，这里逐段计时：
 * <pre>
 *   sort            SubmitNodeStorage.drainPhases → phase.sortInto(...)
 *   featGroup       PreparedGroup.prepare(...)  ← 逐 featureType 分组，最主要的嫌疑
 *   upload          StagedVertexBuffer.upload()
 *   （finishPrepare 与 loop 本身 = featurePrep − 上面三段）
 * </pre>
 * 另按 {@code FeatureRendererType.name()} 累加 {@code featGroup}，打 top5 ——
 * 用来区分「实体模型 / 粒子 / 方块实体 / 文字」到底谁贵。
 *
 * <p>口径：CPU 墙钟（{@code System.nanoTime()}），只在主线程、只在
 * {@code LevelRenderer.render} 窗口内计数（手持物与屏幕特效在 RETURN 之后还会各调一次
 * {@code prepareFrame}，必须剔除）。默认关闭，{@code ORION_LEVEL_PHASES=1} 开启。
 */
public final class LevelPhaseProbe {

    private static final boolean ENABLED =
            "1".equals(System.getenv("ORION_LEVEL_PHASES"));

    /** 打印窗口（帧）。与 [FRAME BUDGET] 的 60 帧窗口对齐。 */
    private static final int WINDOW = 60;

    private static long frameStartNanos;
    private static long submitStartNanos;
    private static long prepStartNanos;
    private static long groupStartNanos;
    private static long featureStartNanos;
    private static long featSortStartNanos;
    private static long featGroupStartNanos;
    private static long featUploadStartNanos;
    private static int groupDepth;

    /** 当前帧累计（render RETURN 时结账并清零）。 */
    private static double frameSubmitMs;
    private static double framePrepMs;
    private static double frameGroupMs;
    private static double frameFeatureMs;
    private static double frameFeatSortMs;
    private static double frameFeatGroupMs;
    private static double frameFeatUploadMs;

    /** 打印窗口累计。 */
    private static double sumLevelMs;
    private static double sumSubmitMs;
    private static double sumPrepMs;
    private static double sumGroupMs;
    private static double sumFeatureMs;
    private static double sumFeatSortMs;
    private static double sumFeatGroupMs;
    private static double sumFeatUploadMs;
    private static double sumUnaccountedMs;

    private static double maxLevelMs;
    private static double maxSubmitMs;
    private static double maxPrepMs;
    private static double maxFeatureMs;
    private static double maxFeatGroupMs;

    private static int frames;
    private static int prepCalls;
    private static int featGroupCalls;

    /** featureType.name() → [0]=窗口累计 ms, [1]=max ms。 */
    private static final Map<String, double[]> FEAT_SUMS = new HashMap<>();

    /** 是否处在 {@code LevelRenderer.render} 窗口内（用于剔除手持物/屏幕特效的重复调用）。 */
    private static boolean inFrameWindow;

    private static String currentFeatName;

    private LevelPhaseProbe() {}

    public static boolean enabled() {
        return ENABLED;
    }

    /**
     * 热路径专用的开关读法。
     *
     * <p>与 {@link #enabled()} 等价，但语义上区分「这里每次调用都要付计时成本」——
     * 2026-11-12 回归就是因为 {@code getVertexBuilder}（每 quad 一次、密集区一帧几十万次）
     * 里无条件调了两回 {@code System.nanoTime()}，导致不开诊断也变慢。
     *
     * <p>{@link #ENABLED} 是 static final，JIT 会把 {@code if (probeEnabled())} 折叠成常量分支。
     */
    public static boolean probeEnabled() {
        return ENABLED;
    }

    private static boolean counting() {
        return ENABLED && inFrameWindow;
    }

    public static void beginFrame() {
        if (ENABLED) {
            frameStartNanos = System.nanoTime();
            inFrameWindow = true;
        }
    }

    public static void endFrame() {
        if (!ENABLED || frameStartNanos == 0L) {
            return;
        }
        inFrameWindow = false;

        final double levelMs =
                (System.nanoTime() - frameStartNanos) / 1_000_000.0;
        frameStartNanos = 0L;

        // 未归因 = level 整段 − 已测子段（天空 / frame graph 执行 / 云雨 / 其余 pass）
        final double unaccounted =
                levelMs - frameSubmitMs - frameFeatureMs - framePrepMs - frameGroupMs;

        sumLevelMs += levelMs;
        sumSubmitMs += frameSubmitMs;
        sumPrepMs += framePrepMs;
        sumGroupMs += frameGroupMs;
        sumFeatureMs += frameFeatureMs;
        sumFeatSortMs += frameFeatSortMs;
        sumFeatGroupMs += frameFeatGroupMs;
        sumFeatUploadMs += frameFeatUploadMs;
        sumCaptureMs += frameCaptureNanos / 1_000_000.0;
        sumCaptureGenericMs += frameCaptureGenericNanos / 1_000_000.0;
        sumCaptureMotionMs += frameCaptureMotionNanos / 1_000_000.0;
        sumCaptureJniMs += frameCaptureJniNanos / 1_000_000.0;
        sumGroupDrawCalls += frameGroupDrawCalls;
        sumGroupDrawMs += frameGroupDrawMs;
        sumItemPrepMs += frameItemPrepMs;
        sumItemBuildMs += frameItemBuildMs;
        sumUnaccountedMs += unaccounted;
        if (levelMs > maxLevelMs) {
            maxLevelMs = levelMs;
        }

        frameSubmitMs = 0.0;
        framePrepMs = 0.0;
        frameGroupMs = 0.0;
        frameFeatureMs = 0.0;
        frameFeatSortMs = 0.0;
        frameFeatGroupMs = 0.0;
        frameFeatUploadMs = 0.0;
        frameCaptureNanos = 0L;
        frameCaptureGenericNanos = 0L;
        frameCaptureMotionNanos = 0L;
        frameCaptureJniNanos = 0L;
        frameGroupDrawCalls = 0L;
        frameItemPrepMs = 0.0;
        frameItemBuildMs = 0.0;
        frameGroupDrawMs = 0.0;

        if (++frames >= WINDOW) {
            report();
            reset();
        }
    }

    public static void submitHead() {
        if (ENABLED) {
            submitStartNanos = System.nanoTime();
        }
    }

    public static void submitTail() {
        if (!ENABLED || submitStartNanos == 0L) {
            return;
        }
        final double ms = (System.nanoTime() - submitStartNanos) / 1_000_000.0;
        submitStartNanos = 0L;
        frameSubmitMs += ms;
        if (ms > maxSubmitMs) {
            maxSubmitMs = ms;
        }
    }

    /** {@code prepareChunkRenders} / {@code prepareChunkRendersIndirect}。 */
    public static void prepareHead() {
        if (ENABLED) {
            prepStartNanos = System.nanoTime();
        }
    }

    public static void prepareTail() {
        if (!ENABLED || prepStartNanos == 0L) {
            return;
        }
        final double ms = (System.nanoTime() - prepStartNanos) / 1_000_000.0;
        prepStartNanos = 0L;
        framePrepMs += ms;
        ++prepCalls;
        if (ms > maxPrepMs) {
            maxPrepMs = ms;
        }
    }

    /** {@code FeatureRenderDispatcher.prepareFrame}：提交节点 → PreparedFrame（逐帧重建）。 */
    public static void featureHead() {
        // 只在 level render 窗口内计数：手持物与屏幕特效在 render RETURN 之后还会各调一次
        if (counting()) {
            featureStartNanos = System.nanoTime();
        }
    }

    public static void featureTail() {
        if (!ENABLED || featureStartNanos == 0L) {
            return;
        }
        final double ms = (System.nanoTime() - featureStartNanos) / 1_000_000.0;
        featureStartNanos = 0L;
        frameFeatureMs += ms;
        if (ms > maxFeatureMs) {
            maxFeatureMs = ms;
        }
    }

    /** 子段 1：{@code SubmitNodeStorage.drainPhases}（按 phase 排序分组）。 */
    public static void featSortHead() {
        if (counting()) {
            featSortStartNanos = System.nanoTime();
        }
    }

    public static void featSortTail() {
        if (!ENABLED || featSortStartNanos == 0L) {
            return;
        }
        frameFeatSortMs += (System.nanoTime() - featSortStartNanos) / 1_000_000.0;
        featSortStartNanos = 0L;
    }

    /** 子段 2：{@code PreparedGroup.prepare}（逐 featureType 建顶点数据）。 */
    public static void featGroupHead(final String featureName) {
        if (counting()) {
            currentFeatName = featureName;
            featGroupStartNanos = System.nanoTime();
        }
    }

    public static void featGroupTail() {
        if (!ENABLED || featGroupStartNanos == 0L) {
            return;
        }
        final double ms = (System.nanoTime() - featGroupStartNanos) / 1_000_000.0;
        featGroupStartNanos = 0L;
        frameFeatGroupMs += ms;
        ++featGroupCalls;
        if (ms > maxFeatGroupMs) {
            maxFeatGroupMs = ms;
        }

        final String key = currentFeatName == null ? "?" : currentFeatName;
        currentFeatName = null;
        final double[] slot = FEAT_SUMS.get(key);
        if (slot == null) {
            FEAT_SUMS.put(key, new double[]{ms, ms});
        } else {
            slot[0] += ms;
            if (ms > slot[1]) {
                slot[1] = ms;
            }
        }
    }

    /** 子段 3：{@code StagedVertexBuffer.upload}（上传共享顶点缓冲）。 */
    public static void featUploadHead() {
        if (counting()) {
            featUploadStartNanos = System.nanoTime();
        }
    }

    public static void featUploadTail() {
        if (!ENABLED || featUploadStartNanos == 0L) {
            return;
        }
        frameFeatUploadMs += (System.nanoTime() - featUploadStartNanos) / 1_000_000.0;
        featUploadStartNanos = 0L;
    }

    /** 本帧我们自己捕获代码的墙钟：model 链（beginModel/endModel）。 */
    // ── 2026-11-12：物品组缓存的诊断（prepareGroup vs buildGroup vs cancel） ──
    private static long itemPrepStartNanos;
    private static long itemBuildStartNanos;
    private static double frameItemPrepMs;
    private static double frameItemBuildMs;
    private static double sumItemPrepMs;
    private static double sumItemBuildMs;
    private static int itemPrepCalls;
    private static int itemBuildCalls;
    private static int itemBuildCancels;
    private static final java.util.HashSet<Long> itemKeyZero = new java.util.HashSet<>();

    public static void itemPrepHead() {
        if (counting()) {
            itemPrepStartNanos = System.nanoTime();
            ++itemPrepCalls;
        }
    }

    public static void itemPrepTail() {
        if (!ENABLED || itemPrepStartNanos == 0L) {
            return;
        }
        frameItemPrepMs += (System.nanoTime() - itemPrepStartNanos) / 1_000_000.0;
        itemPrepStartNanos = 0L;
    }

    public static void itemBuildHead() {
        if (counting()) {
            itemBuildStartNanos = System.nanoTime();
            ++itemBuildCalls;
        }
    }

    public static void itemBuildTail() {
        if (!ENABLED || itemBuildStartNanos == 0L) {
            return;
        }
        frameItemBuildMs += (System.nanoTime() - itemBuildStartNanos) / 1_000_000.0;
        itemBuildStartNanos = 0L;
    }

    public static void itemBuildCancelled() {
        ++itemBuildCancels;
        itemBuildTail();
    }

    /** {@code RenderTypeFeatureRenderer$Group.getOrAddDraw}：新建/复用 draw 的调用与耗时。 */
    private static long groupDrawStartNanos;
    private static long frameGroupDrawCalls;
    private static double frameGroupDrawMs;
    private static long sumGroupDrawCalls;
    private static double sumGroupDrawMs;
    private static double maxGroupDrawMs;

    public static void groupDrawHead() {
        if (counting()) {
            groupDrawStartNanos = System.nanoTime();
            ++frameGroupDrawCalls;
        }
    }

    public static void groupDrawTail() {
        if (!ENABLED || groupDrawStartNanos == 0L) {
            return;
        }
        final double ms = (System.nanoTime() - groupDrawStartNanos) / 1_000_000.0;
        groupDrawStartNanos = 0L;
        frameGroupDrawMs += ms;
        if (ms > maxGroupDrawMs) {
            maxGroupDrawMs = ms;
        }
    }

    private static long frameCaptureNanos;
    private static double sumCaptureMs;

    /** 本帧我们自己捕获代码的墙钟：RenderType 通用链（Item / Custom / BlockModel / MovingBlock）。 */
    private static long frameCaptureGenericNanos;
    private static double sumCaptureGenericMs;

    /** 由 {@code ModelFeatureRendererMixin} 直接喂入：我们那段捕获占 featGroup 的多少。 */
    public static void captureAdd(final long nanos) {
        if (counting() && nanos > 0L) {
            frameCaptureNanos += nanos;
        }
    }

    /** 由 {@code RenderTypeFeatureRendererMixin} 喂入：Item/Custom/BlockModel/MovingBlock 的捕获耗时。 */
    public static void captureGenericAdd(final long nanos) {
        if (counting() && nanos > 0L) {
            frameCaptureGenericNanos += nanos;
        }
    }

    /** {@code EntityRtCapture.uploadBatch} 内部：逐顶点运动矢量 + 历史簿记。 */
    private static long frameCaptureMotionNanos;
    private static double sumCaptureMotionMs;

    /** {@code EntityRtCapture.uploadBatch} 内部：{@code uploadEntityBatch} 的 JNI。 */
    private static long frameCaptureJniNanos;
    private static double sumCaptureJniMs;

    public static void captureMotionAdd(final long nanos) {
        if (counting() && nanos > 0L) {
            frameCaptureMotionNanos += nanos;
        }
    }

    public static void captureJniAdd(final long nanos) {
        if (counting() && nanos > 0L) {
            frameCaptureJniNanos += nanos;
        }
    }

    /** {@code ChunkSectionsToRender.renderGroup}：地形命令录制（一帧 1~2 次）。 */
    public static void terrainHead() {
        if (!ENABLED) {
            return;
        }
        if (groupDepth++ == 0) {
            groupStartNanos = System.nanoTime();
        }
    }

    public static void terrainTail() {
        if (!ENABLED) {
            return;
        }
        if (--groupDepth > 0 || groupStartNanos == 0L) {
            return;
        }
        frameGroupMs += (System.nanoTime() - groupStartNanos) / 1_000_000.0;
        groupStartNanos = 0L;
    }

    private static void report() {
        final double inv = frames <= 0 ? 1.0 : frames;

        String indirect = "n/a";
        try {
            indirect = String.valueOf(
                    net.minecraft.client.Minecraft.getInstance()
                            .levelRenderer
                            .isChunkRenderingUsingMultiDrawIndirect());
        } catch (final Throwable ignored) {
        }

        final double featRest =
                sumFeatureMs - sumFeatSortMs - sumFeatGroupMs - sumFeatUploadMs;

        // 一次取走，避免多个 take 互相清零
        final long itemObjScratch = EntityRtCapture.takeItemSubmitObjects();
        final long poseDistinct = EntityRtCapture.takeItemPoseDistinct();
        final long modelDistinct = EntityRtCapture.takeItemModelDistinct();
        final long gcHits = EntityRtCapture.takeGroupCacheHits();
        final long gcMisses = EntityRtCapture.takeGroupCacheMisses();
        final long gcInvalid = EntityRtCapture.takeGroupCacheInvalidated();
        final long vbCalls = EntityRtCapture.takeVbCalls();
        final long vbRenderTypes = EntityRtCapture.takeVbRenderTypes();
        final long vbConsumers = EntityRtCapture.takeVbConsumers();
        final long itemKeyTotal = EntityRtCapture.takeItemKeyTotal();
        final long itemKeyHits = EntityRtCapture.takeItemKeyHits();
        final long itemKeyDistinct = EntityRtCapture.takeItemKeyDistinct();
        final long vertScratch = frames <= 0 ? 0L : EntityRtCapture.takeCapturedVertexCount();
        final long itemScratch = frames <= 0 ? 0L : EntityRtCapture.takeGenericSubmitCount();
        final long modelScratch = frames <= 0 ? 0L : EntityRtCapture.takeModelSubmitCount();

        System.out.println(
                "[LEVEL PHASES]"
                        + " n=" + frames
                        + " indirect=" + indirect
                        + " takeover=" + TerrainTakeover.level()
                        + " level=" + round(sumLevelMs / inv)
                        + " submit=" + round(sumSubmitMs / inv)
                        + " featurePrep=" + round(sumFeatureMs / inv)
                        + " prepChunk=" + round(sumPrepMs / inv)
                        + " terrainDraw=" + round(sumGroupMs / inv)
                        + " unaccounted=" + round(sumUnaccountedMs / inv)
                        + " | max: level=" + round(maxLevelMs)
                        + " submit=" + round(maxSubmitMs)
                        + " featurePrep=" + round(maxFeatureMs)
                        + " prepChunk=" + round(maxPrepMs)
                        + " | prepCalls/f=" + round((double) prepCalls / inv));

        System.out.println(
                "[LEVEL PHASES FEAT]"
                        + " featurePrep=" + round(sumFeatureMs / inv)
                        + " sort=" + round(sumFeatSortMs / inv)
                        + " featGroup=" + round(sumFeatGroupMs / inv)
                        + " upload=" + round(sumFeatUploadMs / inv)
                        + " rest=" + round(featRest / inv)
                        + " | max featGroup=" + round(maxFeatGroupMs)
                        + " calls/f=" + round((double) featGroupCalls / inv)
                        + " | ours(capture)=" + round(sumCaptureMs / inv)
                        + " ours(generic)=" + round(sumCaptureGenericMs / inv)
                        + " mtn=" + round(sumCaptureMotionMs / inv)
                        + " jni=" + round(sumCaptureJniMs / inv)
                        + " verts/f=" + (vertScratch / frames)
                        + " submits/f=" + ((itemScratch + modelScratch) / frames)
                        + "(item=" + (itemScratch / frames)
                        + " model=" + (modelScratch / frames) + ")"
                        + " itemKeys/f=" + (itemKeyDistinct / frames)
                        + " hit%=" + (itemKeyTotal <= 0 ? 0.0
                                : Math.round(1000.0 * itemKeyHits / itemKeyTotal) / 10.0)
                        + " | vb/f=" + (vbCalls / frames)
                        + " rtypes/f=" + (vbRenderTypes / frames)
                        + " groups/f=" + (vbConsumers / frames)
                        + " vbPerGroup=" + (vbConsumers <= 0 ? 0.0
                                : Math.round(10.0 * vbCalls / vbConsumers) / 10.0)
                        + " | gc h/m/x=" + gcHits + "/" + gcMisses + "/" + gcInvalid
                        + " | itemObjs/f=" + (itemObjScratch / frames)
                        + " poseT=" + poseDistinct
                        + " modelV=" + modelDistinct
                        + " | itemPrep=" + round(sumItemPrepMs / inv)
                        + " itemBuild=" + round(sumItemBuildMs / inv)
                        + " prep/f=" + (itemPrepCalls / frames)
                        + " build/f=" + (itemBuildCalls / frames)
                        + " cancel/f=" + (itemBuildCancels / frames)
                        + " draws/f=" + (sumGroupDrawCalls / frames)
                        + " drawMs=" + round(sumGroupDrawMs / inv)
                        + "(max " + round(maxGroupDrawMs) + ")"
                        + " | " + topFeatures(inv));
    }

    private static String topFeatures(final double inv) {
        final List<Map.Entry<String, double[]>> list =
                new ArrayList<>(FEAT_SUMS.entrySet());
        list.sort((a, b) -> Double.compare(b.getValue()[0], a.getValue()[0]));

        final StringBuilder sb = new StringBuilder();
        final int top = Math.min(5, list.size());
        for (int i = 0; i < top; i++) {
            final Map.Entry<String, double[]> entry = list.get(i);
            if (i > 0) {
                sb.append("  ");
            }
            sb.append(entry.getKey())
                    .append('=').append(round(entry.getValue()[0] / inv))
                    .append("(max ").append(round(entry.getValue()[1])).append(')');
        }
        return sb.toString();
    }

    private static void reset() {
        frames = 0;
        prepCalls = 0;
        featGroupCalls = 0;
        sumLevelMs = 0.0;
        sumSubmitMs = 0.0;
        sumPrepMs = 0.0;
        sumGroupMs = 0.0;
        sumFeatureMs = 0.0;
        sumFeatSortMs = 0.0;
        sumFeatGroupMs = 0.0;
        sumFeatUploadMs = 0.0;
        sumCaptureMs = 0.0;
        sumCaptureGenericMs = 0.0;
        sumCaptureMotionMs = 0.0;
        sumCaptureJniMs = 0.0;
        sumGroupDrawCalls = 0L;
        sumItemPrepMs = 0.0;
        sumItemBuildMs = 0.0;
        itemPrepCalls = 0;
        itemBuildCalls = 0;
        itemBuildCancels = 0;
        sumGroupDrawMs = 0.0;
        maxGroupDrawMs = 0.0;
        sumUnaccountedMs = 0.0;
        maxLevelMs = 0.0;
        maxSubmitMs = 0.0;
        maxPrepMs = 0.0;
        maxFeatureMs = 0.0;
        maxFeatGroupMs = 0.0;
        FEAT_SUMS.clear();
    }

    private static double round(final double value) {
        return Math.round(value * 100.0) / 100.0;
    }
}
