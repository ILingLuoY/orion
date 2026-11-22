package com.example.client.mixin;

import com.example.client.latency.InteractionLatencyProbe;
import net.minecraft.client.renderer.chunk.RenderSectionRegion;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * 2026-11-13：交互延迟探针 —— 提取时刻。纯读时钟，不改任何行为。
 * SectionUpdateRenderState 是 record (long sectionNode, boolean playerChanged,
 * RenderSectionRegion region)，canonical constructor 即提取循环里
 * levelRenderState.sectionUpdateRenderStates.add(...) 的那一刻。
 * 语义见 {@link InteractionLatencyProbe}。
 */
@Mixin(net.minecraft.client.renderer.state.level.SectionUpdateRenderState.class)
public class SectionUpdateRenderStateProbeMixin {

    @Inject(method = "<init>(JZLnet/minecraft/client/renderer/chunk/RenderSectionRegion;)V",
            at = @At("TAIL"))
    private void probe$extracted(long sectionNode, boolean playerChanged,
            RenderSectionRegion region, CallbackInfo ci) {
        InteractionLatencyProbe.noteExtracted(sectionNode);
    }
}
