package com.example.client;

import com.example.client.mixin.RenderSetupAccessor;
import com.example.client.mixin.RenderTypeAccessor;
import com.example.client.mixin.TextureBindingAccessor;
import com.example.vulkan.NativeVulkan;
import com.mojang.renderpearl.api.pipeline.RenderPipeline;
import com.mojang.renderpearl.api.textures.GpuSampler;
import com.mojang.renderpearl.api.textures.GpuTextureView;
import com.mojang.blaze3d.vertex.PoseStack;
import com.mojang.blaze3d.vertex.VertexConsumer;
import com.mojang.math.Axis;
import com.example.client.config.RTXSettingsConfig;
import com.mojang.renderpearl.backend.vulkan.VulkanGpuSampler;
import com.mojang.renderpearl.backend.vulkan.VulkanGpuTextureView;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;
import java.util.Collections;
import java.util.IdentityHashMap;
import java.util.Locale;
import java.util.Set;
import net.minecraft.client.gui.Font;
import net.minecraft.client.gui.font.TextRenderable;
import net.minecraft.client.model.Model;
import net.minecraft.client.renderer.blockentity.BeaconRenderer;
import net.minecraft.client.renderer.blockentity.state.BeaconRenderState;
import net.minecraft.client.renderer.entity.state.AvatarRenderState;
import net.minecraft.client.renderer.feature.ItemFeatureRenderer;
import net.minecraft.client.renderer.feature.ModelFeatureRenderer;
import net.minecraft.client.renderer.RenderPipelines;
import net.minecraft.client.renderer.rendertype.RenderSetup;
import net.minecraft.client.renderer.rendertype.RenderType;
import net.minecraft.client.renderer.texture.AbstractTexture;
import net.minecraft.client.Minecraft;
import net.minecraft.resources.Identifier;
import net.minecraft.server.packs.resources.ResourceManager;
import net.minecraft.util.RandomSource;
import org.joml.Matrix4f;
import org.lwjgl.system.MemoryUtil;

/** Captures 26.2 entity-model batches and the explicit experience-orb quad. */
public final class EntityRtCapture {
    private static final int MAX_TEXTURE_SLOTS = 128;
    private static final int MAX_REGULAR_TEXTURE_SLOTS = MAX_TEXTURE_SLOTS - 1;
    private static final int BYTES_PER_VERTEX = 12 * Float.BYTES;
    private static final Identifier EXPERIENCE_ORB_TEXTURE =
            Identifier.withDefaultNamespace("textures/entity/experience/experience_orb.png");
    private static final Identifier LIGHTNING_TEXTURE =
            Identifier.fromNamespaceAndPath("orion", "textures/rt_lightning.png");
    private static final Identifier[] frameTextureSlots = new Identifier[MAX_TEXTURE_SLOTS];
    private static final String[] frameTextureLabels = new String[MAX_TEXTURE_SLOTS];
    private static int frameTextureSlotCount;
    private static final int[] slotLastSeenFrame = new int[MAX_TEXTURE_SLOTS];
    private static final float MATERIAL_CLASS_SCALE = 32.0f;
    private static final int MATERIAL_CLASS_DEFAULT = 0;
    private static final int MATERIAL_CLASS_IRON = 1;
    private static final int MATERIAL_CLASS_GOLD = 2;
    private static final int MATERIAL_CLASS_DIAMOND = 3;
    private static final int MATERIAL_CLASS_NETHERITE = 4;
    private static final int MATERIAL_CLASS_COPPER = 5;
    private static final int MATERIAL_CLASS_SOFT = 6;
    

    private static final int MATERIAL_CLASS_UNCUTOUT = 7;
    private static final int[] frameTextureClasses = new int[MAX_TEXTURE_SLOTS];
    private static int captureFrameSerial;
    private static final int SLOT_EVICTION_GRACE_FRAMES = 3;
    // 2026-11-15：层间距对齐 Caustica 的 ORDER_OFFSET = 2e-4（原 0.002 会让 16 层图案悬空 3cm）。
    private static final float ENTITY_COPLANAR_LAYER_STEP = 2.0e-4f;
    private static final float ENTITY_COPLANAR_LAYER_MAX = 0.02f;
    // 2026-11-15：同一次 submit 内，同一面旗帜的所有图案层共用同一个 Pose；按"同 Pose 第几次
    // banner_pattern 提交"还原原版 order（底色层 1、其后每层 +1）。每帧 beginFrame 清零。
    private static final java.util.HashMap<Long, Integer> bannerPatternLayerRanks = new java.util.HashMap<>();
    // 2026-11-16：[RTX-BANNER] 只打一次的开关，用来在运行时确认旗帜层序路径确实命中
    // （isBannerPattern 的判据不能再靠推断，见该方法里的注释）。
    private static boolean bannerPipelineLogged;
    // 2026-11-15：短格式顶点（地图）的拼接状态。逐顶点热路径要求零分配，
    // 且区块网格构建线程也会调 BufferBuilder 的短格式 setter，所以必须按线程隔离。
    private static final int POSITION_ONLY_MAX_VERTICES = 8192;
    private static boolean positionOnlyOverflowLogged;
    // 地图批次诊断（[RTX-MAP]）的每帧条数限制。
    private static int mapDiagFrame = -1;
    private static int mapDiagCount;
    // 地图批次探针的上次打印帧。刻意**不**接 isDiagnosticFrame()：
    // 那个条件是 frame%64==1，若再叠加 %90 就等于每 2880 帧才打一次（≈48 秒），
    // 实测等于永远看不到。
    private static int mapProbeFrame = -1;
    private static final ThreadLocal<ShortVertex> shortVertex =
            ThreadLocal.withInitial(ShortVertex::new);
    // 短格式顶点"半截状态"是否存在的保守前置过滤：全格式 addVertex 是全场最热的路径，
    // 不能为了丢一个残留状态就付一次 ThreadLocal.get()（和 ActiveBatchSlot.current 同理，
    // 只作普通字段读写；跨线程读到过期 true 最多让对方多做一次空操作）。
    private static boolean shortVertexPendingFlag;
    private static final long[] publishedEntityViews = new long[MAX_TEXTURE_SLOTS];
    private static final long[] publishedEntitySamplers = new long[MAX_TEXTURE_SLOTS];
    private static final boolean[] slotTexturePublished = new boolean[MAX_TEXTURE_SLOTS];
    private static final java.util.HashMap<Identifier, Identifier[]> ENTITY_PBR_SIBLINGS =
            new java.util.HashMap<>();
    private static final long[] publishedEntityNormalViews = new long[MAX_TEXTURE_SLOTS];
    private static final long[] publishedEntityNormalSamplers = new long[MAX_TEXTURE_SLOTS];
    private static final long[] publishedEntitySpecularViews = new long[MAX_TEXTURE_SLOTS];
    private static final long[] publishedEntitySpecularSamplers = new long[MAX_TEXTURE_SLOTS];
    private static final boolean[] entityPbrSettled = new boolean[MAX_TEXTURE_SLOTS];
    private static boolean entityPbrNativeAvailable = true;
    // 2026-11-09：包一层，只为在"没有任何捕获窗口打开"时用一个 volatile 标志短路。
    // BufferBuilderMixin 对**每一个**顶点都调 captureVertex，而绝大多数顶点
    // （地形/天空/UI 等非实体几何）与 RT 实体捕获无关，之前每个顶点都要付一次
    // ThreadLocal.get()（ThreadLocalMap 哈希查表 + 首次建表）。
    // 语义不变：标志只是"可能有人开着窗口"的保守前置过滤，真正有没有批次仍由
    // ThreadLocal 决定；其它线程读到过期 false 时本来 get() 也是 null。
    private static final ActiveBatchSlot activeBatch = new ActiveBatchSlot();
    private static volatile boolean anyActiveBatch;

    private static final class ActiveBatchSlot {
        private final ThreadLocal<ActiveBatch> local = new ThreadLocal<>();

        /**
         * 2026-11-11：渲染线程的当前批次直读镜像。
         *
         * <p>实体捕获全部发生在渲染线程（{@code prepareModel} / RenderType 通用链），
         * 所以逐顶点热路径不必走 {@code ThreadLocal.get()}（哈希查表）。
         * {@link #captureVertex} 先看 {@code anyActiveBatch} 这个 volatile 门，
         * 命中后再读这个普通静态字段 —— 语义与原来一致（其它线程看到的批次永远为 null）。
         */
        private static ActiveBatch current;

        /**
         * {@link #current} 的归属线程。
         *
         * <p>⚠ 2026-11-11 教训：区块网格构建的**工作线程也会调
         * {@code captureVertex}**。只放一个全局静态镜像时，它们会看到渲染线程正开着的
         * 批次并往里塞顶点，把 {@code pendingQuadVertexCount} 顶过 4 ⇒
         * {@code ArrayIndexOutOfBoundsException: Index 20 out of bounds for length 20}
         * （崩溃报告 {@code crash-2026-11-11_20.47.52-client.txt}）。
         * 所以镜像必须带归属校验，不匹配就走原来的 ThreadLocal（对工作线程为 null）。
         */
        private static Thread owner;

        ActiveBatch get() {
            return local.get();
        }

        void set(ActiveBatch batch) {
            local.set(batch);
            current = batch;
            owner = batch == null ? null : Thread.currentThread();
            anyActiveBatch = batch != null;
        }

        void remove() {
            local.remove();
            current = null;
            owner = null;
            anyActiveBatch = false;
        }
    }
    private static final ThreadLocal<ActiveBatch> reusableBatch =
            ThreadLocal.withInitial(ActiveBatch::new);
    private static final Set<Object> localPlayerProxyStates =
            Collections.newSetFromMap(new IdentityHashMap<>());
    private static boolean localPlayerProxyAvatarPending;
    private static Identifier localPlayerProxyBodyTexture;
    private static final ThreadLocal<ActiveBatch> textBatch = ThreadLocal.withInitial(ActiveBatch::new);
    private static int textBatchSlot;
    private static boolean captureFrameActive;

    private static RenderType lastGlyphRenderType;
    private static int lastGlyphSlot;

    private static boolean textPerfFrame;
    private static long textCaptureNanos;
    private static long textUploadNanos;
    private static long textLookupNanos;
    private static int textGlyphs;
    private static int textVertices;
    private static int textBytes;
    private static int textUploads;
    private static long textEmitNanos;
    private static long textGapNanos;
    private static long lastGlyphHeadNanos;
    private static long lastGlyphTailNanos;

    

    public static final boolean SKIP_VANILLA_WORLD_TEXT = resolveSkipVanillaWorldText();

    private static boolean resolveSkipVanillaWorldText() {
        String raw = System.getProperty("orion.skipVanillaWorldText");
        if (raw == null) {
            raw = System.getenv("ORION_SKIP_VANILLA_WORLD_TEXT");
        }
        return raw == null || !"0".equalsIgnoreCase(raw.trim());
    }

    private static long textPathNanos;
    private static long lastGlyphEntryNanos;

    private static long evictionsInUseTotal;

    private static int textSlotChanges;
    private static int evictionsInUse;
    private static final java.util.HashSet<Identifier> textFrameTextures =
            new java.util.HashSet<>();

    private EntityRtCapture() {}

    private static boolean entityFrameOpen;

    /**
     * 每帧复位开窗标记，由 {@code GameRendererMixin} 在 {@code GameRenderer.render}
     * 的 HEAD 调用。它只清 Java 侧标记，不碰原生状态：窗口现在可能由手部提前打开、
     * 由 {@code LevelRenderer} 收帧，中间若因为关卡未渲染而缺了收帧那一步，标记会一直
     * 挂着 —— 那会导致之后手部再也不开窗（手又消失）。这里保证每帧重新开始。
     */
    public static void resetEntityFrameOpen() {
        entityFrameOpen = false;
    }

    /**
     * 打开实体帧窗口，**幂等**（每帧恰恰一次原生 {@code beginEntityFrame()}）。
     *
     * <p>⚠ 2026-10-25 修正（旧注释与 26.3 源码不符）：26.3 的实际顺序是
     * {@code GameRenderer.renderLevel()} → {@code levelRenderer.render(...)}（世界）
     * → {@code render3dHud(...)} → {@code renderItemInHand(...)}（第一人称手/手持物）。
     * 也就是说<b>手在世界之后提交</b>，而不是旧注释写的"手在世界之前"。</p>
     *
     * <p>由此得到的真实故障（2026-10-25 现场核对）：旧代码在
     * {@code LevelRenderer.render} 的 RETURN 就调用 {@code endEntityFrame()}，
     * 而手的 feature 批次要到之后 {@code render3dHud} 里才 prepareGroup ⇒
     * Java 侧照常捕获（slot、LabPBR 都解析过），但 {@code NativeVulkan.uploadEntityBatch}
     * 撞上原生门禁 {@code if(!g_entityFrameOpen) return;} 被<b>静默丢弃</b>。
     * 结论：<b>手是有的（有几何、有材质），只是完全不参与光追</b>。</p>
     *
     * <p>所以窗口必须跨"世界 + 手"两段：世界开始时在这里打开，手提交完由
     * {@link #finishEntityFrame()} 收帧（见 {@code GameRendererMixin} 的 prepareFrame
     * 注入点）；第三人称/无 HUD 等没有手的帧由 {@code renderLevel} 的 RETURN 兜底。</p>
     */
    public static void beginEarlyFrame() {
        if (entityFrameOpen) {
            return;
        }
        ++entityMotionFrame;
        if (--entityMotionLogCountdown <= 0) {
            entityMotionLogCountdown = 600;
            pruneEntityMotionTrackers();
        }
        beginFrame();
        NativeVulkan.beginEntityFrame();
    }

    private static boolean handPhase;

    /**
     * 进入手部阶段：开窗 + 标记"这一段提交的是手"。
     *
     * <p>⚠ 2026-10-25：<b>没有调用者，是死代码，不要再接回来。</b> 原来调用它的
     * {@code FirstPersonHandsAndItemsRendererMixin} 已经从仓库里删除（grep 可证）。
     * 更关键的是这个标记的两条用途现在<b>都是错的</b>：</p>
     * <ul>
     *   <li>顶点旋转（{@link #captureVertex} 里的 {@code handRotation}）：26.3 的
     *       {@code GameRenderer.renderItemInHand} 已经把
     *       {@code viewRotationMatrix.invert()} 乘进 PoseStack（GameRenderer.java:384），
     *       顶点已是"世界朝向 + 相机相对"，与 ENTITY 层同口径 ⇒ 再旋一次会把
     *       手转到侧面/背后；</li>
     *   <li>代理豁免（{@link #isLocalPlayerProxyState}）：26.3 第一人称手臂走
     *       {@code submitModelPart}，state 是 {@code Unit.INSTANCE}（不是
     *       {@code AvatarRenderState}），本来就不会命中 id 回退，不需要豁免。</li>
     * </ul>
     * <p>因此手的帧窗口问题改由 {@link #beginEarlyFrame()} + {@link #finishEntityFrame()}
     * 的时序解决，与 handPhase 无关。</p>
     */
    public static void beginHandPhase() {
        if (!handPhaseLogged) {
            handPhaseLogged = true;
            System.out.println("[RTX-HAND] first-person hand capture window opened");
        }
        handPhase = true;
        beginEarlyFrame();
    }

    private static boolean handPhaseLogged;
    private static boolean endPortalLogged;
    private static boolean endPortalBatchLogged;
    private static boolean endPortalSyntheticLogged;
    private static boolean endPortalSyntheticSlotLogged;
    private static boolean lightningSyntheticLogged;
    private static boolean lightningSyntheticSlotLogged;
    private static boolean beaconSyntheticLogged;
    private static boolean beaconSyntheticSlotLogged;

    /** 手部阶段结束（世界渲染开始，由 {@code LevelRendererMixin} 调用）。 */
    public static void endHandPhase() {
        handPhase = false;
    }

    public static void beginFrame() {
        com.example.client.cull.EntityVisibilityCuller.tick();
        if (activeBatch.get() != null) throw new IllegalStateException("Entity model capture leaked across frames");
        ++captureFrameSerial;
        entityFrameOpen = true;
        textBatchSlot = 0;
        captureFrameActive = true;
        bannerPatternLayerRanks.clear();
        frameOwnerRecords.clear();
        frameHistoryOrdinals.clear();
        // 上一帧若在某条短格式顶点中途结束（异常/取消），残留的 pending 会串到本帧的批次里。
        cancelShortVertex();

        textPerfFrame = isDiagnosticFrame();
        lastGlyphRenderType = null;
        lastGlyphSlot = 0;
        textCaptureNanos = 0L;
        textUploadNanos = 0L;
        textLookupNanos = 0L;
        textGlyphs = 0;
        textVertices = 0;
        textBytes = 0;
        textUploads = 0;
        textSlotChanges = 0;
        evictionsInUse = 0;
        textPathNanos = 0L;
        lastGlyphEntryNanos = 0L;
        textFrameTextures.clear();
    }

    /**
     * Closes the Java-side capture window (text run flush + counters). The text
     * run is the one batch that is not bounded by a single model submit, so it
     * is flushed here rather than in endModel's caller.
     *
     * <p>⚠ 2026-10-25：不要直接调用它来收帧 —— 必须走 {@link #finishEntityFrame()}，
     * 那个方法才会连同 {@code NativeVulkan.endEntityFrame()} 一起收（原生门禁是
     * {@code g_entityFrameOpen}，只关 Java 标记会让手/实体几何继续被静默丢弃）。</p>
     */
    public static void endFrame() {
        captureFrameActive = false;
        entityFrameOpen = false;
        flushTextBatch();

        if (LOG_TEXT_PERF && textPerfFrame && (textGlyphs != 0 || textUploads != 0)) {
            System.out.println(
                    "[TEXT PERF]"
                            + " glyphs=" + textGlyphs
                            + " verts=" + textVertices
                            + " bytes=" + textBytes
                            + " uploads=" + textUploads
                            + " lookupUs=" + (textLookupNanos / 1000L)
                            + " captureUs=" + (textCaptureNanos / 1000L)
                            + " uploadUs=" + (textUploadNanos / 1000L)
                            + " textPathUs=" + (textPathNanos / 1000L)
                            + " tex=" + textFrameTextures.size()
                            + " changes=" + textSlotChanges
                            + " evictInUse=" + evictionsInUse
                            + " evictInUseTotal=" + evictionsInUseTotal
                            + " tableSize=" + frameTextureSlotCount
                            + " disabled=" + (TEXT_CAPTURE_ENABLED ? 0 : 1));
        }
    }

    /**
     * 2026-10-25：收实体帧 —— Java 标记 + 原生 {@code endEntityFrame()} 一起收，幂等。
     *
     * <p>调用时序（26.3）：</p>
     * <ol>
     *   <li>{@code LevelRenderer.render} HEAD → {@link #beginEarlyFrame()} 开窗（世界实体）；</li>
     *   <li>{@code GameRenderer.render3dHud → renderItemInHand}：手/手持物提交，
     *       {@code FeatureRenderDispatcher.prepareFrame} 返回后由 {@code GameRendererMixin}
     *       调用本方法 —— 此时手顶点已经进了实体批次，且<b>早于</b>
     *       {@code createRenderPass("Item in hand")}（RT 注入就发生在那个 render pass
     *       的 vkCmdBeginRendering 之前），所以同帧就能看到手，没有一帧延迟；</li>
     *   <li>第三人称 / 隐藏 HUD / 无手帧走不到上面那一步 ⇒
     *       {@code GameRenderer.renderLevel} 的 RETURN 兜底调用本方法。</li>
     * </ol>
     *
     * <p>为什么必须在这里收而不是在 {@code LevelRenderer.render} 的 RETURN：手晚于世界，
     * 提前收会把手的批次挡在原生门禁 {@code if(!g_entityFrameOpen) return;} 外面
     * —— 这正是 2026-10-25 之前"手完全不参与光追"的原因。</p>
     */
    public static void finishEntityFrame() {
        if (!entityFrameOpen) {
            return;
        }
        endFrame();
        NativeVulkan.endEntityFrame();
    }

    

    private static final boolean TEXT_CAPTURE_ENABLED = resolveTextCaptureEnabled();

