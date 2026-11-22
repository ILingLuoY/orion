package com.example.client.mixin;

import com.example.client.LevelPhaseProbe;
import net.minecraft.client.renderer.StagedVertexBuffer;
import net.minecraft.client.renderer.rendertype.RenderType;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * 2026-11-12：数每个 group 里**新建 draw 的次数**与耗时。
 *
 * <p>背景：实测一帧只有 60~92 件物品，却产生 9.5~11.5 万次 {@code getVertexBuilder}
 * （每次 1.4 µs、每件 2.2 ms、单组最大 2.6 s）。正常应该是几十纳秒量级。
 *
 * <p>原版 {@code Group.getVertexBuilder}：
 * <pre>
 *   if (lastDraw == null || lastRenderType != renderType
 *       || !renderType.canConsolidateConsecutiveGeometry()) {
 *       lastDraw = getOrAddDraw(renderType);   // ← 这里
 *   }
 * </pre>
 * 若 {@code canConsolidateConsecutiveGeometry()} 为 false ⇒ **每个 quad 都新建 draw**，
 * 而 {@code getOrAddDraw} 里还有 {@code drawRenderTypes.indexOf(...)} 的线性扫描
 * ⇒ O(n²)。本探针就是去证实/否证这一点：如果 {@code draws/f} ≈ {@code vb/f}，
 * 就是 per-quad draw 爆炸。
 *
 * <p>纯计数 + 读时钟，不改行为。需要 {@code ORION_LEVEL_PHASES=1}。
 */
@Mixin(targets = "net.minecraft.client.renderer.feature.RenderTypeFeatureRenderer$Group")
public class RenderTypeGroupProbeMixin {

    @Inject(method = "getOrAddDraw", at = @At("HEAD"))
    private void rtx$drawHead(
            RenderType renderType,
            CallbackInfoReturnable<StagedVertexBuffer.Draw> cir) {
        LevelPhaseProbe.groupDrawHead();
    }

    @Inject(method = "getOrAddDraw", at = @At("RETURN"))
    private void rtx$drawTail(
            RenderType renderType,
            CallbackInfoReturnable<StagedVertexBuffer.Draw> cir) {
        LevelPhaseProbe.groupDrawTail();
    }
}
