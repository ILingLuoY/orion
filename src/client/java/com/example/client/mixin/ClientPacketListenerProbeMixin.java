package com.example.client.mixin;

import com.example.client.latency.InteractionLatencyProbe;
import net.minecraft.client.Minecraft;
import net.minecraft.network.protocol.game.ClientboundBlockUpdatePacket;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * 2026-11-13：交互延迟探针 —— 方块确认包。纯读时钟，不改任何行为。
 *
 * <p><b>本方法会被调用两次</b>：Netty 线程那次在
 * {@code PacketUtils.ensureRunningOnSameThread} 处即返回（只是入队），主线程
 * 由 {@code PacketProcessor.processQueuedPackets} 重放时才真正
 * {@code setServerVerifiedBlockState}。所以必须区分：
 * {@code ack(queued)} 是网络到达，{@code ack(applied)} 才是方块真的改了。</p>
 */
@Mixin(net.minecraft.client.multiplayer.ClientPacketListener.class)
public class ClientPacketListenerProbeMixin {

    @Inject(method = "handleBlockUpdate", at = @At("HEAD"))
    private void probe$blockUpdate(ClientboundBlockUpdatePacket packet, CallbackInfo ci) {
        if (packet == null) {
            return;
        }
        boolean onMain;
        try {
            onMain = Minecraft.getInstance().isSameThread();
        } catch (Throwable t) {
            onMain = false;
        }
        InteractionLatencyProbe.onAck(packet.getPos(), onMain);
    }
}
