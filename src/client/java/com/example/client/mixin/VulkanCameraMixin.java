package com.example.client.mixin;

import com.example.vulkan.NativeVulkan;
import com.example.client.RRFovState;
import net.minecraft.client.Camera;
import net.minecraft.client.DeltaTracker;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.GameRenderer;
import net.minecraft.client.renderer.state.GameRenderState;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import net.minecraft.core.BlockPos;
import net.minecraft.tags.FluidTags;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.material.FluidState;
import net.minecraft.world.phys.Vec3;
import com.example.client.EntityRtCapture;
import org.joml.Matrix4f;
import org.joml.Matrix4fc;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(GameRenderer.class)
public class VulkanCameraMixin {

    @Shadow
    private Minecraft minecraft;

    @Shadow
    private GameRenderState gameRenderState;

    @Shadow
    private Camera mainCamera;

    private final float[] viewInvArray = new float[16];
    private final float[] projInvArray = new float[16];

    private final Matrix4f tempViewInv = new Matrix4f();
    private final Matrix4f tempProjInv = new Matrix4f();
    private final Matrix4f tempView = new Matrix4f();
    private final Matrix4f rotInv = new Matrix4f();
    private final float[] handRotationArray = new float[16];

    private int frameCount = 0;

    private static final int WATER_ENTITY_MAX_BOATS = 8;
    private static final double WATER_ENTITY_RANGE_SQR = 24.0 * 24.0;
    private final java.util.Map<Integer, Boolean> waterEntityWasInWater =
            new java.util.HashMap<>();
    private Level waterEntityLastLevel = null;
    private final float[] waterBoatData = new float[WATER_ENTITY_MAX_BOATS * 5];

    // 2026-11-21 尾迹 stamp（船 + 徒步玩家）：沿路径按位移间隔入队小脉冲，
    // stamp 进波场后自行传播、实体停下后自然存留衰减 —— 取代旧"挂在实体
    // 身上、停住即消失"的解析尾迹图案。间隔 0.75 格。
    // 2026-11-21 顾问轮1（F4）：getDeltaMovement() 的单位是 **格/tick**（20Hz），
    // 不是格/秒 —— 旧档阈值 0.35 直接比较导致游泳玩家（~0.28/tick）永远
    // 触发不了、船只在满速（~0.40/tick）勉强触发。统一换算成格/秒（×20）
    // 后再判：最低 0.5 格/s（约等于慢速划水）。
    private static final double WATER_TRAIL_STAMP_INTERVAL_SQR = 0.75 * 0.75;
    private static final double WATER_TRAIL_TICKS_TO_SECONDS = 20.0;
    private static final double WATER_TRAIL_MIN_SPEED_PS = 0.5;
    private final java.util.Map<Integer, double[]> waterTrailLastStamp =
            new java.util.HashMap<>();
    private double waterPlayerStampX = Double.NaN;
    private double waterPlayerStampZ = Double.NaN;

    // 2026-11-21 水体掩码：玩家周围 48×48 块在水面 Y 平面逐块查流体
    // 2026-11-22 Round 2：升级为 48×48 RGBA8（RGB = BiomeColors.getAverageWaterColor，
    // A = 水体掩码 255/0），静止 ≥250ms 或玩家块/水面变动才重扫上传。
    private static final int WATER_SIM_MASK_EXTENT = 48;
    private static final int WATER_SIM_MASK_MOVE_MIN_BLOCKS = 2;
    private static final long WATER_SIM_MASK_MOVE_INTERVAL_NANOS = 100_000_000L;
    private static final int WATER_SIM_MASK_COARSE_STEP = 4;
    private static final int WATER_SIM_MASK_TILE_COUNT =
            WATER_SIM_MASK_EXTENT / WATER_SIM_MASK_COARSE_STEP;
    private static final int WATER_SIM_MASK_COARSE_GRID =
            WATER_SIM_MASK_TILE_COUNT + 1;
    private final byte[] waterSimMaskData =
            new byte[WATER_SIM_MASK_EXTENT * WATER_SIM_MASK_EXTENT * 4];
    private final boolean[] waterSimTileHasWater =
            new boolean[WATER_SIM_MASK_TILE_COUNT * WATER_SIM_MASK_TILE_COUNT];
    private final boolean[] waterSimCornerNeeded =
            new boolean[WATER_SIM_MASK_COARSE_GRID * WATER_SIM_MASK_COARSE_GRID];
    private final int[] waterSimCoarseR =
            new int[WATER_SIM_MASK_COARSE_GRID * WATER_SIM_MASK_COARSE_GRID];
    private final int[] waterSimCoarseG =
            new int[WATER_SIM_MASK_COARSE_GRID * WATER_SIM_MASK_COARSE_GRID];
    private final int[] waterSimCoarseB =
            new int[WATER_SIM_MASK_COARSE_GRID * WATER_SIM_MASK_COARSE_GRID];
    private final int[] waterSimSmoothR =
            new int[WATER_SIM_MASK_COARSE_GRID * WATER_SIM_MASK_COARSE_GRID];
    private final int[] waterSimSmoothG =
            new int[WATER_SIM_MASK_COARSE_GRID * WATER_SIM_MASK_COARSE_GRID];
    private final int[] waterSimSmoothB =
            new int[WATER_SIM_MASK_COARSE_GRID * WATER_SIM_MASK_COARSE_GRID];
    private final BlockPos.MutableBlockPos waterSimMaskCursor =
            new BlockPos.MutableBlockPos();
    private long lastWaterSimMaskUploadNanos = 0L;
    private int lastWaterSimMaskPlayerX = Integer.MIN_VALUE;
    private int lastWaterSimMaskPlayerZ = Integer.MIN_VALUE;
    private float lastWaterSimMaskSurfaceY = Float.NaN;

    private static final boolean RTX_DEBUG =
            Boolean.getBoolean("rtx.debug");

    private Boolean lastLoadFinished = null;
    private Boolean lastAdvanceGameTime = null;
    private Boolean lastLevelPresent = null;
    private Boolean lastReady = null;
    private BlockPos cachedWaterSurfaceProbeBlock = null;
    private float cachedWaterSurfaceY = 0.0f;

