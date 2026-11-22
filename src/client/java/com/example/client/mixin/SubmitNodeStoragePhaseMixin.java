package com.example.client.mixin;

import com.example.client.LevelPhaseProbe;
import net.minecraft.client.renderer.SubmitNodeStorage;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

import java.util.function.Consumer;

/**
 * 2026-11-11：{@code prepareFrame} 子段 1 —— {@code drainPhases}（按 phase 排序分组）。
 * 纯读时钟，见 {@link LevelPhaseProbe} 的第二层说明。
 */
@Mixin(SubmitNodeStorage.class)
public class SubmitNodeStoragePhaseMixin {

    @Inject(method = "drainPhases", at = @At("HEAD"))
    private void rtx$featSortHead(Consumer<?> consumer, CallbackInfo ci) {
        LevelPhaseProbe.featSortHead();
    }

    @Inject(method = "drainPhases", at = @At("RETURN"))
    private void rtx$featSortTail(Consumer<?> consumer, CallbackInfo ci) {
        LevelPhaseProbe.featSortTail();
    }
}
