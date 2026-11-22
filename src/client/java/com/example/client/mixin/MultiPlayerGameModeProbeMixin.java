package com.example.client.mixin;

import com.example.client.latency.InteractionLatencyProbe;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.InteractionHand;
import net.minecraft.world.InteractionResult;
import net.minecraft.world.phys.BlockHitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * 2026-11-13：交互延迟探针 —— 动作发出时刻登记。纯读时钟，不改任何行为。
 * 语义见 {@link InteractionLatencyProbe}。
 */
@Mixin(net.minecraft.client.multiplayer.MultiPlayerGameMode.class)
public class MultiPlayerGameModeProbeMixin {

    @Inject(method = "startDestroyBlock", at = @At("HEAD"))
    private void probe$destroyStart(BlockPos pos, Direction direction,
            CallbackInfoReturnable<Boolean> cir) {
        InteractionLatencyProbe.onAction(pos, "breakStart");
    }

    @Inject(method = "destroyBlock", at = @At("HEAD"))
    private void probe$destroyDone(BlockPos pos, CallbackInfoReturnable<Boolean> cir) {
        InteractionLatencyProbe.onAction(pos, "breakDone");
    }

    @Inject(method = "useItemOn", at = @At("HEAD"))
    private void probe$useOn(LocalPlayer player, InteractionHand hand, BlockHitResult hit,
            CallbackInfoReturnable<InteractionResult> cir) {
        if (hit == null) {
            return;
        }
        final BlockPos hitPos = hit.getBlockPos();
        InteractionLatencyProbe.onAction(hitPos, "useItemOn");
        // 放置落点 = 点中面外侧一格；开门/交互时落点命中不到也无害（5s 过期）。
        final Direction face = hit.getDirection();
        if (face != null) {
            InteractionLatencyProbe.watchExtra(hitPos.relative(face), "useItemOn(place)");
        }
    }
}