    /**
     * Returns the actual top surface of the connected water column containing
     * the real player.  Camera position is intentionally never consulted: in
     * third person the simulation must remain attached to the entity.
     */
    private float findPlayerWaterSurfaceY(Vec3 playerPosition) {
        if (this.minecraft == null || this.minecraft.level == null ||
                this.minecraft.player == null ||
                !this.minecraft.player.isInWater()) {
            cachedWaterSurfaceProbeBlock = null;
            return (float) playerPosition.y;
        }

        BlockPos playerBlock = BlockPos.containing(playerPosition);
        if (playerBlock.equals(cachedWaterSurfaceProbeBlock)) {
            return cachedWaterSurfaceY;
        }

        BlockPos cursor = playerBlock;
        FluidState state = this.minecraft.level.getFluidState(cursor);

        if (!state.is(FluidTags.WATER)) {
            cursor = cursor.below();
            state = this.minecraft.level.getFluidState(cursor);
        }

        if (!state.is(FluidTags.WATER)) {
            cachedWaterSurfaceProbeBlock = playerBlock;
            cachedWaterSurfaceY = (float) playerPosition.y;
            return (float) playerPosition.y;
        }

        BlockPos topWaterBlock = cursor;
        FluidState topWaterState = state;

        for (int step = 0; step < 128; ++step) {
            BlockPos above = topWaterBlock.above();
            FluidState aboveState = this.minecraft.level.getFluidState(above);
            if (!aboveState.is(FluidTags.WATER)) {
                break;
            }
            topWaterBlock = above;
            topWaterState = aboveState;
        }

        cachedWaterSurfaceProbeBlock = playerBlock;
        cachedWaterSurfaceY = topWaterBlock.getY() +
                topWaterState.getHeight(this.minecraft.level, topWaterBlock);
        return cachedWaterSurfaceY;
    }

    /**
     * 2026-11-16 焦散近水门探针：回答"玩家附近有没有水面"。
     *
     * 旧行为：玩家不在水里时 {@link #findPlayerWaterSurfaceY} 返回 playerY ——
     * 无信息量，后端 {@code playerY - waterSurfaceY} 的近水判据两向都错：
     * 站海边（从未下水，surfaceY 冻结在 0）焦散图一张不生成；游过泳再走进
     * 沙漠（surfaceY 冻结在旧水面）焦散图照跑纯浪费。
     *
     * 现在玩家不在水里时：96 格水平半径（= 后端焦散消费距离
     * WATER_CAUSTIC_MAX_DISTANCE）步长 8 的网格，在玩家高度向下 32 格、
     * 向上 8 格的垂直带里找水柱顶面；找到喂真实顶面 Y（海边 ΔY≈0 ⇒ 门开），
     * 找不到喂 playerY - 1000 哨兵（|ΔY| = 1000 > 32 ⇒ 门关）。500ms 缓存。
     * 玩家在水里时不走这里（走 findPlayerWaterSurfaceY，门判据也短路）。
     * 回退 = 把调用处改回 findPlayerWaterSurfaceY(playerPosition)。
     */
    private static final boolean NEAREST_WATER_SURFACE_PROBE = true;
    private static final int NEARBY_WATER_RING_HALF_EXTENT = 24;
    private static final int NEARBY_WATER_RING_STEP = 2;
    private static final int NEARBY_WATER_RING_UP_BLOCKS = 6;
    private static final int NEARBY_WATER_RING_DOWN_BLOCKS = 16;
    private static final int NEARBY_WATER_PROBE_HALF_EXTENT = 96;
    private static final int NEARBY_WATER_PROBE_STEP = 12;
    private static final int NEARBY_WATER_PROBE_UP_BLOCKS = 8;
    private static final int NEARBY_WATER_PROBE_DOWN_BLOCKS = 32;
    private static final long NEARBY_WATER_PROBE_INTERVAL_NANOS = 250_000_000L;
    private static final long NEARBY_WATER_PROBE_DRY_INTERVAL_NANOS = 500_000_000L;
    private long lastNearbyWaterProbeNanos = 0L;
    private float cachedNearbyWaterSurfaceY = 0.0f;
    private boolean cachedNearbyWaterValid = false;
    private boolean cachedNearbyWaterFound = false;
    private final BlockPos.MutableBlockPos nearbyWaterProbeCursor =
            new BlockPos.MutableBlockPos();
    private final BlockPos.MutableBlockPos nearbyWaterClimbCursor =
            new BlockPos.MutableBlockPos();

    private boolean lastProbedWaterIsSource = false;

    private float probeColumnWaterSurfaceY(
            Level level, int x, int z, int yTop, int yBottom, int dyStep, float playerY) {
        for (int y = yTop; y >= yBottom; y -= dyStep) {
            nearbyWaterProbeCursor.set(x, y, z);
            FluidState state = level.getFluidState(nearbyWaterProbeCursor);
            if (!state.is(FluidTags.WATER)) {
                continue;
            }
            int topY = y;
            FluidState topState = state;
            for (int climb = 0; climb < 64; ++climb) {
                nearbyWaterClimbCursor.set(x, topY + 1, z);
                FluidState above = level.getFluidState(nearbyWaterClimbCursor);
                if (!above.is(FluidTags.WATER)) {
                    break;
                }
                ++topY;
                topState = above;
            }
            nearbyWaterClimbCursor.set(x, topY, z);
            float surfaceY = topY + topState.getHeight(level, nearbyWaterClimbCursor);
            if (Math.abs(surfaceY - playerY) <= 32.0f) {
                lastProbedWaterIsSource = topState.isSource();
                return surfaceY;
            }
            // Water column surface is too far vertically from player; stop climbing this column.
            break;
        }
        lastProbedWaterIsSource = false;
        return Float.NaN;
    }

    private double waterCandidateScore(
            int dx, int dz, float surfaceY, float playerY, boolean isSource) {
        double dySurf = surfaceY - playerY;
        double score = dx * (double) dx + dz * (double) dz + dySurf * dySurf;
        if (!isSource) {
            score += 16.0;
        }
        if (!Float.isNaN(lastWaterSimMaskSurfaceY)
                && Math.abs(surfaceY - lastWaterSimMaskSurfaceY) <= 0.25f) {
            score -= 64.0;
        }
        return score;
    }

