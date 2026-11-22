package com.example.client.mixin;

import com.example.client.LevelPhaseProbe;
import com.mojang.renderpearl.api.commands.RenderPass;
import com.mojang.renderpearl.api.textures.GpuSampler;
import com.mojang.renderpearl.api.textures.GpuTextureView;
import net.minecraft.client.renderer.chunk.ChunkSectionLayerGroup;
import net.minecraft.client.renderer.chunk.ChunkSectionsToRender;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * {@code ChunkSectionsToRender.renderGroup} 的计时点（{@code [LEVEL PHASES]} 的
 * {@code terrainDraw}）。
 *
 * <p><b>这里不做取消</b>。2026-11-11 实测：直接在 {@code renderGroup} 头部取消会崩
 * （`nvoglv64.dll` 里 AV，读 0x0f0，栈顶是我们自己的 {@code recordFrameCommand}）。
 * 原因很可能是 {@code renderLayers} 里的 {@code setUniform("Sampler0", blocks_atlas, …)}
 * 是我们的图集句柄唯一的上报点（{@code VulkanRenderPassAtlasHijackMixin}，
 * 见其 {@code "minecraft:textures/atlas/blocks.png"} 分支），取消后 native 侧拿不到
 * 本帧图集。
 *
 * <p>因此取消点上移到两个具体实现类的 {@code render(...)}：
 * 那里只发 draw，图集/管线/调试组都已绑定完毕。见
 * {@link ChunkSectionsDrawIndirectMixin} 与 {@link ChunkSectionsDrawSeparateMixin}。
 */
@Mixin(ChunkSectionsToRender.class)
public class ChunkSectionsToRenderMixin {

    @Inject(method = "renderGroup", at = @At("HEAD"))
    private void rtx$terrainDrawHead(
            ChunkSectionLayerGroup group,
            RenderPass renderPass,
            GpuSampler sampler,
            GpuTextureView atlas,
            boolean renderWireframeTerrain,
            CallbackInfo ci) {
        LevelPhaseProbe.terrainHead();
    }

    @Inject(method = "renderGroup", at = @At("RETURN"))
    private void rtx$terrainDrawTail(
            ChunkSectionLayerGroup group,
            RenderPass renderPass,
            GpuSampler sampler,
            GpuTextureView atlas,
            boolean renderWireframeTerrain,
            CallbackInfo ci) {
        LevelPhaseProbe.terrainTail();
    }
}
