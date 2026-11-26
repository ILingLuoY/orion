package com.example.client;

/**
 * 2026-10-13：帧预算二分（纯 Java、只读时钟，不调用任何原生接口、不改渲染行为）。
 *
 * <p><b>为什么需要它。</b>至今每一局都出现同一个盲区：`[PERF L10 CPU]` 的帧间隔是
 * 24~93ms，而同一窗口里 GPU 只有 8~15ms、我们自己的 C++（`recordFrameCommand`，
 * 即 `[PERF L10 CPUPHASE]` 的口径）只有 1~12ms、我们计过时的文字分项只有 2~2.5ms。
 * 也就是说**几十毫秒既不在 GPU、也不在任何我们计过时的东西里**，而日志里没有任何
 * 数字能指出它落在哪。这个类就补这一个洞，把帧间隔切成两段：</p>
 *
 * <pre>
 *   avgIntervalMs = 相邻两次 GameRenderer.render() 的间隔 = 整帧周期（与
 *                   [PERF L10 CPU] 的 avgMs 同口径，可互相校验）
 *   avgRenderMs   = GameRenderer.render() 自身的墙钟耗时（世界 + 手 + GUI +
 *                   我们注入在里面的 RT 录制与 Java 钩子）
 *   avgOutsideMs  = avgIntervalMs - avgRenderMs
 *                 = **不在 render 里**的部分：客户端 tick、GC、呈现/帧生成排队、
 *                   以及任何等待
 * </pre>
 *
 * <p>读法：</p>
 * <ul>
 *   <li>{@code avgOutsideMs} 大 ⇒ 卡的是 tick/GC/呈现，不是渲染路径；</li>
 *   <li>{@code avgRenderMs} 几乎等于 {@code avgIntervalMs} ⇒ 卡在渲染路径里。
 *       此时再拿 {@code [PERF L10 CPUPHASE]} 的 total 相减：
 *       差额 = 原版的渲染成本（城市图里怀疑就是招牌文字那条 Text 路径，
 *       代码注释记过原版 profiler 报 Text 占整帧 85.6%）。</li>
 * </ul>
 *
 * <p>挂在 {@code GameRendererMixin} 的 render() HEAD/RETURN 上，60 帧一行。</p>
 *
 * <p><b>2026-10-14 第二段：把 avgOutsideMs 再切成三份。</b>实测 {@code avgOutsideMs}
 * 高达 22.4ms（占整帧 92%），而 {@code render()} 自身只有 2ms。这一段落在
 * {@code Minecraft.runTick()} 里、{@code render()} 之外，但原仪表分不出是"模拟/tick"
 * 还是"呈现/交接"还是原版渲染。于是沿 runTick 再插两个时钟（{@code MinecraftReflexMixin}
 * 本来就占着 runTick 的 HEAD 与"renderFrame 之前"，这里补 RETURN）：</p>
 *
 * <pre>
 *   ┌ runTick ──────────────────────────────────────────────┐
 *   │ tickHead → beforeRenderFrame    = avgSimMs   （模拟/tick、区块提交、光线上传等） │
 *   │ beforeRenderFrame → renderHead                                       │
 *   │ renderHead → renderTail         = avgRenderMs（GameRenderer.render 自身）      │
 *   │ renderTail → tickTail           = avgFrameTailMs（renderFrame 的其余：呈现/acquire…）│
 *   └───────────────────────────────────────────────────────┘
 * </pre>
 *
 * <p>读法：三段之和应与 {@code avgIntervalMs} 同口径（差值 = 帧与帧之间的空档）。
 * 哪一段最大，优化就打那里 —— 这一条是为"整体优化"定靶用的，不改任何行为。</p>
 */
public final class FrameBudget {

    private static final int WINDOW = 60;

    private static long lastHeadNanos;
    private static double lastRenderMs;

    private static long tickHeadNanos;
    private static long beforeRenderFrameNanos;
    private static double lastSimMs;
    private static double lastFrameTailMs;

    private static int frames;
    private static double sumIntervalMs;
    private static double sumRenderMs;
    private static double sumOutsideMs;
    private static double sumSimMs;
    private static double sumFrameTailMs;
    private static double sumAcquireMs;
    private static double sumPresentMs;

    private static long renderTailNanos;
    private static long afterRenderFrameNanos;
    private static double lastRenderFrameInnerMs;
    private static double lastPostRenderFrameMs;

    // 2026-11-09：LevelRenderer.render 整段的墙钟。
    // 病征（用户）：avgRenderMs 20~46ms，但 frameTail 只占 1~2ms、submit 0.7ms，
    // 加起来对不上 ⇒ 时间在 level render 内部，而那里此前没有任何分段。
    // 这一段包含我们的 per-frame JNI（recordFrameCommand / 实体捕获 / 区块上传）
    // 与原版 Java 渲染，先用它把"是我们的"和"是原版的"分开，再逐层下切。
    private static long levelRenderStartNanos;
    private static double lastLevelRenderMs;
    private static double sumLevelRenderMs;
    private static double maxLevelRenderMs;

