package com.example.client.mixin;

import com.example.client.latency.InteractionLatencyProbe;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * 2026-11-13：交互延迟探针 —— 方块更新的原始派发时刻。纯读时钟，不改任何行为。
 *
 * <p>ClientLevel.sendBlockUpdated 是「方块状态真的变了 → 通知渲染层」的唯一入口
 * （内部调 levelExtractor.blockChanged + setBlockDirty）。如果它没在 ack 当下触发，
 * 说明 setBlock→onBlockStateChange 这条链根本没走或走了别的分支。</p>
 */
@Mixin(net.minecraft.client.multiplayer.ClientLevel.class)
public class ClientLevelProbeMixin {

    @Inject(method = "sendBlockUpdated", at = @At("HEAD"))
    private void probe$sendBlockUpdated(BlockPos pos, BlockState oldState, BlockState newState,
            int updateFlags, CallbackInfo ci) {
        InteractionLatencyProbe.noteBlockUpdated(pos, "sendBlockUpdated");
    }

    /**
     * 服务端确认包的落地函数（void）：记时刻 + 落地前状态是否已相同。
     * alreadySame=true ⇒ 客户端已预测过，本次是 no-op，不会触发任何 dirty。
     */
    @Inject(method = "setServerVerifiedBlockState", at = @At("HEAD"))
    private void probe$verifiedEnter(BlockPos pos, BlockState state, int updateFlags,
            CallbackInfo ci) {
        final boolean alreadySame =
                ((net.minecraft.world.level.Level) (Object) this).getBlockState(pos) == state;
        InteractionLatencyProbe.noteVerifiedEnter(pos, alreadySame);
    }

    @Inject(method = "setServerVerifiedBlockState", at = @At("RETURN"))
    private void probe$verifiedState(BlockPos pos, BlockState state, int updateFlags,
            CallbackInfo ci) {
        InteractionLatencyProbe.noteVerifiedState(pos);
    }
}
