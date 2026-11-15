package com.example.client.mixin;

import com.example.client.EntityRtCapture;
import com.mojang.blaze3d.vertex.BufferBuilder;
import com.mojang.blaze3d.vertex.VertexConsumer;
import org.lwjgl.system.MemoryUtil;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(BufferBuilder.class)
public abstract class BufferBuilderMixin {

    @Shadow
    private boolean blockFormat;

    @Shadow
    private long vertexPointer;

    /**
     * 2026-11-15：全格式顶点不会走短格式 setter，进来时把可能残留的半截短格式状态丢掉。
     * 地图（{@code MapRenderer}）走的是短路径，见下面三个钩子与
     * {@link EntityRtCapture#beginShortVertex}。
     */
    @Inject(
            method = "addVertex(FFFIFFIIFFF)V",
            at = @At("HEAD")
    )
    private void onAddVertexHead(
            float x,
            float y,
            float z,
            int color,
            float u,
            float v,
            int overlayCoords,
            int lightCoords,
            float nx,
            float ny,
            float nz,
            CallbackInfo ci
    ) {
        EntityRtCapture.cancelShortVertex();
    }

    /** 短格式顶点起点：{@code MapRenderer} 的地图/装饰四边形走这条。 */
    @Inject(
            method = "addVertex(FFF)Lcom/mojang/blaze3d/vertex/VertexConsumer;",
            at = @At("TAIL")
    )
    private void onAddVertexShort(
            float x,
            float y,
            float z,
            CallbackInfoReturnable<VertexConsumer> cir
    ) {
        EntityRtCapture.beginShortVertex(x, y, z);
    }

    @Inject(
            method = "setColor(I)Lcom/mojang/blaze3d/vertex/VertexConsumer;",
            at = @At("TAIL")
    )
    private void onSetColor(
            int color,
            CallbackInfoReturnable<VertexConsumer> cir
    ) {
        EntityRtCapture.setShortVertexColor(color);
    }

    /** 短格式顶点的收尾：vanilla 的调用顺序是 addVertex → setColor → setUv → setLight。 */
    @Inject(
            method = "setUv(FF)Lcom/mojang/blaze3d/vertex/VertexConsumer;",
            at = @At("TAIL")
    )
    private void onSetUv(
            float u,
            float v,
            CallbackInfoReturnable<VertexConsumer> cir
    ) {
        EntityRtCapture.setShortVertexUv(u, v);
    }

    @Inject(
            method = "addVertex(FFFIFFIIFFF)V",
            at = @At("TAIL")
    )
    private void onAddVertex(
            float x,
            float y,
            float z,
            int color,
            float u,
            float v,
            int overlayCoords,
            int lightCoords,
            float nx,
            float ny,
            float nz,
            CallbackInfo ci
    ) {
        EntityRtCapture.captureVertex(x, y, z, color, u, v);

        if (!this.blockFormat) {
            return;
        }

        if (this.vertexPointer == -1L) {
            return;
        }

        if (Float.isNaN(nx) || Float.isInfinite(nx)) {
            return;
        }

        int materialId = Math.round(nx);

        if (materialId < 0) {
            materialId = 0;
        }

        if (materialId > 255) {
            materialId = 255;
        }

        MemoryUtil.memPutByte(
                this.vertexPointer + 31L,
                (byte) materialId
        );
    }

}