    // 2026-11-09 第二刀：把 levelRender 拆成"我们的 JNI"与"原版渲染"。
    //   captureHeadMs = LevelRenderer.render 的 HEAD 注入整块
    //                   （VoxyLod.tick / DhBridge.tick / FarFieldGate / 实体与粒子
    //                    捕获 / submitFrame / uploadVisibleSections）
    //   recordFrameMs = NativeVulkan.recordFrameCommand（每帧录制全部 RT pass）
    // 两者都是我们的，且都在 render 回调内；剩下的部分才接近"原版渲染"。
    private static long captureHeadStartNanos;
    private static double sumCaptureHeadMs;
    private static double maxCaptureHeadMs;
    private static long recordFrameStartNanos;
    private static double sumRecordFrameMs;
    private static double maxRecordFrameMs;

    // 2026-11-09 第三刀：submitEntities 整段（含我们的实体模型捕获：逐模型
    // beginModel/endModel → uploadEntityBatch JNI + 每帧顶点内容哈希）。
    // levelRender − captureHead − submitEntities ≈ 区块/几何提交（更接近"原版"）。
    private static long submitEntitiesStartNanos;
    private static double sumSubmitEntitiesMs;
    private static double maxSubmitEntitiesMs;

    /** 2026-11-09：LevelRenderer.submitEntities HEAD。 */
    public static void beginSubmitEntities() {
        submitEntitiesStartNanos = System.nanoTime();
        submitAllocBase = allocatedBytes();
    }

    /** 2026-11-09：LevelRenderer.submitEntities TAIL。 */
    public static void endSubmitEntities() {
        if (submitEntitiesStartNanos != 0L) {
            final double ms =
                    (System.nanoTime() - submitEntitiesStartNanos) / 1_000_000.0;
            sumSubmitEntitiesMs += ms;
            if (ms > maxSubmitEntitiesMs) {
                maxSubmitEntitiesMs = ms;
            }
        }
        if (submitEntitiesStartNanos != 0L)
        {
            sumSubmitAlloc += allocatedBytes() - submitAllocBase;
        }
        submitEntitiesStartNanos = 0L;
    }

    private static double lastCaptureHeadMs;
    private static double lastCapVoxyMs;
    private static double lastCapDhMs;
    private static double lastCapFfgMs;
    private static double lastCapDynMs;
    private static double lastCapVisMs;
    private static double lastRecordFrameMs;

    public static void recordCaptureHeadBreakdown(
            double voxyMs, double dhMs, double ffgMs, double dynMs, double visMs) {
        lastCapVoxyMs = voxyMs;
        lastCapDhMs = dhMs;
        lastCapFfgMs = ffgMs;
        lastCapDynMs = dynMs;
        lastCapVisMs = visMs;
    }
    private static long renderLevelStartNanos;
    private static long renderLevelEndNanos;
    private static long lastLevelRenderStartNanos;
    private static long levelRenderEndNanos;
    private static long lastSpikeGcTimeMs;
    private static long lastSpikeGcCount;
    private static final java.util.concurrent.atomic.AtomicLong UPLOAD_CALLS =
            new java.util.concurrent.atomic.AtomicLong();
    private static final java.util.concurrent.atomic.AtomicLong UPLOAD_NANOS =
            new java.util.concurrent.atomic.AtomicLong();
    private static final java.util.concurrent.atomic.AtomicLong UPLOAD_MAX_NANOS =
            new java.util.concurrent.atomic.AtomicLong();

    public static void recordUploadChunk(long nanos) {
        UPLOAD_CALLS.incrementAndGet();
        UPLOAD_NANOS.addAndGet(nanos);
        UPLOAD_MAX_NANOS.accumulateAndGet(nanos, Math::max);
    }

    public static void beginRenderLevel() {
        renderLevelStartNanos = System.nanoTime();
    }

    public static void endRenderLevel() {
        renderLevelEndNanos = System.nanoTime();
    }

    /** 2026-11-09：HEAD 注入块起点。 */
    public static void beginCaptureHead() {
        captureHeadStartNanos = System.nanoTime();
        captureAllocBase = allocatedBytes();
    }

    /** 2026-11-09：HEAD 注入块终点。 */
    public static void endCaptureHead() {
        if (captureHeadStartNanos != 0L) {
            final double ms =
                    (System.nanoTime() - captureHeadStartNanos) / 1_000_000.0;
            lastCaptureHeadMs = ms;
            sumCaptureHeadMs += ms;
            if (ms > maxCaptureHeadMs) {
                maxCaptureHeadMs = ms;
            }
        }
        if (captureHeadStartNanos != 0L)
        {
            sumCaptureAlloc += allocatedBytes() - captureAllocBase;
        }
        captureHeadStartNanos = 0L;
    }

