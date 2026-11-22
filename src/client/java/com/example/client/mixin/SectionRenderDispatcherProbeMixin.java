package com.example.client.mixin;

import com.example.client.latency.InteractionLatencyProbe;
import net.minecraft.client.renderer.SectionBufferBuilderPool;
import net.minecraft.client.renderer.chunk.SectionRenderDispatcher;
import net.minecraft.client.renderer.chunk.SectionTaskDynamicQueue;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * 2026-11-13：编译队列观测（纯读时钟，不改任何行为）。
 *
 * <p>背景：交互延迟探针实测 ack→meshStart 达 912ms~4106ms（方块确认后编译任务
 * 在 SectionRenderDispatcher 队列里干等），而编译本体只要 1~2ms。两个候选机制：
 * <ol>
 *   <li>pack 耗尽停摆：runTask 里 bufferPool.acquire()==null ⇒ NPE ⇒ 任务重新入队
 *       且 <b>不再自续</b>，若此后无新 schedule()，整条编译流水线停摆；</li>
 *   <li>队列积压：poll 虽按相机距离取最近，但活跃 runTask 循环数可能衰减到 0。</li>
 * </ol>
 * 本探针在「最近一次 ack 后 8 秒」窗口内记录每次 schedule / runTask 的
 * queueSize 与 freePacks，直接判别两种机制。</p>
 */
@Mixin(SectionRenderDispatcher.class)
public class SectionRenderDispatcherProbeMixin {

    @Shadow
    private SectionTaskDynamicQueue queue;

    @Shadow
    private SectionBufferBuilderPool bufferPool;

    @Inject(method = "schedule", at = @At("HEAD"))
    private void probe$schedule(SectionRenderDispatcher.RenderSection.SectionTask task, CallbackInfo ci) {
        try {
            InteractionLatencyProbe.noteSchedule(this.queue.size(), this.bufferPool.getFreeBufferCount());
        } catch (Throwable ignored) {
        }
    }

    @Inject(method = "runTask", at = @At("HEAD"))
    private void probe$runTask(CallbackInfo ci) {
        try {
            InteractionLatencyProbe.noteRunTask(this.queue.size(), this.bufferPool.getFreeBufferCount());
        } catch (Throwable ignored) {
        }
    }
}