    private static boolean resolveTextCaptureEnabled() {
        String raw = System.getProperty("orion.textCapture");
        if (raw == null) {
            raw = System.getenv("ORION_TEXT_CAPTURE");
        }
        return raw == null || !"0".equalsIgnoreCase(raw.trim());
    }

    /**
     * Mirrors one glyph quad emitted by TextFeatureRenderer into the entity batch.
     *
     * @return true = 本字形**已经**镜像进实体批次，原版那一份可以被跳过（见
     *         {@link #SKIP_VANILLA_WORLD_TEXT}）；false = 没有镜像成功，原版必须照旧画。
     */
    public static boolean captureTextGlyph(
            TextRenderable glyph,
            Matrix4f pose,
            int lightCoords,
            Font.DisplayMode displayMode) {
        if (!captureFrameActive) return false;

        if (textPerfFrame) {
            final long glyphEntryNanos = System.nanoTime();
            if (lastGlyphEntryNanos != 0L) {
                textPathNanos += glyphEntryNanos - lastGlyphEntryNanos;
            }
            lastGlyphEntryNanos = glyphEntryNanos;
        }

        if (!TEXT_CAPTURE_ENABLED) {
            textGlyphs += 1;
            return false;
        }

        RenderType renderType = glyph.renderType(displayMode);
        if (renderType == null) return false;

        int textureSlot;

        if (renderType == lastGlyphRenderType) {
            textureSlot = lastGlyphSlot;
        } else {
            final long textLookupStartNanos = textPerfFrame ? System.nanoTime() : 0L;

            Identifier texture = resolveSampler0(renderType);
            if (texture == null) return false;

            textureSlot = slotFor(texture);
            if (textureSlot == 0) return false;

            if (textPerfFrame) {
                textLookupNanos += System.nanoTime() - textLookupStartNanos;
                textFrameTextures.add(texture);
            }

            lastGlyphRenderType = renderType;
            lastGlyphSlot = textureSlot;
        }

        ActiveBatch batch = textBatch.get();
        if (textBatchSlot != 0 && textureSlot != textBatchSlot) {
            if (textPerfFrame) ++textSlotChanges;
            flushTextBatch();
        }
        if (textBatchSlot == 0) {
            batch.reset(textureSlot, false, false);
            textBatchSlot = textureSlot;
        }

        final long textStartNanos = textPerfFrame ? System.nanoTime() : 0L;
        final int textVerticesBefore = batch.vertexCount;

        CapturingVertexConsumer consumer = batch.captureConsumer();
        try {
            glyph.render(pose, consumer, lightCoords, false);
        } finally {
            consumer.finish();
        }

        textGlyphs += 1;
        textVertices += batch.vertexCount - textVerticesBefore;

        if (textPerfFrame) {
            textCaptureNanos += System.nanoTime() - textStartNanos;
        }

        return true;
    }

    private static void flushTextBatch() {
        if (textBatchSlot == 0) return;
        textBatchSlot = 0;
        ActiveBatch batch = textBatch.get();
        if (batch.vertexCount != 0) {
            final long textUploadStartNanos = textPerfFrame ? System.nanoTime() : 0L;

            uploadBatch(batch);

            if (textPerfFrame) {
                textUploadNanos += System.nanoTime() - textUploadStartNanos;
            }

            textUploads += 1;
            textBytes += batch.vertexCount * BYTES_PER_VERTEX;
        }
    }

    public static boolean isDiagnosticFrame() {
        return (captureFrameSerial & 63) == 1;
    }

    /**
     * A resource reload destroys Vulkan texture views while this class keeps
     * lightweight handle de-duplication state. The numeric handle may be
     * reused by Vulkan, so comparing it across reloads is not valid.
     */
    public static void invalidateTextureCache() {
        frameTextureSlotCount = 0;
        Arrays.fill(frameTextureSlots, null);
        Arrays.fill(frameTextureLabels, null);
        Arrays.fill(slotLastSeenFrame, -1);
        Arrays.fill(publishedEntityViews, 0L);
        Arrays.fill(publishedEntitySamplers, 0L);
        Arrays.fill(slotTexturePublished, false);
        ENTITY_PBR_SIBLINGS.clear();
        Arrays.fill(publishedEntityNormalViews, 0L);
        Arrays.fill(publishedEntityNormalSamplers, 0L);
        Arrays.fill(publishedEntitySpecularViews, 0L);
        Arrays.fill(publishedEntitySpecularSamplers, 0L);
        Arrays.fill(entityPbrSettled, false);
    }

    public static void beginModel(ModelFeatureRenderer.Submit<?> submit) {
        beginModel(submit, false);
    }

    public static void markLocalPlayerProxyState(Object state) {
        if (localPlayerProxyStates.size() >= 8) localPlayerProxyStates.clear();
        localPlayerProxyStates.add(state);
        localPlayerProxyAvatarPending = state instanceof AvatarRenderState;
        localPlayerProxyBodyTexture = null;
        if (state instanceof AvatarRenderState avatarState && avatarState.skin != null) {
            localPlayerProxyBodyTexture = avatarState.skin.body().texturePath();
        }
    }

    public static void disarmLocalPlayerProxyFallback() {
        localPlayerProxyAvatarPending = false;
        localPlayerProxyBodyTexture = null;
    }

    public static boolean isLocalPlayerProxyState(ModelFeatureRenderer.Submit<?> submit) {
        if (handPhase) return false;
        if (localPlayerProxyStates.contains(submit.state())) return true;
        if (!localPlayerProxyAvatarPending) return false;
        var player = Minecraft.getInstance().player;
        boolean idHit = player != null
                && submit.state() instanceof AvatarRenderState avatarState
                && avatarState.id == player.getId();
        if (isDiagnosticFrame() && idHit) {
            System.err.println("[RTX-PROXY] proxy recognized via id fallback (state copy)");
        }
        return idHit;
    }

    public static boolean isLocalPlayerProxyState(Object renderState) {
        return localPlayerProxyStates.contains(renderState);
    }

    /** Captures the local-player body before it reaches SubmitNodeCollector. */
    public static <S> void captureLocalPlayerModel(
            Model<? super S> model,
            S state,
            PoseStack poseStack,
            RenderType renderType,
            int lightCoords,
            int overlayCoords,
            int tintedColor) {
        Identifier texture = resolveSampler0(renderType);
        if (texture == null) {
            if (isDiagnosticFrame()) {
                System.err.println("[RTX-PROXY] captureLocalPlayerModel sampler0 resolve FAILED");
            }
            return;
        }

        publishTexture(MAX_TEXTURE_SLOTS, texture);
        ActiveBatch batch = acquireBatch(MAX_TEXTURE_SLOTS, true, false);
        activeBatch.set(batch);
        CapturingVertexConsumer captureConsumer = batch.captureConsumer();
        try {
            model.setupAnim(state);
            model.renderToBuffer(poseStack, captureConsumer, lightCoords, overlayCoords, tintedColor);
        } finally {
            captureConsumer.finish();
            endModel();
        }
    }

    public static void captureLocalPlayerProxy(
            ModelFeatureRenderer.Submit<?> submit, PoseStack poseStack) {
        beginModel(submit, true);
        ActiveBatch batch = activeBatch.get();
        if (batch == null) return;

        CapturingVertexConsumer captureConsumer = batch.captureConsumer();
        try {
            renderModelToCaptureConsumer(submit, poseStack, captureConsumer);
        } finally {
            captureConsumer.finish();
            endModel();
        }
    }

    private static <S> void renderModelToCaptureConsumer(
            ModelFeatureRenderer.Submit<S> submit,
            PoseStack poseStack,
            VertexConsumer captureConsumer) {
        Model<? super S> model = submit.model();
        model.setupAnim(submit.state());
        model.renderToBuffer(
                poseStack,
                captureConsumer,
                submit.lightCoords(),
                submit.overlayCoords(),
                submit.tintedColor()
        );
    }

    private static final java.util.HashMap<
            net.minecraft.world.entity.EntityType<?>, java.util.ArrayList<double[]>>
            entityMotionTrackers = new java.util.HashMap<>();

    /**
     * 本帧「EntityRenderState 实例 → 运动记录」。
     *
     * <p>为什么需要它：同一个实体在一帧里会被提交多次（实体本体 / 装备层 / 手持物，
     * 各自不同的 RenderType），这些批次必须共用同一条记录；而<b>不同</b>实体即使
     * 类型相同、位置挨得很近（鸡群、僵尸群、同一格里的掉落物），也绝不能互相借记录。
     * 旧实现用「本帧已更新的最近记录 + 8 格半径」做这个判断，两种情形分不开 ⇒
     * 相邻同类实体会拿到别人的位姿增量和**别人的顶点历史**，表现为鬼影 + 交替抖动。</p>
     *
     * <p>用 {@link java.util.IdentityHashMap} 以 state 对象身份为键，一帧一清。</p>
     */
    private static final java.util.IdentityHashMap<Object, double[]> frameOwnerRecords =
            new java.util.IdentityHashMap<>();

    

    private static final boolean LIMB_MV_ENABLED = resolveLimbMvEnabled();

    private static boolean resolveLimbMvEnabled() {
        String raw = System.getProperty("orion.limbMv");
        if (raw == null) {
            raw = System.getenv("ORION_LIMB_MV");
        }
        return raw == null || !"0".equalsIgnoreCase(raw.trim());
    }

    

    private static final java.util.HashMap<
            double[], java.util.HashMap<ModelBatchKey, VertexHistory>>
            entityVertexHistories = new java.util.HashMap<>();

    private static final int ENTITY_VERTEX_HISTORY_MAX_KEYS = 32;

    /**
     * 一批模型几何的稳定身份：同一实体的同一提交逐帧对应。
     *
     * <p>{@code ordinal} = 本实体本帧第几次提交同一个 (model, renderType)。
     * 少了它，同一模型 + 同一 RenderType 被提交多次时会共用一条历史 ——
     * 最典型的是旗帜：底色层和每个图案层都是 {@code BannerFlagModel} +
     * {@code bannerPattern}，后一层会把前一层的 p_prev 覆盖掉，于是每层都在
     * 拿"另一层"的上一帧顶点位置算 MV，得到一个恒定的假运动 ⇒ 旗帜闪烁/拖影。</p>
     */
    private record ModelBatchKey(Model<?> model, RenderType renderType, int ordinal) {
        ModelBatchKey(Model<?> model, RenderType renderType) {
            this(model, renderType, 0);
        }
    }

    /** 本帧「实体记录 → (model, renderType) → 已提交次数」，与 frameOwnerRecords 同生命周期。 */
    private static final java.util.IdentityHashMap<
            double[], java.util.HashMap<ModelBatchKey, int[]>>
            frameHistoryOrdinals = new java.util.IdentityHashMap<>();

    /** 一条 (模型, RenderType) 批次的上一帧顶点位置（相机相对，与顶点缓冲同序）。 */
    private static final class VertexHistory {
        private int frame = Integer.MIN_VALUE;
        private float[] positions;
    }

    private static int entityMotionFrame;

    private static final double ENTITY_MOTION_MATCH_RADIUS_SQ = 64.0;

    private static final int ENTITY_MOTION_MAX_AGE_FRAMES = 60;

    private static final int ENTITY_MOTION_MAX_TRACKERS_PER_TYPE = 192;

    private static int entityMotionLogCountdown;

    

    private static final int T_X = 0;
    private static final int T_Y = 1;
    private static final int T_Z = 2;
    private static final int T_FRAME = 3;
    private static final int T_MOTION = 4;
    private static final int T_DELTA = 7;
    private static final int T_POSE = 19;
    private static final int T_DELTA_VALID = 35;
    private static final int T_POSE_VALID = 36;
    private static final int T_LEN = 37;

    private static final ThreadLocal<float[]> poseScratch =
            ThreadLocal.withInitial(() -> new float[16]);

    private static double[] entityMotionTracker(double x, double y, double z) {
        final double[] record = new double[T_LEN];
        record[T_X] = x;
        record[T_Y] = y;
        record[T_Z] = z;
        record[T_FRAME] = entityMotionFrame;
        setIdentityDelta(record);
        entityVertexHistories.put(record, new java.util.HashMap<>());
        return record;
    }

    /** 把增量置成恒等，并标记"不可用"（消费端会退化成"这一批不动"）。 */
    private static void setIdentityDelta(double[] record) {
        record[T_DELTA + 0] = 1.0;
        record[T_DELTA + 1] = 0.0;
        record[T_DELTA + 2] = 0.0;
        record[T_DELTA + 3] = 0.0;
        record[T_DELTA + 4] = 0.0;
        record[T_DELTA + 5] = 1.0;
        record[T_DELTA + 6] = 0.0;
        record[T_DELTA + 7] = 0.0;
        record[T_DELTA + 8] = 0.0;
        record[T_DELTA + 9] = 0.0;
        record[T_DELTA + 10] = 1.0;
        record[T_DELTA + 11] = 0.0;
        record[T_DELTA_VALID] = 0.0;
    }

    /** 记录本帧根位姿，供下一帧算增量。 */
    private static void writePose(double[] record, Matrix4f pose) {
        if (pose == null) {
            record[T_POSE_VALID] = 0.0;
            return;
        }
        final float[] scratch = poseScratch.get();
        pose.get(scratch);
        for (int index = 0; index < 16; ++index) {
            record[T_POSE + index] = scratch[index];
        }
        record[T_POSE_VALID] = 1.0;
    }

    /**
     * delta = prevPose · curPose⁻¹，并写进 {@link #T_DELTA}。
     *
     * <p>为什么乘在左边的是 prevPose：顶点 {@code p} 现在是 {@code curPose · m}
     * （{@code m} = 模型局部坐标），它上一帧在 {@code prevPose · m}。
     * 所以 {@code p_prev = prevPose · curPose⁻¹ · p}，右乘 curPose 的逆把
     * "当前世界位置"先拉回模型局部，再用上一帧的位姿放回上一帧的世界。</p>
     *
     * <p>纯平移时这个式子退化成 {@code p + (t_prev − t_cur)}，与旧口径逐位同量。</p>
     */
    private static void writePoseDelta(double[] record, Matrix4f currentPose) {
        if (currentPose == null || record[T_POSE_VALID] == 0.0) {
            setIdentityDelta(record);
            return;
        }

        final float[] scratch = poseScratch.get();
        for (int index = 0; index < 16; ++index) {
            scratch[index] = (float) record[T_POSE + index];
        }

        final Matrix4f delta = new Matrix4f().set(scratch);
        delta.mul(new Matrix4f(currentPose).invert());

        // JOML 是列主序（mXY = 第 X 列、第 Y 行）⇒ 一行 r0 的三个线性分量是
        // m00 / m10 / m20，平移列是 m30 / m31 / m32。实测见交付文档「自证」。
        record[T_DELTA + 0] = delta.m00();
        record[T_DELTA + 1] = delta.m10();
        record[T_DELTA + 2] = delta.m20();
        record[T_DELTA + 3] = delta.m30();
        record[T_DELTA + 4] = delta.m01();
        record[T_DELTA + 5] = delta.m11();
        record[T_DELTA + 6] = delta.m21();
        record[T_DELTA + 7] = delta.m31();
        record[T_DELTA + 8] = delta.m02();
        record[T_DELTA + 9] = delta.m12();
        record[T_DELTA + 10] = delta.m22();
        record[T_DELTA + 11] = delta.m32();

        record[T_DELTA_VALID] = deltaIsIdentity(record) ? 0.0 : 1.0;
    }

    /**
     * 增量是否等价于恒等。判据刻意留 1e-6 的余量而不是严格相等：位姿矩阵每帧
     * 由 float 运算重建，静止实体也会剩下 1e-7 量级的抖动；不挡掉的话，
     * 每个静止实体每帧都会走"逐顶点算 MV"那条路（白花 CPU，且给 DLSS 喂噪声）。
     */
    private static boolean deltaIsIdentity(double[] record) {
        final double[] identity = {
                1.0, 0.0, 0.0, 0.0,
                0.0, 1.0, 0.0, 0.0,
                0.0, 0.0, 1.0, 0.0
        };
        for (int index = 0; index < 12; ++index) {
            if (Math.abs(record[T_DELTA + index] - identity[index]) > 1.0e-6) {
                return false;
            }
        }
        return true;
    }

    /** 把记录里的增量搬到批次上（同一实体的多个部件共用第一次算出的结果）。 */
    private static void applyRecordedMotion(ActiveBatch batch, double[] record) {
        for (int index = 0; index < 12; ++index) {
            batch.poseDelta[index] = (float) record[T_DELTA + index];
        }
        batch.hasPoseDelta = record[T_DELTA_VALID] != 0.0;
        batch.motionX = (float) record[T_MOTION];
        batch.motionY = (float) record[T_MOTION + 1];
        batch.motionZ = (float) record[T_MOTION + 2];
    }

    private static double entityMotionDistanceSq(double[] record, double x, double y, double z) {
        final double dx = record[T_X] - x;
        final double dy = record[T_Y] - y;
        final double dz = record[T_Z] - z;
        return dx * dx + dy * dy + dz * dz;
    }

    /**
     * 算出这一批的**根位姿增量**并挂到批次上。
     *
     * <p>世界空间是对的：rgen 里两次投影都用**上一帧**相机，所以"世界位移"进去之后
     * 得到的正是 DLSS 需要的那一项（总位移里属于相机的那一份由 DLSS 自己加）。
     * 相机跟随的物体（本地玩家自己的身体）自动得到"屏幕上不动"的正确结果。</p>
     *
     * <p>2026-10-17：返回值从"一个平移三元组"换成"一个 3×4 仿射增量"，逐顶点在
     * {@code uploadBatch} 里应用 ⇒ 旋转（转身 / 俯仰 / 展示框摆动）终于有 MV 了。
     * 平移口径不变，所以纯平移实体的结果与旧实现**数值等价**。</p>
     */
    private static void resolveBatchMotion(
            ActiveBatch batch, ModelFeatureRenderer.Submit<?> submit) {
        final Object stateObject = submit.state();

        if (!(stateObject
                instanceof net.minecraft.client.renderer.entity.state.EntityRenderState renderState)) {
            return;
        }

        final double x = renderState.x;
        final double y = renderState.y;
        final double z = renderState.z;

        final com.mojang.blaze3d.vertex.PoseStack.Pose rootPose = submit.pose();
        final Matrix4f currentPose = rootPose != null ? rootPose.pose() : null;

        final Object ownerKey = stateObject;
        final double[] owner = frameOwnerRecords.get(ownerKey);
        if (owner != null) {
            // 同一实体本帧的后续批次（装备层 / 手持物 / 其它 RenderType）：
            // 共用第一次算出的增量与顶点历史。身份判定，不做位置猜测。
            applyRecordedMotion(batch, owner);
            attachVertexHistory(batch, submit, owner);
            return;
        }

        final java.util.ArrayList<double[]> trackers =
                entityMotionTrackers.computeIfAbsent(
                        renderState.entityType, key -> new java.util.ArrayList<>());

        double[] candidate = null;
        double candidateDistance = Double.MAX_VALUE;

        // 只和「上一帧及更早」的记录配对。本帧已被更新的记录属于别的实体，
        // 把位置最近的那个当自己用会串 MV 与顶点历史（见 frameOwnerRecords 注释）。
        for (int index = 0; index < trackers.size(); ++index) {
            final double[] record = trackers.get(index);
            if ((int)record[T_FRAME] == entityMotionFrame) {
                continue;
            }
            final double distance = entityMotionDistanceSq(record, x, y, z);
            if (distance < candidateDistance) {
                candidateDistance = distance;
                candidate = record;
            }
        }

        if (candidate == null) {
            // 这个类型第一次出现：没有上一帧可配对，增量恒等（首帧本来也没有历史）。
            final double[] record = entityMotionTracker(x, y, z);
            writePose(record, currentPose);
            trackers.add(record);
            frameOwnerRecords.put(ownerKey, record);
            // 仍然挂上历史：本帧 uploadBatch 会把顶点位置记进去，下一帧就有 p_prev 了。
            attachVertexHistory(batch, submit, record);
            return;
        }

        final int age = entityMotionFrame - (int)candidate[T_FRAME];
        final boolean paired =
                age <= ENTITY_MOTION_MAX_AGE_FRAMES
                        && candidateDistance <= ENTITY_MOTION_MATCH_RADIUS_SQ;

        final float motionX = paired ? (float)(candidate[T_X] - x) : 0.0F;
        final float motionY = paired ? (float)(candidate[T_Y] - y) : 0.0F;
        final float motionZ = paired ? (float)(candidate[T_Z] - z) : 0.0F;

        if (paired) {
            writePoseDelta(candidate, currentPose);
        } else {
            setIdentityDelta(candidate);
        }

        candidate[T_X] = x;
        candidate[T_Y] = y;
        candidate[T_Z] = z;
        candidate[T_FRAME] = entityMotionFrame;
        candidate[T_MOTION + 0] = motionX;
        candidate[T_MOTION + 1] = motionY;
        candidate[T_MOTION + 2] = motionZ;
        writePose(candidate, currentPose);

        applyRecordedMotion(batch, candidate);
        attachVertexHistory(batch, submit, candidate);
        frameOwnerRecords.put(ownerKey, candidate);

        if (trackers.size() > ENTITY_MOTION_MAX_TRACKERS_PER_TYPE) {
            pruneEntityMotionTrackers();
        }
    }