    /** 2026-11-09：recordFrameCommand 起点。 */
    public static void beginRecordFrame() {
        recordFrameStartNanos = System.nanoTime();
        recordAllocBase = allocatedBytes();
    }

    /** 2026-11-09：recordFrameCommand 终点。 */
    public static void endRecordFrame() {
        if (recordFrameStartNanos != 0L) {
            final double ms =
                    (System.nanoTime() - recordFrameStartNanos) / 1_000_000.0;
            lastRecordFrameMs = ms;
            sumRecordFrameMs += ms;
            if (ms > maxRecordFrameMs) {
                maxRecordFrameMs = ms;
            }
        }
        if (recordFrameStartNanos != 0L)
        {
            sumRecordAlloc += allocatedBytes() - recordAllocBase;
        }
        recordFrameStartNanos = 0L;
    }
    private static double sumRenderFrameInnerMs;
    private static double sumPostRenderFrameMs;

    private static long submitStartNanos;
    private static double frameSubmitMs;
    private static int frameSubmitCount;
    private static double lastSubmitMs;
    private static int lastSubmitCount;
    private static double sumSubmitMs;
    private static int sumSubmitCount;

    private static double sumVanillaTickMs;
    private static double maxRenderMs;
    private static double maxSimMs;

    private static long acquireStartNanos;
    private static long presentStartNanos;
    private static double frameAcquireMs;
    private static double framePresentMs;
    private static double lastAcquireMs;
    private static double lastPresentMs;

    private static long vanillaTickStartNanos;
    private static double frameVanillaTickMs;
    private static double lastVanillaTickMs;

    private static volatile Thread mainThread;
    private static long taskPumpStartNanos;
    private static long taskRunStartNanos;
    private static double frameTaskPumpMs;
    private static double frameTaskRunMs;
    private static int frameTaskCount;
    private static double frameTaskMaxMs;
    private static double lastTaskPumpMs;
    private static double lastTaskRunMs;
    private static int lastTaskCount;
    private static double lastTaskMaxMs;
    private static double sumTaskPumpMs;
    private static double sumTaskRunMs;
    private static int sumTaskCount;
    private static double maxTaskMaxMs;

    private static final int JNI_HEAD_SLOTS = 3;
    private static final double[] frameJniHeadMs = new double[JNI_HEAD_SLOTS];
    private static final double[] lastJniHeadMs = new double[JNI_HEAD_SLOTS];
    private static final double[] sumJniHeadMs = new double[JNI_HEAD_SLOTS];
    private static final double[] maxJniHeadMs = new double[JNI_HEAD_SLOTS];

    private static long lastTickTailNanos;
    private static double lastGapMs;
    private static double sumGapMs;

    private static long prevGcTimeMs;
    private static long prevGcCount;

    private static final int SIM_SUB_SLOTS = 4;
    private static final double[] frameSimSubMs = new double[SIM_SUB_SLOTS];
    private static final double[] lastSimSubMs = new double[SIM_SUB_SLOTS];
    private static final double[] sumSimSubMs = new double[SIM_SUB_SLOTS];

    /** tickHead 内单个 JNI 调用的耗时采样（slot 见上方说明）。 */
    public static void jniTickHead(int slot, long nanos) {
        if (slot < 0 || slot >= JNI_HEAD_SLOTS) {
            return;
        }
        frameJniHeadMs[slot] += nanos / 1_000_000.0;
    }

    private static final long[] simSubStartNanos = new long[SIM_SUB_SLOTS];

    /** 是否处于"主线程 + 本帧 runTick 段内"——四路子项只在这个窗口里累计。 */
    public static boolean isSimSubActive() {
        return tickHeadNanos != 0L && onMainThread();
    }

    public static void markSimSubStart(int slot) {
        if (slot < 0 || slot >= SIM_SUB_SLOTS) {
            return;
        }
        simSubStartNanos[slot] = System.nanoTime();
    }

    public static void endSimSub(int slot) {
        if (slot < 0 || slot >= SIM_SUB_SLOTS) {
            return;
        }
        if (simSubStartNanos[slot] != 0L) {
            frameSimSubMs[slot] +=
                    (System.nanoTime() - simSubStartNanos[slot]) / 1_000_000.0;
            simSubStartNanos[slot] = 0L;
        }
    }

    private FrameBudget() {}

    private static boolean onMainThread() {
        return mainThread != null && Thread.currentThread() == mainThread;
    }