    private float findNearbyWaterSurfaceY(Vec3 playerPosition) {
        long now = System.nanoTime();
        long cooldown = cachedNearbyWaterFound
                ? NEARBY_WATER_PROBE_INTERVAL_NANOS
                : NEARBY_WATER_PROBE_DRY_INTERVAL_NANOS;
        if (cachedNearbyWaterValid && now - lastNearbyWaterProbeNanos < cooldown) {
            return cachedNearbyWaterSurfaceY;
        }
        lastNearbyWaterProbeNanos = now;
        cachedNearbyWaterValid = true;
        cachedNearbyWaterFound = false;
        // 默认哨兵：没找到水 ⇒ 后端 |playerY - surfaceY| = 1000 > 32 ⇒ 门关。
        cachedNearbyWaterSurfaceY = (float) playerPosition.y - 1000.0f;

        if (this.minecraft == null || this.minecraft.level == null) {
            return cachedNearbyWaterSurfaceY;
        }

        BlockPos playerBlock = BlockPos.containing(playerPosition);
        int px = playerBlock.getX();
        int py = playerBlock.getY();
        int pz = playerBlock.getZ();

        if (NEAREST_WATER_SURFACE_PROBE) {
            final float playerY = (float) playerPosition.y;
            final Level level = this.minecraft.level;

            // 时域快路径：沿用上一帧水面的 9 点十字探针（<= 9 次查询），命中同一水域即秒返。
            if (!Float.isNaN(lastWaterSimMaskSurfaceY)
                    && Math.abs(lastWaterSimMaskSurfaceY - playerY) <= 32.0f) {
                final int yHintFloor = (int) Math.floor(lastWaterSimMaskSurfaceY);
                final int yHintTop = yHintFloor + 1;
                final int yHintBottom = yHintFloor - 1;
                final int[] hintOffsets = { 0, -8, 8 };
                double bestHintScore = Double.MAX_VALUE;
                float bestHintY = Float.NaN;
                for (int oz : hintOffsets) {
                    for (int ox : hintOffsets) {
                        if (ox != 0 && oz != 0) {
                            continue;
                        }
                        float s = probeColumnWaterSurfaceY(
                                level, px + ox, pz + oz, yHintTop, yHintBottom, 1, playerY);
                        if (!Float.isNaN(s) && Math.abs(s - lastWaterSimMaskSurfaceY) <= 0.5f) {
                            double score = waterCandidateScore(
                                    ox, oz, s, playerY, lastProbedWaterIsSource);
                            if (score < bestHintScore) {
                                bestHintScore = score;
                                bestHintY = s;
                            }
                        }
                    }
                }
                if (!Float.isNaN(bestHintY)) {
                    cachedNearbyWaterSurfaceY = bestHintY;
                    cachedNearbyWaterFound = true;
                    return cachedNearbyWaterSurfaceY;
                }
            }

            final int yRingTop = py + NEARBY_WATER_RING_UP_BLOCKS;
            final int yRingBottom = py - NEARBY_WATER_RING_DOWN_BLOCKS;

            for (int r = 0; r <= NEARBY_WATER_RING_HALF_EXTENT; r += (r < 12 ? 2 : 4)) {
                final int step = (r < 12) ? 2 : 4;
                boolean foundInRing = false;
                double bestScore = Double.MAX_VALUE;
                float bestSurfaceY = 0.0f;

                if (r == 0) {
                    float s = probeColumnWaterSurfaceY(level, px, pz, yRingTop, yRingBottom, 2, playerY);
                    if (!Float.isNaN(s)) {
                        cachedNearbyWaterSurfaceY = s;
                        cachedNearbyWaterFound = true;
                        return cachedNearbyWaterSurfaceY;
                    }
                    continue;
                }

                for (int dx = -r; dx <= r; dx += step) {
                    for (int edge = -1; edge <= 1; edge += 2) {
                        int dz = edge * r;
                        float s = probeColumnWaterSurfaceY(
                                level, px + dx, pz + dz, yRingTop, yRingBottom, 2, playerY);
                        if (!Float.isNaN(s)) {
                            double score = waterCandidateScore(
                                    dx, dz, s, playerY, lastProbedWaterIsSource);
                            if (score < bestScore) {
                                bestScore = score;
                                bestSurfaceY = s;
                                foundInRing = true;
                            }
                        }
                    }
                }
                for (int dz = -r + step; dz <= r - step; dz += step) {
                    for (int edge = -1; edge <= 1; edge += 2) {
                        int dx = edge * r;
                        float s = probeColumnWaterSurfaceY(
                                level, px + dx, pz + dz, yRingTop, yRingBottom, 2, playerY);
                        if (!Float.isNaN(s)) {
                            double score = waterCandidateScore(
                                    dx, dz, s, playerY, lastProbedWaterIsSource);
                            if (score < bestScore) {
                                bestScore = score;
                                bestSurfaceY = s;
                                foundInRing = true;
                            }
                        }
                    }
                }

                if (foundInRing) {
                    cachedNearbyWaterSurfaceY = bestSurfaceY;
                    cachedNearbyWaterFound = true;
                    return cachedNearbyWaterSurfaceY;
                }
            }

            final int yFarTop = py + NEARBY_WATER_PROBE_UP_BLOCKS;
            final int yFarBottom = py - NEARBY_WATER_PROBE_DOWN_BLOCKS;
            boolean foundFar = false;
            double bestFarScore = Double.MAX_VALUE;
            float bestFarSurfaceY = 0.0f;
            for (int dz = -NEARBY_WATER_PROBE_HALF_EXTENT;
                    dz <= NEARBY_WATER_PROBE_HALF_EXTENT;
                    dz += NEARBY_WATER_PROBE_STEP) {
                for (int dx = -NEARBY_WATER_PROBE_HALF_EXTENT;
                        dx <= NEARBY_WATER_PROBE_HALF_EXTENT;
                        dx += NEARBY_WATER_PROBE_STEP) {
                    float s = probeColumnWaterSurfaceY(
                            level, px + dx, pz + dz, yFarTop, yFarBottom, 2, playerY);
                    if (!Float.isNaN(s)) {
                        double score = waterCandidateScore(
                                dx, dz, s, playerY, lastProbedWaterIsSource);
                        if (score < bestFarScore) {
                            bestFarScore = score;
                            bestFarSurfaceY = s;
                            foundFar = true;
                        }
                    }
                }
            }
            if (foundFar) {
                cachedNearbyWaterSurfaceY = bestFarSurfaceY;
                cachedNearbyWaterFound = true;
            }
            return cachedNearbyWaterSurfaceY;
        } else {
            for (int dz = -NEARBY_WATER_PROBE_HALF_EXTENT;
                    dz <= NEARBY_WATER_PROBE_HALF_EXTENT;
                    dz += NEARBY_WATER_PROBE_STEP) {
                for (int dx = -NEARBY_WATER_PROBE_HALF_EXTENT;
                        dx <= NEARBY_WATER_PROBE_HALF_EXTENT;
                        dx += NEARBY_WATER_PROBE_STEP) {
                    for (int dy = NEARBY_WATER_PROBE_UP_BLOCKS;
                            dy >= -NEARBY_WATER_PROBE_DOWN_BLOCKS;
                            dy -= 2) {
                        BlockPos probe = new BlockPos(px + dx, py + dy, pz + dz);
                        FluidState state = this.minecraft.level.getFluidState(probe);
                        if (!state.is(FluidTags.WATER)) {
                            continue;
                        }
                        BlockPos topBlock = probe;
                        FluidState topState = state;
                        for (int climb = 0; climb < 64; ++climb) {
                            FluidState above =
                                    this.minecraft.level.getFluidState(topBlock.above());
                            if (!above.is(FluidTags.WATER)) {
                                break;
                            }
                            topBlock = topBlock.above();
                            topState = above;
                        }
                        float surfaceY = topBlock.getY() +
                                topState.getHeight(this.minecraft.level, topBlock);
                        if (Math.abs(surfaceY - (float) playerPosition.y) <= 32.0f) {
                            cachedNearbyWaterSurfaceY = surfaceY;
                            return cachedNearbyWaterSurfaceY;
                        }
                        // 该水柱顶面离玩家太远（悬崖俯瞰等），这一柱不再向上爬
                        break;
                    }
                }
            }
            return cachedNearbyWaterSurfaceY;
        }
    }

