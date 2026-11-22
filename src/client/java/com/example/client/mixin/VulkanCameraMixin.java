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
    private final byte[] waterSimMaskData =
            new byte[WATER_SIM_MASK_EXTENT * WATER_SIM_MASK_EXTENT * 4];
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
    private static final int NEARBY_WATER_PROBE_HALF_EXTENT = 96;
    private static final int NEARBY_WATER_PROBE_STEP = 8;
    private static final int NEARBY_WATER_PROBE_UP_BLOCKS = 8;
    private static final int NEARBY_WATER_PROBE_DOWN_BLOCKS = 32;
    private static final long NEARBY_WATER_PROBE_INTERVAL_NANOS = 500_000_000L;
    private long lastNearbyWaterProbeNanos = 0L;
    private float cachedNearbyWaterSurfaceY = 0.0f;
    private boolean cachedNearbyWaterValid = false;

    private float findNearbyWaterSurfaceY(Vec3 playerPosition) {
        long now = System.nanoTime();
        if (cachedNearbyWaterValid &&
                now - lastNearbyWaterProbeNanos < NEARBY_WATER_PROBE_INTERVAL_NANOS) {
            return cachedNearbyWaterSurfaceY;
        }
        lastNearbyWaterProbeNanos = now;
        cachedNearbyWaterValid = true;
        // 默认哨兵：没找到水 ⇒ 后端 |playerY - surfaceY| = 1000 > 32 ⇒ 门关。
        cachedNearbyWaterSurfaceY = (float) playerPosition.y - 1000.0f;

        if (this.minecraft == null || this.minecraft.level == null) {
            return cachedNearbyWaterSurfaceY;
        }

        BlockPos playerBlock = BlockPos.containing(playerPosition);
        int px = playerBlock.getX();
        int py = playerBlock.getY();
        int pz = playerBlock.getZ();

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
        float playerInWater = this.minecraft.player != null &&
                this.minecraft.player.isInWater() ? 1.0f : 0.0f;
        // 2026-11-16：玩家在水里走原探针（水柱顶面）；不在水里走近水探针
        //（96 格网格找水面，找不到喂哨兵）—— 供后端焦散近水门使用，详见
        // findNearbyWaterSurfaceY 的注释。
        float waterSurfaceY =
                playerInWater > 0.5f
                        ? findPlayerWaterSurfaceY(playerPosition)
                        : findNearbyWaterSurfaceY(playerPosition);
        float cameraInWater = isRenderCameraInWater(pos) ? 1.0f : 0.0f;

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

        tickWaterInteractionEntities(waterSurfaceY);

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
    private void tickWaterInteractionEntities(float waterSurfaceY) {
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

        stampPlayerWaterTrail();
        updateWaterSimMask(waterSurfaceY);
    }

    /**
     * 2026-11-21 徒步玩家尾迹 stamp：与船同款按位移间隔入队；坐船/坐其他
     * 载具时不发（载具自己有 stamp，避免双份）。速度停摆即重置锚点。
     */
    private void stampPlayerWaterTrail() {
        if (this.minecraft == null || this.minecraft.player == null) {
            return;
        }
        final var player = this.minecraft.player;
        if (!player.isInWater() || player.getVehicle() != null) {
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
        final boolean moved = playerBlock.getX() != lastWaterSimMaskPlayerX ||
                playerBlock.getZ() != lastWaterSimMaskPlayerZ;
        final boolean surfaceChanged =
                Float.isNaN(lastWaterSimMaskSurfaceY) ||
                Math.abs(waterSurfaceY - lastWaterSimMaskSurfaceY) > 0.01f;
        final long now = System.nanoTime();
        final boolean due =
                now - lastWaterSimMaskUploadNanos >= 250_000_000L;
        if (!due && !moved && !surfaceChanged) {
            return;
        }
        lastWaterSimMaskUploadNanos = now;
        lastWaterSimMaskPlayerX = playerBlock.getX();
        lastWaterSimMaskPlayerZ = playerBlock.getZ();
        lastWaterSimMaskSurfaceY = waterSurfaceY;

        final int originX = playerBlock.getX() - WATER_SIM_MASK_EXTENT / 2;
        final int originZ = playerBlock.getZ() - WATER_SIM_MASK_EXTENT / 2;
        final int probeY = (int) Math.floor(waterSurfaceY - 0.01);
        for (int dz = 0; dz < WATER_SIM_MASK_EXTENT; ++dz) {
            final int rowBase = dz * WATER_SIM_MASK_EXTENT;
            for (int dx = 0; dx < WATER_SIM_MASK_EXTENT; ++dx) {
                waterSimMaskCursor.set(originX + dx, probeY, originZ + dz);
                final FluidState state = clientLevel.getFluidState(waterSimMaskCursor);
                final int tintRgb = net.minecraft.client.renderer.BiomeColors.getAverageWaterColor(
                        clientLevel,
                        waterSimMaskCursor);
                final int offset = (rowBase + dx) * 4;
                waterSimMaskData[offset] = (byte) ((tintRgb >> 16) & 0xFF);
                waterSimMaskData[offset + 1] = (byte) ((tintRgb >> 8) & 0xFF);
                waterSimMaskData[offset + 2] = (byte) (tintRgb & 0xFF);
                waterSimMaskData[offset + 3] =
                        state.is(FluidTags.WATER) ? (byte) 0xFF : (byte) 0x00;
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
