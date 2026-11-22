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
 * 2026-11-11 接管式渲染第一层 —— **变体 B′**（默认）：只取消间接路径的
 * {@code drawIndexedIndirect}，不动上游任何簿记。
 *
 * <p>掐点选在这里而不是 {@code renderGroup}：{@code renderLayers} 已经把
 * {@code TerrainUniform} / {@code Sampler0}（方块图集，native 侧图集句柄的唯一上报点）/
 * {@code Sampler2} 和管线都绑好了，取消 {@code render(...)} 只跳过 draw 本身。
 *
 * <p>上游保持原样：{@code sectionRenderDispatcher.lock()} 窗口、
 * {@code getRenderSectionSlice}、动态 UBO 写入、{@code largestIndexCount} 与
 * auto index buffer 申请，逐位与 vanilla 相同。
 */
@Mixin(ChunkSectionsToRender.DrawIndirect.class)
public class ChunkSectionsDrawIndirectMixin {

    @Inject(method = "render", at = @At("HEAD"), cancellable = true)
    private void rtx$skipIndirectDraw(
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