    /**
     * The wave field follows the player, while optical medium state follows
     * the actual render camera.  They differ in third-person views.
     */
    private boolean isRenderCameraInWater(Vec3 cameraPosition) {
        if (this.minecraft == null || this.minecraft.level == null) {
            return false;
        }

        BlockPos cameraBlock = BlockPos.containing(cameraPosition);
        FluidState state = this.minecraft.level.getFluidState(cameraBlock);
        if (!state.is(FluidTags.WATER)) {
            return false;
        }

        float fluidSurfaceY = cameraBlock.getY() +
                state.getHeight(this.minecraft.level, cameraBlock);
        return cameraPosition.y < fluidSurfaceY;
    }

    @Inject(method = "extract", at = @At("HEAD"))
    private void onExtractState(
            DeltaTracker deltaTracker,
            boolean advanceGameTime,
            CallbackInfo ci
    ) {
        if (this.minecraft == null) {
            if (RTX_DEBUG) {
                System.out.println(
                        "[WORLD STATE] minecraft=null"
                );
            }
            return;
        }


        boolean loadFinished =
                this.minecraft.isGameLoadFinished();

        boolean levelPresent =
                this.minecraft.level != null;

        boolean ready =
                loadFinished &&
                        advanceGameTime &&
                        levelPresent;


        boolean changed =
                lastLoadFinished == null ||
                        lastAdvanceGameTime == null ||
                        lastLevelPresent == null ||
                        lastReady == null ||
                        lastLoadFinished != loadFinished ||
                        lastAdvanceGameTime != advanceGameTime ||
                        lastLevelPresent != levelPresent ||
                        lastReady != ready;


        if (changed && RTX_DEBUG) {
            System.out.println(
                    "[WORLD STATE]"
                            + " loadFinished=" + loadFinished
                            + " advanceGameTime=" + advanceGameTime
                            + " levelPresent=" + levelPresent
                            + " ready=" + ready
            );
        }


        lastLoadFinished = loadFinished;
        lastAdvanceGameTime = advanceGameTime;
        lastLevelPresent = levelPresent;
        lastReady = ready;


        if (!ready) {
            RRFovState.invalidateCalculatedFov();
        }

        NativeVulkan.setInWorld(ready);
    }