    /** Minecraft.runTick() 入口（MinecraftReflexMixin @At HEAD）。 */
    public static void tickHead() {
        mainThread = Thread.currentThread();
        tickHeadNanos = System.nanoTime();
        if (lastTickTailNanos != 0L) {
            lastGapMs = (tickHeadNanos - lastTickTailNanos) / 1_000_000.0;
        } else {
            lastGapMs = 0.0;
        }
        beforeRenderFrameNanos = 0L;
        afterRenderFrameNanos = 0L;
        renderTailNanos = 0L;
        frameAcquireMs = 0.0;
        framePresentMs = 0.0;
        frameSubmitMs = 0.0;
        frameSubmitCount = 0;
        frameVanillaTickMs = 0.0;
        frameTaskPumpMs = 0.0;
        frameTaskRunMs = 0.0;
        frameTaskCount = 0;
        frameTaskMaxMs = 0.0;
        java.util.Arrays.fill(frameJniHeadMs, 0.0);
        java.util.Arrays.fill(frameSimSubMs, 0.0);
    }

    /** 原版 Minecraft.tick() 开始（MinecraftReflexMixin @At HEAD）。 */
    public static void vanillaTickHead() {
        vanillaTickStartNanos = System.nanoTime();
    }

    /** 原版 Minecraft.tick() 结束（@At RETURN）。 */
    public static void vanillaTickTail() {
        if (vanillaTickStartNanos != 0L) {
            frameVanillaTickMs +=
                    (System.nanoTime() - vanillaTickStartNanos) / 1_000_000.0;
            vanillaTickStartNanos = 0L;
        }
    }

    /** 交换链获取开始（VulkanGpuSurface.acquireNextTexture @At HEAD）。 */
    public static void acquireHead() {
        acquireStartNanos = System.nanoTime();
    }

    /** 交换链获取结束（@At RETURN）。 */
    public static void acquireTail() {
        if (acquireStartNanos != 0L) {
            frameAcquireMs += (System.nanoTime() - acquireStartNanos) / 1_000_000.0;
            acquireStartNanos = 0L;
        }
    }

    /** 呈现开始（VulkanGpuSurface.present @At HEAD）。 */
    public static void presentHead() {
        presentStartNanos = System.nanoTime();
    }

    /** 呈现结束（@At RETURN）。 */
    public static void presentTail() {
        if (presentStartNanos != 0L) {
            framePresentMs += (System.nanoTime() - presentStartNanos) / 1_000_000.0;
            presentStartNanos = 0L;
        }
    }

    /**
     * 主线程任务泵整段开始（BlockableEventLoop.runAllTasks @At HEAD）。
     * 只统计主线程那次：worker 循环的同名方法被主线程门卫挡掉。
     */
    public static void taskPumpHead() {
        if (onMainThread()) {
            taskPumpStartNanos = System.nanoTime();
        }
    }

    /** 任务泵整段结束（runAllTasks @At RETURN）。 */
    public static void taskPumpTail() {
        if (onMainThread() && taskPumpStartNanos != 0L) {
            frameTaskPumpMs +=
                    (System.nanoTime() - taskPumpStartNanos) / 1_000_000.0;
            taskPumpStartNanos = 0L;
        }
    }

    /** 单个任务开始（BlockableEventLoop.doRunTask @At HEAD）。 */
    public static void taskRunHead() {
        if (onMainThread()) {
            taskRunStartNanos = System.nanoTime();
        }
    }

    /** 单个任务结束（doRunTask @At RETURN）：次数/合计/单任务最大值。 */
    public static void taskRunTail() {
        if (onMainThread() && taskRunStartNanos != 0L) {
            final double ms =
                    (System.nanoTime() - taskRunStartNanos) / 1_000_000.0;
            frameTaskRunMs += ms;
            ++frameTaskCount;
            if (ms > frameTaskMaxMs) {
                frameTaskMaxMs = ms;
            }
            taskRunStartNanos = 0L;
        }
    }

    /**
     * runTick() 内、调用 renderFrame() 之前那一刻（MinecraftReflexMixin 的 INVOKE 点）。
     * 模拟段到这里结束。
     */
    public static void beforeRenderFrame() {
        if (tickHeadNanos == 0L) {
            return;
        }

        beforeRenderFrameNanos = System.nanoTime();
        lastSimMs = (beforeRenderFrameNanos - tickHeadNanos) / 1_000_000.0;
    }

    /**
     * Minecraft.runTick() 出口。本帧的 renderFrame 剩余（呈现/acquire/HUD 等）
     * = runTick 总时长 − 模拟段 − render() 自身。
     */
    public static void tickTail() {
        if (tickHeadNanos == 0L || beforeRenderFrameNanos == 0L) {
            tickHeadNanos = 0L;
            return;
        }

        final long tickTailNanos = System.nanoTime();
        final double tickMs = (tickTailNanos - tickHeadNanos) / 1_000_000.0;
        lastFrameTailMs = tickMs - lastSimMs - lastRenderMs;

        if (afterRenderFrameNanos != 0L) {
            lastPostRenderFrameMs = (tickTailNanos - afterRenderFrameNanos) / 1_000_000.0;
        } else {
            lastPostRenderFrameMs = 0.0;
            lastRenderFrameInnerMs = 0.0;
        }

        lastAcquireMs = frameAcquireMs;
        lastPresentMs = framePresentMs;
        lastSubmitMs = frameSubmitMs;
        lastSubmitCount = frameSubmitCount;
        lastVanillaTickMs = frameVanillaTickMs;
        lastTaskPumpMs = frameTaskPumpMs;
        lastTaskRunMs = frameTaskRunMs;
        lastTaskCount = frameTaskCount;
        lastTaskMaxMs = frameTaskMaxMs;
        System.arraycopy(frameJniHeadMs, 0, lastJniHeadMs, 0, JNI_HEAD_SLOTS);
        System.arraycopy(frameSimSubMs, 0, lastSimSubMs, 0, SIM_SUB_SLOTS);

        lastTickTailNanos = tickTailNanos;

        tickHeadNanos = 0L;
        beforeRenderFrameNanos = 0L;
    }

