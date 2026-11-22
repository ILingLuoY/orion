package com.example.client.latency;

import net.minecraft.core.BlockPos;
import net.minecraft.core.SectionPos;

import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/**
 * 2026-11-13：交互延迟探针（纯读时钟，不改任何行为）。
 *
 * <p>用户症状：开门 / 破坏 / 放置方块慢好几秒才有反应。已实测 use→ack 仅
 * 7~23ms（server 健康）、编译本体 1~2ms（健康），大头在 ack→meshStart
 * （912~4106ms）。这条链路的分相：</p>
 * <ol>
 *   <li><b>ack→dirtyMark</b>：ClientLevel 应用方块更新 → LevelExtractor.setSectionDirty
 *       （SectionUpdateTracker.setDirty）。理论上应为 0（同一线程内紧跟着发生）。</li>
 *   <li><b>ack→extracted</b>：dirty → 下一帧 LevelExtractor 提取循环把它加入
 *       sectionUpdateRenderStates（条件：visibleSections() 包含该 section，且
 *       已有 mesh 或邻居齐全）。这是重点嫌疑段。</li>
 *   <li><b>ack→meshStart</b>：SectionDispatcher.schedule → runTask poll → 编译开跑。</li>
 *   <li><b>ack→meshDone</b>：编译本体完成。</li>
 * </ol>
 *
 * <p>多槽位：按 sectionKey（SectionPos.asLong）记录每个被 ack 的 section，互不覆盖。
 * 每个 section 的时间戳只打印一次（首次到达即打印并置标志）。10s TTL 清理。</p>
 */
public final class InteractionLatencyProbe {

    private static final long ENTRY_TTL_NS = 10_000_000_000L; // 10s 未走完即过期

    private static final class Ack {
        final String action;
        final long ackNanos;
        boolean dirtyLogged;
        boolean extractedLogged;
        boolean meshStartLogged;

        Ack(String action, long ackNanos) {
            this.action = action;
            this.ackNanos = ackNanos;
        }
    }

    /** key = SectionPos.asLong()。 */
    private static final Map<Long, Ack> ACKED = new ConcurrentHashMap<>();

    /** 动作发出时就登记的 section（key=SectionPos.asLong），用于从「动作时刻」起算全链。 */
    private static final class Act {
        final String action;
        final long t0Nanos;
        boolean dirtyLogged;
        boolean meshDoneLogged;

        Act(String action, long t0Nanos) {
            this.action = action;
            this.t0Nanos = t0Nanos;
        }
    }

    private static final Map<Long, Act> ACTIONED = new ConcurrentHashMap<>();

    /** setServerVerifiedBlockState 落地前状态是否已经相同（客户端预测过的标志）。 */
    private static final Map<Long, Boolean> VERIFY_ALREADY_SAME = new ConcurrentHashMap<>();

    /** 动作发出时登记：key = BlockPos.asLong()。 */
    private static final Map<Long, long[]> PENDING =
            new ConcurrentHashMap<>(); // [0]=t0Nanos

    /** 队列观测窗口：最近一次 ack 的墙钟（毫秒）；0=从未 ack，窗口关。 */
    private static volatile long lastAckWallMs = 0L;

    private static final long QUEUE_WINDOW_MS = 8_000L;

    private InteractionLatencyProbe() {
    }

    private static boolean queueWindowOpen() {
        final long last = lastAckWallMs;
        return last != 0L
                && System.currentTimeMillis() - last < QUEUE_WINDOW_MS;
    }

    /** 玩家动作发出（渲染主线程）。 */
    public static void onAction(BlockPos pos, String action) {
        if (pos == null) {
            return;
        }
        final long now = System.nanoTime();
        PENDING.put(pos.asLong(), new long[] {now});
        ACTIONED.put(SectionPos.of(pos).asLong(), new Act(action, now));
    }

    /** 额外登记一个位置（如放置落点）。 */
    public static void watchExtra(BlockPos pos, String action) {
        if (pos == null) {
            return;
        }
        final long now = System.nanoTime();
        PENDING.put(pos.asLong(), new long[] {now});
        ACTIONED.put(SectionPos.of(pos).asLong(), new Act(action, now));
    }

    /**
     * 服务端确认包到达。会被调用两次：Netty 线程（onMain=false，仅入队）与主线程
     * （onMain=true，真正 setServerVerifiedBlockState）。
     */
    public static void onAck(BlockPos pos, boolean onMain) {
        if (pos == null) {
            return;
        }
        final long now = System.nanoTime();
        final long[] t0 = PENDING.get(pos.asLong());
        if (t0 == null) {
            return;
        }
        final long actionToNowMs = (now - t0[0]) / 1_000_000L;
        if (!onMain) {
            System.out.println("[LATENCY] ack(queued)=" + actionToNowMs + "ms"
                    + " pos=" + pos.getX() + "," + pos.getY() + "," + pos.getZ());
            return; // 保留 PENDING，等主线程那次
        }

        PENDING.remove(pos.asLong());
        ACKED.put(SectionPos.of(pos).asLong(), new Ack("ack", t0[0]));
        lastAckWallMs = System.currentTimeMillis();
        System.out.println("[LATENCY] ack(applied)=" + actionToNowMs + "ms"
                + " pos=" + pos.getX() + "," + pos.getY() + "," + pos.getZ());
    }