    @Inject(method = "renderLevel", at = @At("HEAD"))
    private void onRenderLevel(
            CallbackInfo ci
    ) {
        if (this.gameRenderState == null ||
                this.gameRenderState.levelRenderState == null ||
                this.gameRenderState.levelRenderState.cameraRenderState == null ||
                this.mainCamera == null) {
            return;
        }

        CameraRenderState cameraState =
                this.gameRenderState.levelRenderState.cameraRenderState;

        final com.example.client.bench.CameraPathDriver.Pose replayPose =
                com.example.client.bench.CameraPathDriver.frame(
                        cameraState.pos,
                        this.mainCamera.xRot(),
                        this.mainCamera.yRot());

        Vec3 pos = replayPose == null
                ? cameraState.pos
                : new Vec3(replayPose.x(), replayPose.y(), replayPose.z());

        if ((frameCount % 60) == 0 && RTX_DEBUG) {
            System.out.println(
                    "[PRIMARY ORIGIN]"
                            + " x=" + pos.x
                            + " y=" + pos.y
                            + " z=" + pos.z
            );
        }

        Matrix4fc viewRotationMatrix =
                replayPose == null
                        ? cameraState.viewRotationMatrix
                        : replayPose.viewRotationMatrix();

        Matrix4fc projectionMatrix =
                cameraState.projectionMatrix;



        float cameraNear =
                Camera.PROJECTION_Z_NEAR;


        float cameraFar =
                cameraState.depthFar;


        float settingFovDegrees =
                (float) this.minecraft.options
                        .fov()
                        .get();

        boolean usingCapturedFov =
                RRFovState.hasValidCalculatedFov();

        float effectiveFovDegrees;

        if (usingCapturedFov) {
            effectiveFovDegrees =
                    RRFovState.getCalculatedFovDegrees();
        } else {
            effectiveFovDegrees =
                    settingFovDegrees;
        }

        float cameraFov =
                (float) Math.toRadians(
                        effectiveFovDegrees
                );

        if ((frameCount % 60) == 0 && RTX_DEBUG) {
            System.out.println(
                    "[RR-3.4 FOV FINAL]"
                            + " source="
                            + (usingCapturedFov
                            ? "calculated"
                            : "setting")
                            + " degrees="
                            + effectiveFovDegrees
                            + " radians="
                            + cameraFov
            );
        }


        rotInv.set(viewRotationMatrix).invert();

        tempViewInv
                .translation(
                        (float) pos.x,
                        (float) pos.y,
                        (float) pos.z
                )
                .mul(rotInv);

        tempViewInv.get(viewInvArray);

        rotInv.get(handRotationArray);
        EntityRtCapture.setHandRotation(handRotationArray);

        EntityRtCapture.setCameraOrigin((float) pos.x, (float) pos.y, (float) pos.z);

        tempProjInv
                .set(projectionMatrix)
                .invert();

        tempProjInv.get(projInvArray);


        if ((frameCount % 60) == 0 && RTX_DEBUG) {

            org.joml.Vector4f target =
                    new org.joml.Vector4f(
                            0.0f,
                            0.0f,
                            1.0f,
                            1.0f
                    );

            tempProjInv.transform(target);

            org.joml.Vector3f rayDir;
            if (Math.abs(target.w) > 1.0e-6f) {
                float invW = 1.0f / target.w;
                rayDir = new org.joml.Vector3f(
                        target.x * invW,
                        target.y * invW,
                        target.z * invW
                );
            } else {
                rayDir = new org.joml.Vector3f(
                        target.x,
                        target.y,
                        target.z
                );
            }

            if (rayDir.lengthSquared() > 1.0e-12f) {
                rayDir.normalize();
            } else {
                rayDir.set(0.0f, 0.0f, -1.0f);
            }

            org.joml.Vector4f worldDir =
                    new org.joml.Vector4f(
                            rayDir.x,
                            rayDir.y,
                            rayDir.z,
                            0.0f
                    );

            tempViewInv.transform(worldDir);

            System.out.println(
                    "[PRIMARY DIR]"
                            + " x=" + worldDir.x
                            + " y=" + worldDir.y
                            + " z=" + worldDir.z
                            + " length="
                            + Math.sqrt(
                            worldDir.x * worldDir.x +
                                    worldDir.y * worldDir.y +
                                    worldDir.z * worldDir.z
                    )
            );
        }

        float overworldTimePhase =
                this.minecraft.level == null
                        ? 0.0f
                        : (float) Math.floorMod(
                        this.minecraft.level.getOverworldClockTime(),
                        24000L
                ) / 24000.0f;

        if (RTX_DEBUG && (frameCount % 64) == 0) {
            System.err.println(
                    "[RTX-TIME] phase=" + overworldTimePhase
            );
        }

        float rainLevel = 0.0f;
        float thunderLevel = 0.0f;
        float dimensionCode = 0.0f;
        if (this.minecraft.level != null) {
            Level level = this.minecraft.level;
            rainLevel = level.getRainLevel(1.0f);
            thunderLevel = level.getThunderLevel(1.0f);

            if (level.dimension().equals(Level.OVERWORLD)) {
                dimensionCode = 1.0f;
            } else if (level.dimension().equals(Level.NETHER)) {
                dimensionCode = 2.0f;
            } else if (level.dimension().equals(Level.END)) {
                dimensionCode = 3.0f;
            }
        }

        Vec3 playerPosition = this.minecraft.player == null
                ? Vec3.ZERO : this.minecraft.player.position();
        Vec3 playerVelocity = this.minecraft.player == null
                ? Vec3.ZERO : this.minecraft.player.getDeltaMovement();
        boolean playerInWaterRaw = this.minecraft.player != null &&
                this.minecraft.player.isInWater();
        // 2026-11-16：玩家在水里走原探针（水柱顶面）；不在水里走近水探针
        //（96 格网格找水面，找不到喂哨兵）—— 供后端焦散近水门使用，详见
        // findNearbyWaterSurfaceY 的注释。
        float waterSurfaceY =
                playerInWaterRaw
                        ? findPlayerWaterSurfaceY(playerPosition)
                        : findNearbyWaterSurfaceY(playerPosition);
        if (playerInWaterRaw) {
            cachedNearbyWaterSurfaceY = waterSurfaceY;
            cachedNearbyWaterValid = true;
            lastNearbyWaterProbeNanos = System.nanoTime();
        }

        // 2026-11-26 (Batch V, Bug 3): Distinguish touching the water surface (1.0f) from fully
        // submerged diving (2.0f) so submerged swimming does not keep triggering surface ripples.
        boolean playerTouchesSurface = false;
        if (playerInWaterRaw && waterSurfaceY > -500.0f && this.minecraft.player != null) {
            double feetY = this.minecraft.player.getY();
            double topY = feetY + this.minecraft.player.getBbHeight();
            playerTouchesSurface =
                    (topY >= (double) waterSurfaceY - 0.12) &&
                    (feetY <= (double) waterSurfaceY + 0.25);
        }
        float playerInWater = !playerInWaterRaw
                ? 0.0f
                : (playerTouchesSurface ? 1.0f : 2.0f);

        // 2026-11-26 (Batch V, Bug 2): Encode sky light above water surface into [1.0, 2.0] when camera is submerged.
        boolean camInWaterBool = isRenderCameraInWater(pos);
        float underwaterSkyFactor = 1.0f;
        if (camInWaterBool && this.minecraft.level != null) {
            double probeY = waterSurfaceY > -500.0f
                    ? Math.ceil((double) waterSurfaceY)
                    : (pos.y + 1.0);
            int skyLight = this.minecraft.level.getBrightness(
                    net.minecraft.world.level.LightLayer.SKY,
                    BlockPos.containing(pos.x, probeY, pos.z));
            underwaterSkyFactor = net.minecraft.util.Mth.clamp(skyLight / 15.0f, 0.0f, 1.0f);
        }
        float cameraInWater = camInWaterBool ? (1.0f + underwaterSkyFactor) : 0.0f;

        // 2026-11-24: update mask before updateCamera so CameraUBO receives matching origin same frame.
        updateWaterSimMask(waterSurfaceY);

        NativeVulkan.updateCamera(
                viewInvArray,
                projInvArray,
                frameCount,
                cameraNear,
                cameraFar,
                cameraFov,
                overworldTimePhase,
                rainLevel,
                thunderLevel,
                dimensionCode,
                (float) playerPosition.x,
                (float) playerPosition.y,
                (float) playerPosition.z,
                (float) playerVelocity.x,
                (float) playerVelocity.y,
                (float) playerVelocity.z,
                playerInWater,
                waterSurfaceY,
                cameraInWater
        );

        // 2026-11-21 群系水色：玩家所在群系的 vanilla 水染色 → UBO 死车道
        // waterSimImpulse (R,G,B,valid)，着色端按海洋参照归一调制吸收/散射。
        // 每帧一次调用（getAverageWaterColor 内部做 4 角群系混合，代价低）。
        if (this.minecraft.level instanceof net.minecraft.client.multiplayer.ClientLevel tintLevel
                && this.minecraft.player != null) {
            // 本 mappings（26.x）里 BiomeColors 在 client.renderer 包，
            // getAverageWaterColor(BlockAndTintGetter, BlockPos)。
            final int tintRgb = net.minecraft.client.renderer.BiomeColors.getAverageWaterColor(
                    tintLevel,
                    BlockPos.containing(playerPosition));
            NativeVulkan.updateWaterBiomeTint(
                    ((tintRgb >> 16) & 0xFF) / 255.0f,
                    ((tintRgb >> 8) & 0xFF) / 255.0f,
                    (tintRgb & 0xFF) / 255.0f,
                    true);
        } else {
            NativeVulkan.updateWaterBiomeTint(0.0f, 0.0f, 0.0f, false);
        }

        // 2026-11-19 rasterLod：把本引擎相机矩阵交给 EntityRtCapture（mode=14
        // 用它构造本引擎 MVP，替代 DH 自己的矩阵；DhFarFieldGeometry 从那读取）。
        // 仅拷贝两个 16 元素数组，无 Vulkan 调用。本方法是唯一权威相机入口
        //（紧接 updateCamera 之后取值，同帧一致），与 setCameraOrigin 同一模式。
        EntityRtCapture.setEngineCameraMatrices(
                viewInvArray,
                projInvArray
        );

        tickWaterInteractionEntities(waterSurfaceY, playerTouchesSurface);

        frameCount++;


    }