    /** GameRenderer.render() 入口：收尾上一帧，开这一帧。 */
    public static void head() {
        final long now = System.nanoTime();

        if (lastHeadNanos != 0L) {
            final double intervalMs = (now - lastHeadNanos) / 1_000_000.0;

            sumIntervalMs += intervalMs;
            sumRenderMs += lastRenderMs;
            sumOutsideMs += intervalMs - lastRenderMs;

            sumSimMs += lastSimMs;
            sumFrameTailMs += lastFrameTailMs;
            sumGapMs += lastGapMs;
            sumAcquireMs += lastAcquireMs;
            sumPresentMs += lastPresentMs;
            sumRenderFrameInnerMs += lastRenderFrameInnerMs;
            sumPostRenderFrameMs += lastPostRenderFrameMs;
            sumSubmitMs += lastSubmitMs;
            sumSubmitCount += lastSubmitCount;
            sumVanillaTickMs += lastVanillaTickMs;
            sumTaskPumpMs += lastTaskPumpMs;
            sumTaskRunMs += lastTaskRunMs;
            sumTaskCount += lastTaskCount;
            if (lastTaskMaxMs > maxTaskMaxMs) {
                maxTaskMaxMs = lastTaskMaxMs;
            }
            for (int slot = 0; slot < JNI_HEAD_SLOTS; ++slot) {
                sumJniHeadMs[slot] += lastJniHeadMs[slot];
                if (lastJniHeadMs[slot] > maxJniHeadMs[slot]) {
                    maxJniHeadMs[slot] = lastJniHeadMs[slot];
                }
            }
            for (int slot = 0; slot < SIM_SUB_SLOTS; ++slot) {
                sumSimSubMs[slot] += lastSimSubMs[slot];
            }

            if (lastRenderMs > maxRenderMs) {
                maxRenderMs = lastRenderMs;
            }

            if (lastSimMs > maxSimMs) {
                maxSimMs = lastSimMs;
            }

            if (++frames >= WINDOW) {
                report();
            }
        }

        lastHeadNanos = now;
        lastCaptureHeadMs = 0.0;
        lastCapVoxyMs = 0.0;
        lastCapDhMs = 0.0;
        lastCapFfgMs = 0.0;
        lastCapDynMs = 0.0;
        lastCapVisMs = 0.0;
        lastRecordFrameMs = 0.0;
        lastLevelRenderMs = 0.0;
        renderLevelStartNanos = 0L;
        renderLevelEndNanos = 0L;
        lastLevelRenderStartNanos = 0L;
        levelRenderEndNanos = 0L;
    }

