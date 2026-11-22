package com.example.client.mixin;

import com.example.client.latency.InteractionLatencyProbe;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * 2026-11-13：交互延迟探针 —— dirty 标记时刻。纯读时钟，不改任何行为。
 * LevelExtractor.setSectionDirty(x,y,z,playerChanged) 是方块更新进入
 * SectionUpdateTracker 的唯一入口（setBlocksDirty/setBlockDirty 内部都收敛到这里）。
 * 语义见 {@link InteractionLatencyProbe}。
 */
@Mixin(net.minecraft.client.renderer.extract.LevelExtractor.class)
public class LevelExtractorProbeMixin {

    @Inject(method = "setSectionDirty(IIIZ)V", at = @At("HEAD"))
    private void probe$dirtyMark(int sectionX, int sectionY, int sectionZ, boolean playerChanged,
            CallbackInfo ci) {
        InteractionLatencyProbe.noteDirtyMark(sectionX, sectionY, sectionZ);
    }

    @Inject(method = "setBlockDirty(Lnet/minecraft/core/BlockPos;Lnet/minecraft/world/level/block/state/BlockState;Lnet/minecraft/world/level/block/state/BlockState;)V", at = @At("HEAD"))
    private void probe$setBlockDirty(net.minecraft.core.BlockPos pos,
            net.minecraft.world.level.block.state.BlockState oldState,
            net.minecraft.world.level.block.state.BlockState newState, CallbackInfo ci) {
        InteractionLatencyProbe.noteBlockUpdated(pos, "setBlockDirty");
    }
}