    /**
     * 2026-10-17 实体交互波。每帧在 updateCamera 之后扫一遍可见实体：
     * <ul>
     *   <li>掉落物：入水沿（id 状态机）→ {@code queueWaterImpulse} 一次性脉冲；</li>
     *   <li>船（AbstractBoat 全家：船/木筏/运输船）：在水中且距玩家 ≤24 格 →
     *       {@code updateWaterBoats} 持续移动波源，入水/离水沿另发脉冲；
     *       2026-11-21 起再沿路径按 0.75 格间隔 stamp 尾迹脉冲（半径 0.60）。</li>
     *   <li>2026-11-21：徒步玩家（非载具）同样沿路径 stamp 尾迹（半径 0.55）；
     *       水体掩码在本轮末尾按 {@code waterSurfaceY} 平面扫描上传。</li>
     * </ul>
     * 24 格 = 水模拟盒（48×48，玩家居中）的半宽——盒外的源本来就落不进波场。
     */
    private void tickWaterInteractionEntities(float waterSurfaceY, boolean playerTouchesSurface) {
        final Level level = this.minecraft == null ? null : this.minecraft.level;
        if (level == null) {
            return;
        }
        if (level != waterEntityLastLevel) {
            waterEntityLastLevel = level;
            waterEntityWasInWater.clear();
        }

        final net.minecraft.world.phys.Vec3 playerPosition =
                this.minecraft.player == null
                        ? net.minecraft.world.phys.Vec3.ZERO
                        : this.minecraft.player.position();

        final net.minecraft.client.multiplayer.ClientLevel clientLevel =
                (net.minecraft.client.multiplayer.ClientLevel) level;
        int boatCount = 0;
        for (net.minecraft.world.entity.Entity entity : clientLevel.entitiesForRendering()) {
            final boolean isBoat =
                    entity instanceof net.minecraft.world.entity.vehicle.boat.AbstractBoat;
            final boolean isItem =
                    entity instanceof net.minecraft.world.entity.item.ItemEntity;
            if (!isBoat && !isItem) {
                continue;
            }

            final boolean inWater = entity.isInWater();
            final Integer entityId = entity.getId();
            final Boolean previous = waterEntityWasInWater.get(entityId);
            if (previous != null && inWater != previous.booleanValue()) {
                if (inWater) {
                    final double speed = entity.getDeltaMovement().length();
                    final float strength = isBoat
                            ? (float) (0.50 + 0.06 * Math.min(speed, 4.0))
                            : (float) (0.24 + 0.10 * Math.min(speed, 4.0));
                    NativeVulkan.queueWaterImpulse(
                            (float) entity.getX(),
                            (float) entity.getZ(),
                            strength,
                            0.0f
                    );
                } else if (isBoat) {
                    NativeVulkan.queueWaterImpulse(
                            (float) entity.getX(),
                            (float) entity.getZ(),
                            -0.30f,
                            0.0f
                    );
                }
            }
            waterEntityWasInWater.put(entityId, inWater);

            if (isBoat && inWater
                    && boatCount < WATER_ENTITY_MAX_BOATS
                    && entity.distanceToSqr(playerPosition) <= WATER_ENTITY_RANGE_SQR) {
                final int base = boatCount * 5;
                waterBoatData[base] = (float) entity.getX();
                waterBoatData[base + 1] = (float) entity.getZ();
                // 2026-11-22 Round 2 (Item C): velocity in blocks/sec for continuous dipole wake
                waterBoatData[base + 2] = (float) (entity.getDeltaMovement().x * WATER_TRAIL_TICKS_TO_SECONDS);
                waterBoatData[base + 3] = (float) (entity.getDeltaMovement().z * WATER_TRAIL_TICKS_TO_SECONDS);
                waterBoatData[base + 4] = 1.5f;
                boatCount++;
            }
        }

        if (waterEntityWasInWater.size() > 1024) {
            waterEntityWasInWater.clear();
            waterTrailLastStamp.clear();
        }
        NativeVulkan.updateWaterBoats(waterBoatData, boatCount);

        stampPlayerWaterTrail(playerTouchesSurface);
    }