    /** GameRenderer.render() 出口：记下本帧 render() 自身的耗时。 */
    public static void tail() {
        if (lastHeadNanos == 0L) {
            return;
        }

        final long now = System.nanoTime();
        lastRenderMs = (now - lastHeadNanos) / 1_000_000.0;
        renderTailNanos = now;

        if (lastRenderMs >= 25.0) {
            final double preRL = renderLevelStartNanos > 0L
                    ? (renderLevelStartNanos - lastHeadNanos) / 1_000_000.0
                    : 0.0;
            final double rlPreLv = (renderLevelStartNanos > 0L && lastLevelRenderStartNanos > 0L)
                    ? (lastLevelRenderStartNanos - renderLevelStartNanos) / 1_000_000.0
                    : 0.0;
            final double rlPostLv = (levelRenderEndNanos > 0L && renderLevelEndNanos > 0L)
                    ? (renderLevelEndNanos - levelRenderEndNanos) / 1_000_000.0
                    : 0.0;
            final double postRLGui = renderLevelEndNanos > 0L
                    ? (now - renderLevelEndNanos) / 1_000_000.0
                    : 0.0;

            final long upN = UPLOAD_CALLS.getAndSet(0L);
            final long upNs = UPLOAD_NANOS.getAndSet(0L);
            final long upMaxNs = UPLOAD_MAX_NANOS.getAndSet(0L);

            long gcTimeMs = 0L;
            long gcCount = 0L;
            for (java.lang.management.GarbageCollectorMXBean bean
                    : java.lang.management.ManagementFactory.getGarbageCollectorMXBeans()) {
                final long t = bean.getCollectionTime();
                final long c = bean.getCollectionCount();
                if (t >= 0L) gcTimeMs += t;
                if (c >= 0L) gcCount += c;
            }
            final long spikeGcMs = gcTimeMs - lastSpikeGcTimeMs;
            final long spikeGcN = gcCount - lastSpikeGcCount;
            lastSpikeGcTimeMs = gcTimeMs;
            lastSpikeGcCount = gcCount;

            final String capBreakdown = lastCaptureHeadMs >= 5.0
                    ? "[voxy=" + round(lastCapVoxyMs)
                            + " dh=" + round(lastCapDhMs)
                            + " ffg=" + round(lastCapFfgMs)
                            + " dyn=" + round(lastCapDynMs)
                            + " vis=" + round(lastCapVisMs) + "]"
                    : "";

            System.out.println(
                    "[RENDER SPIKE] total=" + round(lastRenderMs)
                            + "ms | preRL=" + round(preRL)
                            + " rlPreLv=" + round(rlPreLv)
                            + " lv=" + round(lastLevelRenderMs)
                            + "(cap=" + round(lastCaptureHeadMs) + capBreakdown + ")"
                            + " rlPostLv=" + round(rlPostLv)
                            + "(rec=" + round(lastRecordFrameMs) + ")"
                            + " postRL_gui=" + round(postRLGui)
                            + " | upN=" + upN
                            + " upMs=" + round(upNs / 1_000_000.0)
                            + " upMax=" + round(upMaxNs / 1_000_000.0)
                            + " | gcMs=" + spikeGcMs
                            + " gcN=" + spikeGcN
            );
        }
    }

    /**
     * 2026-10-22 A1：{@code Minecraft.renderFrame(boolean)} 返回的那一刻
     * （MinecraftReflexMixin 在 renderFrame 调用点 INVOKE/Shift.AFTER 注入）。
     *
     * <p>与 {@link #tail()} 的采样点差分 = {@code renderFrameInnerMs}：renderFrame 里、
     * {@code GameRenderer.render()} 之后的剩余（present/acquire 已单独量过，只占 0.21ms）。
     * 纯读时钟，不改任何行为。</p>
     */
    public static void afterRenderFrame() {
        afterRenderFrameNanos = System.nanoTime();
        if (renderTailNanos != 0L) {
            lastRenderFrameInnerMs =
                    (afterRenderFrameNanos - renderTailNanos) / 1_000_000.0;
        } else {
            lastRenderFrameInnerMs = 0.0;
        }
    }

    // ========================================================================
    // 2026-11-11：按段计量"分配字节"。
    //
    // 背景：GC 日志显示 Young GC 一次就 845M->380M，随后触发 14 次
    // Concurrent Mark Cycle（中位 124ms）抢渲染线程 CPU，表现为 100~270ms 长卡。
    // JFR 在 D:\RTX 这个精简运行时里 dump 恒为 0 字节，jcmd attach 也被拒，
    // 所以改用 JDK 自带的按线程分配计数器：精度到字节、开销极低、无需 attach。
    // ========================================================================
    // 注意：必须走 getThreadMXBean()。getOperatingSystemMXBean() 返回的是
    // OperatingSystemMXBean，它**不实现** com.sun.management.ThreadMXBean，
    // 转换会失败并退化成 0（第一版就是这么写错的，读数全 0.00）。
    private static final com.sun.management.ThreadMXBean ALLOC_MX =
            java.lang.management.ManagementFactory.getThreadMXBean()
                    instanceof com.sun.management.ThreadMXBean
                    ? (com.sun.management.ThreadMXBean)
                        java.lang.management.ManagementFactory.getThreadMXBean()
                    : null;

    private static long allocatedBytes() {
        if (ALLOC_MX == null) {
            return 0L;
        }
        return ALLOC_MX.getThreadAllocatedBytes(Thread.currentThread().getId());
    }

    private static long levelAllocBase;
    private static long captureAllocBase;
    private static long recordAllocBase;
    private static long submitAllocBase;
    private static double sumLevelAlloc;
    private static double sumCaptureAlloc;
    private static double sumRecordAlloc;
    private static double sumSubmitAlloc;

    private static double lastLevelAlloc;
    private static double lastRecordAlloc;

    /** 2026-11-09：LevelRenderer.render HEAD。纯读时钟，不改任何行为。 */
    public static void beginLevelRender() {
        levelRenderStartNanos = System.nanoTime();
        lastLevelRenderStartNanos = levelRenderStartNanos;
        levelAllocBase = allocatedBytes();
    }

