package com.example.client.mixin;

import com.example.client.TerrainTakeover;
import com.mojang.renderpearl.api.commands.RenderPass;
import net.minecraft.client.renderer.feature.FeatureRenderDispatcher;
import net.minecraft.client.renderer.oit.OitStage;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * 2026-11-11 接管式渲染第二档：关掉主 pass 里的**原版要素光栅**。
 *
 * <p>这些 {@code execute*} 的唯一产物是 main render target 上的像素，而那张图随后会被
 * RT 整屏覆盖 ⇒ 录制它们是纯浪费（CPU 录命令 + GPU 光栅）。
 *
 * <p><b>不在其中</b>：手持物 / 屏幕特效走的是静态方法
 * {@code FeatureRenderDispatcher.renderAllFeatures(renderPass, frame)}（见
 * {@code GameRenderer.renderItemInHand} 与 {@code renderScreenEffects}），不经过这里，
 * 所以手持物与水下、着火等屏幕特效照旧。
 *
 * <p><b>几何捕获不受影响</b>：我们的实体/粒子捕获发生在 {@code submit} 与
 * {@code prepareFrame} 阶段（模型顶点在被提交成节点时读取），早于这些 {@code execute*}。
 *
 * <p>开关：{@code ORION_TAKEOVER} 档位 ≥ 2（默认）且 RT 上一帧确实出图。
 */
@Mixin(FeatureRenderDispatcher.PreparedFrame.class)
public class FeaturePreparedFrameTakeoverMixin {

    @Inject(method = "executeSolid", at = @At("HEAD"), cancellable = true)
    private void rtx$skipSolid(RenderPass renderPass, CallbackInfo ci) {
        if (TerrainTakeover.skipFeatureRaster()) {
            ci.cancel();
        }
    }

    @Inject(method = "executeTranslucent", at = @At("HEAD"), cancellable = true)
    private void rtx$skipTranslucent(RenderPass renderPass, CallbackInfo ci) {
        if (TerrainTakeover.skipFeatureRaster()) {
            ci.cancel();
        }
    }

    @Inject(method = "executeTranslucentAfterTerrain", at = @At("HEAD"), cancellable = true)
    private void rtx$skipTranslucentAfterTerrain(RenderPass renderPass, CallbackInfo ci) {
        if (TerrainTakeover.skipFeatureRaster()) {
            ci.cancel();
        }
    }

    @Inject(method = "executeOit", at = @At("HEAD"), cancellable = true)
    private void rtx$skipOit(OitStage stage, RenderPass renderPass, CallbackInfo ci) {
        if (TerrainTakeover.skipFeatureRaster()) {
            ci.cancel();
        }
    }

    @Inject(method = "executeWaterMask", at = @At("HEAD"), cancellable = true)
    private void rtx$skipWaterMask(RenderPass renderPass, CallbackInfo ci) {
        if (TerrainTakeover.skipFeatureRaster()) {
            ci.cancel();
        }
    }

    @Inject(method = "executeOutline", at = @At("HEAD"), cancellable = true)
    private void rtx$skipOutline(RenderPass renderPass, CallbackInfo ci) {
        if (TerrainTakeover.skipFeatureRaster()) {
            ci.cancel();
        }
    }

    @Inject(method = "executeSeeThrough", at = @At("HEAD"), cancellable = true)
    private void rtx$skipSeeThrough(RenderPass renderPass, CallbackInfo ci) {
        if (TerrainTakeover.skipFeatureRaster()) {
            ci.cancel();
        }
    }

    @Inject(method = "executeAlwaysOnTop", at = @At("HEAD"), cancellable = true)
    private void rtx$skipAlwaysOnTop(RenderPass renderPass, CallbackInfo ci) {
        if (TerrainTakeover.skipFeatureRaster()) {
            ci.cancel();
        }
    }
}