    /**
     * 2026-11-21 徒步玩家尾迹 stamp：与船同款按位移间隔入队；坐船/坐其他
     * 载具时不发（载具自己有 stamp，避免双份）。速度停摆或完全潜入水下即重置锚点。
     */
    private void stampPlayerWaterTrail(boolean playerTouchesSurface) {
        if (this.minecraft == null || this.minecraft.player == null) {
            return;
        }
        final var player = this.minecraft.player;
        if (!playerTouchesSurface || player.getVehicle() != null) {
            waterPlayerStampX = Double.NaN;
            return;
        }
        final net.minecraft.world.phys.Vec3 delta = player.getDeltaMovement();
        final double speedPS = Math.sqrt(delta.x * delta.x + delta.z * delta.z)
                * WATER_TRAIL_TICKS_TO_SECONDS;
        if (speedPS <= WATER_TRAIL_MIN_SPEED_PS) {
            waterPlayerStampX = Double.NaN;
            return;
        }
        if (Double.isNaN(waterPlayerStampX)) {
            waterPlayerStampX = player.getX();
            waterPlayerStampZ = player.getZ();
            return;
        }
        final double ddx = player.getX() - waterPlayerStampX;
        final double ddz = player.getZ() - waterPlayerStampZ;
        if (ddx * ddx + ddz * ddz >= WATER_TRAIL_STAMP_INTERVAL_SQR) {
            NativeVulkan.queueWaterImpulse(
                    (float) player.getX(),
                    (float) player.getZ(),
                    (float) (0.10 + 0.05 * Math.min(speedPS, 6.0)),
                    0.55f
            );
            waterPlayerStampX = player.getX();
            waterPlayerStampZ = player.getZ();
        }
    }

