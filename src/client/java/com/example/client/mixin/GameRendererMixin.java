package com.example.client.mixin;

import com.example.client.EntityRtCapture;
import com.example.client.FrameBudget;
import com.example.client.bench.BenchmarkSession;
import com.example.vulkan.NativeVulkan;
import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.renderpearl.api.commands.RenderPass;
import com.mojang.renderpearl.api.textures.GpuTexture;
import com.mojang.renderpearl.backend.vulkan.VulkanGpuTexture;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.GameRenderer;
import net.minecraft.client.renderer.feature.FeatureRenderDispatcher;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.Redirect;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * 2026-10-06：每帧复位实体捕获的"已开窗"标记，并且**把光追合成提前到手之前**。
 *
 * <p>{@code GameRenderer.render()} 是**每个渲染帧**必然执行的最外层入口（26.3 无语参，
 * 已按字节码核对）。实体帧窗口由 {@code LevelRenderer} 打开并收帧；如果某一帧开了窗
 * 却没走到收帧（关卡未渲染：进世界加载中、全屏界面、第三人称且无实体提交等），标记会
 * 一直挂着，之后就不开窗了。这里只清 Java 侧标记，不产生任何原生调用。</p>
 */
@Mixin(GameRenderer.class)
public class GameRendererMixin {

    @Inject(method = "render", at = @At("HEAD"))
    private void rtx$resetEntityFrameOpenFlag(CallbackInfo ci) {
        EntityRtCapture.resetEntityFrameOpen();
        FrameBudget.head();
        BenchmarkSession.frameStart();
    }

    /**
     * 2026-10-13：帧预算二分器的出口端（见 {@link FrameBudget}）。
     *
     * <p>与上面的 HEAD 配对，量的是 {@code GameRenderer.render()} **自身**的墙钟耗时。
     * 它与帧间隔之差就是"不在渲染里"的那一段（tick / GC / 呈现与帧生成排队 / 等待）——
     * 这是至今所有日志都读不出来的那个洞：帧间隔 90ms 而 GPU 只有 15ms、我们自己的
     * C++ 只有 7ms 时，那几十毫秒到底在 render 里还是外面。</p>
     */
    @Inject(method = "render", at = @At("RETURN"))
    private void rtx$frameBudgetTail(CallbackInfo ci) {
        FrameBudget.tail();
        BenchmarkSession.frameEnd();
    }

    /**
     * 光追画面必须在**第一人称手之前**写进 main target —— 否则光栅手会被整片盖掉。
     *
     * <p>原先的注入点在 GUI 之前（{@code GuiRendererMixin} 上膛，见
     * {@code VulkanCommandEncoderMixin.vulkan$injectRtBeforeGui}）。而 MC 的绘制顺序是
     * 世界 → **手** → GUI：RT 在 GUI 前写入 ⇒ MC 画好的手被整片盖掉 ⇒ 表现为
     * "GUI 看得见、手看不见"（手的光栅在，只是没了）。所以上膛点放在手之前：
     * RT 落在手那一遍渲染作用域之前，手随后画在 RT 之上并被保留。</p>
     *
     * <p>⚠ 2026-10-25 修正：上面的"保留"只对**光栅手**成立。手臂与手持物现在也会
     * 被捕获进 RT（帧窗口搬迁见 {@code EntityRtCapture.finishEntityFrame}），
     * 但 RT 那一份和光栅手画在同一屏幕位置、光栅手后画 ⇒ 看到的仍是原版光照版，
     * 不是 PBR/RT 版。要让 RT 手显形，必须另有一步取消光栅手的绘制；
     * 仅调整这里的上膛位置解决不了。</p>
     *
     * <p>上膛用 main render target 的颜色图（与 GUI 那条路径取的是同一张图），
     * 后面的注入逻辑（{@code VulkanCommandEncoderMixin}）完全不用改。第三人称或
     * 无手渲染时不会走到这里，{@code GuiRendererMixin} 的老路径仍然是兜底。</p>
     *
     * <p>2026-11-16：手已改为<b>退出光追、回到原版光栅</b>（默认
     * {@code HAND_RT_ENABLED == false}）。这一步只负责把 RT 合成上膛到光栅手之前，
     * 并在光栅手提交前收掉世界的实体帧，使手几何不进 RT。上膛位置不变——
     * RT 世界必须先写进 main target，光栅手才会画在它上面。</p>
     */
    @Inject(method = "renderItemInHand", at = @At("HEAD"))
    private void rtx$armRtInjectionBeforeHand(CallbackInfo ci) {
        if (HAND_RT_ENABLED) {
            // 旧行为：给手单独开一个实体帧窗口，手几何进 RT，再取消光栅手。
            EntityRtCapture.beginEarlyFrame();
            EntityRtCapture.beginHandCaptureWindow();
        } else {
            // 2026-11-16：手退回光栅。世界（含粒子/文字）在这一步之前已经全部
            // 提交完，所以这里直接收帧；手之后的 model/item 批次会撞上原生门禁
            // （Entities.cpp: if(!g_entityFrameOpen) return;）被丢弃 ⇒ 手彻底
            // 不参与光追，画面里的手就是原版光栅手。
            // 注意不能在这里再调 beginEarlyFrame()，那会重新开窗把手收进去。
            EntityRtCapture.finishEntityFrame();
            if (!rasterHandRestoredLogged) {
                rasterHandRestoredLogged = true;
                System.out.println("[RTX-HAND] hand restored to raster; RT hand capture"
                        + " disabled (-Dorion.handRt=1 to re-enable)");
            }
        }
        if (NativeVulkan.rtInjectedToMainTarget || NativeVulkan.guiRtPending) {
            return;
        }
        final Minecraft minecraft = Minecraft.getInstance();
        if (minecraft == null || minecraft.gameRenderer == null) {
            return;
        }
        final RenderTarget mainTarget = minecraft.gameRenderer.mainRenderTarget();
        if (mainTarget == null) {
            return;
        }
        final GpuTexture colorTexture = mainTarget.getColorTexture();
        if (!(colorTexture instanceof VulkanGpuTexture vulkanTexture)) {
            return;
        }
        if ((colorTexture.usage() & GpuTexture.USAGE_COPY_DST) == 0) {
            return;
        }
        final long image = vulkanTexture.vkImage();
        final int width = colorTexture.getWidth(0);
        final int height = colorTexture.getHeight(0);
        if (image == 0L || width <= 0 || height <= 0) {
            return;
        }
        NativeVulkan.pendingGuiTargetImage = image;
        NativeVulkan.pendingGuiTargetWidth = width;
        NativeVulkan.pendingGuiTargetHeight = height;
        NativeVulkan.guiRtPending = true;
        if (!rtSeamLogged) {
            rtSeamLogged = true;
            System.out.println("[RTX-SEAM] RT injection armed BEFORE first-person hand"
                    + " image=0x" + Long.toHexString(image)
                    + " " + width + "x" + height);
        }
    }

    

