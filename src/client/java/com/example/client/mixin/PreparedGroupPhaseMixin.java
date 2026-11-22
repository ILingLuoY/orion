package com.example.client.mixin;

import com.example.client.LevelPhaseProbe;
import net.minecraft.client.renderer.feature.FeatureFrameContext;
import net.minecraft.client.renderer.feature.FeatureRendererMap;
import net.minecraft.client.renderer.feature.FeatureRendererType;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

import java.util.List;

/**
 * 2026-11-11：{@code prepareFrame} 子段 2 —— {@code PreparedGroup.prepare}。
 *
 * <p>这是「3.49 MB/帧」与「13.9 ms 的 featurePrep」最可能的落点：每个 featureType 的
 * 每个 group 都要把提交节点转成顶点数据（模型解包、UV、法线、粒子四边形…），
 * 我们的实体/粒子捕获也是在这一段里把顶点读走的。
 *
 * <p>按 {@code FeatureRendererType.name()} 分别累加，用来区分到底是
 * 「实体模型 / 粒子 / 方块实体破坏动画 / 文字」里的哪一个贵。
 *
 * <p>目标类是包私有的内部类，所以用 {@code targets} 字符串指定，不能直接引用类型。
 */
@Mixin(targets = "net.minecraft.client.renderer.feature.FeatureRenderDispatcher$PreparedGroup")
public class PreparedGroupPhaseMixin {

    @Shadow
    private FeatureRendererType<?> featureType;

    @Inject(method = "prepare", at = @At("HEAD"))
    private void rtx$featGroupHead(
            FeatureFrameContext context,
            FeatureRendererMap renderers,
            List<?> allSubmits,
            CallbackInfo ci) {
        if (!LevelPhaseProbe.enabled()) {
            return;
        }
        final FeatureRendererType<?> type = this.featureType;
        LevelPhaseProbe.featGroupHead(type == null ? "?" : type.name());
    }

    @Inject(method = "prepare", at = @At("RETURN"))
    private void rtx$featGroupTail(
            FeatureFrameContext context,
            FeatureRendererMap renderers,
            List<?> allSubmits,
            CallbackInfo ci) {
        LevelPhaseProbe.featGroupTail();
    }
}