    /**
     * 把该记录所属的顶点位置历史挂到批次上。没有历史（开关关闭 / 记录刚被 prune）
     * 时保持 null ⇒ uploadBatch 退回根位姿增量。
     */
    private static void attachVertexHistory(
            ActiveBatch batch, ModelFeatureRenderer.Submit<?> submit, double[] record) {
        if (!LIMB_MV_ENABLED) {
            return;
        }
        final java.util.HashMap<ModelBatchKey, VertexHistory> history =
                entityVertexHistories.get(record);
        if (history == null) {
            return;
        }
        batch.vertexHistory = history;
        final java.util.HashMap<ModelBatchKey, int[]> ordinals =
                frameHistoryOrdinals.computeIfAbsent(record, key -> new java.util.HashMap<>());
        final int[] counter = ordinals.computeIfAbsent(
                new ModelBatchKey(submit.model(), submit.renderType()), key -> new int[1]);
        // 同一实体本帧的第 N 次同 key 提交 → 历史条目 N，逐帧稳定对应。
        batch.vertexHistoryKey =
                new ModelBatchKey(submit.model(), submit.renderType(), counter[0]++);
    }

    /** 清掉太久没被匹配到的记录（以及随之变空的类型）。 */
    private static void pruneEntityMotionTrackers() {
        final java.util.Iterator<
                java.util.Map.Entry<
                        net.minecraft.world.entity.EntityType<?>,
                        java.util.ArrayList<double[]>>> types =
                entityMotionTrackers.entrySet().iterator();

        while (types.hasNext()) {
            final java.util.ArrayList<double[]> trackers = types.next().getValue();

            for (int index = trackers.size() - 1; index >= 0; --index) {
                final double[] record = trackers.get(index);
                if (entityMotionFrame - (int)record[T_FRAME] > ENTITY_MOTION_MAX_AGE_FRAMES) {
                    trackers.remove(index);
                    // 顶点位置历史与记录同生命周期，否则表会随实体消失一直涨。
                    entityVertexHistories.remove(record);
                }
            }

            if (trackers.isEmpty()) {
                types.remove();
            }
        }
    }

    private static void beginModel(
            ModelFeatureRenderer.Submit<?> submit,
            boolean localPlayerProxy) {
        if (!CAPTURE_ENABLED) return;
        ++modelSubmitCount;
        if (activeBatch.get() != null) throw new IllegalStateException("Nested entity model capture");
        Identifier texture = resolveSampler0(submit.renderType());
        if (handPhase) {
            if (!handTextureSubstituted) {
                handTextureSubstituted = true;
                System.out.println("[RTX-HAND] texture probe: original="
                        + texture + " -> substituted with blocks atlas");
            }
            texture = BLOCKS_ATLAS_TEXTURE;
        }
        if (localPlayerProxy && isLocalPlayerBodyTexture(submit, texture)) {
            int textureSlot = MAX_TEXTURE_SLOTS;
            publishTexture(textureSlot, texture);
            ActiveBatch proxyBatch = acquireBatch(textureSlot, true, false);
            proxyBatch.diagHand = false;
            // 2026-11-04：Submit 这个 record 自带 uvMapping（ModelPart 0..1 → 图集矩形）。
            proxyBatch.uvMapping = submit.uvMapping();
            activeBatch.set(proxyBatch);
            resolveBatchMotion(proxyBatch, submit);
            handDiagLog("PROXY-PATH", submit, texture, textureSlot);
            return;
        }
        int textureSlot = slotFor(texture);
        // 第一人称手部（手臂 + 手持物）的 state 是 Unit.INSTANCE，不是 AvatarRenderState，
        // 所以这里不能按 state 类型过滤，只能按"第一人称"这个相机状态判断。
        final boolean handBatch = isFirstPersonHandState(submit);
        if (handBatch) {
            handDiagLog("SLOT-PATH", submit, texture, textureSlot);
        }
        if (isDiagnosticFrame()
                && submit.state() instanceof AvatarRenderState avatarState) {
            var player = Minecraft.getInstance().player;
            if (player != null && avatarState.id == player.getId()) {
                System.err.println("[RTX-PROXY] ordinary path saw local player, firstPerson="
                        + Minecraft.getInstance().options.getCameraType().isFirstPerson()
                        + ", textureNull=" + (texture == null) + ", slot=" + textureSlot);
            }
        }
        if (textureSlot != 0) {
            ActiveBatch batch = acquireBatch(textureSlot, localPlayerProxy, false);
            batch.diagHand = handBatch;
            // 2026-11-04：见 ActivaBatch.uvMapping 处的说明。
            batch.uvMapping = submit.uvMapping();
            // 2026-11-15：旗帜图案层按 Caustica 的 order 递增层序（不再是固定 +2）。
            // 同一面旗帜的底布（非 banner_pattern）为 0，底色层为 1，其后每层 +1；
            // batching 会把每层拆成独立批次，所以 rank 必须跨批次累积（见 nextBannerLayerRank）。
            final boolean bannerPattern = isBannerPattern(submit.renderType());
            batch.coplanarLayerBoost = bannerPattern ? nextBannerLayerRank(submit.pose()) : 0;
            if (bannerPattern) {
                customDiagLog("banner_rank" + batch.coplanarLayerBoost,
                        renderTypeName(submit.renderType()), texture);
                if (!bannerPipelineLogged) {
                    bannerPipelineLogged = true;
                    final RenderPipeline pipeline = bannerPipeline(submit.renderType());
                    System.out.println("[RTX-BANNER] pattern layer hit, pipeline="
                            + (pipeline == null ? "?" : pipeline.getLocation())
                            + ", rank=" + batch.coplanarLayerBoost);
                }
            }
            activeBatch.set(batch);
            resolveBatchMotion(batch, submit);
        }
    }

    private static boolean isLocalPlayerBodyTexture(
            ModelFeatureRenderer.Submit<?> submit, Identifier texture) {
        if (texture == null || !(submit.state() instanceof AvatarRenderState avatarState)) return false;
        var player = Minecraft.getInstance().player;
        return player != null
                && avatarState.id == player.getId()
                && avatarState.skin != null
                && texture.equals(avatarState.skin.body().texturePath());
    }

    public static void endModel() {
        ActiveBatch batch = activeBatch.get();
        activeBatch.remove();
        if (batch == null || batch.vertexCount == 0) {
            if (isDiagnosticFrame() && batch != null && batch.localPlayerProxy) {
                System.err.println("[RTX-PROXY] proxy batch EMPTY, slot=" + batch.textureSlot);
            }
            return;
        }
        uploadBatch(batch);
        if (batch.localPlayerProxy && isDiagnosticFrame()) {
            System.err.println("[RTX-PROXY] proxy upload slot=" + batch.textureSlot
                    + " vertices=" + batch.vertexCount);
        }
        if (handPhase && !handBatchLogged) {
            handBatchLogged = true;
            Minecraft minecraft = Minecraft.getInstance();
            final float cx = (batch.minX + batch.maxX) * 0.5f;
            final float cy = (batch.minY + batch.maxY) * 0.5f;
            final float cz = (batch.minZ + batch.maxZ) * 0.5f;
            double yaw = 0.0;
            double pitch = 0.0;
            if (minecraft.player != null) {
                yaw = Math.toRadians(minecraft.player.getYRot());
                pitch = Math.toRadians(minecraft.player.getXRot());
            }
            final double lx = -Math.sin(yaw) * Math.cos(pitch);
            final double ly = -Math.sin(pitch);
            final double lz = Math.cos(yaw) * Math.cos(pitch);
            final double dist = Math.sqrt(cx * cx + cy * cy + cz * cz);
            final double along = cx * lx + cy * ly + cz * lz;
            final double offAxis = dist > 1.0e-4
                    ? Math.toDegrees(Math.acos(Math.max(-1.0, Math.min(1.0, along / dist))))
                    : -1.0;
            final float rx = handRotation[0] * cx + handRotation[4] * cy + handRotation[8] * cz;
            final float ry = handRotation[1] * cx + handRotation[5] * cy + handRotation[9] * cz;
            final float rz = handRotation[2] * cx + handRotation[6] * cy + handRotation[10] * cz;
            final double alongM = rx * lx + ry * ly + rz * lz;
            final float sx = handRotation[0] * cx + handRotation[1] * cy + handRotation[2] * cz;
            final float sy = handRotation[4] * cx + handRotation[5] * cy + handRotation[6] * cz;
            final float sz = handRotation[8] * cx + handRotation[9] * cy + handRotation[10] * cz;
            final double alongT = sx * lx + sy * ly + sz * lz;
            handTransformMode = (alongM > along + 0.05 && alongM >= alongT)
                    ? 1
                    : (alongT > along + 0.05 ? 2 : 0);
            System.out.println("[RTX-HAND] batch slot=" + batch.textureSlot
                    + " verts=" + batch.vertexCount
                    + " center=[" + cx + "," + cy + "," + cz + "]"
                    + " dist=" + dist
                    + " alongRaw=" + along
                    + " alongM=" + alongM
                    + " alongMt=" + alongT
                    + " offAxisDeg=" + offAxis
                    + " transformMode=" + handTransformMode);
        }
    }

    private static boolean handBatchLogged;
    private static boolean vertexColorLogged;
    private static boolean handTextureSubstituted;

    // 2026-11-03 手部诊断：第一人称手臂 / 手持物到底走哪条上传路径。
    // 目的：区分"手臂走 proxy 路径（slot=128 皮肤纹理，mask 0x02 主射线不可见）"
    // 与"手持物走 slotFor 普通路径（自己的贴图，mask 0xff 可见）"。
    // 每 64 帧（isDiagnosticFrame）打印一次，一次运行就能定性。
    private static long handDiagFrame = -1L;
    private static int handDiagCount;

    private static void handDiagLog(
            String where,
            ModelFeatureRenderer.Submit<?> submit,
            Identifier texture,
            int slot) {
        if (!isDiagnosticFrame()) return;
        if (handDiagFrame != captureFrameSerial) {
            handDiagFrame = captureFrameSerial;
            handDiagCount = 0;
        }
        // 一帧最多打 12 条，够看清手部由哪几个批次组成，又不会刷屏。
        if (handDiagCount >= 12) return;
        ++handDiagCount;
        final Object state = submit.state();
        System.err.println("[RTX-HANDDIAG] " + where
                + " state=" + (state == null ? "null" : state.getClass().getSimpleName())
                + " texture=" + texture
                + " slot=" + slot
                + " renderType=" + submit.renderType()
                + " firstPerson="
                + Minecraft.getInstance().options.getCameraType().isFirstPerson());
    }

    /**
     * 2026-11-03：第一人称手部批次的判据。26.3 的手臂/手持物走
     * {@code submitModelPart}，state 是 {@code Unit.INSTANCE}（不是 AvatarRenderState），
     * 所以只能按类名判。
     */
    private static boolean isFirstPersonHandState(ModelFeatureRenderer.Submit<?> submit) {
        if (!Minecraft.getInstance().options.getCameraType().isFirstPerson()) return false;
        final Object state = submit.state();
        return state != null && "Unit".equals(state.getClass().getSimpleName());
    }

    /** 取小数部分并夹到 [0,1)：末地门合成 UV 用（顶点在整数角点上，见 captureVertex）。 */
    private static float fract01(float value) {
        final float f = value - (float) Math.floor(value);
        return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    }

    private static final Identifier BLOCKS_ATLAS_TEXTURE =
            Identifier.withDefaultNamespace("textures/atlas/blocks.png");

    

    private static int handTransformMode = -1;

    

    private static final float[] handRotation = new float[] {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
    };

    // =====================================================
    // 2026-11-03 手部 MV：上一帧的相机状态。
    //
    // 手部几何是"相机相对"的（GameRenderer.renderItemInHand 已把
    // viewRotationMatrix.invert() 乘进 PoseStack），所以它每帧的世界位置变化
    // 完全由相机状态决定：
    //     p_prev = R_prev·R_cur⁻¹·p_cur + (pos_prev − R_prev·R_cur⁻¹·pos_cur)
    // 这两个 setter 每帧各调一次（VulkanCameraMixin:300-303），所以在写入本帧值
    // 之前把旧值存下来，就得到干净的上一帧。
    // =====================================================
    private static final float[] prevHandRotation = new float[] {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
    };
    private static final float[] handMotionMatrix = new float[9];
    private static float prevCameraWorldX;
    private static float prevCameraWorldY;
    private static float prevCameraWorldZ;
    private static boolean prevCameraRotationValid;
    private static boolean cameraRotationSeen;

    /** A/B 开关：-DORION_HAND_CAMERA_MV=0 关掉手部相机相对 MV。 */
    private static final boolean HAND_CAMERA_MV = resolveHandCameraMv();

    private static boolean resolveHandCameraMv() {
        String raw = System.getProperty("orion.handCameraMv");
        if (raw == null) raw = System.getenv("ORION_HAND_CAMERA_MV");
        return raw == null || !"0".equalsIgnoreCase(raw.trim());
    }

    /**
     * 2026-11-15：UV 重映射**默认关闭**。
     *
     * <p>26.3 的 {@code ModelFeatureRenderer.renderModel} 在把 buffer 交给 ModelPart 之前，
     * 已经用 {@code submit.uvMapping().wrap(buffer)}（{@code SpriteCoordinateExpander}）
     * 映射过一遍，连 full-path {@code addVertex(FFFIFFIIFFF)} 的 u/v 也在内部映射
     * （见 SpriteCoordinateExpander#addVertex）。所以 captureVertex 里再映射一次就是
     * **第二次**，会压到 sprite 一角，正是旗帜图案/箱子/告示牌贴图错乱的原因。
     *
     * <p>captureVertex 里那一支只保留作诊断：-DORION_ENTITY_UV_REMAP=1 可复现双重映射。
     */
    private static final boolean UV_REMAP_ENABLED = resolveUvRemapEnabled();

    private static boolean resolveUvRemapEnabled() {
        String raw = System.getProperty("orion.entityUvRemap");
        if (raw == null) raw = System.getenv("ORION_ENTITY_UV_REMAP");
        if (raw == null) return false;
        return !"0".equalsIgnoreCase(raw.trim());
    }

    public static void setHandRotation(float[] columnMajor) {
        if (columnMajor == null || columnMajor.length < 16) return;
        System.arraycopy(handRotation, 0, prevHandRotation, 0, 16);
        System.arraycopy(columnMajor, 0, handRotation, 0, 16);
        prevCameraRotationValid = cameraRotationSeen;
        cameraRotationSeen = true;
    }

    

    private static float cameraWorldX;
    private static float cameraWorldY;
    private static float cameraWorldZ;

    public static void setCameraOrigin(float x, float y, float z) {
        prevCameraWorldX = cameraWorldX;
        prevCameraWorldY = cameraWorldY;
        prevCameraWorldZ = cameraWorldZ;
        cameraWorldX = x;
        cameraWorldY = y;
        cameraWorldZ = z;
    }

    // ------------------------------------------------------------------
    // 2026-11-19 rasterLod：本引擎相机逆矩阵缓存（与 setCameraOrigin 同源：
    // VulkanCameraMixin 在 NativeVulkan.updateCamera 之后写入）。供
    // compat.dh.DhFarFieldGeometry 在 mode=14 时构造本引擎 inverse MVP。
    // 列主序 float[16]，与 CameraUBO 同约定。仅渲染线程读写。
    // ------------------------------------------------------------------
    private static volatile float[] engineCameraViewInverse;
    private static volatile float[] engineCameraProjInverse;

    public static void setEngineCameraMatrices(float[] viewInverse, float[] projInverse) {
        if (viewInverse == null || projInverse == null
                || viewInverse.length < 16 || projInverse.length < 16) {
            return;
        }
        // 2026-11-19 NaN 守卫：首帧相机矩阵未就绪时 VulkanCameraMixin 可能写入 NaN
        // （实测 v0/v5=NaN 被首帧锁进载荷）。非 finite 一律拒收，保留旧值（或 null）。
        for (int i = 0; i < 16; i++) {
            if (!Float.isFinite(viewInverse[i]) || !Float.isFinite(projInverse[i])) {
                return;
            }
        }
        float[] viewCopy = new float[16];
        float[] projCopy = new float[16];
        System.arraycopy(viewInverse, 0, viewCopy, 0, 16);
        System.arraycopy(projInverse, 0, projCopy, 0, 16);
        engineCameraViewInverse = viewCopy;
        engineCameraProjInverse = projCopy;
        // 2026-11-19 mode 14/15 推送挂点：引擎相机是每帧权威入口；DH 的
        // before-render 事件会在 DH 渲染停摆时停发（实测进世界 21 秒后整局
        // 无推送 ⇒ C++ matrixValid=false ⇒ 远场永远 stage-1 裸色）。回调由
        // DhFarFieldGeometry 注册（compat.dh 包私有，经本类公共桥接）。
        Runnable cameraCallback = engineCameraCallback;
        if (cameraCallback != null) {
            try {
                cameraCallback.run();
            } catch (Throwable ignored) {
            }
        }
    }

    public static float[] getEngineCameraViewInverse() {
        return engineCameraViewInverse;
    }

    public static float[] getEngineCameraProjInverse() {
        return engineCameraProjInverse;
    }

    // 2026-11-19 mode 14/15 推送回调（DhFarFieldGeometry 注册，见上）。
    private static volatile Runnable engineCameraCallback;

    public static void setEngineCameraCallback(Runnable callback) {
        engineCameraCallback = callback;
    }

    /**
     * 把一整批模型几何沿「质心 → 相机」方向外移 rank 个层间距。
     *
     * <p>参数与短格式路径 {@code applyCoplanarLayerOffset()} 完全一致：
     * {@link #ENTITY_COPLANAR_LAYER_STEP}、{@link #ENTITY_COPLANAR_LAYER_MAX}。
     * 顶点是相机相对坐标（native 侧才加 cameraWorld），所以相机在原点，
     * 方向就是 {@code -normalize(centroid)}。整批用同一个方向，避免把 quad 推歪。</p>
     */
    private static void applyBannerLayerOffset(ActiveBatch batch) {
        final int vertexCount = batch.vertexCount;
        float centerX = 0.0f;
        float centerY = 0.0f;
        float centerZ = 0.0f;
        for (int vertex = 0; vertex < vertexCount; ++vertex) {
            final int base = vertex * BYTES_PER_VERTEX;
            centerX += batch.buffer.getFloat(base);
            centerY += batch.buffer.getFloat(base + 4);
            centerZ += batch.buffer.getFloat(base + 8);
        }
        final float inverse = 1.0f / vertexCount;
        centerX *= inverse;
        centerY *= inverse;
        centerZ *= inverse;
        final float toCamera = (float) Math.sqrt(
                centerX * centerX + centerY * centerY + centerZ * centerZ);
        if (toCamera <= 1.0e-4f) {
            return;
        }
        final float offset = Math.min(
                batch.coplanarLayerBoost * ENTITY_COPLANAR_LAYER_STEP,
                ENTITY_COPLANAR_LAYER_MAX);
        final float dx = -centerX / toCamera * offset;
        final float dy = -centerY / toCamera * offset;
        final float dz = -centerZ / toCamera * offset;
        for (int vertex = 0; vertex < vertexCount; ++vertex) {
            final int base = vertex * BYTES_PER_VERTEX;
            batch.buffer.putFloat(base, batch.buffer.getFloat(base) + dx);
            batch.buffer.putFloat(base + 4, batch.buffer.getFloat(base + 4) + dy);
            batch.buffer.putFloat(base + 8, batch.buffer.getFloat(base + 8) + dz);
        }
    }

