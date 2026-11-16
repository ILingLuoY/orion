

// =====================================================
// One-bounce water-bed diffuse GI
//
// Deterministic interface transport: all non-TIR interfaces refract and
// apply (1-F), while TIR reflects.  This removes the old roulette flicker
// without adding paths or changing water-surface normals.
// =====================================================
vec3 tracePrimaryWaterReceiverIndirectReworked(
    vec3 receiverPos,
    vec3 receiverNormal,
    vec3 receiverAlbedo,
    float lodDistance,
    vec3 sunDirection
#if GI_GLASS_RECEIVER_INDIRECT
    , bool startInsideWater
#endif
)
{
    uint lodState =
        uint(gl_LaunchIDEXT.x) * 73856093u ^
        uint(gl_LaunchIDEXT.y) * 19349663u ^
        cam.frameCount * 83492791u ^
        0x57474931u;
    uint lodRandom = pcg_hash(lodState);

    bool runWaterGI = true;
    float lodWeight = 1.0;
    // 2026-10-06 近水降本：32~64 格也掷骰子（×2 权重）。0~32 格保留全预算
    //（玩家脚下的水底细节最显眼，避免可见噪声）；32~64 格的抽样噪声由时间滤波
    // 平滑。回退：删掉这个分支即回到 <64 格全预算。
    if(lodDistance >= 32.0 * perfL10TierScale() && lodDistance < 64.0 * perfL10TierScale())
    {
        runWaterGI = (lodRandom & 1u) == 0u;
        lodWeight = 2.0;
    }
    else if(lodDistance >= 64.0 * perfL10TierScale() && lodDistance < 128.0 * perfL10TierScale())
    {
        runWaterGI = (lodRandom & 1u) == 0u;
        lodWeight = 2.0;
    }
    else if(lodDistance >= 128.0 * perfL10TierScale() && lodDistance < 160.0 * perfL10TierScale())
    {
        runWaterGI = (lodRandom & 3u) == 0u;
        lodWeight = 4.0;
    }
    else if(lodDistance >= 160.0 * perfL10TierScale())
    {
        runWaterGI = false;
    }
    if(!runWaterGI) return vec3(0.0);

    uint pathState = lodRandom ^ 0x57474942u;
    vec3 currentDirection =
        sampleHemisphereCosine(normalize(receiverNormal), pathState);
    vec3 currentOrigin = receiverPos + currentDirection * 0.001;
    vec3 throughput = receiverAlbedo * BSDF_GI_THROUGHPUT_SCALE;
#if GI_GLASS_RECEIVER_INDIRECT
    bool insideWater = startInsideWater;
#else
    bool insideWater = true;
#endif
    int waterInterfaceSteps = 0;

    const vec3 WATER_ABSORPTION = waterAbsorptionColor();
    const vec3 WATER_SCATTER_COLOR = waterScatterColor();

const float WATER_EXTINCTION_DISTANCE =
    16.0;
    const float WATER_IOR = 1.33;
    const float WATER_F0 = 0.0200593;
    // 2026-10-15 降本：水下 GI 界面步数 6→3（水路径成本账里"水下 GI walker
    // ≤7 条遍历"的那一刀）。相机在水上时这条 walker 只服务透过水面的接收器，
    // 影响小；潜水看波光时 GI 会略暗/略带状。回退 = 改回 6。
    const int MAX_WATER_INTERFACE_STEPS = 3;

    for(int pathStep = 0; pathStep <= MAX_WATER_INTERFACE_STEPS; ++pathStep)
    {
        hit.hitDistance = 1.0;
        hit.state = makeTraceState(
            insideWater ? RT_PURPOSE_WATER_MEDIUM : RT_PURPOSE_FULL
        );
#ifdef RT_ENABLE_SER
        // Perf-L10.8: same ray, same flags; SER only reschedules the hit shader.
        if(cam.perfSerAb.z != 0u)
        {
            serTraceRay(
                currentOrigin,
                0.001,
                currentDirection,
                10000.0,
                secondaryTraverseFlags(),
                0xff,
                pathStep
            );
        }
        else
#endif
        {
            traceRayEXT(
                topLevelAS, secondaryTraverseFlags(), 0xff, 0, 0, 0,
                currentOrigin, 0.001, currentDirection, 10000.0, 0
            );
        }

        if(payload.hitDistance < 0.0)
        {
            return throughput * unpackAlbedoUnorm(payload.albedoPacked) * 1.8 * lodWeight;
        }

        float hitDistance = payload.hitDistance;
        vec3 hitPos = currentOrigin + currentDirection * hitDistance;
        float hitMaterialId = getLegacyMaterialId(payload.state);
        if(insideWater)
if(insideWater)
{
    vec3 extinction =
        exp(
            -WATER_ABSORPTION *
            hitDistance
        );

float scatterAmount = 0.0;

    throughput *= extinction;

    throughput =
        mix(
            throughput,
            WATER_SCATTER_COLOR,
            scatterAmount
        );
}

        if(hitMaterialId <= 0.5)
        {
            vec3 hitNormal = payload.normal;
            vec3 hitAlbedo = unpackAlbedoUnorm(payload.albedoPacked);
            vec3 incomingRadiance = evaluateOpaqueSurfaceRadiance(
                hitPos,
                hitNormal,
                -currentDirection,
                hitAlbedo,
                max(payload.roughness, 0.05),
                payload.f0Metal,
                payload.emission +
                    evaluateVLIEmitterRadianceAtHit(hitPos, hitNormal, hitAlbedo),
                payload.pomSelfShadow,
                sunDirection
            );
            uint localState = pathState ^ 0x5747494Cu;
            pcg_hash(localState);
            // 2026-10-07：这里把本跳命中的 LabPBR 材质解出来传下去（移动光源的镜面项
            // 要用）。粗糙度与上面 evaluateOpaqueSurfaceRadiance 用的是同一个 clamp。
            int bounceF0MetalCode =
                decodeF0MetalCode(payload.f0Metal);
            incomingRadiance +=
                evaluateVPTRISDirectLighting(
                    hitPos, hitNormal, hitAlbedo, vec3(0.0),
                    decodeBaseF0(bounceF0MetalCode, hitAlbedo),
                    decodeMetallicFromCode(bounceF0MetalCode),
                    max(payload.roughness, 0.05),
                    localState);
            return throughput * incomingRadiance * 1.8 * lodWeight;
        }

        if(hitMaterialId >= 2.5)
        {
            // Glass is a colour filter, never a water-medium transition.
            if(hitMaterialId < 3.5)
            {
                throughput *= glassSheetTransmission(
                    unpackAlbedoUnorm(payload.albedoPacked),
                    payload.normal,
                    currentDirection
                );
            }
            currentOrigin = hitPos + currentDirection * 0.001;
            continue;
        }

        if(++waterInterfaceSteps > MAX_WATER_INTERFACE_STEPS)
        {
            return vec3(0.0);
        }

        bool exitingWater = hitMaterialId > 1.5;
        vec3 interfaceNormal =
            stabilizeWaterInterfaceNormal(payload.normal);
        float cosTheta = clamp(
            -dot(currentDirection, interfaceNormal), 0.0, 1.0
        );
        float fresnelX = 1.0 - cosTheta;
        float fresnelX2 = fresnelX * fresnelX;
        float fresnel = WATER_F0 +
            (1.0 - WATER_F0) * (fresnelX2 * fresnelX2) * fresnelX;
        vec3 refractedDirection = refract(
            currentDirection,
            interfaceNormal,
            exitingWater ? WATER_IOR : (1.0 / WATER_IOR)
        );

        if(dot(refractedDirection, refractedDirection) < 1.0e-6)
        {
            currentOrigin = hitPos + interfaceNormal * 0.001;
            currentDirection = reflect(currentDirection, interfaceNormal);
            continue;
        }

        throughput *= 1.0 - fresnel;
        insideWater = !exitingWater;
        currentOrigin = hitPos - interfaceNormal * 0.001;
        currentDirection = refractedDirection;
    }

    return vec3(0.0);
}