    @Inject(
            method = "renderItemInHand",
            at = @At(
                    value = "INVOKE",
                    target = "Lnet/minecraft/client/renderer/feature/FeatureRenderDispatcher;"
                            + "prepareFrame(Lnet/minecraft/client/renderer/SubmitNodeStorage;)"
                            + "Lnet/minecraft/client/renderer/feature/FeatureRenderDispatcher$PreparedFrame;",
                    shift = At.Shift.AFTER
            )
    )
    private void rtx$finishEntityFrameAfterHandPrepare(CallbackInfo ci) {
        EntityRtCapture.finishEntityFrame();
    }

    /**
     * 无手帧兜底：{@code renderLevel} 正常返回时若实体帧还开着，在这里收。
     *
     * <p>幂等（{@code EntityRtCapture.finishEntityFrame} 内部按 entityFrameOpen 早退），
     * 所以与手路径叠加调用是安全的。这同时覆盖"手路径提前 return（第三人称 /
     * HUD 隐藏 / 旁观）"的帧。注意粒子层不在这里收，它仍在
     * {@code LevelRendererMixin} 的 render RETURN。</p>
     */
    @Inject(method = "renderLevel", at = @At("HEAD"))
    private void rtx$frameBudgetRenderLevelHead(CallbackInfo ci) {
        FrameBudget.beginRenderLevel();
    }

    @Inject(method = "renderLevel", at = @At("RETURN"))
    private void rtx$finishEntityFrameFallback(CallbackInfo ci) {
        FrameBudget.endRenderLevel();
        EntityRtCapture.finishEntityFrame();
    }

    

    @Redirect(
            method = "renderItemInHand",
            at = @At(
                    value = "INVOKE",
                    target = "Lnet/minecraft/client/renderer/feature/FeatureRenderDispatcher;"
                            + "renderAllFeatures(Lcom/mojang/renderpearl/api/commands/RenderPass;"
                            + "Lnet/minecraft/client/renderer/feature/FeatureRenderDispatcher$PreparedFrame;)V"
            )
    )
    private void rtx$skipRasterHand(RenderPass renderPass, FeatureRenderDispatcher.PreparedFrame frame) {
        if (!HAND_RT_ENABLED) {
            // 默认路径：手走原版光栅（RT 手已关闭，见 resolveHandRtEnabled）。
            FeatureRenderDispatcher.renderAllFeatures(renderPass, frame);
            return;
        }
        if (!rasterHandSkipLogged) {
            rasterHandSkipLogged = true;
            System.out.println("[RTX-HAND] raster hand skipped; RT hand is the visible one"
                    + " (-Dorion.handRt=1)");
        }
    }

    private static boolean rasterHandSkipLogged;
    private static boolean rasterHandRestoredLogged;

    

    /**
     * 2026-11-16：默认改为 {@code false} —— 第一人称手/手持物退出光追，回到原版光栅。
     *
     * <p>为什么不是"只取消光栅手"：RT 手和光栅手画在同一屏幕位置，只能二选一。
     * 之前默认 RT 手，但 RT 侧的相机相对 MV / 投射 UV / 顶点色都被特殊处理过，
     * 手臂与手持物的观感与整体不一致，所以默认交回光栅。</p>
     *
     * <p>需要对比时仍可 {@code -Dorion.handRt=1}（或环境变量 {@code ORION_HAND_RT=1}）
     * 切回 RT 手，这条路径的捕获/上膛逻辑一并保留。</p>
     */
    private static final boolean HAND_RT_ENABLED = resolveHandRtEnabled();

    private static boolean resolveHandRtEnabled() {
        String raw = System.getProperty("orion.handRt");
        if (raw == null) {
            raw = System.getenv("ORION_HAND_RT");
        }
        // 只有显式写 "1" 才启用；未设置 = 关闭（与旧版本"默认开启"相反）。
        return raw != null && "1".equalsIgnoreCase(raw.trim());
    }

    private static boolean rtSeamLogged;
}