    /** LevelExtractor.setSectionDirty 入口。 */
    public static void noteDirtyMark(int sectionX, int sectionY, int sectionZ) {
        final long key = SectionPos.of(sectionX, sectionY, sectionZ).asLong();

        // 从「动作发出」起算：能抓到预测路径（在 ack 之前）的标脏。
        final Act act = ACTIONED.get(key);
        if (act != null && !act.dirtyLogged) {
            act.dirtyLogged = true;
            final long actMs = (System.nanoTime() - act.t0Nanos) / 1_000_000L;
            System.out.println("[LATENCY] action->dirtyMark=" + actMs + "ms"
                    + " action=" + act.action
                    + " thread=" + Thread.currentThread().getName());
        }

        final Ack ack = ACKED.get(key);
        if (ack != null && !ack.dirtyLogged) {
            ack.dirtyLogged = true;
            final long ms = (System.nanoTime() - ack.ackNanos) / 1_000_000L;
            final StringBuilder sb = new StringBuilder();
            if (ms > 300L) {
                // 晚到的标脏：抓调用栈，定位是哪条路径触发（sendBlockUpdated 之外的来源）。
                final StackTraceElement[] st = Thread.currentThread().getStackTrace();
                final int limit = Math.min(st.length, 12);
                for (int i = 3; i < limit; i++) {
                    sb.append(" | ").append(st[i].getClassName()).append('.')
                            .append(st[i].getMethodName()).append(':').append(st[i].getLineNumber());
                }
            }
            System.out.println("[LATENCY] ack->dirtyMark=" + ms + "ms" + sb);
        }
    }

    /** 方块更新派发点（sendBlockUpdated / setBlockDirty）观测。 */
    public static void noteBlockUpdated(BlockPos pos, String site) {
        if (pos == null) {
            return;
        }
        final Ack ack = ACKED.get(SectionPos.of(pos).asLong());
        if (ack == null) {
            return;
        }
        final long ms = (System.nanoTime() - ack.ackNanos) / 1_000_000L;
        System.out.println("[LATENCY] " + site + " at +" + ms + "ms"
                + " thread=" + Thread.currentThread().getName());
    }

    /** setServerVerifiedBlockState 落地前：记录当前状态是否已与包内状态相同。 */
    public static void noteVerifiedEnter(BlockPos pos, boolean alreadySame) {
        if (pos != null) {
            VERIFY_ALREADY_SAME.put(pos.asLong(), alreadySame);
        }
    }

    /** setServerVerifiedBlockState 落地时刻。 */
    public static void noteVerifiedState(BlockPos pos) {
        if (pos == null) {
            return;
        }
        final Boolean alreadySame = VERIFY_ALREADY_SAME.remove(pos.asLong());
        final long[] t0 = PENDING.get(pos.asLong());
        if (t0 == null) {
            return;
        }
        final long ms = (System.nanoTime() - t0[0]) / 1_000_000L;
        System.out.println("[LATENCY] serverVerified at +" + ms + "ms"
                + " alreadySame=" + alreadySame
                + " thread=" + Thread.currentThread().getName());
    }

    /** SectionUpdateRenderState 构造（提取循环）。 */
    public static void noteExtracted(long sectionNode) {
        final Ack ack = ACKED.get(sectionNode);
        if (ack != null && !ack.extractedLogged) {
            ack.extractedLogged = true;
            final long ms = (System.nanoTime() - ack.ackNanos) / 1_000_000L;
            System.out.println("[LATENCY] ack->extracted=" + ms + "ms");
        }
    }

    /** SectionCompiler.compile 入口（worker 或渲染线程）。 */
    public static void noteCompileStart(SectionPos sectionPos) {
        if (sectionPos == null) {
            return;
        }
        final Ack ack = ACKED.get(sectionPos.asLong());
        if (ack != null && !ack.meshStartLogged) {
            ack.meshStartLogged = true;
            final long ms = (System.nanoTime() - ack.ackNanos) / 1_000_000L;
            System.out.println("[LATENCY] ack->meshStart=" + ms + "ms");
        }
    }

    /** SectionCompiler.compile RETURN。 */
    public static void noteCompileReturn(SectionPos sectionPos) {
        if (sectionPos == null) {
            return;
        }
        final Act act = ACTIONED.remove(sectionPos.asLong());
        if (act != null) {
            final long actMs = (System.nanoTime() - act.t0Nanos) / 1_000_000L;
            System.out.println("[LATENCY] action->meshDone=" + actMs + "ms"
                    + " action=" + act.action
                    + " thread=" + Thread.currentThread().getName());
        }
        final Ack ack = ACKED.remove(sectionPos.asLong());
        if (ack != null) {
            final long ms = (System.nanoTime() - ack.ackNanos) / 1_000_000L;
            System.out.println("[LATENCY] ack->meshDone=" + ms + "ms");
        }
        // TTL 清理。
        if (!ACKED.isEmpty() || !PENDING.isEmpty()) {
            final long now = System.nanoTime();
            ACKED.values().removeIf(a -> now - a.ackNanos > ENTRY_TTL_NS);
            PENDING.values().removeIf(t -> now - t[0] > ENTRY_TTL_NS);
        }
    }

    /** SectionRenderDispatcher.schedule 入口（任意线程）。 */
    public static void noteSchedule(int queueSize, int freePacks) {
        if (queueWindowOpen()) {
            System.out.println("[LATENCY-Q] schedule queueSize=" + queueSize
                    + " freePacks=" + freePacks
                    + " thread=" + Thread.currentThread().getName());
        }
    }

    /** SectionRenderDispatcher.runTask 入口（worker 线程）。 */
    public static void noteRunTask(int queueSize, int freePacks) {
        if (queueWindowOpen()) {
            System.out.println("[LATENCY-Q] runTask queueSize=" + queueSize
                    + " freePacks=" + freePacks
                    + " thread=" + Thread.currentThread().getName());
        }
    }
}