    private static void uploadBatch(ActiveBatch batch) {
        batch.pendingQuadVertexCount = 0;
        batch.buffer.flip();
        // 2026-11-19：旗帜图案层的 order 必须落到几何上。
        // 在此之前 coplanarLayerBoost 只被短格式 quad 路径读取，模型批次从不经过
        // 那条路 ⇒ 底布 / 底色层 / 图案层在 RT 里仍然严格共面，谁被 BLAS 命中
        // 完全随机，表现就是"旗帜图案时有时无"。这里整批沿指向相机的方向外移。
        if (batch.coplanarLayerBoost > 0 && batch.vertexCount > 0) {
            applyBannerLayerOffset(batch);
        }
        if (!endPortalBatchLogged && batch.textureSlot >= 1
                && batch.textureSlot <= MAX_TEXTURE_SLOTS) {
            final String endPortalLabel = frameTextureLabels[batch.textureSlot - 1];
            if (endPortalLabel != null && endPortalLabel.contains("end_portal")) {
                endPortalBatchLogged = true;
                System.out.println("[RTX-ENTITY] end portal batch upload slot="
                        + batch.textureSlot + " vertices=" + batch.vertexCount
                        + " label=" + endPortalLabel);
            }
        }
        // =====================================================
        // 2026-11-03：第一人称手部（手臂 + 手持物）的运动矢量。
        //
        // resolveBatchMotion 对手臂直接 return（它的 state 是 Unit，不是
        // EntityRenderState），手持物走 custom 路径压根不调那个函数 ⇒ 两条路径
        // 都拿不到 motion ⇒ MV 恒为 0。
        //
        // 而手部是"相机相对"的：转视角/走动时它的世界位置每帧都在变。MV=0 等于告诉
        // DLSS"这个表面相对世界静止"，DLSS 于是按相机运动去重投影它 —— 但手明明
        // 没跟着动，重投影落到错误位置，表现为闪动 + 鬼影。
        //
        // 这里用相机矩阵直接构造 poseDelta（3×4 行主序，p_prev = M·p_cur + t）：
        //     M = R_prev·R_cur⁻¹      t = pos_prev − M·pos_cur
        // handRotation 存的是相机旋转的**逆**（column-major），所以 R = (R⁻¹)ᵀ，
        // 即 R_ij = inv[i*4+j]。
        // A/B：-DORION_HAND_CAMERA_MV=0 关闭。
        // =====================================================
        if (HAND_CAMERA_MV && batch.diagHand && !batch.hasPoseDelta
                && prevCameraRotationValid) {
            final float[] hp = prevHandRotation;
            final float[] hc = handRotation;
            final float[] m = handMotionMatrix;
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    float sum = 0.0f;
                    for (int k = 0; k < 3; ++k) {
                        sum += hp[i * 4 + k] * hc[j * 4 + k];
                    }
                    m[i * 3 + j] = sum;
                }
            }
            final float tx = prevCameraWorldX
                    - (m[0] * cameraWorldX + m[1] * cameraWorldY + m[2] * cameraWorldZ);
            final float ty = prevCameraWorldY
                    - (m[3] * cameraWorldX + m[4] * cameraWorldY + m[5] * cameraWorldZ);
            final float tz = prevCameraWorldZ
                    - (m[6] * cameraWorldX + m[7] * cameraWorldY + m[8] * cameraWorldZ);
            batch.poseDelta[0] = m[0];
            batch.poseDelta[1] = m[1];
            batch.poseDelta[2] = m[2];
            batch.poseDelta[3] = tx;
            batch.poseDelta[4] = m[3];
            batch.poseDelta[5] = m[4];
            batch.poseDelta[6] = m[5];
            batch.poseDelta[7] = ty;
            batch.poseDelta[8] = m[6];
            batch.poseDelta[9] = m[7];
            batch.poseDelta[10] = m[8];
            batch.poseDelta[11] = tz;
            batch.hasPoseDelta = true;
        }

        long motionPointer = 0L;
        final long motionStartNanos =
                com.example.client.LevelPhaseProbe.enabled() ? System.nanoTime() : 0L;
        if (batch.vertexCount > 0) {
            final int motionFloats = batch.vertexCount * 3;
            final boolean useVertexHistory = batch.vertexHistoryIsPreviousFrame(motionFloats);
            final float[] positionStore = batch.vertexHistoryPositions(motionFloats);
            final boolean needMotion = useVertexHistory || batch.hasPoseDelta;
            if (needMotion || positionStore != null) {
                final ByteBuffer motion = needMotion ? batch.motionScratchFor(motionFloats) : null;
                final ByteBuffer positions = batch.buffer;
                final float[] delta = batch.poseDelta;
                boolean anyMotion = false;
                for (int vertex = 0; vertex < batch.vertexCount; ++vertex) {
                    final int offset = vertex * BYTES_PER_VERTEX;
                    final float px = positions.getFloat(offset);
                    final float py = positions.getFloat(offset + 4);
                    final float pz = positions.getFloat(offset + 8);
                    if (needMotion) {
                        final float mx;
                        final float my;
                        final float mz;
                        if (useVertexHistory) {
                            final int motionIndex = vertex * 3;
                            mx = positionStore[motionIndex] - px;
                            my = positionStore[motionIndex + 1] - py;
                            mz = positionStore[motionIndex + 2] - pz;
                        } else {
                            mx = delta[0] * px + delta[1] * py + delta[2] * pz + delta[3] - px;
                            my = delta[4] * px + delta[5] * py + delta[6] * pz + delta[7] - py;
                            mz = delta[8] * px + delta[9] * py + delta[10] * pz + delta[11] - pz;
                        }
                        anyMotion |= mx != 0.0F || my != 0.0F || mz != 0.0F;
                        motion.putFloat(mx).putFloat(my).putFloat(mz);
                    }
                    if (positionStore != null) {
                        final int motionIndex = vertex * 3;
                        positionStore[motionIndex] = px;
                        positionStore[motionIndex + 1] = py;
                        positionStore[motionIndex + 2] = pz;
                    }
                }
                if (positionStore != null) {
                    batch.storeVertexHistory(positionStore);
                }
                if (needMotion && anyMotion) {
                    motion.flip();
                    motionPointer = MemoryUtil.memAddress(motion);
                }
            }
        }

        if (motionStartNanos != 0L) {
            com.example.client.LevelPhaseProbe.captureMotionAdd(
                    System.nanoTime() - motionStartNanos);
        }

        // 2026-11-04 诊断：旗帜批次的几何事实。
        // 要回答三个问题：
        //   1) 底布与图案层各自的顶点数（能否对上"同一面旗帜的几层"）；
        //   2) applyCoplanarLayerOffset 到底有没有命中（coplanarHits=0 ⇒ 共面判定从未触发）；
        //   3) 一次都没触发的话，说明各层质心并不重合 ⇒ 沿法线外移这条路从一开始就不成立。
        if (isDiagnosticFrame() && batch.textureSlot >= 1
                && batch.textureSlot <= MAX_TEXTURE_SLOTS) {
            final String bannerLabel = frameTextureLabels[batch.textureSlot - 1];
            if (bannerLabel != null && bannerLabel.contains("banner") && batch.vertexCount > 0) {
                // 2026-11-04：打印**整批的 UV 范围**（不是首顶点）。
                // 原版某个图案 PNG 是 64×64，图集由 entity/banner/ 下约 40 张拼成，
                // 所以一个格子的 UV 跨度应该是 64/图集宽（512 → 0.125）。
                // 如果实测跨度远小于这个值，就是 UV 被压缩（映射叠了两次）；
                // 如果约等于，说明 UV 是对的，问题在 alpha/材质通道。
                float uMin = Float.MAX_VALUE;
                float uMax = -Float.MAX_VALUE;
                float vMin = Float.MAX_VALUE;
                float vMax = -Float.MAX_VALUE;
                for (int vertex = 0; vertex < batch.vertexCount; ++vertex) {
                    final int base = vertex * BYTES_PER_VERTEX;
                    final float u = batch.buffer.getFloat(base + 3 * Float.BYTES);
                    final float v = batch.buffer.getFloat(base + 4 * Float.BYTES);
                    if (u < uMin) uMin = u;
                    if (u > uMax) uMax = u;
                    if (v < vMin) vMin = v;
                    if (v > vMax) vMax = v;
                }
                final float c0 = batch.buffer.getFloat(5 * Float.BYTES);
                final float c1 = batch.buffer.getFloat(6 * Float.BYTES);
                final float c2 = batch.buffer.getFloat(7 * Float.BYTES);
                final float c3 = batch.buffer.getFloat(8 * Float.BYTES);
                customDiagLog("banner_geom verts=" + batch.vertexCount
                        + " hits=" + batch.coplanarHits
                        + String.format(" u=[%.4f,%.4f](d=%.4f) v=[%.4f,%.4f](d=%.4f) rgba=%.3f,%.3f,%.3f,%.3f",
                                uMin, uMax, uMax - uMin, vMin, vMax, vMax - vMin, c0, c1, c2, c3),
                        bannerLabel, null);
            }
        }

        // 2026-11-03 诊断：第一人称手部批次的 UV 范围。用来判定这批几何到底是
        // "皮肤 UV 空间"（手臂，0..1 且贴合皮肤布局）还是"item/方块图集空间"
        // （手持物，UV 会带图集偏移），从而确认手持物是否被并进了皮肤纹理批次。
        final boolean mapProbeDue = batch.positionOnlyCapture && batch.vertexCount > 0
                && (mapProbeFrame < 0 || captureFrameSerial - mapProbeFrame >= 120);
        if (((batch.diagHand && isDiagnosticFrame()) || mapProbeDue) && batch.vertexCount > 0) {
            final ByteBuffer source = batch.buffer;
            float uMin = Float.MAX_VALUE;
            float uMax = -Float.MAX_VALUE;
            float vMin = Float.MAX_VALUE;
            float vMax = -Float.MAX_VALUE;
            for (int vertex = 0; vertex < batch.vertexCount; ++vertex) {
                final int base = vertex * BYTES_PER_VERTEX + 3 * Float.BYTES;
                final float u = source.getFloat(base);
                final float v = source.getFloat(base + Float.BYTES);
                if (u < uMin) uMin = u;
                if (u > uMax) uMax = u;
                if (v < vMin) vMin = v;
                if (v > vMax) vMax = v;
            }
            // 2026-11-11：默认**关闭**。这行每帧要打几十~上百条（766 个 section 的多层
            // 批次各一条），每条都做 String 拼接 + 一次 frameTextureLabels 查表，
            // 一局下来日志能到 30 MB。GC 日志显示年轻代一次就 845M->380M，
            // 随后触发 14 次 Concurrent Mark Cycle（中位 124ms）抢渲染线程 CPU —— 
            // 这是"走/站都卡"最可疑的分配源。需要时：ORION_LOG_HANDUV=1。
            // 地图探针限频：isDiagnosticFrame() 在诊断模式下可能每帧都成立，
            // 而地图批次每帧都有（手持 + 展示框），不限频会把日志刷爆。
            final boolean mapProbe = mapProbeDue;
            if (mapProbe) {
                mapProbeFrame = captureFrameSerial;
            }
            if (LOG_HANDUV || mapProbe) {
                final StringBuilder line = new StringBuilder(160);
                line.append("[RTX-MAP] slot=").append(batch.textureSlot)
                        .append(" vertices=").append(batch.vertexCount)
                        // rem = 收尾时还剩几个凑不满 quad 的顶点。地图本体应该是 4 的倍数、
                        // rem=0；rem≠0 说明短格式顶点的 addVertex/setUv 配对漏了。
                        .append(" rem=").append(batch.pendingQuadVertexCount)
                        .append(" u=[").append(uMin).append(',').append(uMax).append(']')
                        .append(" v=[").append(vMin).append(',').append(vMax).append(']')
                        .append(" texture=").append(frameTextureLabels[
                            Math.min(Math.max(batch.textureSlot - 1, 0), MAX_TEXTURE_SLOTS - 1)]);
                // 地图：把前 4 个顶点的 (x,y,z|u,v) 一起打出来。贴图错而几何对时，
                // 先确认这 4 个顶点是不是 (0,128)(128,128)(128,0)(0,0) 且 UV 一一对应
                // (0,1)(1,1)(1,0)(0,0) —— 对不上就是批次里混进了别的顶点（UV 错位）。
                if (batch.positionOnlyCapture) {
                    if (mapDiagFrame != captureFrameSerial) {
                        mapDiagFrame = captureFrameSerial;
                        mapDiagCount = 0;
                    }
                    final int probes = Math.min(batch.vertexCount, 8);
                    for (int vertex = 0; vertex < probes; ++vertex) {
                        final int base = vertex * BYTES_PER_VERTEX;
                        line.append(" v").append(vertex).append('=')
                                .append(source.getFloat(base)).append('/')
                                .append(source.getFloat(base + Float.BYTES)).append('/')
                                .append(source.getFloat(base + 2 * Float.BYTES)).append('|')
                                .append(source.getFloat(base + 3 * Float.BYTES)).append('/')
                                .append(source.getFloat(base + 4 * Float.BYTES));
                    }
                    ++mapDiagCount;
                    if (mapDiagCount > 4) {
                        line.setLength(0);
                    }
                }
                if (line.length() > 0) {
                    System.err.println(line);
                }
            }
        }

        // 2026-11-03 回退：曾把第一人称手部批次打到独立的 RT 层（pad1 = 2.0），
        // 由 native 建 SECTION_AS_CLASS_HAND + mask 0x04，目的是让手不投世界阴影。
        // 实测第一视角贴图错乱 —— 该层的顶点上传路径没有接通，instance 指向无效的
        // 顶点地址，读到的贴图槽是垃圾。整项已回退（rgen 的 cullMask 也已还原为
        // 0xff / 0x01）；native 侧新增的 HAND 层定义保留为死代码，不影响运行。
        final long uploadJniStartNanos =
                com.example.client.LevelPhaseProbe.enabled() ? System.nanoTime() : 0L;
        NativeVulkan.uploadEntityBatch(batch.textureSlot, MemoryUtil.memAddress(batch.buffer), batch.vertexCount, BYTES_PER_VERTEX, motionPointer);
        if (uploadJniStartNanos != 0L) {
            com.example.client.LevelPhaseProbe.captureJniAdd(
                    System.nanoTime() - uploadJniStartNanos);
        }
    }

    /**
     * 2026-11-15：短格式顶点的捕获入口（由 {@code BufferBuilderMixin} 的短格式钩子调用）。
     *
     * <p>背景：{@code MapRenderer} 用 {@code submitCustomGeometry} + 短格式
     * {@code addVertex(x,y,z).setColor(-1).setUv(u,v).setLight(light)} 写地图与装饰图标，
     * 全格式钩子 {@code addVertex(FFFIFFIIFFF)V} 一个顶点都收不到 ⇒ 地图（含物品展示框里
     * 的地图）在 RT 世界里完全不显形。短格式的 setter 没有一并上报的 "当前值"，
     * 所以这里按调用顺序拼一个顶点：{@code addVertex} 开始、{@code setColor}/{@code setUv}
     * 填字段、{@code setUv} 结束时提交（地图的调用顺序固定为 color → uv → light）。
     *
     * <p>只对 {@link ActiveBatch#positionOnlyCapture} 的批次生效（目前只有地图），
     * 避免把其它短格式几何（GUI/全屏特效/末地门）误收进来 —— 后者顶点数没有上界，
     * 曾经把驱动直接打崩（{@code hs_err_pid45160.log}）。这里再加
     * {@link #POSITION_ONLY_MAX_VERTICES} 的硬上限兜底。
     */
    public static void beginShortVertex(float x, float y, float z) {
        if (!anyActiveBatch) {
            return;
        }
        final ActiveBatch batch = currentCaptureBatch();
        if (batch == null || !batch.positionOnlyCapture) {
            return;
        }
        flushShortVertex();
        final ShortVertex vertex = shortVertex.get();
        vertex.x = x;
        vertex.y = y;
        vertex.z = z;
        vertex.u = 0.0f;
        vertex.v = 0.0f;
        vertex.color = 0xFFFFFFFF;
        vertex.pending = true;
        shortVertexPendingFlag = true;
    }

    public static void setShortVertexColor(int color) {
        if (!shortVertexPendingFlag) {
            return;
        }
        final ShortVertex vertex = shortVertexPending();
        if (vertex == null) {
            return;
        }
        vertex.color = color;
    }

    public static void setShortVertexUv(float u, float v) {
        if (!shortVertexPendingFlag) {
            return;
        }
        final ShortVertex vertex = shortVertexPending();
        if (vertex == null) {
            return;
        }
        vertex.u = u;
        vertex.v = v;
        flushShortVertex();
    }

    /** 全格式顶点进来时说明这段几何不走短路径，丢弃半截状态，避免串味。 */
    public static void cancelShortVertex() {
        if (!shortVertexPendingFlag) {
            return;
        }
        shortVertexPendingFlag = false;
        shortVertex.get().pending = false;
    }

    private static ShortVertex shortVertexPending() {
        final ActiveBatch batch = currentCaptureBatch();
        if (batch == null || !batch.positionOnlyCapture) {
            return null;
        }
        final ShortVertex vertex = shortVertex.get();
        return vertex.pending ? vertex : null;
    }

    private static void flushShortVertex() {
        if (!shortVertexPendingFlag) {
            return;
        }
        final ShortVertex vertex = shortVertex.get();
        if (!vertex.pending) {
            return;
        }
        vertex.pending = false;
        shortVertexPendingFlag = false;
        final ActiveBatch batch = currentCaptureBatch();
        if (batch == null || !batch.positionOnlyCapture) {
            return;
        }
        if (batch.vertexCount + batch.pendingQuadVertexCount >= POSITION_ONLY_MAX_VERTICES) {
            if (!positionOnlyOverflowLogged) {
                positionOnlyOverflowLogged = true;
                System.err.println("[RTX-ENTITY] short-path capture overflow, drop: type"
                        + " slot=" + batch.textureSlot + " texture="
                        + frameTextureLabels[Math.min(Math.max(batch.textureSlot - 1, 0),
                                MAX_TEXTURE_SLOTS - 1)]);
            }
            return;
        }
        captureVertex(vertex.x, vertex.y, vertex.z, vertex.color, vertex.u, vertex.v);
    }

    /** 短格式顶点在"接到 setter 之前"的暂存区，见 {@link #beginShortVertex}。 */
    private static final class ShortVertex {
        private float x, y, z, u, v;
        private int color = 0xFFFFFFFF;
        private boolean pending;
    }

    /**
     * 当前线程正在捕获的批次。
     *
     * <p>热路径：直读渲染线程镜像（带归属线程校验），避开 {@code ThreadLocal.get()} 的哈希查表；
     * 工作线程（区块网格构建）读镜像会因归属不符而回落到自己的 ThreadLocal（通常为 null）。
     */
    private static ActiveBatch currentCaptureBatch() {
        final ActiveBatch batch =
                ActiveBatchSlot.owner == Thread.currentThread()
                        ? ActiveBatchSlot.current
                        : null;
        return batch != null ? batch : activeBatch.get();
    }

    public static void captureVertex(float x, float y, float z, int color, float u, float v) {
        // 2026-11-11：命中 group 缓存 ⇒ 本组顶点已由回放灌入，逐顶点转换整段跳过
        if (skipGroupCapture) {
            return;
        }
        if (!anyActiveBatch) {
            return;
        }
        ActiveBatch batch = currentCaptureBatch();
        if (batch == null) return;
        ++capturedVertexCount;
        if (handPhase && handTransformMode != 0) {
            final float rx;
            final float ry;
            final float rz;
            if (handTransformMode == 1) {
                rx = handRotation[0] * x + handRotation[4] * y + handRotation[8] * z;
                ry = handRotation[1] * x + handRotation[5] * y + handRotation[9] * z;
                rz = handRotation[2] * x + handRotation[6] * y + handRotation[10] * z;
            } else {
                rx = handRotation[0] * x + handRotation[1] * y + handRotation[2] * z;
                ry = handRotation[4] * x + handRotation[5] * y + handRotation[6] * z;
                rz = handRotation[8] * x + handRotation[9] * y + handRotation[10] * z;
            }
            x = rx;
            y = ry;
            z = rz;
        }
        if (batch.projectiveUv) {
            final float wx = x + cameraWorldX;
            final float wy = y + cameraWorldY;
            final float wz = z + cameraWorldZ;
            u = fract01(wx * 0.25f);
            v = fract01(wz * 0.25f + wy * 0.125f);
        }
        final int incomingColor = color;
        if (batch.projectiveUv || handPhase) {
            color = 0xFFFFFFFF;
        }
        if ((batch.projectiveUv || handPhase) && !vertexColorLogged) {
            vertexColorLogged = true;
            System.out.println("[RTX-VCOLOR] incoming color=0x"
                    + Integer.toHexString(incomingColor)
                    + " -> forced white; hand=" + handPhase
                    + " portal=" + batch.projectiveUv
                    + " slot=" + batch.textureSlot);
        }
        // =====================================================
        // 2026-11-04：UV 重映射**默认关闭**（曾经的"双重映射"事故）。
        //
        // ModelFeatureRenderer.renderModel 内部已经用 Submit.uvMapping 去 wrap 过
        // VertexConsumer 了（那个接口专门提供 wrap() 就是给这一步用的）。而我们抓的
        // 是**渲染阶段**的顶点 —— getVertexBuilder 返回的那个 consumer —— 拿到的 u/v
        // 已经是映射后的最终值。这里再乘一次 getU/getV 就是压两遍：
        //   · 箱子/告示牌只采到 sprite 内部一小块，颜色相近，看起来"好像正常"；
        //   · 旗帜图案有精细花纹，被压两遍后只剩图案一角 ⇒ 错乱。
        // 直接证据：旗帜实测 u 跨度 0.0027（≈1/370），而一个 sprite 正常应是 0.015 量级。
        //
        // A/B：-DORION_ENTITY_UV_REMAP=1 打开（回到双重映射那版，仅用于对比）。
        // =====================================================
        if (UV_REMAP_ENABLED && batch.uvMapping != null && !batch.projectiveUv) {
            u = batch.uvMapping.getU(u);
            v = batch.uvMapping.getV(v);
        }
        batch.capture(x, y, z, color, u, v);
    }

    /**
     * Mirrors ExperienceOrbRenderer's submitCustomGeometry quad into the
     * existing dynamic-entity RT batch. The original raster submission is
     * untouched.
     */
    public static void captureExperienceOrb(
            PoseStack.Pose pose,
            float u0, float u1, float v0, float v1,
            int red, int green, int blue) {
        if (activeBatch.get() != null) {
            throw new IllegalStateException("Nested experience-orb RT capture");
        }
        int textureSlot = slotFor(EXPERIENCE_ORB_TEXTURE);
        if (textureSlot == 0) return;

        ActiveBatch batch = acquireBatch(textureSlot, false, true);
        activeBatch.set(batch);
        CapturingVertexConsumer consumer = batch.captureConsumer();
        try {
            consumer.addVertex(pose, -0.5F, -0.25F, 0.0F).setColor(red, green, blue, 128).setUv(u0, v1);
            consumer.addVertex(pose, 0.5F, -0.25F, 0.0F).setColor(red, green, blue, 128).setUv(u1, v1);
            consumer.addVertex(pose, 0.5F, 0.75F, 0.0F).setColor(red, green, blue, 128).setUv(u1, v0);
            consumer.addVertex(pose, -0.5F, 0.75F, 0.0F).setColor(red, green, blue, 128).setUv(u0, v0);
        } finally {
            consumer.finish();
            endModel();
        }
    }

    /**
     * 2026-10-06 末地传送门：**自合成几何**（不再拦截 BufferBuilder）。
     *
     * <p>为什么换路线：末地门走的是短格式 {@code addVertex(FFF)}（字节码已核对），原来的
     * 钩子只覆盖全格式 ⇒ 顶点一个都进不来。直接补钩短格式会**崩驱动**
     *（{@code hs_err_pid45160.log}：nvoglv64.dll 里 EXCEPTION_ACCESS_VIOLATION，
     * Render thread / in_native），因为它按"方向 × 面 × 方块"提交、**顶点数没有上界**，
     * 超出实体批次缓冲的容量假设 ⇒ 上传越界。所以这里照 {@link #captureExperienceOrb}
     * 的先例，由 Java 侧自己画 —— 顶点数与内容全部可控、有硬上界。</p>
     *
     * <p>几何（2026-10-18 修）：原版末地门是**压扁的立方体**（{@code TheEndPortalRenderer}
     * 的 TRANSFORMATION：平移 y=0.375、y 缩放 0.375 ⇒ 盒体 y∈[0.375,0.75]，x,z∈[0,1]），
     * 可见面是**水平面**。此前手搓的是两片竖直交叉四边形（本意"任意侧可见"），观感就是
     * 用户报的"十字交叉竖着"。现改为还原水平面：上（y=0.75）与下（y=0.375）两块水平
     * 四边形，各正反两遍（8 顶点）。pose 原点是**方块角点**（LevelRenderer
     * {@code submitBlockEntities} 只平移到 {@code blockPos − camPos}，无 0.5 偏移）
     * ⇒ 覆盖 [0,1]²。UV 按顶点世界坐标合成（projectiveUv 路径）。</p>
     */
    public static void captureEndPortal(PoseStack.Pose pose) {
        if (!endPortalSyntheticLogged) {
            endPortalSyntheticLogged = true;
            System.out.println("[RTX-ENTITY] end portal synthetic reached: activeBatch="
                    + (activeBatch.get() != null));
        }
        if (activeBatch.get() != null) {
            return;
        }
        final int textureSlot = slotForLabel("end_portal");
        if (textureSlot == 0) {
            if (!endPortalSyntheticSlotLogged) {
                endPortalSyntheticSlotLogged = true;
                System.out.println("[RTX-ENTITY] end portal synthetic: slot lookup FAILED"
                        + " (label not registered this frame)");
            }
            return;
        }
        ActiveBatch batch = acquireBatch(textureSlot, false, true);
        batch.projectiveUv = true;
        activeBatch.set(batch);
        CapturingVertexConsumer consumer = batch.captureConsumer();
        try {
            emitPortalHorizontalQuad(consumer, pose, 0.75F, true);
            emitPortalHorizontalQuad(consumer, pose, 0.375F, true);
            emitPortalHorizontalQuad(consumer, pose, 0.75F, false);
            emitPortalHorizontalQuad(consumer, pose, 0.375F, false);
        } finally {
            consumer.finish();
            endModel();
        }
    }

    /**
     * 末地门的**水平**四边形（XZ 平面，y 恒定），覆盖整个方块 [0,1]²。顶点顺序按原版
     * CCW（从上往下看）；forward=false 时反向，供从下方看。UV 由批次的 {@code projectiveUv}
     * 路径按顶点世界坐标合成（这里的 setUv 占位，会被覆盖）。
     */
    private static void emitPortalHorizontalQuad(
            CapturingVertexConsumer consumer,
            PoseStack.Pose pose,
            float y,
            boolean forward) {
        final float ax = forward ? 0.0F : 1.0F;
        final float bx = forward ? 1.0F : 0.0F;
        consumer.addVertex(pose, ax, y, 0.0F).setColor(255, 255, 255, 255).setUv(0.0F, 1.0F);
        consumer.addVertex(pose, bx, y, 0.0F).setColor(255, 255, 255, 255).setUv(1.0F, 1.0F);
        consumer.addVertex(pose, bx, y, 1.0F).setColor(255, 255, 255, 255).setUv(1.0F, 0.0F);
        consumer.addVertex(pose, ax, y, 1.0F).setColor(255, 255, 255, 255).setUv(0.0F, 0.0F);
    }

    /**
     * 2026-10-21 闪电：原版 {@code LightningBoltRenderer.submit} 用
     * {@code addVertex(pose,…)+setColor} 链写顶点，不经过
     * {@code BufferBuilder.addVertex(FFFIFFIIFFF)} 的全格式钩子；同时
     * {@code RenderTypes.lightning()} 没有 Sampler0，通用捕获会在贴图解析处
     * 直接 return。两条路都收不到 ⇒ 闪电完全不可见。
     *
     * <p>这里照原版算法合成 3 分支 quad（种子/偏移/半径逐项一致；只发最内层壳，
     * 见下方 radiusScale 的说明），贴图取 8 列闪电精灵图（CC0，见
     * {@code rt_lightning.png} 的下载来源注释）里的一列 + 顶点色，并把整批标成
     * 自发光（pad1 = -1）。透明像素由 any-hit 的 alpha 裁切（{@code rt_test.rahit}
     * 的 combinedAlpha &lt; 0.1）剔掉，所以闪电是贴图里的细亮线而不是整块卡片。
     * 顶点数有硬上界，不会重演末地门短格式钩子的驱动越界。</p>
     */
    public static void captureLightning(long seed, PoseStack poseStack) {
        if (activeBatch.get() != null) {
            return;
        }
        final int textureSlot = slotFor(LIGHTNING_TEXTURE);
        if (textureSlot == 0) {
            if (!lightningSyntheticSlotLogged) {
                lightningSyntheticSlotLogged = true;
                System.out.println("[RTX-ENTITY] lightning synthetic: slot lookup FAILED");
            }
            return;
        }
        if (!lightningSyntheticLogged) {
            lightningSyntheticLogged = true;
            System.out.println("[RTX-ENTITY] lightning synthetic captured, slot=" + textureSlot);
        }
        final ActiveBatch batch = acquireBatch(textureSlot, false, true);
        activeBatch.set(batch);
        final CapturingVertexConsumer consumer = batch.captureConsumer();
        final PoseStack.Pose pose = poseStack.last();
        try {
            final float[] xOffsets = new float[8];
            final float[] zOffsets = new float[8];
            float xOffset = 0.0F;
            float zOffset = 0.0F;
            final RandomSource random = RandomSource.createThreadLocalInstance(seed);
            for (int i = 7; i >= 0; --i) {
                xOffsets[i] = xOffset;
                zOffsets[i] = zOffset;
                xOffset += random.nextInt(11) - 5;
                zOffset += random.nextInt(11) - 5;
            }
            final float finalXOffset = xOffset;
            final float finalZOffset = zOffset;
            final float radiusScale = 1.8F;
            for (int layer = 0; layer < 1; ++layer) {
                final RandomSource layerRandom = RandomSource.createThreadLocalInstance(seed);
                for (int branch = 0; branch < 3; ++branch) {
                    final boolean trunk = branch == 0;
                    final int branchStart = 7 - branch;
                    final int branchEnd = trunk ? 0 : branchStart - 3 + 1;
                    float startX = xOffsets[branchStart] - finalXOffset;
                    float startZ = zOffsets[branchStart] - finalZOffset;
                    for (int segment = branchStart; segment >= branchEnd; --segment) {
                        final float endX = startX;
                        final float endZ = startZ;
                        if (trunk) {
                            startX += layerRandom.nextInt(11) - 5;
                            startZ += layerRandom.nextInt(11) - 5;
                        } else {
                            startX += layerRandom.nextInt(31) - 15;
                            startZ += layerRandom.nextInt(31) - 15;
                        }
                        float topRadius = 0.1F + layer * 0.2F;
                        float bottomRadius = topRadius;
                        if (trunk) {
                            topRadius *= segment * 0.1F + 1.0F;
                            bottomRadius *= (segment - 1) * 0.1F + 1.0F;
                        }
                        topRadius *= radiusScale;
                        bottomRadius *= radiusScale;
                        emitLightningQuad(consumer, pose, startX, startZ, endX, endZ,
                                segment, topRadius, bottomRadius, false, false, true, false,
                                (segment + branch) & 7);
                        emitLightningQuad(consumer, pose, startX, startZ, endX, endZ,
                                segment, topRadius, bottomRadius, true, false, true, true,
                                (segment + branch + 2) & 7);
                        emitLightningQuad(consumer, pose, startX, startZ, endX, endZ,
                                segment, topRadius, bottomRadius, true, true, false, true,
                                (segment + branch + 4) & 7);
                        emitLightningQuad(consumer, pose, startX, startZ, endX, endZ,
                                segment, topRadius, bottomRadius, false, true, false, false,
                                (segment + branch + 6) & 7);
                    }
                }
            }
        } finally {
            consumer.finish();
            endModel();
        }
    }

    private static void emitLightningQuad(
            CapturingVertexConsumer consumer,
            PoseStack.Pose pose,
            float segmentStartX,
            float segmentStartZ,
            float segmentEndX,
            float segmentEndZ,
            int segment,
            float topRadius,
            float bottomRadius,
            boolean rightXPositive,
            boolean rightZPositive,
            boolean leftXPositive,
            boolean leftZPositive,
            int column) {
        final int color = 0xFFB9C8FF;
        final float u0 = (column + 0.03F) * 0.125F;
        final float u1 = (column + 0.97F) * 0.125F;
        consumer.addVertex(pose,
                        segmentStartX + (rightXPositive ? bottomRadius : -bottomRadius),
                        segment * 16,
                        segmentStartZ + (rightZPositive ? bottomRadius : -bottomRadius))
                .setColor(color).setUv(u0, 0.0F);
        consumer.addVertex(pose,
                        segmentEndX + (rightXPositive ? topRadius : -topRadius),
                        (segment + 1) * 16,
                        segmentEndZ + (rightZPositive ? topRadius : -topRadius))
                .setColor(color).setUv(u1, 0.0F);
        consumer.addVertex(pose,
                        segmentEndX + (leftXPositive ? topRadius : -topRadius),
                        (segment + 1) * 16,
                        segmentEndZ + (leftZPositive ? topRadius : -topRadius))
                .setColor(color).setUv(u1, 1.0F);
        consumer.addVertex(pose,
                        segmentStartX + (leftXPositive ? bottomRadius : -bottomRadius),
                        segment * 16,
                        segmentStartZ + (leftZPositive ? bottomRadius : -bottomRadius))
                .setColor(color).setUv(u0, 1.0F);
    }

    /**
     * 2026-10-21 信标光柱：{@code BeaconRenderer.submit} 同样走
     * {@code addVertex(pose,…)+setColor/setUv} 链，全格式钩子收不到顶点，
     * 通用捕获只开了一个空批次 ⇒ 光柱不可见。
     *
     * <p>按原版 {@code submitBeaconBeam} 的几何、UV 滚动与转速重建实心柱
     *（外辉那层是加色混合，RT 里没有对应通路，省略；实心柱本身已带贴图渐变）。
     * 每个 section 4 个 quad、16 个顶点，上界固定。</p>
     */
    public static void captureBeaconBeam(BeaconRenderState state, PoseStack poseStack) {
        if (state.sections.isEmpty() || activeBatch.get() != null) {
            return;
        }
        final int textureSlot = slotFor(BeaconRenderer.BEAM_LOCATION);
        if (textureSlot == 0) {
            if (!beaconSyntheticSlotLogged) {
                beaconSyntheticSlotLogged = true;
                System.out.println("[RTX-ENTITY] beacon synthetic: slot lookup FAILED");
            }
            return;
        }
        if (!beaconSyntheticLogged) {
            beaconSyntheticLogged = true;
            System.out.println("[RTX-ENTITY] beacon synthetic captured, slot=" + textureSlot);
        }
        final ActiveBatch batch = acquireBatch(textureSlot, false, true);
        activeBatch.set(batch);
        final CapturingVertexConsumer consumer = batch.captureConsumer();
        try {
            int beamStart = 0;
            final int sectionCount = state.sections.size();
            for (int i = 0; i < sectionCount; ++i) {
                final BeaconRenderState.Section section = state.sections.get(i);
                final int height = i == sectionCount - 1 ? 2048 : section.height();
                emitBeaconBeam(consumer, poseStack, state.animationTime,
                        beamStart, height, section.color(), 0.2F * state.beamRadiusScale);
                beamStart += section.height();
            }
        } finally {
            consumer.finish();
            endModel();
        }
    }

    private static void emitBeaconBeam(
            CapturingVertexConsumer consumer,
            PoseStack poseStack,
            float animationTime,
            int beamStart,
            int height,
            int color,
            float radius) {
        if (radius <= 1.0E-4F) {
            return;
        }
        final int beamEnd = beamStart + height;
        final float scroll = height < 0 ? animationTime : -animationTime;
        final float rawV = scroll * 0.2F - (float) Math.floor(scroll * 0.1F);
        final float texVOff = rawV - (float) Math.floor(rawV);
        final float vv2 = -1.0F + texVOff;
        final float vv1 = height * (0.5F / radius) + vv2;

        poseStack.pushPose();
        poseStack.translate(0.5F, 0.0F, 0.5F);
        poseStack.rotateDegrees(Axis.YP, animationTime * 2.25F - 45.0F);
        final PoseStack.Pose pose = poseStack.last();
        emitBeaconQuad(consumer, pose, color, beamStart, beamEnd,
                0.0F, radius, radius, 0.0F, 0.0F, 1.0F, vv1, vv2);
        emitBeaconQuad(consumer, pose, color, beamStart, beamEnd,
                -radius, 0.0F, 0.0F, -radius, 0.0F, 1.0F, vv1, vv2);
        emitBeaconQuad(consumer, pose, color, beamStart, beamEnd,
                radius, 0.0F, 0.0F, -radius, 0.0F, 1.0F, vv1, vv2);
        emitBeaconQuad(consumer, pose, color, beamStart, beamEnd,
                0.0F, -radius, 0.0F, radius, 0.0F, 1.0F, vv1, vv2);
        poseStack.popPose();
    }

    private static void emitBeaconQuad(
            CapturingVertexConsumer consumer,
            PoseStack.Pose pose,
            int color,
            int beamStart,
            int beamEnd,
            float westX,
            float westZ,
            float eastX,
            float eastZ,
            float uu1,
            float uu2,
            float vv1,
            float vv2) {
        consumer.addVertex(pose, westX, beamEnd, westZ).setColor(color).setUv(uu2, vv1);
        consumer.addVertex(pose, westX, beamStart, westZ).setColor(color).setUv(uu2, vv2);
        consumer.addVertex(pose, eastX, beamStart, eastZ).setColor(color).setUv(uu1, vv2);
        consumer.addVertex(pose, eastX, beamEnd, eastZ).setColor(color).setUv(uu1, vv1);
    }

    /**
     * 按**贴图标签子串**取槽位（末地门用）。刻意不自己构造 {@code Identifier}：
     * 标签来自 {frameTextureLabels}，也就是渲染管线自己解析出的那个，
     * 避免路径/命名空间约定与版本不一致时静默失配。
     */
    private static int slotForLabel(String needle) {
        for (int index = 0; index < frameTextureSlotCount; ++index) {
            final String label = frameTextureLabels[index];
            if (label != null && label.contains(needle)) {
                slotLastSeenFrame[index] = captureFrameSerial;
                return index + 1;
            }
        }
        return 0;
    }

    // 2026-11-11：命中上一次的槽位先做一次校验式快查。
    //
    // 病征：本方法挂在 FrontendRenderPass.setUniform 上，每绑定一次 Sampler0 就走一遍
    // **线性扫描**（最多 127 个槽 × String.equals），而 766 个 section × 多层意味着
    // 每帧上千次调用。这部分开销藏在原版区块提交那 15.5 ms 里，账上一直算作"原版"。
    //
    // 做法：记住上一次命中的槽位，先比对那一格；只有不匹配才回落到线性扫描。
    // 缓存命中时只做 1 次字符串比较（≈O(1)）。不用 HashMap，是为了**自校验**：
    // 槽位被重新分配时，frameTextureLabels[slot-1] 的内容会变，比较自然失败，
    // 不会读到一个已经指向别处的陈旧槽。
    private static int lastBoundSlot = 0;

    // 2026-11-11：A/B 开关。默认开（走快查）；ORION_TEXBOUND_CACHE=0 时逐位回到
    // 原来的线性扫描，用于在同一场景里把这条的收益单独量出来。
    private static final boolean TEXBOUND_FAST_PATH =
            !"0".equals(System.getenv("ORION_TEXBOUND_CACHE"));

    // 2026-11-11：逐帧诊断日志的总闸，**默认全关**。
    // 这两组日志每帧会产生几十~上百个 String（拼接 + 查表），
    // 是当前年轻代分配速率偏高（845M->380M 一次回收）的可疑来源之一。
    // 需要排障时：ORION_LOG_HANDUV=1 / ORION_LOG_TEXTPERF=1。
    private static final boolean LOG_HANDUV =
            "1".equals(System.getenv("ORION_LOG_HANDUV"));
    private static final boolean LOG_TEXT_PERF =
            "1".equals(System.getenv("ORION_LOG_TEXTPERF"));

    public static void onTextureBound(String textureLabel, long imageView, long sampler) {
        final int cached = lastBoundSlot;
        if (TEXBOUND_FAST_PATH
                && cached > 0 && cached <= frameTextureSlotCount
                && textureLabel.equals(frameTextureLabels[cached - 1])) {
            publishTextureHandle(cached, imageView, sampler);
            return;
        }

        for (int index = 0; index < frameTextureSlotCount; ++index) {
            if (frameTextureLabels[index].equals(textureLabel)) {
                lastBoundSlot = index + 1;
                publishTextureHandle(index + 1, imageView, sampler);
                return;
            }
        }
    }

    /**
     * 2026-11-11：上一次命中的槽位（快查用，-1 = 无）。
     *
     * <p>病征：{@code slotFor} 是**每个实体模型**都要走一遍的线性扫描
     * （最多 127 槽 × {@code Identifier.equals}）。实体多的场景一帧几百个模型 ⇒
     * 实测 {@code [LEVEL PHASES FEAT]} 里 {@code ours(capture)} 约 20 ms，其中约 9 ms
     * 落在这里（{@code uploadBatch} 的运动矢量只要 0.6 ms、JNI 1.0 ms）。
     *
     * <p>做法与 {@link #slotForLabel} 相同：先比上一次命中的那一格，不匹配才回落线性扫描。
     * 槽位被重新分配时那一格的内容会变，比较自然失败 ⇒ **自校验**，
     * 不会读到一个已经指向别处的陈旧槽。
     */
    private static int lastSlotForIndex = -1;

    /**
     * 2026-11-11：实体/物品捕获总开关，{@code ORION_ENTITY_CAPTURE=0} 关闭。
     *
     * <p>只用于 A/B：关掉后 RT 世界里不再有实体与物品（画面会缺东西），
     * 目的是把「{@code Item}/{@code Entity Model} 桶里的时间到底有多少是我们的」
     * 量出来 —— 因为我们那个 {@code VertexConsumer} 包装是在**原版写顶点的循环里**
     * 逐顶点跑的，它的开销会被算进「原版」那一栏，只看 {@code ours(capture)} 会低估。
     */
    private static final boolean CAPTURE_ENABLED =
            !"0".equals(System.getenv("ORION_ENTITY_CAPTURE"));

    /** 逐顶点捕获计数（诊断用：算出 ns/顶点，判断热路径还剩多少优化空间）。 */
    private static long capturedVertexCount;

    /** 本窗口内的「提交次数」：Item/Custom/BlockModel 通用链 vs 实体模型链。 */
    private static long genericSubmitCount;
    private static long modelSubmitCount;

    /** 由 {@code LevelPhaseProbe} 在窗口末尾取走并清零。 */
    public static long takeCapturedVertexCount() {
        final long v = capturedVertexCount;
        capturedVertexCount = 0L;
        return v;
    }

    public static long takeGenericSubmitCount() {
        final long v = genericSubmitCount;
        genericSubmitCount = 0L;
        return v;
    }

    public static long takeModelSubmitCount() {
        final long v = modelSubmitCount;
        modelSubmitCount = 0L;
        return v;
    }

    // ── 2026-11-11：物品几何缓存的命中率探针（只统计，不改行为） ──
    private static final java.util.HashSet<Long> itemKeyCache = new java.util.HashSet<>();
    private static long itemKeyHits;
    private static long itemKeyTotal;
    private static final java.util.HashSet<Long> itemKeysThisWindow = new java.util.HashSet<>();

    /**
     * 键 = {@code ItemFeatureRenderer.Submit} 的全部渲染输入。
     *
     * <p>注意 {@code tintLayers} 是 {@code int[]}，record 自动生成的 {@code hashCode}
     * 对数组取的是**identity**，不能直接用 ⇒ 这里逐项混入内容。
     */
    public static void noteItemSubmitKey(final ItemFeatureRenderer.Submit submit) {
        if (!LevelPhaseProbe.enabled() || submit == null) {
            return;
        }

        long key = 1125899906842597L;

        final com.mojang.blaze3d.vertex.PoseStack.Pose pose = submit.pose();
        key = key * 31 + (pose == null ? 0 : pose.pose().hashCode());
        key = key * 31 + submit.displayContext().ordinal();
        key = key * 31 + submit.lightCoords();
        key = key * 31 + submit.overlayCoords();
        key = key * 31 + submit.outlineColor();

        final int[] tint = submit.tintLayers();
        key = key * 31 + (tint == null ? 0 : java.util.Arrays.hashCode(tint));

        final java.util.List<?> quads = submit.quads();
        key = key * 31 + (quads == null ? 0 : quads.size());
        // 列表本身若每帧新建，identity 会变 ⇒ 命中率会掉到 0，届时改按元素哈希
        key = key * 31 + (quads == null ? 0 : System.identityHashCode(quads));
        key = key * 31 + submit.foilType().ordinal();

        ++itemKeyTotal;
        itemKeysThisWindow.add(key);
        if (!itemKeyCache.add(key)) {
            ++itemKeyHits;
        }
    }

    public static long takeItemKeyTotal() {
        final long v = itemKeyTotal;
        itemKeyTotal = 0L;
        return v;
    }

    public static long takeItemKeyHits() {
        final long v = itemKeyHits;
        itemKeyHits = 0L;
        return v;
    }

    public static long takeItemKeyDistinct() {
        final long v = itemKeysThisWindow.size();
        itemKeysThisWindow.clear();
        if (itemKeyCache.size() > 200_000) {
            itemKeyCache.clear();
        }
        return v;
    }

    // ── 2026-11-11：缓存粒度探针。getVertexBuilder 每帧 65 万次，要看这 65 万次
    //    到底散落在多少个 RenderType / 多少个 group consumer 上 —— 决定缓存按什么粒度做。
    private static long vbCalls;
    private static final java.util.HashSet<Long> vbRenderTypes = new java.util.HashSet<>();
    private static final java.util.HashSet<Long> vbConsumers = new java.util.HashSet<>();

    public static void noteVertexBuilder(final Object renderType, final Object consumer) {
        if (!LevelPhaseProbe.enabled()) {
            return;
        }
        ++vbCalls;
        if (renderType != null) {
            vbRenderTypes.add((long) System.identityHashCode(renderType));
        }
        if (consumer != null) {
            vbConsumers.add((long) System.identityHashCode(consumer));
        }
    }

    public static long takeVbCalls() {
        final long v = vbCalls;
        vbCalls = 0L;
        return v;
    }

    public static long takeVbRenderTypes() {
        final long v = vbRenderTypes.size();
        vbRenderTypes.clear();
        return v;
    }

    public static long takeVbConsumers() {
        final long v = vbConsumers.size();
        vbConsumers.clear();
        return v;
    }

    // ═══════════════════════════════════════════════════════════════════
    // 2026-11-11：**group 级几何缓存（第一阶段）**
    //
    // 依据（§12.12.2 实测）：一帧 64 万次 getVertexBuilder 只落在 25 个 group 上，
    // 每 group 约 2.5 万次 ⇒ 缓存的正确粒度是 group，不是 submit / renderType。
    //
    // 做法：在 RenderTypeFeatureRenderer.prepareGroup 的 HEAD 用
    // (submit 列表 identity, size) 做键。
    //   · 命中 ⇒ 本组不跑逐顶点转换（captureVertex 直接返回），
    //            flush 前把上一帧捕获的顶点块整块灌回批次；
    //   · 未命中 ⇒ 正常转换，flush 前把本组顶点块拷进缓存。
    //
    // 安全阀（错了会缺几何/串贴图，全部走"废掉该键 + 本帧不复用"，下一帧自动回到未命中）：
    //   · 只在 ItemFeatureRenderer 的 group 上启用；
    //   · 只缓存 uvMapping==null 且非 projectiveUv 的批次（否则 UV 语义不止是顶点数据）；
    //   · 回放前校验 textureSlot 一致、缓冲容量足够。
    //
    // 开关：ORION_ITEM_CACHE=1（默认关）。
    // ═══════════════════════════════════════════════════════════════════
    private static final boolean GROUP_CACHE_ENABLED =
            "1".equals(System.getenv("ORION_ITEM_CACHE"));

    private static final java.util.HashMap<Long, GroupGeom> groupCache =
            new java.util.HashMap<>();

    /**
     * 缓存硬上限。一个 group 可能是十几万顶点 × 48 B ≈ 数 MB，
     * 若键不稳定（submit 列表每帧新建 ⇒ identity 变）会**无界增长**：
     * 2026-11-11 第一版就这样把堆打爆了（{@code OutOfMemoryError: Java heap space}）。
     */
    private static final long GROUP_CACHE_MAX_BYTES = 256L << 20;
    private static final int GROUP_CACHE_MAX_ENTRIES = 96;

    private static long groupCacheBytes;

    private static long pendingGroupKey;
    private static GroupGeom pendingGroupEntry;
    private static boolean skipGroupCapture;
    private static long groupCacheHits;
    private static long groupCacheMisses;
    private static long groupCacheInvalidated;

    private static final class GroupGeom {
        /**
         * 贴图标识（不是槽位号）。槽位会被 {@code slotFor} 的 LRU 重排，
         * 所以命中时要用它重新取一次槽位，不能直接复用旧槽号。
         */
        net.minecraft.resources.Identifier texture;
        int textureSlot;
        int vertexCount;
        byte[] bytes;
    }

    /**
     * 命中后自建批次回放（第二阶段）。
     *
     * <p>命中时会 cancel 掉原版 {@code prepareGroup}，于是 vanilla 不再调
     * {@code getVertexBuilder}、不会开批次 ⇒ 必须自己 {@code acquireBatch} 一个。
     *
     * @return true = 已回放并上传完毕（调用方应 cancel 原版方法）
     */
    private static boolean replayStandalone(final GroupGeom entry) {
        final int bytes = entry.vertexCount * BYTES_PER_VERTEX;
        if (bytes <= 0 || entry.texture == null) {
            return false;
        }

        // 重新解析槽位：既保证贴图还在，也顺手更新 lastSeen（防止 LRU 把我们淘汰）
        final int slot = slotFor(entry.texture);
        if (slot <= 0) {
            return false;
        }

        final ActiveBatch batch = acquireBatch(slot, false, false);
        if (batch.buffer.capacity() < bytes) {
            return false;
        }

        // 缓存里已经是最终顶点字节（共面偏移等都已应用），不要再走 emit
        batch.coplanarLayering = false;
        batch.buffer.clear();
        batch.buffer.put(entry.bytes, 0, bytes);
        batch.vertexCount = entry.vertexCount;

        activeBatch.set(batch);
        try {
            endModel();          // → uploadBatch → NativeVulkan.uploadEntityBatch
        } finally {
            activeBatch.remove();
        }
        return true;
    }

    /**
     * 为物品 group 的 submit 列表算一个**语义**键。
     *
     * <p>不能用 {@code List.hashCode()}：{@code ItemFeatureRenderer.Submit} 虽是 record，
     * 但 {@code pose}（{@code PoseStack.Pose}）、{@code tintLayers}（{@code int[]}）、
     * {@code quads}（{@code List}）走的都是 identity 哈希 ⇒ 每帧都变（实测命中 0/142）。
     *
     * <p>这里只混入**真正决定几何**的量：pose 矩阵分量（JOML 的 hashCode 是数值的）、
     * displayContext、light/overlay/outline、tint 内容、quad 数量与首个 BakedQuad 的
     * identity（BakedQuad 来自模型，跨帧稳定），外加 foil。
     *
     * @return 0 表示不可缓存（列表为空或元素类型不符）。
     */
    public static long itemGroupKey(final java.util.List<?> submits) {
        if (submits == null || submits.isEmpty()) {
            return 0L;
        }

        noteItemSubmitCount(submits.size());

        long key = 0xcbf29ce484222325L;
        for (int i = 0; i < submits.size(); ++i) {
            final Object raw = submits.get(i);
            if (!(raw instanceof ItemFeatureRenderer.Submit submit)) {
                return 0L;
            }

            long h = 1125899906842597L;
            final com.mojang.blaze3d.vertex.PoseStack.Pose pose = submit.pose();
            final org.joml.Matrix4f matrix = pose == null ? null : pose.pose();
            h = h * 31 + (matrix == null ? 0 : matrix.hashCode());

            // 第 0 步探针：平移分量去重 ⇒ 有多少个**不同位置**
            if (LevelPhaseProbe.enabled() && matrix != null) {
                long t = (long) Float.floatToIntBits(matrix.m30()) * 31L
                        + Float.floatToIntBits(matrix.m31());
                t = t * 31L + Float.floatToIntBits(matrix.m32());
                itemPoseSet.add(t);
            }
            h = h * 31 + submit.displayContext().ordinal();
            h = h * 31 + submit.lightCoords();
            h = h * 31 + submit.overlayCoords();
            h = h * 31 + submit.outlineColor();

            final int[] tint = submit.tintLayers();
            h = h * 31 + (tint == null ? 0 : java.util.Arrays.hashCode(tint));

            final java.util.List<?> quads = submit.quads();
            final int quadCount = quads == null ? 0 : quads.size();
            h = h * 31 + quadCount;
            // BakedQuad 是模型的稳定对象；取首个 identity + 数量即可区分不同物品
            final int modelHash = quadCount == 0 ? 0 : System.identityHashCode(quads.get(0));
            h = h * 31 + modelHash;

            // 第 0 步探针：模型种类去重 ⇒ 有多少种**不同的模型数据**
            if (LevelPhaseProbe.enabled()) {
                itemModelSet.add((long) modelHash * 31L + quadCount);
            }

            h = h * 31 + submit.foilType().ordinal();

            key = (key ^ h) * 0x100000001b3L;
        }
        return key;
    }

    /**
     * {@code prepareGroup} HEAD：命中则**当场回放并返回 true**（调用方要 cancel 原版），
     * 未命中返回 false（正常转换，等 RETURN 时登记）。
     */
    public static boolean beginGroup(final long key) {
        if (!GROUP_CACHE_ENABLED) {
            skipGroupCapture = false;
            pendingGroupEntry = null;
            return false;
        }

        pendingGroupKey = key;
        pendingGroupEntry = null;
        skipGroupCapture = false;

        final GroupGeom entry = groupCache.get(key);
        if (entry == null) {
            ++groupCacheMisses;
            return false;
        }

        if (replayStandalone(entry)) {
            ++groupCacheHits;
            return true;
        }

        // 回放失败（贴图没了 / 容量不够）⇒ 废掉这个键，本帧回落到正常路径
        groupCache.remove(key);
        groupCacheBytes -= (long) entry.vertexCount * BYTES_PER_VERTEX;
        ++groupCacheInvalidated;
        return false;
    }

    /**
     * {@code prepareGroup} RETURN：回放或登记本组顶点块，然后照常 flush。
     * 替代原来的 {@code endCustomGroup()}（后者只做 flush）。
     */
    public static void finishGroup() {
        if (GROUP_CACHE_ENABLED) {
            final ActiveBatch batch = ActiveBatchSlot.current != null
                    ? ActiveBatchSlot.current
                    : activeBatch.get();

            if (batch != null && batch.uvMapping == null && !batch.projectiveUv
                    && batch.vertexCount > 0
                    && batch.buffer.capacity() >= batch.vertexCount * BYTES_PER_VERTEX) {
                final int bytes = batch.vertexCount * BYTES_PER_VERTEX;

                // 单个 group 超过 8 MB 就不进缓存（否则一进来就顶满）。
                // ⚠ 这里绝对不能 return —— 后面还有 endCustomGroup()，提前返回会把
                //    整个批次吞掉（批次不 flush ⇒ 本帧几何丢失且批次悬挂）。
                final boolean cacheable = bytes <= (8 << 20);

                while (cacheable && (groupCacheBytes + bytes > GROUP_CACHE_MAX_BYTES
                        || groupCache.size() >= GROUP_CACHE_MAX_ENTRIES)) {
                    if (groupCache.isEmpty()) {
                        break;
                    }
                    // 键不稳定时这是唯一的止血手段（否则无界增长 → OOM）
                    groupCache.clear();
                    groupCacheBytes = 0L;
                }

                final net.minecraft.resources.Identifier texture = customRenderType == null
                        ? null
                        : resolveSampler0(customRenderType);

                if (cacheable && texture != null) {
                    final GroupGeom entry = new GroupGeom();
                    entry.texture = texture;
                    entry.textureSlot = batch.textureSlot;
                    entry.vertexCount = batch.vertexCount;
                    entry.bytes = new byte[bytes];
                    final java.nio.ByteBuffer view = batch.buffer.duplicate();
                    view.clear();
                    view.get(entry.bytes, 0, bytes);

                    final GroupGeom old = groupCache.put(pendingGroupKey, entry);
                    if (old != null) {
                        groupCacheBytes -= (long) old.vertexCount * BYTES_PER_VERTEX;
                    }
                    groupCacheBytes += bytes;
                }
            }

            pendingGroupEntry = null;
            skipGroupCapture = false;
        }
        endCustomGroup();
    }

    /**
     * 2026-11-12：本窗口内物品 group 里**实际有多少个 Submit 对象**（= 多少件东西）。
     *
     * <p>这个数决定方向：
     * <ul>
     *   <li>若 ≈ 数千件 × 每件一两百 quad ⇒ 是"东西本来就多"，只能靠 instancing / LOD；</li>
     *   <li>若只有几十件却有几十万 quad ⇒ 是"同一件被重复提交"，那才是 bug，得去查提交侧。</li>
     * </ul>
     */
    private static long itemSubmitObjects;

    public static void noteItemSubmitCount(final int count) {
        if (LevelPhaseProbe.enabled() && count > 0) {
            itemSubmitObjects += count;
        }
    }

    public static long takeItemSubmitObjects() {
        final long v = itemSubmitObjects;
        itemSubmitObjects = 0L;
        return v;
    }

    // ── 2026-11-12 第 0 步：实例化是否有意义 ──
    // 需要区分两种可能：
    //   a) 250 件物品在 250 个不同位置、只有 4 种模型 ⇒ 实例化收益最大（1 模型 1 BLAS + N 实例）
    //   b) 它们其实集中在少数几个位置 ⇒ 实例化没意义，应放弃这条路线
    private static final java.util.HashSet<Long> itemPoseSet = new java.util.HashSet<>();
    private static final java.util.HashSet<Long> itemModelSet = new java.util.HashSet<>();

    public static long takeItemPoseDistinct() {
        final long v = itemPoseSet.size();
        itemPoseSet.clear();
        return v;
    }

    public static long takeItemModelDistinct() {
        final long v = itemModelSet.size();
        itemModelSet.clear();
        return v;
    }

    public static long takeGroupCacheHits() {
        final long v = groupCacheHits;
        groupCacheHits = 0L;
        return v;
    }

    public static long takeGroupCacheMisses() {
        final long v = groupCacheMisses;
        groupCacheMisses = 0L;
        return v;
    }

    public static long takeGroupCacheInvalidated() {
        final long v = groupCacheInvalidated;
        groupCacheInvalidated = 0L;
        return v;
    }

    private static int slotFor(Identifier texture) {
        if (texture == null) return 0;

        final int cached = lastSlotForIndex;
        if (cached >= 0 && cached < frameTextureSlotCount
                && texture.equals(frameTextureSlots[cached])) {
            slotLastSeenFrame[cached] = captureFrameSerial;
            assignTextureClass(cached, texture);
            if (!slotTexturePublished[cached]) {
                publishTexture(cached + 1, texture);
            }
            if (!entityPbrSettled[cached]) {
                publishEntityPbrTexture(cached + 1, texture);
            }
            return cached + 1;
        }

        for (int index = 0; index < frameTextureSlotCount; ++index) {
            if (texture.equals(frameTextureSlots[index])) {
                slotLastSeenFrame[index] = captureFrameSerial;
                assignTextureClass(index, texture);
                if (!slotTexturePublished[index]) {
                    publishTexture(index + 1, texture);
                }
                if (!entityPbrSettled[index]) {
                    publishEntityPbrTexture(index + 1, texture);
                }
                lastSlotForIndex = index;
                return index + 1;
            }
        }
        int victim;
        if (frameTextureSlotCount < MAX_REGULAR_TEXTURE_SLOTS) {
            victim = frameTextureSlotCount;
        } else {
            victim = -1;
            int bestAge = -1;
            for (int index = 0; index < MAX_REGULAR_TEXTURE_SLOTS; ++index) {
                final int age = captureFrameSerial - slotLastSeenFrame[index];
                if (age >= SLOT_EVICTION_GRACE_FRAMES) {
                    victim = index;
                    break;
                }
                if (age > bestAge) {
                    bestAge = age;
                    victim = index;
                }
            }
            if (victim < 0) {
                if (isDiagnosticFrame()) {
                    System.err.println("[RTX-ENTITY] slot table full, dropping texture " + texture);
                }
                return 0;
            }
            if (slotLastSeenFrame[victim] == captureFrameSerial) {
                ++evictionsInUse;
                ++evictionsInUseTotal;
            }
        }
        frameTextureSlots[victim] = texture;
        frameTextureLabels[victim] = texture.toString();
        assignTextureClass(victim, texture);
        slotLastSeenFrame[victim] = captureFrameSerial;
        if (victim == frameTextureSlotCount) ++frameTextureSlotCount;
        entityPbrSettled[victim] = false;
        publishTexture(victim + 1, texture);
        lastSlotForIndex = victim;
        return victim + 1;
    }

    private static void assignTextureClass(int index, Identifier texture) {
        final int materialClass = isUncutoutTexture(texture)
                ? MATERIAL_CLASS_UNCUTOUT
                : classifyEntityMaterial(texture);


        if (frameTextureClasses[index] == materialClass) {
            return;
        }


        frameTextureClasses[index] = materialClass;


        if (materialClass != MATERIAL_CLASS_DEFAULT) {
            System.out.println(
                    "[RTX-ENTITY] material class " + materialClass
                            + " slot " + (index + 1)
                            + " for " + texture
            );
        }
    }

    private static final java.util.HashMap<Identifier, Integer> MATERIAL_CLASS_CACHE =
            new java.util.HashMap<>();
    private static boolean materialClassCacheMetalEnabled;

    private static int classifyEntityMaterial(Identifier texture) {
        if (texture == null) {
            return MATERIAL_CLASS_DEFAULT;
        }


        boolean metalEnabled = RTXSettingsConfig.isEntityArmorMetalEnabled();
        if (!metalEnabled) {
            if (materialClassCacheMetalEnabled) {
                materialClassCacheMetalEnabled = false;
                MATERIAL_CLASS_CACHE.clear();
            }
            return MATERIAL_CLASS_DEFAULT;
        }
        if (!materialClassCacheMetalEnabled) {
            materialClassCacheMetalEnabled = true;
            MATERIAL_CLASS_CACHE.clear();
        }

        Integer cached = MATERIAL_CLASS_CACHE.get(texture);
        if (cached != null) {
            return cached;
        }

        int materialClass = classifyEntityMaterialUncached(texture);
        MATERIAL_CLASS_CACHE.put(texture, materialClass);
        return materialClass;
    }

    private static int classifyEntityMaterialUncached(Identifier texture) {
        final String path =
                texture.toString().toLowerCase(Locale.ROOT);


        if (!path.contains("/equipment/")
                && !path.contains("armor")
                && !path.contains("elytra")) {
            return MATERIAL_CLASS_DEFAULT;
        }


        if (path.contains("netherite")) {
            return MATERIAL_CLASS_NETHERITE;
        }

        if (path.contains("diamond")) {
            return MATERIAL_CLASS_DIAMOND;
        }

        if (path.contains("gold")) {
            return MATERIAL_CLASS_GOLD;
        }

        if (path.contains("copper")) {
            return MATERIAL_CLASS_COPPER;
        }

        if (path.contains("iron") || path.contains("chainmail")) {
            return MATERIAL_CLASS_IRON;
        }


        return MATERIAL_CLASS_SOFT;
        }

        /**
        * 大片透明但必须**整体可见**的贴图（目前只有末地传送门的星幕
        * {@code end_portal.png}）。走"不裁切"档，且刻意**不经过**
        * {@link #classifyEntityMaterial}（那条路在 {@code entityArmorMetal} 关闭时返回
        * DEFAULT，会让末地门的可见性跟着盔甲设置变化）。消费端见 {@code rt_test.rahit}。
        */
        private static boolean isUncutoutTexture(Identifier texture) {
        return texture != null && texture.toString().contains("end_portal");
        }

    private static void publishTexture(int slot, Identifier textureId) {
        AbstractTexture texture = Minecraft.getInstance()
                .getTextureManager()
                .getTexture(textureId);
        GpuTextureView view = texture.getTextureView();
        GpuSampler sampler = texture.getSampler();
        if (view instanceof VulkanGpuTextureView vulkanView
                && sampler instanceof VulkanGpuSampler vulkanSampler) {
            publishTextureHandle(slot, vulkanView.vkImageView(), vulkanSampler.vkSampler());
        } else if (isDiagnosticFrame()) {
            // 2026-11-04 诊断：纹理槽绑定失败 ⇒ shader 采样返回 0 ⇒
            // cutout 把每个纹素都当透明丢掉 ⇒ 几何"消失"（旗帜图案正是这种表现：
            // 底色在、图案完全不见，也不遮挡）。只打印失败，不刷屏。
            System.err.println("[RTX-TEX] publish failed slot=" + slot
                    + " texture=" + textureId
                    + " view=" + view + " sampler=" + sampler);
        }
        publishEntityPbrTexture(slot, textureId);
    }


    /** 一次解析并推送一个槽的 `_n`/`_s`；没有贴图/视图未就绪时会保持 unsettled 以便重试。 */
    private static void publishEntityPbrTexture(int slot, Identifier albedoTexture) {
        if (slot < 1 || slot > MAX_TEXTURE_SLOTS || albedoTexture == null) return;
        int index = slot - 1;

        Identifier[] siblings = resolveEntityPbrSiblings(albedoTexture);
        Identifier normalId = siblings[0];
        Identifier specularId = siblings[1];

        long normalView = 0L, normalSampler = 0L, specularView = 0L, specularSampler = 0L;
        boolean pending = false;
        if (normalId != null) {
            long[] handle = entityTextureHandle(normalId);
            if (handle == null) pending = true;
            else { normalView = handle[0]; normalSampler = handle[1]; }
        }
        if (specularId != null) {
            long[] handle = entityTextureHandle(specularId);
            if (handle == null) pending = true;
            else { specularView = handle[0]; specularSampler = handle[1]; }
        }
        if (pending) return;

        entityPbrSettled[index] = true;

        if (normalView == publishedEntityNormalViews[index]
                && normalSampler == publishedEntityNormalSamplers[index]
                && specularView == publishedEntitySpecularViews[index]
                && specularSampler == publishedEntitySpecularSamplers[index]) {
            return;
        }

        publishedEntityNormalViews[index] = normalView;
        publishedEntityNormalSamplers[index] = normalSampler;
        publishedEntitySpecularViews[index] = specularView;
        publishedEntitySpecularSamplers[index] = specularSampler;

        if (!entityPbrNativeAvailable) return;
        try {
            NativeVulkan.updateEntityPbrTexture(
                    slot, normalView, normalSampler, specularView, specularSampler);
        } catch (UnsatisfiedLinkError staleNative) {
            entityPbrNativeAvailable = false;
        }
    }

    /**
     * 解析 albedo Identifier 对应的 `_n`/`_s`（存在的返回 Identifier，不存在返回
     * null）。结果按 albedo Identifier 缓存 —— ResourceManager 查表要扫资源包，
     * 绝不能逐帧做；资源包重载时随 {@link #invalidateTextureCache()} 清空。
     */
    private static Identifier[] resolveEntityPbrSiblings(Identifier albedoTexture) {
        Identifier[] cached = ENTITY_PBR_SIBLINGS.get(albedoTexture);
        if (cached != null) return cached;

        Identifier normalId = siblingTextureId(albedoTexture, "_n");
        Identifier specularId = siblingTextureId(albedoTexture, "_s");
        ResourceManager manager = Minecraft.getInstance().getResourceManager();
        if (normalId != null && manager.getResource(normalId).isEmpty()) normalId = null;
        if (specularId != null && manager.getResource(specularId).isEmpty()) specularId = null;

        Identifier[] result = new Identifier[] { normalId, specularId };
        ENTITY_PBR_SIBLINGS.put(albedoTexture, result);
        return result;
    }

    /** {@code .../foo.png} → {@code .../foo_n.png}；非 .png 的贴图路径直接放弃。 */
    private static Identifier siblingTextureId(Identifier albedo, String suffix) {
        String path = albedo.getPath();
        if (!path.endsWith(".png")) return null;
        return Identifier.fromNamespaceAndPath(
                albedo.getNamespace(),
                path.substring(0, path.length() - 4) + suffix + ".png");
    }

    /** 取一张已注册贴图的 Vulkan 句柄；视图尚未就绪时返回 null（调用方据此重试）。 */
    private static long[] entityTextureHandle(Identifier textureId) {
        try {
            AbstractTexture texture = Minecraft.getInstance()
                    .getTextureManager()
                    .getTexture(textureId);
            if (texture == null) return null;
            GpuTextureView view = texture.getTextureView();
            GpuSampler sampler = texture.getSampler();
            if (view instanceof VulkanGpuTextureView vulkanView
                    && sampler instanceof VulkanGpuSampler vulkanSampler) {
                long imageView = vulkanView.vkImageView();
                long vkSampler = vulkanSampler.vkSampler();
                if (imageView != 0L && vkSampler != 0L) {
                    return new long[] { imageView, vkSampler };
                }
            }
        } catch (RuntimeException ignored) {
        }
        return null;
    }

    private static void publishTextureHandle(int slot, long imageView, long sampler) {
        if (slot < 1 || slot > MAX_TEXTURE_SLOTS || imageView == 0L || sampler == 0L) return;
        int index = slot - 1;
        if (publishedEntityViews[index] == imageView && publishedEntitySamplers[index] == sampler) {
            slotTexturePublished[index] = true;
            return;
        }
        publishedEntityViews[index] = imageView;
        publishedEntitySamplers[index] = sampler;
        slotTexturePublished[index] = true;
        NativeVulkan.updateEntityTexture(slot, imageView, sampler);
    }

    private static RenderType customRenderType;
    private static boolean customBatchOpen;

    /** 由 {@code RenderTypeFeatureRendererMixin} 在 getVertexBuilder 返回时调用。 */
    public static void beginCustomSubmit(RenderType renderType) {
        if (!CAPTURE_ENABLED) {
            return;
        }
        ++genericSubmitCount;
        // ⚠ 2026-11-16 修：一次 prepareGroup 里会**连续出现多个不同 RenderType** 的 custom 提交。
        // 26.3 的 CustomFeatureRenderer#buildGroup 是 for 循环逐个 getVertexBuilder(renderType)，
        // 而同一个 Group 内部用 draws 列表承载多个 RenderType（见 RenderTypeFeatureRenderer.Group），
        // 我们的 endCustomGroup() 只在 prepareGroup 收尾时调一次。
        //
        // 原实现在 activeBatch 非空时直接 return，后果是：整组只有**第一个** RenderType 真正
        // 开出了批次，后面所有提交（含它们的贴图槽和 positionOnlyCapture 判定）都被塞进这
        // 一个批次。地图墙就是最典型的受害者 —— 几十张 map/<id> 全部采样第一张地图的贴图，
        // 于是"所有展示框地图永远长一个样子"；而"哪张是第一张"取决于该帧提交排序，于是"会闪"。
        //
        // 正确语义：RenderType 变了就必须收掉上一批（贴图槽是批次的属性，不是组的属性）；
        // 只有 RenderType 相同才复用同一批次。
        if (customBatchOpen) {
            if (customRenderType == renderType) {
                return;
            }
            flushCustomBatch();
        } else if (activeBatch.get() != null) {
            // 别的子系统（ModelFeatureRenderer 的模型批次）正开着槽，不归我们管。
            return;
        }
        if (renderType == null) {
            return;
        }
        final String typeName = renderTypeName(renderType);
        if (typeName != null && typeName.contains("shadow")) {
            customDiagLog("drop:shadow", typeName, null);
            return;
        }
        if (isScreenOverlayRenderType(typeName)) {
            customDiagLog("drop:screen_overlay", typeName, null);
            return;
        }
        final boolean endPortal = typeName != null && typeName.contains("end_portal");
        Identifier texture = endPortal ? resolveSampler(renderType, "Sampler1") : null;
        if (texture == null) {
            texture = resolveSampler(renderType, "Sampler0");
        }
        if (texture == null) {
            customDiagLog("drop:no_texture", typeName, null);
            return;
        }
        int textureSlot = slotFor(texture);
        if (textureSlot == 0) {
            customDiagLog("drop:no_slot", typeName, texture);
            return;
        }
        customDiagLog("captured", typeName, texture);
        ActiveBatch customBatch = acquireBatch(textureSlot, false,
                endPortal || isEmissiveGeometry(texture));
        customBatch.projectiveUv = endPortal;
        // 2026-11-15：地图（含物品展示框里的地图）与装饰图标走短格式 addVertex(FFF)，
        // 必须显式打开短路径捕获，见 beginShortVertex。判据用贴图路径而不是 RenderType
        // 名字：地图是动态贴图 minecraft:map/<id>，装饰是 map_decorations 图集。
        customBatch.positionOnlyCapture = isMapGeometry(texture);
        if (customBatch.positionOnlyCapture) {
            customDiagLog("map_short", typeName, texture);
        }
        // 2026-11-03 诊断：第一人称下这条 custom 路径就是"手持物品"（RenderTypeFeatureRendererMixin
        // 的注释第 28 行：Item → 物品展示框里的物品、掉落物、第一人称手持物品）。
        // 打上标记后 uploadBatch 会顺带打印它的 vertexCount 与 UV 范围。
        customBatch.diagHand = Minecraft.getInstance().options.getCameraType().isFirstPerson();
        if (endPortal && !endPortalLogged) {
            endPortalLogged = true;
            System.out.println("[RTX-ENTITY] end portal geometry captured, slot="
                    + textureSlot + " texture=" + texture);
        }
        activeBatch.set(customBatch);
        customBatchOpen = true;
        customRenderType = renderType;
    }

    /** 由 {@code RenderTypeFeatureRendererMixin} 在 prepareGroup 收尾时调用。 */
    public static void endCustomGroup() {
        if (!CAPTURE_ENABLED) {
            return;
        }
        flushCustomBatch();
    }

    // =====================================================
    // 2026-11-04 诊断：custom 路径（物品展示框里的物品、掉落物、第一人称手持物、
    // 地图、末地传送门……）到底有没有被捕获。
    // 物品展示框里的**地图**不显形，需要先分清是"根本没进这条路径"（被某个 return
    // 挡了）还是"进来了但没画对"。每 64 帧打一组，每组最多 14 条。
    // =====================================================
    private static long customDiagFrame = -1L;
    private static int customDiagCount;

    private static void customDiagLog(String reason, String typeName, Identifier texture) {
        if (!isDiagnosticFrame()) {
            return;
        }
        if (customDiagFrame != captureFrameSerial) {
            customDiagFrame = captureFrameSerial;
            customDiagCount = 0;
        }
        if (customDiagCount >= 14) {
            return;
        }
        ++customDiagCount;
        System.err.println("[RTX-CUSTOM] " + reason
                + " type=" + typeName
                + " texture=" + texture);
    }

    private static void flushCustomBatch() {
        if (!customBatchOpen) {
            return;
        }
        customBatchOpen = false;
        customRenderType = null;
        // 批次收尾时丢掉没走完 setUv 的半截短格式顶点，避免它被下一个批次的
        // 第一个 addVertex 顺手 flush 进去（那会让后面所有顶点的 UV 整体错位）。
        cancelShortVertex();
        endModel();
    }

    /**
     * 屏幕特效（水下 / 火焰 / 方块遮罩）也是 submitCustomGeometry，但它是**贴着
     * 相机**的全屏四边形 —— 一旦当世界几何收进 TLAS，就会变成糊在镜头上的一层。
     * 只能按 RenderType 名字拒：火焰那一档的 Sampler0 是方块图集，按贴图路径
     * 会连正常方块一起拒掉。
     */
    private static boolean isScreenOverlayRenderType(String typeName) {
        return typeName != null && typeName.contains("screen_effect");
    }

    /**
     * 2026-11-15：这批 custom 几何是不是地图（地图本体或装饰图标）。
     *
     * <p>地图贴图是 {@code minecraft:map/<id>}（{@code MapTextureManager} 直接把
     * {@code Identifier} 注册成动态贴图，路径里没有 {@code textures/} 前缀）；装饰图标
     * 来自 {@code textures/atlas/map_decorations.png}。两者都用短格式写顶点，见
     * {@link #beginShortVertex}。
     */
    private static boolean isMapGeometry(Identifier texture) {
        if (texture == null) {
            return false;
        }
        final String path = texture.getPath();
        return path.startsWith("map/") || path.contains("map_decorations");
    }

    /** RenderType 的构造名（26.3 字段 {@code name}，经 {@code RenderTypeAccessor} 读）。 */
    private static String renderTypeName(RenderType renderType) {
        return ((RenderTypeAccessor) (Object) renderType).rtx$getName();
    }

    /**
     * 自带发光的几何。判据是 Sampler0 的贴图路径（这些渲染类型都把发光贴图
     * 挂在 Sampler0 上），命名与 26.3 资源路径一致。
     */
    private static boolean isEmissiveGeometry(Identifier texture) {
        final String path = texture.toString();
        return path.contains("end_sky")
                || path.contains("end_portal")
                || path.contains("end_gateway")
                || path.contains("beacon_beam")
                || path.contains("dragon_beam")
                || path.contains("dragon_fireball")
                || path.contains("guardian_beam");
    }

    private static Identifier resolveSampler0(RenderType renderType) {
        return resolveSampler(renderType, "Sampler0");
    }

    /**
     * 2026-11-04：这是不是旗帜的**图案层**。
     *
     * <p>旗帜 = 底布 + 逐层图案，几何完全共面。原版给 base-colour pass 的 order 是 0，
     * 但它其实画在"无色的、同样共面的底布"之后 —— 两者在 BVH 里完全打平，命中哪个
     * 是随机的，于是旗帜会随机显示成白布或缺失图案。</p>
     *
     * <p>判据与 Caustica 的 {@code RtEntityCollector#isBannerPattern} 一致：
     * 看 RenderPipeline 的 location。命中后本批次整体外移一个 rank
     * （见 {@link ActiveBatch#coplanarLayerBoost}）。</p>
     */
    private static boolean isBannerPattern(RenderType renderType) {
        final RenderPipeline pipeline = bannerPipeline(renderType);
        if (pipeline == null) {
            return false;
        }
        // 2026-11-16：优先用对象同一性判定。RenderPipelines.BANNER_PATTERN 是 register()
        // 进 PIPELINES_BY_LOCATION 的单例，所有旗帜图案层的 RenderSetup 都引用它；
        // 字符串比较只作兜底 —— withLocation("pipeline/banner_pattern") 的命名空间补全
        // 规则是推断来的，若格式有任何变化，只靠字符串会让整条旗帜层序静默失效
        // （表现：图案与底色共面、互相闪）。
        return pipeline == RenderPipelines.BANNER_PATTERN
                || "minecraft:pipeline/banner_pattern".equals(pipeline.getLocation().toString());
    }

    /** 取 RenderSetup 上的 pipeline；字段是包私有的，只能靠 accessor 读。 */
    private static RenderPipeline bannerPipeline(RenderType renderType) {
        if (renderType == null) {
            return null;
        }
        final RenderSetup setup = ((RenderTypeAccessor) (Object) renderType).rtx$getState();
        return setup == null ? null : ((RenderSetupAccessor) (Object) setup).rtx$getPipeline();
    }

    /**
     * 2026-11-15：还原原版 order 的层序（对应 Caustica 的 {@code pendingOrder + 1}）。
     *
     * <p>26.3 的 order 已经不再是"提交时的一个字段"，而是按 order 值分桶成多个
     * {@code SubmitNodeCollection}，feature 阶段拿不到单个 submit 的 order。好在同一面
     * 旗帜的底色层与每个图案层共用**同一个 PoseStack.Pose**（BannerRenderer#submitPatterns
     * 中间没有 push/pop），且按 order 升序处理，所以用"同一 Pose 第几次 banner_pattern
     * 提交"即可还原：底色层 = 1，第 i 层图案 = i + 2。底布不是 banner_pattern，rank = 0。</p>
     *
     * <p>返回 1 起步而不是 0：底布 rank 0 已经占掉"贴着模型"的那一层，图案层必须逐层外移
     * 一个 STEP，否则底色层会和底布重新共面。</p>
     */
    private static int nextBannerLayerRank(final PoseStack.Pose pose) {
        final Matrix4f matrix = pose == null ? null : pose.pose();
        long hash = 0xcbf29ce484222325L;
        if (matrix != null) {
            hash = hash * 31L + Float.floatToIntBits(matrix.m00());
            hash = hash * 31L + Float.floatToIntBits(matrix.m01());
            hash = hash * 31L + Float.floatToIntBits(matrix.m02());
            hash = hash * 31L + Float.floatToIntBits(matrix.m03());
            hash = hash * 31L + Float.floatToIntBits(matrix.m10());
            hash = hash * 31L + Float.floatToIntBits(matrix.m11());
            hash = hash * 31L + Float.floatToIntBits(matrix.m12());
            hash = hash * 31L + Float.floatToIntBits(matrix.m13());
            hash = hash * 31L + Float.floatToIntBits(matrix.m20());
            hash = hash * 31L + Float.floatToIntBits(matrix.m21());
            hash = hash * 31L + Float.floatToIntBits(matrix.m22());
            hash = hash * 31L + Float.floatToIntBits(matrix.m23());
            hash = hash * 31L + Float.floatToIntBits(matrix.m30());
            hash = hash * 31L + Float.floatToIntBits(matrix.m31());
            hash = hash * 31L + Float.floatToIntBits(matrix.m32());
            hash = hash * 31L + Float.floatToIntBits(matrix.m33());
        }
        final Integer previous = bannerPatternLayerRanks.get(hash);
        final int rank = previous == null ? 0 : previous;
        bannerPatternLayerRanks.put(hash, rank + 1);
        return rank + 1;
    }

    private static Identifier resolveSampler(RenderType renderType, String samplerName) {
        RenderSetup setup = ((RenderTypeAccessor) (Object) renderType).rtx$getState();
        Object binding = ((RenderSetupAccessor) (Object) setup).rtx$getTextures().get(samplerName);
        return binding == null ? null : ((TextureBindingAccessor) binding).rtx$getLocation();
    }

    private static ActiveBatch acquireBatch(
            int textureSlot, boolean localPlayerProxy, boolean emissive) {
        ActiveBatch batch = reusableBatch.get().reset(textureSlot, localPlayerProxy, emissive);
        batch.coplanarLayering = true;
        return batch;
    }

    private static final class ActiveBatch {
        private int textureSlot;
        private boolean localPlayerProxy;
        private boolean emissive;
        // 2026-11-03 诊断：这批是不是第一人称手部（state=Unit）。每次 beginModel 重设，
        // 不需要在 reset() 里清（避免漏一条路径就串味）。
        private boolean diagHand;
        // 2026-11-15：旗帜图案层的层序（1 = 底色层，2.. = 图案层；底布为 0）。
        // 由 nextBannerLayerRank 按"同一 Pose 的提交次数"给出，见 beginModel。
        private int coplanarLayerBoost;
        // 2026-11-04 诊断：本批次里 applyCoplanarLayerOffset 真正命中重复质心的次数。
        // 0 ⇒ 共面判定从未触发（那才是旗帜修不好的根因）；>0 ⇒ 偏移发生了但没起作用（方向问题）。
        private int coplanarHits;
        // 2026-11-04：本批次的 UV 映射（来自 ModelFeatureRenderer.Submit 的 record 字段）。
        //   null  ⇒ 整张贴图，UV 原样（mob 皮肤、items atlas 都是这种）
        //   非 null ⇒ ModelPart 的 0..1 UV 需要按 sprite 在图集里的矩形重映射
        //            （箱子 / 告示牌 / 床 / 旗帜 等方块实体走这条）
        // 不重映射的后果：采样落到图集左上角，贴图整片错乱。
        private net.minecraft.client.renderer.texture.UvMapping uvMapping;
        

        private boolean projectiveUv;
        // 2026-11-15：允许从短格式 addVertex(FFF)/setColor/setUv 收顶点。
        // 只对地图（含物品展示框里的地图）打开：其它短格式几何顶点数无上界，
        // 全量捕获曾把驱动打崩（见 beginShortVertex 的说明）。
        private boolean positionOnlyCapture;
        private float minX = Float.MAX_VALUE;
        private float minY = Float.MAX_VALUE;
        private float minZ = Float.MAX_VALUE;
        private float maxX = -Float.MAX_VALUE;
        private float maxY = -Float.MAX_VALUE;
        private float maxZ = -Float.MAX_VALUE;
        private ByteBuffer buffer = ByteBuffer.allocateDirect(768 * BYTES_PER_VERTEX).order(ByteOrder.nativeOrder());

        private final float[] poseDelta = new float[12];
        private boolean hasPoseDelta;
        private float motionX, motionY, motionZ;
        private ByteBuffer motionScratch;
        

        private java.util.HashMap<ModelBatchKey, VertexHistory> vertexHistory;
        private ModelBatchKey vertexHistoryKey;

        private static final int PENDING_VERTEX_FLOATS = 5;
        private final float[] pendingQuadVertices = new float[4 * PENDING_VERTEX_FLOATS];
        private final int[] pendingQuadColors = new int[4];
        private final CapturingVertexConsumer consumer = new CapturingVertexConsumer(this);
        private int pendingQuadVertexCount;
        private int vertexCount;
        private final java.util.HashMap<Long, Integer> coplanarQuadCounts = new java.util.HashMap<>();
        private boolean coplanarLayering;

        private ActiveBatch reset(int textureSlot, boolean localPlayerProxy, boolean emissive) {
            this.textureSlot = textureSlot;
            this.localPlayerProxy = localPlayerProxy;
            this.emissive = emissive;
            this.projectiveUv = false;
            this.positionOnlyCapture = false;
            this.coplanarLayering = false;
            this.coplanarHits = 0;
            this.minX = this.minY = this.minZ = Float.MAX_VALUE;
            this.maxX = this.maxY = this.maxZ = -Float.MAX_VALUE;
            buffer.clear();
            pendingQuadVertexCount = 0;
            vertexCount = 0;
            coplanarQuadCounts.clear();
            hasPoseDelta = false;
            uvMapping = null;
            coplanarLayerBoost = 0;
            java.util.Arrays.fill(poseDelta, 0.0F);
            motionX = motionY = motionZ = 0.0F;
            vertexHistory = null;
            vertexHistoryKey = null;
            return this;
        }

        private CapturingVertexConsumer captureConsumer() {
            consumer.reset();
            return consumer;
        }

        /**
         * 本批次顶点位置的落点数组；null = 这一批不缓存（文字/通用几何/开关关闭）。
         * 命中上一帧时返回历史数组本身（读一个下标、写同一个下标，原地滚动）。
         */
        private float[] vertexHistoryPositions(int floats) {
            if (vertexHistory == null || vertexHistoryKey == null) {
                return null;
            }
            final VertexHistory entry = vertexHistory.get(vertexHistoryKey);
            if (entry != null && entry.positions != null && entry.positions.length == floats) {
                return entry.positions;
            }
            return new float[floats];
        }

        /** 历史里这一批是否恰好是上一帧采样的（帧号连续才信任下标对应关系）。 */
        private boolean vertexHistoryIsPreviousFrame(int floats) {
            if (vertexHistory == null || vertexHistoryKey == null) {
                return false;
            }
            final VertexHistory entry = vertexHistory.get(vertexHistoryKey);
            return entry != null
                    && entry.frame == entityMotionFrame - 1
                    && entry.positions != null
                    && entry.positions.length == floats;
        }

        /** 把本帧位置登记为下一帧的 p_prev。 */
        private void storeVertexHistory(float[] positions) {
            VertexHistory entry = vertexHistory.get(vertexHistoryKey);
            if (entry == null) {
                if (vertexHistory.size() >= ENTITY_VERTEX_HISTORY_MAX_KEYS) {
                    vertexHistory.clear();
                }
                entry = new VertexHistory();
                vertexHistory.put(vertexHistoryKey, entry);
            }
            entry.positions = positions;
            entry.frame = entityMotionFrame;
        }

        private ByteBuffer motionScratchFor(int floats) {
            final int need = floats * Float.BYTES;
            if (motionScratch == null || motionScratch.capacity() < need) {
                motionScratch = ByteBuffer.allocateDirect(need).order(ByteOrder.nativeOrder());
            } else {
                motionScratch.clear();
            }
            return motionScratch;
        }

        private void capture(float x, float y, float z, int color, float u, float v) {
            int vertex = pendingQuadVertexCount++;
            int offset = vertex * PENDING_VERTEX_FLOATS;
            pendingQuadVertices[offset] = x;
            pendingQuadVertices[offset + 1] = y;
            pendingQuadVertices[offset + 2] = z;
            pendingQuadVertices[offset + 3] = u;
            pendingQuadVertices[offset + 4] = v;
            pendingQuadColors[vertex] = color;
            if (pendingQuadVertexCount == 4) {
                if (coplanarLayering) {
                    applyCoplanarLayerOffset();
                }
                emit(0); emit(1); emit(2);
                emit(0); emit(2); emit(3);
                pendingQuadVertexCount = 0;
            }
        }

        /**
         * 共面重合 quad 的稳定层序（见 {@code ENTITY_COPLANAR_LAYER_STEP} 的说明）。
         * 在 4 个顶点凑齐、准备 emit 前调用：质心与先前 quad 重合的，沿自身几何
         * 法线外移 dup × STEP —— 先画的不动，后画的依次前移，平局变确定性。
         * 顶点顺序按原版 CCW（法线朝外）约定，所以外移方向就是"朝观察者那一侧"。
         */
        private void applyCoplanarLayerOffset() {
            final float ax = pendingQuadVertices[0];
            final float ay = pendingQuadVertices[1];
            final float az = pendingQuadVertices[2];
            final float bx = pendingQuadVertices[PENDING_VERTEX_FLOATS];
            final float by = pendingQuadVertices[PENDING_VERTEX_FLOATS + 1];
            final float bz = pendingQuadVertices[PENDING_VERTEX_FLOATS + 2];
            final float cx = pendingQuadVertices[2 * PENDING_VERTEX_FLOATS];
            final float cy = pendingQuadVertices[2 * PENDING_VERTEX_FLOATS + 1];
            final float cz = pendingQuadVertices[2 * PENDING_VERTEX_FLOATS + 2];
            final float dx = pendingQuadVertices[3 * PENDING_VERTEX_FLOATS];
            final float dy = pendingQuadVertices[3 * PENDING_VERTEX_FLOATS + 1];
            final float dz = pendingQuadVertices[3 * PENDING_VERTEX_FLOATS + 2];

            final float e1x = bx - ax, e1y = by - ay, e1z = bz - az;
            final float e2x = cx - ax, e2y = cy - ay, e2z = cz - az;
            float nx = e1y * e2z - e1z * e2y;
            float ny = e1z * e2x - e1x * e2z;
            float nz = e1x * e2y - e1y * e2x;
            final float length = (float) Math.sqrt(nx * nx + ny * ny + nz * nz);

            final long key = coplanarKey(
                    (ax + bx + cx + dx) * 0.25f,
                    (ay + by + cy + dy) * 0.25f,
                    (az + bz + cz + dz) * 0.25f);
            final Integer previous = coplanarQuadCounts.get(key);
            final int seenBefore = previous == null ? 0 : previous;
            coplanarQuadCounts.put(key, seenBefore + 1);
            if (seenBefore > 0) {
                ++coplanarHits;
            }

            // 2026-11-15：rank 的两种来源。
            //   · 旗帜图案层（coplanarLayerBoost > 0）：整批统一用该层的 order 序号，
            //     不再叠加本批内的 seenBefore —— 原版一个 submit 只有一个 order，
            //     同层的正反两面本就该在同一深度；若把背面再推一步，会正好撞上
            //     下一层图案的正面。rank = 1 是底色层，2.. 是图案层，层间距
            //     ENTITY_COPLANAR_LAYER_STEP = 2e-4（与 Caustica 的 ORDER_OFFSET 一致），
            //     ENTITY_COPLANAR_LAYER_MAX = 0.02 对应 100 层，远大于原版 20 层上限。
            //   · 其余几何：沿用"同批次内质心重复次数"，先画的不动、后画的依次前移。
            final int rank = coplanarLayerBoost > 0 ? coplanarLayerBoost : seenBefore;
            if (rank == 0 || length < 1.0e-6f) {
                return;
            }

            final float offset = Math.min(
                    rank * ENTITY_COPLANAR_LAYER_STEP, ENTITY_COPLANAR_LAYER_MAX);
            if (coplanarLayerBoost > 0) {
                // 2026-11-04：旗帜图案层**直接朝相机推**，不用几何法线。
                // 旗帜是双面渲染，顶点绕序不保证"法线朝外"；实测几何法线可能朝内，
                // 那样偏移会把图案推到底布**后面** ⇒ 表现正是"只有底色、没有图案"，
                // 而且因为被遮挡得干净，连错乱都看不到。
                // 顶点是相机相对坐标（native 侧才加 cameraWorld），所以相机在原点。
                final float centerX = (ax + bx + cx + dx) * 0.25f;
                final float centerY = (ay + by + cy + dy) * 0.25f;
                final float centerZ = (az + bz + cz + dz) * 0.25f;
                final float toCamera = (float) Math.sqrt(
                        centerX * centerX + centerY * centerY + centerZ * centerZ);
                if (toCamera <= 1.0e-4f) {
                    return;
                }
                nx = -centerX / toCamera * offset;
                ny = -centerY / toCamera * offset;
                nz = -centerZ / toCamera * offset;
            } else {
                nx = nx / length * offset;
                ny = ny / length * offset;
                nz = nz / length * offset;
            }
            for (int vertex = 0; vertex < 4; ++vertex) {
                final int slot = vertex * PENDING_VERTEX_FLOATS;
                pendingQuadVertices[slot] += nx;
                pendingQuadVertices[slot + 1] += ny;
                pendingQuadVertices[slot + 2] += nz;
            }
        }

        /** 质心量化到 1/256 格的 64 位散列键（重合 quad 量化后必得同键）。 */
        private static long coplanarKey(float x, float y, float z) {
            final long qx = Math.round(x * 256.0f);
            final long qy = Math.round(y * 256.0f);
            final long qz = Math.round(z * 256.0f);
            long hash = qx * 2654435761L;
            hash = hash * 31L + qy * 2246822519L;
            hash = hash * 31L + qz * 3266489917L;
            return hash;
        }

        private void emit(int vertex) {
            ensureCapacity(BYTES_PER_VERTEX);
            int offset = vertex * PENDING_VERTEX_FLOATS;
            int color = pendingQuadColors[vertex];
            buffer.putFloat(pendingQuadVertices[offset]).putFloat(pendingQuadVertices[offset + 1])
                    .putFloat(pendingQuadVertices[offset + 2]).putFloat(pendingQuadVertices[offset + 3])
                    .putFloat(pendingQuadVertices[offset + 4]);
            buffer.putFloat(((color >>> 16) & 0xFF) / 255.0f).putFloat(((color >>> 8) & 0xFF) / 255.0f);
            buffer.putFloat((color & 0xFF) / 255.0f).putFloat(((color >>> 24) & 0xFF) / 255.0f);
            buffer.putFloat(0.0f)
                    .putFloat(localPlayerProxy ? 1.0f : emissive ? -1.0f : 0.0f)
                    .putFloat(encodedTextureSlot());
            ++vertexCount;
            final float px = pendingQuadVertices[offset];
            final float py = pendingQuadVertices[offset + 1];
            final float pz = pendingQuadVertices[offset + 2];
            if (px < minX) minX = px;
            if (py < minY) minY = py;
            if (pz < minZ) minZ = pz;
            if (px > maxX) maxX = px;
            if (py > maxY) maxY = py;
            if (pz > maxZ) maxZ = pz;
        }

        private float encodedTextureSlot() {
            if (textureSlot < 1 || textureSlot > MAX_TEXTURE_SLOTS) {
                return 0.0f;
            }


            final int materialClass =
                    frameTextureClasses[textureSlot - 1];


            return materialClass <= 0
                    ? textureSlot
                    : textureSlot + materialClass / MATERIAL_CLASS_SCALE;
        }

        private void ensureCapacity(int extra) {
            if (buffer.remaining() >= extra) return;
            int target = buffer.position() + extra;
            int capacity = buffer.capacity();
            while (capacity < target) capacity = Math.multiplyExact(capacity, 2);
            ByteBuffer replacement = ByteBuffer.allocateDirect(capacity).order(ByteOrder.nativeOrder());
            buffer.flip(); replacement.put(buffer); buffer = replacement;
        }
    }

    private static final class CapturingVertexConsumer implements VertexConsumer {
        private final ActiveBatch batch;
        private boolean hasVertex;
        private float x, y, z, u, v;
        private int color = 0xFFFFFFFF;

        private CapturingVertexConsumer(ActiveBatch batch) { this.batch = batch; }

        @Override public VertexConsumer addVertex(float x, float y, float z) {
            flush();
            this.x = x; this.y = y; this.z = z;
            this.color = 0xFFFFFFFF; this.u = 0.0f; this.v = 0.0f;
            hasVertex = true;
            return this;
        }
        @Override public VertexConsumer setColor(int r, int g, int b, int a) {
            color = (a & 0xFF) << 24 | (r & 0xFF) << 16 | (g & 0xFF) << 8 | (b & 0xFF);
            return this;
        }
        @Override public VertexConsumer setColor(int color) { this.color = color; return this; }
        @Override public VertexConsumer setUv(float u, float v) { this.u = u; this.v = v; return this; }
        @Override public VertexConsumer setUv1(int u, int v) { return this; }
        @Override public VertexConsumer setUv2(int u, int v) { return this; }
        @Override public VertexConsumer setUv3(float u, float v) { return this; }
        @Override public VertexConsumer setNormal(float x, float y, float z) { return this; }
        @Override public VertexConsumer setLineWidth(float lineWidth) { return this; }
        private void finish() { flush(); }
        private void reset() { hasVertex = false; }
        private void flush() {
            if (hasVertex) {
                final float cu;
                final float cv;
                if (batch.projectiveUv) {
                    final float wx = x + cameraWorldX;
                    final float wy = y + cameraWorldY;
                    final float wz = z + cameraWorldZ;
                    cu = fract01(wx * 0.25f);
                    cv = fract01(wz * 0.25f + wy * 0.125f);
                } else {
                    cu = u;
                    cv = v;
                }
                batch.capture(x, y, z, color, cu, cv);
            }
            hasVertex = false;
        }
    }

}
