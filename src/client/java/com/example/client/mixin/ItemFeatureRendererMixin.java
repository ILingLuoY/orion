package com.example.client.mixin;

import com.example.client.EntityRtCapture;
import net.minecraft.client.renderer.feature.FeatureFrameContext;
import net.minecraft.client.renderer.feature.ItemFeatureRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

import java.util.List;

/**
 * 2026-11-11：物品组几何缓存（第二阶段）——命中时跳过原版**写顶点**。
 *
 * <p>⚠ 取消点必须在这里而不是 {@code prepareGroup}：
 * {@code prepareGroup} 会无条件 {@code groups.add(currentGroup)}，而
 * {@code executeGroup(groupIndex)} 用下标取同一个列表。在 HEAD 取消会让组数少一个
 * ⇒ {@code IndexOutOfBoundsException: Index 1 out of bounds for length 1}
 * （{@code crash-2026-11-11_21.35.19-client.txt}）。
 * 取消 {@code buildGroup} 则组照常入列、只是 {@code draws} 为空，
 * {@code executeGroup} 的循环走 0 次，安全。
 *
 * <p>命中时 {@code EntityRtCapture.beginGroup} 已用缓存顶点自建批次并上传完毕，
 * 所以原版这组的几何整段不写（省掉那 41~77 ms）。
 */
@Mixin(ItemFeatureRenderer.class)
public class ItemFeatureRendererMixin {

    @Inject(method = "buildGroup", at = @At("HEAD"), cancellable = true)
    private void rtx$buildGroupHead(
            FeatureFrameContext context,
            List<?> submits,
            CallbackInfo ci) {
        com.example.client.LevelPhaseProbe.itemBuildHead();

        final long key = EntityRtCapture.itemGroupKey(submits);
        if (key != 0L && EntityRtCapture.beginGroup(key)) {
            com.example.client.LevelPhaseProbe.itemBuildCancelled();
            ci.cancel();
        }
    }

    @Inject(method = "buildGroup", at = @At("RETURN"))
    private void rtx$buildGroupTail(
            FeatureFrameContext context,
            List<?> submits,
            CallbackInfo ci) {
        com.example.client.LevelPhaseProbe.itemBuildTail();
        EntityRtCapture.finishGroup();
    }
}
