package com.example.client.mixin;

import com.example.client.LevelPhaseProbe;
import net.minecraft.client.renderer.SubmitNodeStorage;
import net.minecraft.client.renderer.feature.FeatureRenderDispatcher;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * 2026-11-11：量 {@code prepareFrame} —— 提交节点树 → {@code PreparedFrame}。
 *
 * <p>为什么值得单独量：这一步每帧把 {@code SubmitNodeStorage} 里的所有节点重新分组、
 * 排序、写进动态顶点缓冲，是「level 段 3.49 MB/帧」最可疑的分配源之一
 * （{@code PreparedGroup} 列表 + 顶点缓冲切片）。它和 {@code submitFeatures}
 * （收集节点）是两笔账：前者我们**不能**跳过（实体几何捕获发生在提交阶段之后、
 * 且顶点数据就是在这里被我们读走的），所以要靠数字判断值不值得再拆。
 *
 * <p>纯读时钟，需 {@code ORION_LEVEL_PHASES=1}。
 */
@Mixin(FeatureRenderDispatcher.class)
public class FeatureRenderDispatcherProbeMixin {

    @Inject(method = "prepareFrame", at = @At("HEAD"))
    private void rtx$featurePrepHead(
            SubmitNodeStorage storage,
            CallbackInfoReturnable<FeatureRenderDispatcher.PreparedFrame> cir) {
        LevelPhaseProbe.featureHead();
    }

    @Inject(method = "prepareFrame", at = @At("RETURN"))
    private void rtx$featurePrepTail(
            SubmitNodeStorage storage,
            CallbackInfoReturnable<FeatureRenderDispatcher.PreparedFrame> cir) {
        LevelPhaseProbe.featureTail();
    }
}