    /** 2026-11-09：LevelRenderer.render RETURN。 */
    public static void endLevelRender() {
        final long now = System.nanoTime();
        levelRenderEndNanos = now;
        if (levelRenderStartNanos != 0L) {
            final double ms =
                    (now - levelRenderStartNanos) / 1_000_000.0;
            lastLevelRenderMs = ms;
            sumLevelRenderMs += ms;
            if (ms > maxLevelRenderMs) {
                maxLevelRenderMs = ms;
            }
        }
        if (levelRenderStartNanos != 0L)
        {
            sumLevelAlloc += allocatedBytes() - levelAllocBase;
        }
        levelRenderStartNanos = 0L;
    }

    /**
     * 2026-10-22 A1：{@code VulkanCommandEncoder.submit} 开始。
     * 这一段是 {@code vkQueueSubmit} 与其伴随的 fence/信号量操作 ——
     * 队列满或帧生成排队时它会阻塞，是 9ms frameTail 的头号嫌疑。
     */
    public static void submitHead() {
        submitStartNanos = System.nanoTime();
    }

    /** 2026-10-22 A1：{@code submit} 结束（一帧可能多次 ⇒ 按帧累加）。 */
    public static void submitTail() {
        if (submitStartNanos == 0L) {
            return;
        }
        frameSubmitMs += (System.nanoTime() - submitStartNanos) / 1_000_000.0;
        ++frameSubmitCount;
        submitStartNanos = 0L;
    }

