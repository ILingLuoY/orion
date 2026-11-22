package com.example.client.mixin;

import com.example.client.TerrainTakeover;
import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.commands.RenderPass;
import com.mojang.renderpearl.api.pipeline.IndexType;
import com.mojang.renderpearl.api.pipeline.RenderPipeline;
import net.minecraft.client.renderer.chunk.ChunkSectionLayer;
import net.minecraft.client.renderer.chunk.ChunkSectionsToRender;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * 变体 B′ 的分离绘制路径：取消 {@code drawMultipleIndexed}。
 * 说明见 {@link ChunkSectionsDrawIndirectMixin}。
 */
@Mixin(ChunkSectionsToRender.DrawSeparate.class)
public class ChunkSectionsDrawSeparateMixin {

    @Inject(method = "render", at = @At("HEAD"), cancellable = true)
    private void rtx$skipSeparateDraw(
            ChunkSectionLayer layer,
            RenderPass renderPass,
            GpuBuffer defaultIndexBuffer,
            IndexType defaultIndexType,
            RenderPipeline renderPipelineOverride,
            RenderPipeline renderPipelineOverrideMultidraw,
            CallbackInfo ci) {
        if (TerrainTakeover.active()) {
            ci.cancel();
        }
    }
}
