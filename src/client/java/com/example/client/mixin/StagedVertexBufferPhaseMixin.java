package com.example.client.mixin;

import com.example.client.LevelPhaseProbe;
import net.minecraft.client.renderer.StagedVertexBuffer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * 2026-11-11：{@code prepareFrame} 子段 3 —— {@code StagedVertexBuffer.upload()}
 * （把这一帧所有要素顶点写进共享 GPU 缓冲；池化失败时会在这里 {@code createBuffer}）。
 */
@Mixin(StagedVertexBuffer.class)
public class StagedVertexBufferPhaseMixin {

    @Inject(method = "upload", at = @At("HEAD"))
    private void rtx$featUploadHead(CallbackInfo ci) {
        LevelPhaseProbe.featUploadHead();
    }

    @Inject(method = "upload", at = @At("RETURN"))
    private void rtx$featUploadTail(CallbackInfo ci) {
        LevelPhaseProbe.featUploadTail();
    }
}
