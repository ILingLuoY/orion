package com.example.client.mixin;

import com.example.client.EntityRtCapture;
import net.minecraft.client.renderer.feature.ItemFeatureRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * 2026-11-11：物品几何缓存的**命中率探针**（只统计，不改任何行为）。
 *
 * <p>为什么要先测命中率：物品密集区一帧有 40~59 万次 {@code getVertexBuilder}
 * （≈ 每 quad 一次）、约 200 万顶点，其中原版写顶点 41~77 ms、我们逐顶点转换
 * 50~80 ms。要一次性消掉这两笔，唯一可行的路子是「同一个物品的几何只转换一次，
 * 之后直接复用」——命中缓存时既 cancel 原版 {@code prepareSubmit}（那份顶点本来就被
 * RT 覆盖）、也跳过我们的逐顶点捕获。
 *
 * <p>前提是键要稳定：键取自 {@code ItemFeatureRenderer.Submit} 的全部渲染输入
 * （pose / displayContext / light / overlay / outlineColor / tintLayers / quads / foil）。
 * 如果 {@code quads} 列表每帧新建，identity 会变 ⇒ 命中率掉到 0，那就得改成按
 * quad 元素哈希。**先测出来再决定**，不要盲改。
 *
 * <p>观测：{@code [LEVEL PHASES FEAT]} 里的 {@code itemKeys/f}（本帧不同键数）与
 * {@code hit%}（跨帧命中率）。需要 {@code ORION_LEVEL_PHASES=1}。
 */
@Mixin(ItemFeatureRenderer.class)
public class ItemFeatureRendererSubmitMixin {

    @Inject(method = "prepareSubmit", at = @At("HEAD"))
    private void rtx$noteItemSubmit(ItemFeatureRenderer.Submit submit, CallbackInfo ci) {
        EntityRtCapture.noteItemSubmitKey(submit);
    }
}