    /**
     * 2026-11-21 水体掩码上传：玩家周围 48×48 块、在 {@code waterSurfaceY}
     * 平面（向下 0.01 格取整确定探针层）逐块查流体 —— 水 = 255、陆地/未加载
     * = 0，行主序（v = z, u = x）。玩家在水里时始终跟随上传；不在水里时仅当
     * 探针结果与上次上传的水面一致（同一片水域，如岸上远眺海面）才跟随 ——
     * 探针找到别的水面时跳过，sim 侧保留上一张掩码（窗口错位时着色端按
     * UV 越界回落到无掩码旧档，不会产生垃圾数据）。
     */
    private void updateWaterSimMask(float waterSurfaceY) {
        if (Float.isNaN(waterSurfaceY) || waterSurfaceY < -500.0f) {
            return;
        }
        if (this.minecraft == null ||
                !(this.minecraft.level instanceof net.minecraft.client.multiplayer.ClientLevel clientLevel) ||
                this.minecraft.player == null) {
            return;
        }
        final var player = this.minecraft.player;
        if (!player.isInWater() && Math.abs(player.getY() - waterSurfaceY) > 32.0) {
            return;
        }

        final BlockPos playerBlock = BlockPos.containing(player.position());
        final boolean firstMask =
                lastWaterSimMaskPlayerX == Integer.MIN_VALUE ||
                lastWaterSimMaskPlayerZ == Integer.MIN_VALUE;
        final boolean moved =
                firstMask ||
                Math.abs((long) playerBlock.getX() - (long) lastWaterSimMaskPlayerX)
                        >= WATER_SIM_MASK_MOVE_MIN_BLOCKS ||
                Math.abs((long) playerBlock.getZ() - (long) lastWaterSimMaskPlayerZ)
                        >= WATER_SIM_MASK_MOVE_MIN_BLOCKS;
        final boolean surfaceChanged =
                Float.isNaN(lastWaterSimMaskSurfaceY) ||
                Math.abs(waterSurfaceY - lastWaterSimMaskSurfaceY) > 0.01f;
        final long now = System.nanoTime();
        final long elapsed = now - lastWaterSimMaskUploadNanos;
        final boolean due = elapsed >= 1_000_000_000L;
        final boolean moveIntervalReady = elapsed >= WATER_SIM_MASK_MOVE_INTERVAL_NANOS;
        if (!surfaceChanged && !due && !(moved && moveIntervalReady)) {
            return;
        }
        lastWaterSimMaskUploadNanos = now;
        lastWaterSimMaskPlayerX = playerBlock.getX();
        lastWaterSimMaskPlayerZ = playerBlock.getZ();
        lastWaterSimMaskSurfaceY = waterSurfaceY;

        final int originX = playerBlock.getX() - WATER_SIM_MASK_EXTENT / 2;
        final int originZ = playerBlock.getZ() - WATER_SIM_MASK_EXTENT / 2;
        final int probeY = (int) Math.floor(waterSurfaceY - 0.01);

        java.util.Arrays.fill(waterSimTileHasWater, false);
        boolean anyWater = false;
        for (int dz = 0; dz < WATER_SIM_MASK_EXTENT; ++dz) {
            final int rowBase = dz * WATER_SIM_MASK_EXTENT;
            final int tzBase = (dz >> 2) * WATER_SIM_MASK_TILE_COUNT;
            for (int dx = 0; dx < WATER_SIM_MASK_EXTENT; ++dx) {
                waterSimMaskCursor.set(originX + dx, probeY, originZ + dz);
                final boolean isWater =
                        clientLevel.getFluidState(waterSimMaskCursor).is(FluidTags.WATER);
                final int offset = (rowBase + dx) * 4;
                waterSimMaskData[offset + 3] = isWater ? (byte) 0xFF : (byte) 0x00;
                if (isWater) {
                    waterSimTileHasWater[tzBase + (dx >> 2)] = true;
                    anyWater = true;
                }
            }
        }

        // 2026-11-25 Batch N: bypass BiomeColors.getAverageWaterColor (which synchronously populates
        // 16 chunks of BlockTintCache per block moved during flight). Direct getBiome quart-grid
        // lookup on coarse 4x4 corners around water tiles + 3x3 box smooth keeps vanilla blending.
        final int fallbackRgb = clientLevel.getBiome(playerBlock).value().getWaterColor();
        final int fbR = (fallbackRgb >> 16) & 0xFF;
        final int fbG = (fallbackRgb >> 8) & 0xFF;
        final int fbB = fallbackRgb & 0xFF;

        if (!anyWater) {
            final byte rB = (byte) fbR;
            final byte gB = (byte) fbG;
            final byte bB = (byte) fbB;
            final int pixelCount = WATER_SIM_MASK_EXTENT * WATER_SIM_MASK_EXTENT;
            for (int p = 0; p < pixelCount; ++p) {
                final int offset = p * 4;
                waterSimMaskData[offset] = rB;
                waterSimMaskData[offset + 1] = gB;
                waterSimMaskData[offset + 2] = bB;
            }
            NativeVulkan.updateWaterSimMask(
                    waterSimMaskData,
                    WATER_SIM_MASK_EXTENT,
                    originX,
                    originZ
            );
            return;
        }

        java.util.Arrays.fill(waterSimCornerNeeded, false);
        for (int tz = 0; tz < WATER_SIM_MASK_TILE_COUNT; ++tz) {
            for (int tx = 0; tx < WATER_SIM_MASK_TILE_COUNT; ++tx) {
                if (!waterSimTileHasWater[tz * WATER_SIM_MASK_TILE_COUNT + tx]) {
                    continue;
                }
                final int minCz = Math.max(0, tz - 1);
                final int maxCz = Math.min(WATER_SIM_MASK_COARSE_GRID - 1, tz + 2);
                final int minCx = Math.max(0, tx - 1);
                final int maxCx = Math.min(WATER_SIM_MASK_COARSE_GRID - 1, tx + 2);
                for (int cz = minCz; cz <= maxCz; ++cz) {
                    final int cRow = cz * WATER_SIM_MASK_COARSE_GRID;
                    for (int cx = minCx; cx <= maxCx; ++cx) {
                        waterSimCornerNeeded[cRow + cx] = true;
                    }
                }
            }
        }

        for (int cz = 0; cz < WATER_SIM_MASK_COARSE_GRID; ++cz) {
            final int cRow = cz * WATER_SIM_MASK_COARSE_GRID;
            final int wz = originZ + cz * WATER_SIM_MASK_COARSE_STEP;
            for (int cx = 0; cx < WATER_SIM_MASK_COARSE_GRID; ++cx) {
                final int idx = cRow + cx;
                if (waterSimCornerNeeded[idx]) {
                    waterSimMaskCursor.set(originX + cx * WATER_SIM_MASK_COARSE_STEP, probeY, wz);
                    final int rgb = clientLevel.getBiome(waterSimMaskCursor).value().getWaterColor();
                    waterSimCoarseR[idx] = (rgb >> 16) & 0xFF;
                    waterSimCoarseG[idx] = (rgb >> 8) & 0xFF;
                    waterSimCoarseB[idx] = rgb & 0xFF;
                } else {
                    waterSimCoarseR[idx] = fbR;
                    waterSimCoarseG[idx] = fbG;
                    waterSimCoarseB[idx] = fbB;
                }
            }
        }

        for (int cz = 0; cz < WATER_SIM_MASK_COARSE_GRID; ++cz) {
            final int cRow = cz * WATER_SIM_MASK_COARSE_GRID;
            for (int cx = 0; cx < WATER_SIM_MASK_COARSE_GRID; ++cx) {
                final int idx = cRow + cx;
                if (!waterSimCornerNeeded[idx]) {
                    waterSimSmoothR[idx] = fbR;
                    waterSimSmoothG[idx] = fbG;
                    waterSimSmoothB[idx] = fbB;
                    continue;
                }
                int sumR = 0;
                int sumG = 0;
                int sumB = 0;
                int count = 0;
                for (int oz = -1; oz <= 1; ++oz) {
                    final int nz = cz + oz;
                    if (nz < 0 || nz >= WATER_SIM_MASK_COARSE_GRID) {
                        continue;
                    }
                    final int nRow = nz * WATER_SIM_MASK_COARSE_GRID;
                    for (int ox = -1; ox <= 1; ++ox) {
                        final int nx = cx + ox;
                        if (nx < 0 || nx >= WATER_SIM_MASK_COARSE_GRID) {
                            continue;
                        }
                        final int nIdx = nRow + nx;
                        sumR += waterSimCoarseR[nIdx];
                        sumG += waterSimCoarseG[nIdx];
                        sumB += waterSimCoarseB[nIdx];
                        ++count;
                    }
                }
                waterSimSmoothR[idx] = (sumR + (count >> 1)) / count;
                waterSimSmoothG[idx] = (sumG + (count >> 1)) / count;
                waterSimSmoothB[idx] = (sumB + (count >> 1)) / count;
            }
        }

        final byte fbRByte = (byte) fbR;
        final byte fbGByte = (byte) fbG;
        final byte fbBByte = (byte) fbB;
        for (int dz = 0; dz < WATER_SIM_MASK_EXTENT; ++dz) {
            final int rowBase = dz * WATER_SIM_MASK_EXTENT;
            final int cz = dz >> 2;
            final int fz = dz & 3;
            final int fzInv = 4 - fz;
            final int cRow0 = cz * WATER_SIM_MASK_COARSE_GRID;
            final int cRow1 = (cz + 1) * WATER_SIM_MASK_COARSE_GRID;
            final int tRow = cz * WATER_SIM_MASK_TILE_COUNT;
            for (int dx = 0; dx < WATER_SIM_MASK_EXTENT; ++dx) {
                final int offset = (rowBase + dx) * 4;
                final int cx = dx >> 2;
                if (!waterSimTileHasWater[tRow + cx]) {
                    waterSimMaskData[offset] = fbRByte;
                    waterSimMaskData[offset + 1] = fbGByte;
                    waterSimMaskData[offset + 2] = fbBByte;
                    continue;
                }
                final int fx = dx & 3;
                final int fxInv = 4 - fx;
                final int w00 = fxInv * fzInv;
                final int w10 = fx * fzInv;
                final int w01 = fxInv * fz;
                final int w11 = fx * fz;
                final int i00 = cRow0 + cx;
                final int i10 = i00 + 1;
                final int i01 = cRow1 + cx;
                final int i11 = i01 + 1;
                waterSimMaskData[offset] = (byte) ((waterSimSmoothR[i00] * w00 + waterSimSmoothR[i10] * w10 + waterSimSmoothR[i01] * w01 + waterSimSmoothR[i11] * w11 + 8) >> 4);
                waterSimMaskData[offset + 1] = (byte) ((waterSimSmoothG[i00] * w00 + waterSimSmoothG[i10] * w10 + waterSimSmoothG[i01] * w01 + waterSimSmoothG[i11] * w11 + 8) >> 4);
                waterSimMaskData[offset + 2] = (byte) ((waterSimSmoothB[i00] * w00 + waterSimSmoothB[i10] * w10 + waterSimSmoothB[i01] * w01 + waterSimSmoothB[i11] * w11 + 8) >> 4);
            }
        }
        NativeVulkan.updateWaterSimMask(
                waterSimMaskData,
                WATER_SIM_MASK_EXTENT,
                originX,
                originZ
        );
    }
}