    private static void report() {
        final double inv = frames <= 0 ? 1.0 : (double) frames;
        final double avgInterval = sumIntervalMs / inv;
        final double avgRender = sumRenderMs / inv;
        final double avgOutside = sumOutsideMs / inv;
        final double avgSim = sumSimMs / inv;
        final double avgFrameTail = sumFrameTailMs / inv;

        System.out.println(
                "[FRAME BUDGET]"
                        + " frames=" + frames
                        + " avgIntervalMs=" + round(avgInterval)
                        + " avgRenderMs=" + round(avgRender)
                        + " avgOutsideMs=" + round(avgOutside)
                        + " maxRenderMs=" + round(maxRenderMs)
                        // 2026-11-09：把渲染距离写进日志，让"改视距"的 A/B 可自动对齐。
                        + " rd=" + net.minecraft.client.Minecraft.getInstance()
                                .options.renderDistance().get()
                        + " renderPct=" + round(avgInterval <= 0.0
                                ? 0.0
                                : 100.0 * avgRender / avgInterval)
                        + "  判读：Outside 大 ⇒ tick/GC/呈现；Render≈Interval ⇒ 在渲染路径里"
                        + System.lineSeparator()
                        + "                [FRAME SPLIT] simMs=" + round(avgSim)
                        + " renderFrameTailMs=" + round(avgFrameTail)
                        + " gapMs=" + round(sumGapMs / inv)
                        + " maxSimMs=" + round(maxSimMs)
                        + "  (= runTick 拆三段 + 帧间空档；"
                        + "四者相加应与 avgIntervalMs 同口径，差值 = runTick 内未钩的段)");
        System.out.println(
                "                [FRAME SURFACE] acquireMs=" + round(sumAcquireMs / inv)
                        + " presentMs=" + round(sumPresentMs / inv)
                        + "  (= frameTail 里交换链的占比；"
                        + "acquire 阻塞 ⇒ 队列满/FG 排队；present 阻塞 ⇒ 驱动/vsync)");
        System.out.println(
                "                [FRAME TAIL SPLIT] renderFrameInnerMs="
                        + round(sumRenderFrameInnerMs / inv)
                        + " postRenderFrameMs=" + round(sumPostRenderFrameMs / inv)
                        + " submitMs=" + round(sumSubmitMs / inv)
                        + " submitCount=" + sumSubmitCount
                        + " levelRenderMs=" + round(sumLevelRenderMs / inv)
                        + " maxLevelRenderMs=" + round(maxLevelRenderMs)
                        + " captureHeadMs=" + round(sumCaptureHeadMs / inv)
                        + " maxCaptureHeadMs=" + round(maxCaptureHeadMs)
                        + " recordFrameMs=" + round(sumRecordFrameMs / inv)
                        + " MB/f: level=" + round(sumLevelAlloc / inv / 1048576.0)
                        + " record=" + round(sumRecordAlloc / inv / 1048576.0)
                        + " capture=" + round(sumCaptureAlloc / inv / 1048576.0)
                        + " submit=" + round(sumSubmitAlloc / inv / 1048576.0)
                        + " maxRecordFrameMs=" + round(maxRecordFrameMs)
                        + " submitEntitiesMs=" + round(sumSubmitEntitiesMs / inv)
                        + " maxSubmitEntitiesMs=" + round(maxSubmitEntitiesMs)
                        + "  (= frameTail 拆两段 + 我们自己的 submit 墙钟；"
                        + "submit≈frameTail ⇒ 队列等待/帧节奏（减 GPU pass 才有 fps）；"
                        + "levelRenderMs 覆盖 LevelRenderer.render 整段："
                        + "它≈avgRenderMs ⇒ 卡在 level render 内（我们的 JNI + 原版）；"
                        + "它远小于 avgRenderMs ⇒ 卡在 render 之外的帧内空档)");
        System.out.println(
                "                [FRAME SIM] vanillaTickMs=" + round(sumVanillaTickMs / inv)
                        + "  (= simMs 里的原版 tick；"
                        + "simMs − vanillaTickMs = 区块结果消费 + 我们的上传/JNI)");
        System.out.println(
                "                [FRAME SIM SPLIT] packetsMs=" + round(sumSimSubMs[0] / inv)
                        + " extractMs=" + round(sumSimSubMs[1] / inv)
                        + " soundMs=" + round(sumSimSubMs[2] / inv)
                        + " mouseMs=" + round(sumSimSubMs[3] / inv)
                        + "  (= sim 段四路：主线程网络包 / 26.3 区块提取 / 声音 / 鼠标；"
                        + "sim − vanillaTick − task − JNI三槽 − 本行四项 的残差 ⇒ "
                        + "advanceGameTime/textureTick/gizmos 等小额项)");
        System.out.println(
                "                [FRAME TASKS] runAllTasksMs=" + round(sumTaskPumpMs / inv)
                        + " taskMs=" + round(sumTaskRunMs / inv)
                        + " taskCount=" + sumTaskCount
                        + " maxTaskMs=" + round(maxTaskMaxMs)
                        + "  (= simMs 里的主线程任务泵：区块编译完成后的消费/上传任务；"
                        + "taskMs 与 vanillaTickMs 可能重叠——tick 内也会 pump)");
        System.out.println(
                "                [FRAME JNI HEAD] beginStreamlineMs="
                        + round(sumJniHeadMs[0] / inv)
                        + " maxBeginStreamlineMs=" + round(maxJniHeadMs[0])
                        + " markSimMs=" + round(sumJniHeadMs[1] / inv)
                        + " menuMs=" + round(sumJniHeadMs[2] / inv)
                        + "  (= simMs 里我们自己的三个 JNI；"
                        + "beginStreamline 大 ⇒ 帧围栏/GPU 等待藏在 sim 段)");
        long gcTimeMs = 0L;
        long gcCount = 0L;
        for (java.lang.management.GarbageCollectorMXBean bean
                : java.lang.management.ManagementFactory
                        .getGarbageCollectorMXBeans()) {
            final long t = bean.getCollectionTime();
            final long c = bean.getCollectionCount();
            if (t >= 0L) {
                gcTimeMs += t;
            }
            if (c >= 0L) {
                gcCount += c;
            }
        }
        final long gcMsWindow = gcTimeMs - prevGcTimeMs;
        final long gcCountWindow = gcCount - prevGcCount;
        prevGcTimeMs = gcTimeMs;
        prevGcCount = gcCount;
        System.out.println(
                "                [FRAME GAP/GC] gcMs=" + gcMsWindow
                        + " gcCount=" + gcCountWindow
                        + "  (= 本窗口 GC 墙钟耗时/次数的 MXBean 差分；"
                        + "gapMs 见 FRAME SPLIT 行，interval−sim−render−frameTail−gap "
                        + "的残差 ⇒ runTick 内未钩的段)");

        sumVanillaTickMs = 0.0;
        sumTaskPumpMs = 0.0;
        sumTaskRunMs = 0.0;
        sumTaskCount = 0;
        maxTaskMaxMs = 0.0;
        java.util.Arrays.fill(sumJniHeadMs, 0.0);
        java.util.Arrays.fill(maxJniHeadMs, 0.0);
        java.util.Arrays.fill(sumSimSubMs, 0.0);

        frames = 0;
        sumIntervalMs = 0.0;
        sumRenderMs = 0.0;
        sumOutsideMs = 0.0;
        sumSimMs = 0.0;
        sumFrameTailMs = 0.0;
        sumGapMs = 0.0;
        sumAcquireMs = 0.0;
        sumPresentMs = 0.0;
        sumRenderFrameInnerMs = 0.0;
        sumPostRenderFrameMs = 0.0;
        sumLevelRenderMs = 0.0;
        maxLevelRenderMs = 0.0;
        sumCaptureHeadMs = 0.0;
        maxCaptureHeadMs = 0.0;
        sumRecordFrameMs = 0.0;
        sumLevelAlloc = 0.0;
        sumCaptureAlloc = 0.0;
        sumRecordAlloc = 0.0;
        sumSubmitAlloc = 0.0;
        maxRecordFrameMs = 0.0;
        sumSubmitEntitiesMs = 0.0;
        maxSubmitEntitiesMs = 0.0;
        sumSubmitMs = 0.0;
        sumSubmitCount = 0;
        maxRenderMs = 0.0;
        maxSimMs = 0.0;
    }

    private static double round(double value) {
        return Math.round(value * 1000.0) / 1000.0;
    }
}