// =====================================================
// 2026-10-06 近水掉帧降本：反射/折射 walker 的步数距离分档。
//
// walker 的每一步都要打一条硬件射线并对命中面做完整着色（反射走
// RT_PURPOSE_FULL、水下走 RT_PURPOSE_WATER_MEDIUM）。近水时每水像素有两条
// walker（反射 + 折射）各 ≤8 步 ⇒ 16+ 条着色射线。
//
// 远水的实际观感信息量极低：水面在屏幕上只占细窄一条，折射内容（水底）已被
// Beer-Lambert 压到 16 格后基本不可见，且 RR/NRD 会从邻域重建。因此把步数按
// 距离分档 —— 近水（<48 格）保持 8 步不变，中距 5 步、远距 3 步。
// 截断与"步数自然用尽"同语义（返回已累积辐射），不是硬切黑。
//
// 回退：把三个常量都改回 8（或把整个函数体 return 8）即逐位回旧档。
// =====================================================
const float WATER_WALKER_FULL_DISTANCE  = 48.0;
const float WATER_WALKER_MID_DISTANCE   = 96.0;
// 2026-10-18 用户定档：水面反射/折射步数 8/5/3 → 3/2/1（降本；step0 是主射线
// 本身，故仍保留一次反射/折射，只砍嵌套续接）。回退：改回 8/5/3。
//
// =====================================================
// 2026-11-16：3/2/1 → 6/4/2（修「看到第二个透射面时不透明、发白光」）。
//
// 3/2/1 定档时 walker 还带着「缺一个 }」的死代码 bug（见 938 注释）：无论
// 步数给多少，实际每条射线只打 1 次 trace，3/2/1 从未被真正执行过。修复后
// 预算生效，「水 → 第二透射面（玻璃 2 面 / 另一片水）→ 接收面」这类 3~4 跳
// 链路在中/远档必然耗尽，耗尽回退读作不透明的固定白光（用户报告）。
// 6/4/2 给 4 跳链路留足余量；撞天空/不透明即 return 的像素成本不变
//（step0 就返回），只有跨界面链路多付 trace。回退：改回 3/2/1。
// =====================================================
const int   WATER_WALKER_STEPS_FULL     = 6;
const int   WATER_WALKER_STEPS_MID      = 4;
const int   WATER_WALKER_STEPS_FAR      = 2;

// =====================================================
// 2026-10-08（B）：水相关 LOD 的**帧时间反馈环**。
//
// 输入是宿主写进 CameraUBO.rtSampleParams.w 的**平滑后实测 GPU 帧时间**（ms）。
// 水像素是整帧最吃射线的像素（反射/折射两条 walker 各 ≤8 步 + 水底 GI 弹射 +
// 太阳透射 march ≤6 步 + 局部光 RIS 与阴影），而开销与水面的**屏幕占比**成正比
// ⇒ 一靠近水帧时间没有上限。这里按实测帧时间收紧水面那几道**距离闸**，
// 于是"贵"会自己收敛：
//
//   ≤12ms    ：scale = 1.0（不收紧，与改动前的距离档完全一致）
//   12→22ms  ：线性收到 0.35
//   >22ms    ：保持 0.35（下限；0.35 × 96 格焦散闸 ≈ 33.6 格 ≈ 旧的 32 格档）
//
// lane = 0（宿主未启用 / 拿不到 GPU 时间戳）⇒ 恒 1.0 ⇒ **与改动前逐位相同**。
// 回退：ORION_WATER_ADAPTIVE=0 让宿主发送 0。
//
// 用法是把距离**阈值**按 scale 收紧（写成一律"除以 scale"的形式），不改步数表
// 与档位表本身 ⇒ 每一档的语义不变，只是"什么时候进入该档"被提前。
// =====================================================
float perfL10WaterAdaptiveScale()
{
    const float frameMs =
        cam.rtSampleParams.w;

    if(frameMs <= 0.0)
    {
        return 1.0;
    }

    const float tightness = clamp(
        (frameMs - 12.0) / 10.0,
        0.0,
        1.0
    );

    return mix(1.0, 0.35, tightness);
}

int waterTransparentWalkerSteps(float lodDistance)
{
    // 2026-10-08（B）：距离档跟着实测帧时间收紧（口径见
    // perfL10WaterAdaptiveScale；未启用时 scale = 1.0 ⇒ 逐位旧档）。
    const float adaptiveScale =
        perfL10WaterAdaptiveScale();

    const float scaledDistance =
        lodDistance / adaptiveScale;

    return scaledDistance < WATER_WALKER_FULL_DISTANCE
        ? WATER_WALKER_STEPS_FULL
        : (scaledDistance < WATER_WALKER_MID_DISTANCE
            ? WATER_WALKER_STEPS_MID
            : WATER_WALKER_STEPS_FAR);
}

// =====================================================
// Primary water transmission radiance
// =====================================================
vec3 tracePrimaryTransparentRadiance(
    vec3 startOrigin,
    vec3 startDirection,
    bool startInsideWater,
    vec3 sunDirection,
    bool enableWaterReceiverGI,
    float waterGILodDistance,
    // 相机可见的第一层透明面命中点（历史键）。见 TRANSPARENT_HISTORY_KEY_FIRST_SURFACE。
    vec3 historyKeyPosition,
    // 2026-10-22: 水面点太阳完全不可见时，反射天空不得再从遮挡物上方漏回
    // 日盘/日冕（bit 14/15，消费者在 rt_test.rmiss）。
    bool suppressSkySun,
    // 2026-11-16：反射瓣标记（true = 本 walker 在为水面反射 lobe 服务）。
    // 反射瓣撞到**另一片水**时走镜面而不是折射进水 —— 见下方
    // WATER_REFLECTION_LOBE_MIRROR。透射瓣（false）行为不变。
    bool reflectionLobe,
    out WaterTransmissionInfo transmissionInfo
)
{
#if VPT_TRANSPARENT_HISTORY_KEY && TRANSPARENT_HISTORY_KEY_FIRST_SURFACE
    // 只有相机射线打到的那一层，针孔投影才精确落在本像素上。嵌套路径里
    // startOrigin 是水面/第二片玻璃，投影在别的像素上 ⇒ 历史逐帧被拒。
    vec3 transparentHistoryKey = historyKeyPosition;
#else
    vec3 transparentHistoryKey = startOrigin;
#endif

    transmissionInfo.waterPathLength = 0.0;
    transmissionInfo.receiverWorldPos = vec3(0.0);
    transmissionInfo.receiverNormal = vec3(0.0, 1.0, 0.0);
    transmissionInfo.receiverViewDirection = vec3(0.0, 0.0, 1.0);
    transmissionInfo.receiverAlbedo = vec3(0.0);
    transmissionInfo.receiverRoughness = 1.0;
    transmissionInfo.receiverF0Metal = 0.0;
    transmissionInfo.reachedUnderwaterReceiver = 0.0;
    transmissionInfo.waterSunVisibility = 1.0;
    transmissionInfo.receiverSelfEmissionRadiance = vec3(0.0);
    // 2026-11-16：相机水柱透射哨兵（负值 = 本 walker 未提供，消费端回退
    // 旧的单发公式）。真正赋值见 opaque 接收器分支。
    transmissionInfo.cameraWaterColumnTransmittance = vec3(-1.0);

    const float WATER_IOR =
        1.33;


const vec3 WATER_ABSORPTION =
    waterAbsorptionColor();

// 已确认：0~3 格保持高通透。
const float WATER_SHALLOW_DISTANCE =
    3.0;

// 已确认：约 16 格后水底基本不可见。
const float WATER_EXTINCTION_DISTANCE =
    16.0;

// e^-4 ≈ 1.8%。
// 从 3 -> 16 格累计约 4 个 scattering optical depths。
const float WATER_SCATTERING =
    waterScatteringAmount();

const vec3 WATER_SCATTER_COLOR = waterScatterColor();


    const float WATER_F0 =
        0.0200593;


    const float RAY_BIAS =
        0.001;


    vec3 throughput =
        vec3(1.0);
        vec3 accumulatedWaterScattering =
    vec3(0.0);

    // 2026-11-16：相机水柱透射的逐段累乘器（与 throughput 收到的水体乘法
    // 同源：Beer 吸收 × GLASS_TINT 混色 × 深水 0.15 压透）。opaque 接收器
    // 分支把它存进 transmissionInfo.cameraWaterColumnTransmittance 供焦散
    // 复用 —— 焦散端的单发公式在多段水程时与实际累乘不等，复用后以实值为准。
    vec3 waterColumnFactor =
        vec3(1.0);


    vec3 currentOrigin =
        startOrigin;


    vec3 currentDirection =
        normalize(
            startDirection
        );


    // Preserve the incident direction that actually entered water.  It is
    // used only when the refracted continuation incorrectly misses along a
    // shallow water/block boundary.
    vec3 waterEntryDirection =
        currentDirection;


    bool hasWaterEntryDirection =
        startInsideWater;


    // 2026-10-03 水体散射的太阳可见性门控。相机已在水下（startInsideWater）
    // 时不 trace：那条路的水色由 applyUnderwaterWaterColumn 负责，保持 1.0
    // 等于不改它的行为。
    float waterSunVisibility =
        startInsideWater ? 1.0 : 0.0;

    bool waterSunVisibilityTraced =
        false;


    bool insideWater =
        startInsideWater;


    for(
        int step = 0;
        step < waterTransparentWalkerSteps(waterGILodDistance);
        ++step
    )
    {
        hit.hitDistance =
            1.0;


        hit.state = makeTraceState(
            insideWater ? RT_PURPOSE_WATER_MEDIUM : RT_PURPOSE_FULL
        );

#if WATER_SKY_SUN_OCCLUSION
        // 2026-10-22: shadowed water reflection.  Re-applied every step because a
        // glass/water hit rebuilds the state through makeHitState(); any miss in
        // this walker must not re-introduce the sun disk or its ±8° aureole.
        if(suppressSkySun)
        {
            hit.state |=
                RT_STATE_NO_SUN_DISK_BIT |
                RT_STATE_NO_SUN_GLOW_BIT;
        }
#endif

#ifdef RT_ENABLE_SER
        // Perf-L10.8: same ray, same flags; SER only reschedules the hit shader.
        if(cam.perfSerAb.z != 0u)
        {
            serTraceRay(
                currentOrigin,
                0.01,
                currentDirection,
                10000.0,
                secondaryTraverseFlags(),
                0xff,
                step
            );
        }
        else
#endif
        {
            traceRayEXT(
                topLevelAS,
                secondaryTraverseFlags(),
                0xff,
                0,
                0,
                0,
                currentOrigin,
                0.01,
                currentDirection,
                10000.0,
                0
            );
        }


        // =============================================
        // Miss
        //
        // miss shader 的 unpackAlbedoUnorm(payload.albedoPacked) 在这里代表天空结果，
        // 这种情况下它才是 Radiance，可以直接使用。
        // =============================================
if(payload.hitDistance < 0.0)
{
    // A refracted ray can leave the shallow, triangulated water volume at a
    // block edge and reach miss before it reaches the adjacent solid.  Do
    // not turn that transport failure into a strip of sky colour: retry the
    // local segment along the direction that entered water.  A genuine open
    // water view still misses again and therefore keeps the real sky.
    if(insideWater && hasWaterEntryDirection)
    {
        hit.hitDistance =
            1.0;

        hit.state = makeTraceState(RT_PURPOSE_WATER_MEDIUM);

#ifdef RT_ENABLE_SER
        // Perf-L10.8: same ray, same flags; SER only reschedules the hit shader.
        if(cam.perfSerAb.z != 0u)
        {
            serTraceRay(
                currentOrigin,
                0.01,
                waterEntryDirection,
                10000.0,
                secondaryTraverseFlags(),
                0xff,
                step
            );
        }
        else
#endif
        {
            traceRayEXT(
                topLevelAS,
                secondaryTraverseFlags(),
                0xff,
                0,
                0,
                0,
                currentOrigin,
                0.01,
                waterEntryDirection,
                10000.0,
                0
            );
        }

        if(
            payload.hitDistance >= 0.0 &&
            getLegacyMaterialId(payload.state) <= 0.5
        )
        {
            vec3 fallbackHitPos =
                currentOrigin +
                waterEntryDirection *
                payload.hitDistance;

            vec3 fallbackHitNormal =
                payload.normal;

            transmissionInfo.waterPathLength +=
                payload.hitDistance;
            transmissionInfo.receiverWorldPos = fallbackHitPos;
            transmissionInfo.receiverNormal = fallbackHitNormal;
            transmissionInfo.receiverViewDirection = -waterEntryDirection;
            transmissionInfo.receiverAlbedo = unpackAlbedoUnorm(payload.albedoPacked);
            transmissionInfo.receiverRoughness = payload.roughness;
            transmissionInfo.receiverF0Metal = payload.f0Metal;
            transmissionInfo.reachedUnderwaterReceiver = 1.0;
            // 保留入口点 trace 到的太阳可见性，不能重置成 1.0。
            transmissionInfo.waterSunVisibility = waterSunVisibility;
            transmissionInfo.receiverSelfEmissionRadiance = vec3(0.0);

            vec3 fallbackLocalDirect =
         evaluateTransparentReceiverDirectLighting(
                    // 相机可见的第一层透明面命中点 = 本像素的精确历史键
                    // （见 TRANSPARENT_HISTORY_KEY_FIRST_SURFACE）。
                    transparentHistoryKey,
                    vptGeometricFaceNormalFromRay(
                        fallbackHitPos, waterEntryDirection),
                    fallbackHitPos,
                    fallbackHitNormal,
                    unpackAlbedoUnorm(payload.albedoPacked),
                    0x57424F54u
                    );

            vec3 fallbackRadiance =
                evaluateOpaqueSurfaceRadiance(
                    fallbackHitPos,
                    fallbackHitNormal,
                    -waterEntryDirection,
                    unpackAlbedoUnorm(payload.albedoPacked),
                    payload.roughness,
                    payload.f0Metal,
                    payload.emission +
                        evaluateVLIEmitterRadianceAtHit(
                            fallbackHitPos,
                            fallbackHitNormal,
                            unpackAlbedoUnorm(payload.albedoPacked)
                        ),
                    payload.pomSelfShadow,
                    sunDirection
                ) +
                fallbackLocalDirect;

            return
                throughput *
                exp(
                    -WATER_ABSORPTION *
                    payload.hitDistance
                ) *
                fallbackRadiance;
        }
    }

return
    accumulatedWaterScattering +
    throughput *
    unpackAlbedoUnorm(payload.albedoPacked);
}


        // =============================================
        // 必须立刻保存命中数据。
        //
        // 后面的太阳可见性检测会继续覆盖 payload。
        // =============================================

        float hitDistance =
            payload.hitDistance;


        vec3 hitNormal =
            normalize(
                payload.normal
            );


        vec3 hitAlbedo =
            unpackAlbedoUnorm(payload.albedoPacked);


        float hitMaterialId =
            getLegacyMaterialId(payload.state);


        float hitF0Metal =
            payload.f0Metal;
            float hitRoughness =
    payload.roughness;



        vec3 hitEmission =
            payload.emission;


        float hitPOMSelfShadow =
            clamp(
                payload.pomSelfShadow,
                0.0,
                1.0
            );


        vec3 hitPos =
            currentOrigin +
            currentDirection *
            hitDistance;


// =====================================================
// WATER VOLUME TRANSPORT
//
// 0~3 blocks:
//   absorption only, high transparency.
//
// >3 blocks:
//   absorption + scattering extinction.
//
// Scattering is accumulated as radiance separately from
// bottom/surface throughput.
// =====================================================
if(insideWater)
{
    transmissionInfo.waterPathLength +=
        hitDistance;

    // =====================================================================
    // 2026-10-03 参考重构第 2 步（HPWaterBSDFLibary.hlsl 的
    // diffR = G_entry × T_entry × S_volume）：**水上看向水下那段水柱的入散射**。
    //
    // 参考的 S_volume 由 WaterVolumetrics 射线步进得到（指数步进 EXP_FACTOR = 12，
    // 每步 CaculateScatteredLight = scatterColor × phase × exp(-extinction·d) × dt）。
    // 我们这里本来就是**逐格步进**（每个水体 LOD 格一次迭代、hitDistance = 该格行程），
    // 所以按同一数学的闭式累加：
    //     S += scatterColor × phase(cosθ) × skyRadiance × (1 - exp(-ext·d)) × T_entry
    // T_entry = 当前 throughput（入射光到本格前已被外层格衰减的透过率），
    // (1 - exp(-ext·d)) 是本格内的积分（逐步累加的闭式等价）。
    // 相位复用 3820 那套（含 waterParams.w 的 HPWater 物理相位分支）。
    // 亮度对账：与水下解析式 underwaterScatter 取同一常量
    // UNDERWATER_COLUMN_SCATTER_BOOST = 4.0 ⇒ 水上/水下亮度一致。
    // 互斥：相机在水里时由 applyUnderwaterWaterColumn 负责（避免重复计账）⇒
    // 这里只在相机不在水里时累加。
    // =====================================================================
    if(cam.waterSimState.w == 0u)
    {
        vec3 volumeExtinction = waterAbsorptionColor() + waterScatterColor();
        float scatterCos = dot(currentDirection, getTimeDrivenSkyLightDirection());
        vec3 volumePhase;
        if(cam.waterParams.w > 0.5)
        {
            float rayleigh = (3.0 / (4.0 * 3.14159265359)) *
                (1.0 + scatterCos * scatterCos);
            vec3 phase3 = vec3(0.18, 0.42, 1.0) * (rayleigh * 0.05) +
                vec3(underwaterHGPhase(scatterCos, 0.6) * 0.95);
            volumePhase = mix(phase3, vec3(1.0), 0.35);
        }
        else
        {
            volumePhase = vec3(underwaterHGPhase(scatterCos, 0.6));
        }
        vec3 stepInScatter = vec3(1.0) - exp(-volumeExtinction * hitDistance);
        // 2026-10-03 参考重构（HPWater WaterEvaluateBSDF）：**G_entry 入射菲涅尔分工**。
        // 参考在进水体前先乘 T_entry = 1 - F(NdotL)（入射侧），出射侧另有
        // T_exit = 1 - F(NdotV)（我们的 P1/P1b 已用 (1-F) 承担）。
        // 这里补入射侧：阳光穿过（近水平的）水面进入水体时，被菲涅尔镜面反射掉的那
        // 部分能量不该参与入散射 ⇒ 用太阳仰角当入射角（水平水面下的正确入射角）。
        float waterEntryCos = clamp(getTimeDrivenSkyLightDirection().y, 0.0, 1.0);
        float waterEntryFresnel = WATER_F0 +
            (1.0 - WATER_F0) * pow(1.0 - waterEntryCos, 5.0);
        accumulatedWaterScattering +=
            waterScatterColor() * waterScatteringAmount() *
            getTimeDrivenSkyLightColor() * getTimeDrivenSkyLightRadianceScale() *
            volumePhase * stepInScatter * throughput *
            (1.0 - waterEntryFresnel) *
            // 太阳可见性门控（见入口 trace 处注释）：无太阳照到的水体
            // （洞穴/室内）该项归零 —— SSS 不该在黑暗里发光。
            waterSunVisibility *
            UNDERWATER_COLUMN_SCATTER_BOOST;
    }


    const vec3 WATER_GLASS_TINT =
        vec3(
            0.20,
            0.75,
            0.78
        );


    const float WATER_END_DISTANCE =
        16.0;


    float waterFade =
        clamp(
            transmissionInfo.waterPathLength /
            WATER_END_DISTANCE,
            0.0,
            1.0
        );


    // 保持浅水透明，深水逐渐加厚
    waterFade =
        waterFade *
        waterFade;


    vec3 absorption =
        exp(
            -WATER_ABSORPTION *
            hitDistance
        );


    throughput *= absorption;


    // 青色玻璃厚度
    throughput =
        mix(
            throughput,
            throughput * WATER_GLASS_TINT,
            waterFade
        );


    // 深水降低透射
    throughput *=
        mix(
            1.0,
            0.15,
            waterFade
        );

    // 2026-11-16：把与 throughput 同源的水体乘法逐段同步累乘进相机水柱
    // 透射器（Beer 吸收 × GLASS_TINT 混色 × 深水 0.15 压透），opaque 接收器
    // 命中时存进 transmissionInfo 供焦散复用 —— 焦散端旧的单发公式在多段
    // 水程（水+玻璃+水）时与实际累乘不等，复用后以实值为准。
    waterColumnFactor *= absorption;
    waterColumnFactor =
        mix(
            waterColumnFactor,
            waterColumnFactor * WATER_GLASS_TINT,
            waterFade
        );
    waterColumnFactor *=
        mix(
            1.0,
            0.15,
            waterFade
        );
}


// W1: an opaque receiver attenuated below this level contributes less
// than 3% of its air radiance.  Skipping it avoids the direct-sun water
// march, the VPT/RIS local-direct evaluation and the water-receiver GI
// bounce for work the eye cannot resolve.
//
// 2026-10-03 修老 bug「光源+玻璃+水之后就看不见光了」：水体吸收 + 深水压暗
// + 玻璃染色三重衰减把 throughput 压进 3% 时，旧档在这里直接 return 水体
// 散射 —— 后面的灯被整段丢弃（洞穴里水体散射又已归零 ⇒ 纯黑）。
// 发射块携带 ~10× 场景辐亮度（rchit: albedo × emission × 10），3% 残留
// 仍有 ~0.3，暗环境里清晰可见。这里补回 emission 项：非发射接收器不受
// 影响（emission = 0，加 0），太阳直射/VPT/GI 依旧按阈值跳过（性能不变）。
if(max(throughput.r, max(throughput.g, throughput.b)) < 0.03)
{
    return accumulatedWaterScattering + throughput * hitEmission;
}


        // =============================================
// Opaque surface
// =============================================

if(hitMaterialId <= 0.5)
{
    transmissionInfo.receiverWorldPos = hitPos;
    transmissionInfo.receiverNormal = hitNormal;
    transmissionInfo.receiverViewDirection = -currentDirection;
    transmissionInfo.receiverAlbedo = hitAlbedo;
    transmissionInfo.receiverRoughness = hitRoughness;
    transmissionInfo.receiverF0Metal = hitF0Metal;
    transmissionInfo.reachedUnderwaterReceiver = 1.0;
    // 保留入口点 trace 到的太阳可见性，不能重置成 1.0。
    transmissionInfo.waterSunVisibility = waterSunVisibility;
    // 2026-11-16：把逐段累乘的相机水柱透射实值交给焦散消费端（含 Beer
    // 吸收 × GLASS_TINT 混色 × 深水 0.15 压透，不含界面 Fresnel）。
    transmissionInfo.cameraWaterColumnTransmittance = waterColumnFactor;

    vec3 unifiedHitEmission =
        hitEmission +
        evaluateVLIEmitterRadianceAtHit(
            hitPos,
            hitNormal,
            hitAlbedo
        );

    // 2026-10-05 次表面散射只响应水面上方的光源：导出接收器自身的发光
    // 辐亮度（× 同一套水体吞吐），多散射项从源里扣掉 —— 水下光源没有
    // 穿过水面进入水体，不该在水面再叠一层 SSS 光斑。
    transmissionInfo.receiverSelfEmissionRadiance =
        throughput * unifiedHitEmission;


// 2026-10-10（③ 近水降本）：接收器照明只在浅水才付。
// 这一段为一个"水下接收器"额外打出三类射线：太阳穿水 march（≤6）+
// 本地光 RIS 面积采样可见性（2）+ 水下 GI walker（≤6）≈ 最多 14 条真实射线，
// 而它们**还要再乘一次**相机到接收器的 Beer-Lambert 衰减（见下面 15094 的注释，
// 衰减在 return 里由 throughput 施加）。水吸收每格 0.24（红通道）⇒ 24 格水程
// exp(-0.24*24) ≈ 0.003 ⇒ 深水/远接收器这十几条实质白付。
// ⚠ 公共量与出参必须在闸外：transmissionInfo.receiverSelfEmissionRadiance（15076）
//   是出参，调用方要消费，所以上面那段照旧算满，只有下面三个**发射线**的调用进闸。
// 回退：把常量改大（或让它 > 最远水程）。
// =====================================================
// 2026-10-10 **回退**：原值 24.0（只给 24 格水程内的接收器算照明）。
//
// 实测观感有问题 ⇒ 阈值改为 INF（闸门恒真）⇒ **逐位回到改动前**：
// 三个发射线的调用（接收器太阳 march / 本地光 RIS 可见性 / 水下 GI walker）
// 在任何水程下都照旧执行。刻意用"改阈值"而不是"删 if"，是因为后者要动作用域与
// 括号，风险不为零；改一个数可以证明行为等价。
//
// 24.0 这个数是**我按水吸收推算的**（0.24/格，exp(-0.24*24)≈0.003），不是实测
// 出来的 ⇒ 教训：观感类阈值不能用推算值直接上线，必须先 A/B。
// 若日后要重做，请按"深水是否真的看不出来"实测取证后再给值。
// =====================================================
const float WATER_RECEIVER_LIGHTING_MAX_PATH = 1.0e9;

bool waterReceiverLightingNear =
    length(hitPos - startOrigin) <=
    WATER_RECEIVER_LIGHTING_MAX_PATH * perfL10WaterAdaptiveScale();

vec3 surfaceRadiance =
    vec3(0.0);

if(waterReceiverLightingNear)
{
surfaceRadiance =
    evaluateOpaqueSurfaceRadiance(
        hitPos,
        hitNormal,
        -currentDirection,
        hitAlbedo,
        hitRoughness,
        hitF0Metal,
        unifiedHitEmission,
        hitPOMSelfShadow,
        sunDirection
    );


// This receiver was reached through the water transport.  Evaluate its
// active VPT/RIS local direct light before the return below applies the
// accumulated Beer-Lambert attenuation for the camera-to-receiver path.
// Same screen-space history as a pane receiver: this surface was reached by
// retracing through water, so NRD never sees it either.
vec3 waterReceiverLocalDirect =
    evaluateTransparentReceiverDirectLighting(
        // 相机可见的第一层透明面命中点 = 本像素的精确历史键
        // （见 TRANSPARENT_HISTORY_KEY_FIRST_SURFACE）。
        transparentHistoryKey,
        vptGeometricFaceNormalFromRay(hitPos, currentDirection),
        hitPos,
        hitNormal,
        hitAlbedo,
        0x57424F54u
    );

surfaceRadiance +=
    waterReceiverLocalDirect;

if(insideWater && enableWaterReceiverGI)
{
    surfaceRadiance +=
        tracePrimaryWaterReceiverIndirectReworked(
            hitPos,
            hitNormal,
            hitAlbedo,
            waterGILodDistance,
            sunDirection
#if GI_GLASS_RECEIVER_INDIRECT
            // The guard above already knows the medium; passing it keeps the
            // walker's start state honest instead of assuming water.
            , insideWater
#endif
        );
}
}


return
    throughput *
    surfaceRadiance;
}

// 2026-11-16 修「walker 整段死代码」：上面这个 } 原本缺失——
// if(hitMaterialId <= 0.5) 一直开到下面的水界面处理段末尾，导致 1130 的
// 无条件 return 之后整段（玻璃处理 / 水界面折射 / 入口太阳可见性 trace）
// 全部不可达，walker 实际每条射线只打 1 次 trace，撞到第二层水/玻璃界面
// 直接掉进循环后的"耗尽回退"——这就是 2026-11-13「玻璃叠水固定白光」的
// 结构性根因（当时的 WATER_WALKER_EXHAUST_GLOW=0 只是调暗症状）。
// 修复后：玻璃/水界面按既有 3/2/1 步数预算正常走查，耗尽回退回到
// "步数用尽才执行"的正确语义。回退 = 删掉这个 } 并把 1445 附近的
// 函数闭括号数回去（逐位回到修复前）。
// SPIR-V 双验证（2026-11-16）：修复前该函数 OpTraceRayKHR 仅 2 条
//（循环主 trace + miss 重试）、OpExtInst refract 0 条。

// =============================================
// Generic translucent interface
//
// Glass stays straight-through and contributes its filter once.  It is not
// a water interface and must never consume WATER_IOR / insideWater state.
// =============================================
if(hitMaterialId >= 2.5)
{
    if(hitMaterialId < 3.5)
    {
        throughput *=
            glassSheetTransmission(
                hitAlbedo,
                payload.normal,
                currentDirection
            );
    }

    currentOrigin =
        hitPos +
        currentDirection *
        RAY_BIAS;

    continue;
}


// =============================================
// Another water interface

        hitNormal =
            stabilizeWaterInterfaceNormal(
                hitNormal
            );

// =====================================================
// 2026-11-16 修「水面经常有莫名其妙的白色反光」（用户报告）。
//
// 反射瓣的射线撞到**另一片水**（大水面掠射时被扰动法线压下去、重新命中远处
// 水面 —— 海洋上最常见的二次命中路径）时，旧逻辑把它当透射继续折射进水：
// 反射像变成「被反射水体的水下景」，并从那一段起逐段累加 4×
// UNDERWATER_COLUMN_SCATTER_BOOST 的水体 in-scatter（日光下白蓝色，太阳可见
// 时满功率），多段界面后再叠耗尽回退 ⇒ 水面反射里一片与场景无关的白光。
//
// 单瓣近似的正确选择是镜面：按 F 带权沿反射方向继续，**永不进入水体**
//（不积累 in-scatter、不触发水入口太阳可见性 trace）。掠射处 F→1，反射回到
// 正确的「天空镜面」；陡视角 F≈0.02，该瓣本来就被主面 fresnel 混合压到可
// 忽略，观感无损。透射瓣（reflectionLobe=false）逐位不变。
// 回退：-DWATER_REFLECTION_LOBE_MIRROR=0。
// =====================================================
#ifndef WATER_REFLECTION_LOBE_MIRROR
#define WATER_REFLECTION_LOBE_MIRROR 1
#endif
#if WATER_REFLECTION_LOBE_MIRROR
        if(reflectionLobe)
        {
            // 法线必须迎着射线，否则 reflect 会把射线送进水体内部
            //（与折射路径共用 payload.normal 的朝向假设不同，这里显式对齐）。
            vec3 mirrorNormal =
                dot(currentDirection, hitNormal) < 0.0
                    ? hitNormal
                    : -hitNormal;

            float mirrorCos =
                clamp(
                    -dot(
                        currentDirection,
                        mirrorNormal
                    ),
                    0.0,
                    1.0
                );

            float mirrorFresnel =
                WATER_F0 +
                (1.0 - WATER_F0) *
                pow(1.0 - mirrorCos, 5.0);

            throughput *= mirrorFresnel;

            currentOrigin =
                hitPos +
                mirrorNormal *
                RAY_BIAS;

            currentDirection =
                normalize(
                    reflect(
                        currentDirection,
                        mirrorNormal
                    )
                );

            continue;
        }
#endif

        bool exitingWater =
            hitMaterialId > 1.5;

        // =====================================================
        // 2026-11-13：介质状态优先于面分类。
        //
        // insideWater 记录的是"刚才这一段确实在水里走"，它是权威的：
        // 水面几何是体素外壳 ⇒ 在水里能命中的水面只能是**出射面**，
        // 在空气里能命中的只能是**入射面**。而面分类（hitMaterialId）由
        // 三角形绕序判定（material_resolve.glsl 的 dot(geoNormal, rayDirection)），
        // 本仓库已在玻璃那段记录过"绕序对 MC 半透明 quad 不可靠"
        //（material_resolve.glsl 2100-2104）。
        //
        // 一旦某次判反，insideWater 会永久错位：之后整段**空气**都被当成水
        // 累加 waterPathLength 并乘 Beer 吸收 ⇒ 水柱轮廓内一片暗蓝
        //（waterFade = (path/16)² ⇒ 十几格就 mix 到 WATER_GLASS_TINT 并 ×0.15）；
        // 反射射线走同一条 walker（rgen_main_body 的反射调用点）时还会累加
        // accumulatedWaterScattering ⇒ 乳白雾状的"反射白光"。
        //
        // 分类一致时（水平水面、绕序正常）两式逐位相同。
        // A/B：-DWATER_MEDIUM_STATE_AUTHORITY=0 回到只看面分类。
        // =====================================================
#ifndef WATER_MEDIUM_STATE_AUTHORITY
#define WATER_MEDIUM_STATE_AUTHORITY 1
#endif
#if WATER_MEDIUM_STATE_AUTHORITY
        exitingWater = insideWater;
#endif

        if(!exitingWater)
        {
            waterEntryDirection =
                currentDirection;

            hasWaterEntryDirection =
                true;

            // =================================================================
            // 2026-10-03 修「水体散射/SSS 无太阳环境发光」（老 bug
            // 「光源+玻璃+水看不见光」的观感来源之一）：S_volume、波峰透光、
            // 薄层 SSS 的光源是 getTimeDrivenSkyLight* —— 只门控时间与天气，
            // 洞穴/室内水体在正午照样满功率发光。首次进入水体时对水面入口点
            // 做一次太阳可见性 trace（与焦散同一约定：miss = 可见，命中 =
            // 被遮挡；玻璃染色等遮挡体同样算遮挡，与 caustics 口径一致）。
            // =================================================================
            if(!waterSunVisibilityTraced)
            {
                waterSunVisibilityTraced = true;
                if(sunDirection.y > 0.0)
                {
                    hit.hitDistance = 1.0;
                    hit.state =
                        makeTraceState(RT_PURPOSE_VISIBILITY_ONLY);
#if RTAO_VISIBILITY_EARLYOUT
                    // 2026-11-16：水体入口太阳可见性的 inline ray query 可选臂
                    // （ORION_VIS_QUERY bit2，cam.visQueryAb.z）。默认 0 = 走下方既有
                    // traceRayEXT，逐位旧行为。同一 TLAS / cullMask 0xff / tmin 0.02 /
                    // 同方向同射程；旗标只用 TerminateOnFirstHit（不带
                    // SkipClosestHitShader，理由见 AO 臂注释）。未命中写 -1.0，与
                    // rt_test.rmiss 对 purpose 3 的写入值一致（下面按 hitDistance < 0.0
                    // 判穿透 = 水体可见太阳）。
                    if(cam.visQueryAb.z != 0u)
                    {
                        rayQueryEXT receiverSunVisQuery;
                        rayQueryInitializeEXT(
                            receiverSunVisQuery,
                            topLevelAS,
                            gl_RayFlagsTerminateOnFirstHitEXT,
                            0xff,
                            hitPos + sunDirection * 0.001,
                            0.02,
                            sunDirection,
                            10000.0
                        );

                        bool receiverSunVisCommitted = false;
                        int receiverSunVisGuard = 0;
                        // 防死循环上限（参照 volumetric_fog.glsl 的 proceedGuard）。
                        while(rayQueryProceedEXT(receiverSunVisQuery) &&
                              ++receiverSunVisGuard < 32)
                        {
                            if(rayQueryGetIntersectionTypeEXT(receiverSunVisQuery, true) !=
                                gl_RayQueryCommittedIntersectionNoneEXT)
                            {
                                hit.hitDistance =
                                    rayQueryGetIntersectionTEXT(receiverSunVisQuery, true);
                                receiverSunVisCommitted = true;
                                break;
                            }
                        }
                        if(!receiverSunVisCommitted)
                        {
                            hit.hitDistance = -1.0;
                        }
                    }
                    else
#endif
#ifdef RT_ENABLE_SER
                    if(cam.perfSerAb.y != 0u)
                    {
                        serTraceRay(
                            hitPos + sunDirection * 0.001,
                            0.02,
                            sunDirection,
                            10000.0,
                            gl_RayFlagsTerminateOnFirstHitEXT |
                                gl_RayFlagsSkipClosestHitShaderEXT,
                            0xff,
                            0u
                        );
                    }
                    else
#endif
                    {
                        traceRayEXT(
                            topLevelAS,
                            gl_RayFlagsTerminateOnFirstHitEXT |
                                gl_RayFlagsSkipClosestHitShaderEXT,
                            0xff,
                            0,
                            0,
                            0,
                            hitPos + sunDirection * 0.001,
                            0.02,
                            sunDirection,
                            10000.0,
                            0
                        );
                    }
                    waterSunVisibility =
                        hit.hitDistance < 0.0 ? 1.0 : 0.0;
                }
                transmissionInfo.waterSunVisibility =
                    waterSunVisibility;
            }
        }


        float cosTheta =
            clamp(
                -dot(
                    currentDirection,
                    hitNormal
                ),
                0.0,
                1.0
            );


        float fresnel =
            WATER_F0 +
            (
                1.0 -
                WATER_F0
            ) *
            pow(
                1.0 -
                cosTheta,
                5.0
            );


        float eta =
            exitingWater
                ? WATER_IOR
                : (1.0 / WATER_IOR);


        vec3 transmittedDirection =
            refract(
                currentDirection,
                hitNormal,
                eta
            );


        // =============================================
        // Total internal reflection
        // =============================================

        if(
            dot(
                transmittedDirection,
                transmittedDirection
            ) < 1.0e-6
        )
        {
            currentOrigin =
                hitPos +
                hitNormal *
                RAY_BIAS;


            currentDirection =
                normalize(
                    reflect(
                        currentDirection,
                        hitNormal
                    )
                );


            // 没有跨介质。
            continue;
        }


        // 这里没有再做随机 roulette，
        // 所以透射能量必须显式乘 (1-F)。
        throughput *=
            1.0 -
            fresnel;


        insideWater =
            !exitingWater;


        currentOrigin =
            hitPos -
            hitNormal *
            RAY_BIAS;


        currentDirection =
            normalize(
                transmittedDirection
            );
    }


    // 2026-10-15 机制 A：走查预算耗尽从"纯黑"改"软降级"。旧行为（return vec3(0.0)）
    // 把已累积的水体散射与 throughput 一并丢弃 —— 玻璃叠水（1 块玻璃 2 面 + 水面
    // + 水底 = 4 步起步）在走查 3~4 步时会整块变黑。降级 = 按当前介质补一个
    // 单位长 in-scatter 的平坦近似：水里用与逐步积累（上方 waterScatterColor 块）
    // 同一套因子，玻璃栈里给天光环境色。代价 = 0（只对耗尽的那条射线执行）。
    // 回退 = 还原为 return vec3(0.0)。
    //
    // =====================================================
    // 2026-11-13：耗尽时的"补一个单位长 in-scatter"默认关闭。
    //
    // 现象（用户报告）：只要场景里出现"能看进看出的两层半透明面"
    //（水+水 / 玻璃+水 / 水面之后还有一层水界面），水面反射里就出现
    // 位置固定、亮度与背景无关的白光。
    //
    // 机制：近水 walker 现在只有 3 步（2026-10-18 定档 8/5/3 → 3/2/1，见上方
    // WATER_WALKER_STEPS_FULL）。"进水 → 第二层界面 → 接收面"已经用满三步，
    // 再多一层必然耗尽；而耗尽分支返回的是**与场景无关的固定亮度**：
    //   WATER_SCATTER_COLOR × WATER_SCATTERING × 天光 ×
    //   UNDERWATER_COLUMN_SCATTER_BOOST(4.0)
    // 各向同性、不随视角/背景/太阳位置变化 ⇒ 读作"莫名其妙的白光"。
    //
    // 1 = 旧档（保留那道散射）；0 = 只保留真实累积 + throughput 推过去的天空底
    //（口径与下面"非水介质耗尽"那条一致）。
    // 预算耗尽本身是性能定档，不动步数；这里只改"耗尽时给什么"。
    // =====================================================
#ifndef WATER_WALKER_EXHAUST_GLOW
#define WATER_WALKER_EXHAUST_GLOW 0
#endif
    // =====================================================
    // 2026-11-16 修「看到第二个透射面时不透明、发白光」（用户报告）。
    //
    // 耗尽回退的"天空"旧实现是**直射光颜色代理**
    //（getTimeDrivenSkyLightColor × RadianceScale）—— 那是光源色（正午 ≈
    // 纯白、带 radianceScale 增益），不是天空辐亮度。走查预算耗尽的射线把它
    // 满功率返回 ⇒ 一块与背景无关的固定白光（2026-11-13 注释描述的正是这个
    // 症状；当时关掉 WATER_WALKER_EXHAUST_GLOW 只去掉了叠加散射项，代理本身
    // 没换）。现在换成真实天空 LUT 沿 currentDirection 的采样 —— 与 miss
    // 分支返回的天空（rt_test.rmiss 同一份 skyScatteringTex）亮度口径一致；
    // 日盘/星图不在 LUT 里，与 suppressSkySun 的口径也天然一致。步数提到
    // 6/4/2 后耗尽本身已是少数路径，这里只负责"少数路径不再发白"。
    // 回退 = 把下面两处 exhaustSkyRadiance 换回 getTimeDrivenSkyLightColor()
    // × getTimeDrivenSkyLightRadianceScale()（逐位旧档）。
    // =====================================================
    vec3 exhaustSkyRadiance =
        textureLod(
            skyScatteringTex,
            directionToEquirect(currentDirection),
            0.0
        ).rgb;
    if(insideWater)
    {
#if WATER_WALKER_EXHAUST_GLOW
        return
            accumulatedWaterScattering +
            throughput *
            WATER_SCATTER_COLOR *
            WATER_SCATTERING *
            getTimeDrivenSkyLightColor() *
            getTimeDrivenSkyLightRadianceScale() *
            waterSunVisibility *
            UNDERWATER_COLUMN_SCATTER_BOOST;
#else
        return
            accumulatedWaterScattering +
            throughput *
            exhaustSkyRadiance;
#endif
    }

    // =====================================================================
    // 2026-10-22（用户定案 c）：水后天空回退的「洞内压暗」。
    //
    // 这一支是折射/透射射线打空（或步数耗尽）时的返回：throughput × 满辐亮度
    // 天空色。throughput 里已含 Beer 吸收与 WATER_GLASS_TINT(0.20,0.75,0.78)
    // 的按厚度混色 ⇒ **洞里的水只要有一条折射路径能瞄到天空，就按「天空色 ×
    // 青 tint」满功率上色**，与水是否在阴影里无关。这正是「洞里水发蓝」的主因，
    // 也是水面 SSS 门控（P1/P1b）在结构上管不到它的原因 —— 那两项是水面自身的
    // 叶瓣，不是这条背景。
    //
    // 物理上这条背景确实是「看得见的天空」，不该按直射光门控；但玩家读到的是
    // 「背光洞穴里的水在发光」。折中：用入口那次太阳可见性（16386 的
    // waterSunVisibility）把天空回退压到 WATER_SKY_FALLBACK_MIN，**不归零**
    // —— 洞里保留一点天光，满饱和青消失。
    //
    // 闸门条件必须是 `waterSunVisibilityTraced && sunDirection.y > 0.0`，不能只看
    // waterSunVisibility 的值：那个变量在「整条走查没进过水」时是初值 0.0
    // （15808 的 `startInsideWater ? 1.0 : 0.0`），而水面**反射**走查（19280）
    // 就是这种情况 —— 它只撞天空，若按 0.0 判就会把天空反射压到 30%。
    // 太阳在地平线下时同样保持 1.0（夜里本来就没有「直射」可言，压了只会让
    // 夜空倒影变脏）。
    //
    // 只压 throughput×sky 这一半：accumulatedWaterScattering 内部已逐项乘过
    // waterSunVisibility（16133），不能再压第二次。
    // =====================================================================
    const float WATER_SKY_FALLBACK_MIN = 0.30;
    float waterSkyFallbackDim = 1.0;
    if(waterSunVisibilityTraced && sunDirection.y > 0.0)
    {
        waterSkyFallbackDim =
            mix(WATER_SKY_FALLBACK_MIN, 1.0, waterSunVisibility);
    }

    return
        accumulatedWaterScattering +
        throughput *
        exhaustSkyRadiance *
        waterSkyFallbackDim;
}

// =====================================================
// A2-1
// Primary ray-traced glossy reflection radiance
//
// 只负责：
// Primary opaque surface
// -> GGX reflection ray
// -> sky / opaque surface
//
// 如果中途撞到水：
// 允许真实 reflection / refraction 穿过水界面，
// 但水界面本身不作为最终 Radiance。
// =====================================================