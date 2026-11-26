
// =====================================================
// 2026-11-13：玻璃光学焦散（GLASS_CAUSTIC_LEVEL）
//
// ⚠ 自创设计——仓库内没有玻璃焦散的参考实现（Caustica 未 vendored，docs 只有
//   水焦散 docs/decisions/water-wave-field.md 一族）。数学约定复用仓内既有
//   口径：透射 = 乘性 transmittance、"无扰动 = 不加光"（水焦散的 density 1
//   契约，rgen_water_caustics.glsl:480-489）；薄片 Fresnel 系列复用
//   glassSheetTransmission（rgen_glass_profile.glsl，含 Schlick 0.04 双程）。
//
// 物理依据（为什么这样做）：
//   1. 覆盖率裁剪 = 平板玻璃唯一真实的光学焦散来源。MC 玻璃是平面，折射波前
//      Jacobian ≡ 1 ⇒ 浓度恒 ≤ 1（能量守恒，不存在透镜式会聚）；可观测的
//      "光透过玻璃形成亮纹图案"来自贴图 alpha 对日盘的逐纹素裁剪：心部
//      alpha=0（原版玻璃透明心）⇒ 日盘直通，边框/条纹 alpha≈1 ⇒ 走薄片滤光
//      （含 Fresnel）。合成式 mix(1, 薄片, alpha) 与 2026-11-02 相机重写档
//      "贴图 alpha 只当覆盖率"同一语义（Caustica GLASS_COVERAGE 条款）。
//      只对清玻璃生效（RT_STATE_CLEAR_GLASS_BIT）：染色玻璃/致密体的 alpha
//      是整片透射率而非二值覆盖率，混入会把染色心部抬成白光。
//   2. 折射位移：march 续段起点沿介质内折射方向平移，出射方向恢复入射
//      方向（平行板）。2026-11-13② 起对**全部玻璃族**生效并用真实板厚
//      （glassSlabThicknessFromFace 同款解析公式，块 1 格 / 板 1/8 格；位移
//      上界 1.0）——染色玻璃的焦散主体就是这条：色斑在影子内部随光学路径
//      偏移。此前清玻璃用的 3cm 薄壁钳制是相机路径的伪影口径，对块玻璃会
//      把位移钳到不可见。附带修正：位移后起点落在出射面之外 ⇒ 竖直面玻璃
//      的"进出两面各 tint 一次"双计随之消失。
//      与 2026-10-11 被删除的 R14（POM 浮雕调制的彩色光斑，rahit 里 #if 0
//      封存）完全不同：那条是浮雕遮蔽，这条是覆盖率透光 + 折射位移。
//
// 历史：2026-10-11 曾有一版"玻璃焦散"（R14 出射面 POM 遮蔽）被用户要求删除，
//   当时用户的口径是"焦散 = POM 光斑"。本次是用户 2026-11-13 明确要求的
//   光学（折射/透光）焦散，二者不冲突。
//
// 0 = 旧档（整片 tint、无位移）。回退：-DGLASS_CAUSTIC_LEVEL=0。
// =====================================================
// 2026-11-13（用户拍板）：玻璃光学焦散**整体回退**，LEVEL 默认 0 ⇒ 太阳 march
// 逐位回到焦散前档（整片 tint、无位移）。下方 LEVEL>=1 的覆盖率/位移代码
// 全部编译掉，保留作 A/B。
#ifndef GLASS_CAUSTIC_LEVEL
#define GLASS_CAUSTIC_LEVEL 0
#endif
#ifndef GLASS_CAUSTIC_LIFT
// 2026-11-13⑧：覆盖率合成的"阴影底"参数。纯覆盖率（lift=1）下原版清玻璃
// 心部 alpha=0 ⇒ 透射=全阳光 ⇒ 玻璃下的染色阴影整体消失——折射位移没有
// 可位移的影子边缘，用户实测"根本没焦散、折射也没了"。lift=0 = 旧档均匀
// 滤光（有阴影无图案）。默认 0.5：心部 = tint + (1-tint)×0.5，亮于边框/
// 条纹（图案可见）且整体仍暗于露天（阴影可见）。
#define GLASS_CAUSTIC_LIFT 0.5
#endif

// 2026-11-15：透射统一随水深 —— 签名去掉 stableTransmission。
// 旧 stable 档把水下段吸收钳到 min(hitDistance, 2.0)，水底直射太阳与真实
// 水深脱钩（浅水深水一样亮）；且旧的步数耗尽语义（一律 return 0）让远档
// （着色点超出"太阳透射距离"设置）的水底 march 在穿出水面后直接判 0，
// 远处水底太阳/焦散整片消失。现在：吸收一律按真实水程 Beer-Lambert，
// "最后一步完成出射即返回累积透射"（见函数尾），透射观感只由水深决定，
// 与性能档位无关。
vec3 traceSunTransmittanceThroughWater(
    vec3 startOrigin,
    vec3 rayDir
) {

    const float WATER_IOR =
        1.33;

    // Waterlight 风格青绿松石水色：红强吸收、蓝轻压、绿最透。
    const vec3 WATER_ABSORPTION =
        waterAbsorptionColor();
        const vec3 WATER_SCATTER_COLOR = waterScatterColor();

const float WATER_EXTINCTION_DISTANCE =
    16.0;

    const float WATER_F0 =
        0.0200593;

    const float RAY_BIAS =
        0.001;

    vec3 transmittance =
        vec3(1.0);

    vec3 currentOrigin =
        startOrigin;

    vec3 currentDir =
        normalize(rayDir);

    // 2026-11-15：标记"最后一步刚好完成 Water->Air 出射（人已在空中）"。
    // 步数耗尽时用它决定返回累积透射还是保守 0 —— 见循环尾注释。
    bool exitedToAir = false;

    for(
        int step = 0;
        step < perfL10MarchSteps(startOrigin);
        ++step
    )
    {
        // 每步重置：只有"本步完成出射"才算数；进入/玻璃步保持保守。
        exitedToAir = false;

        hit.hitDistance =
            1.0;

        hit.state = makeTraceState(RT_PURPOSE_CLASSIFY);

#ifdef RT_ENABLE_SER
        // Perf-L10.8: same ray, same flags; SER only reschedules the hit shader.
        if(cam.perfSerAb.y != 0u)
        {
            serTraceRay(
                currentOrigin,
                0.02,
                currentDir,
                10000.0,
                secondaryTraverseFlags(),
                0xcfu,
                step
            );
        }
        else
#endif
        {
            #ifndef ORION_K2I_COMPOSE_COMPUTE
            #ifdef ORION_RT_STAGE_COUNTERS
            rtCntRayIssued();
            #endif
            traceRayEXT(
                topLevelAS,
                secondaryTraverseFlags(),
                0xcfu,
                0,
                0,
                0,
                currentOrigin,
                0.02,
                currentDir,
                10000.0,
                0
            );
            #else
            payload.hitDistance = -1.0;
            #endif
        }

        // 没有任何几何体阻挡。
        if(
            payload.hitDistance < 0.0
        )
        {
            return transmittance;
        }

        float hitMaterialId =
            getLegacyMaterialId(payload.state);


        // =================================================
        // T1/T9/T3 COMBINED
        //
        // payload:
        //   0 = opaque
        //   1 = Air -> Water
        //   2 = Water -> Air
        //   3 = exact glass / stained glass
        //
        // Glass uses straight transmission: NO refraction.
        // One Orion boundary sheet per block.
        // =================================================
if(hitMaterialId >= 2.5)
{
    // ID 3 = entering / normal glass face.
    // Apply the Orion sheet transmission once per block.
    //
    // ID 4 = exit or bottom face.
    // Continue through it, but do not tint a second time.
    if(hitMaterialId < 3.5)
    {
#if GLASS_CAUSTIC_LEVEL >= 1
        // 2026-11-13 玻璃光学焦散（设计说明见文件顶部 GLASS_CAUSTIC_LEVEL 处）。
        // 清玻璃：逐纹素覆盖率合成 mix(1, 薄片, a) —— 心部直通、边框/条纹
        // 走薄片滤光；染色/致密体保持整片薄片（alpha 是透射率不是覆盖率）。
        // 局部 IOR 表与 glass_dielectric_common.glsl 的 glassIorForProfile 同值
        // （rgen TU 在本 include 点还没有那个头）：2 = 冰 1.309，其余 1.5。
        const bool causticClearGlass =
            (payload.state & RT_STATE_CLEAR_GLASS_BIT) != 0u &&
            cam.glassCausticParams.x > 0.5;
        if(causticClearGlass)
        {
            const float causticSheetAlpha =
                clamp(payload.glassAlpha, 0.0, 1.0);
            const vec3 causticSheetTint =
                glassSheetTransmission(
                    unpackAlbedoUnorm(payload.albedoPacked),
                    payload.normal,
                    currentDir
                );
            // 2026-11-13⑧：见顶部 GLASS_CAUSTIC_LIFT——保留阴影底 + 图案亮化，
            // 替换此前的纯覆盖率 mix(1, tint, a)（心部直通 ⇒ 阴影消失）。
            transmittance *=
                causticSheetTint +
                (vec3(1.0) - causticSheetTint) *
                    ((1.0 - causticSheetAlpha) * GLASS_CAUSTIC_LIFT);
        }
        else
        {
            transmittance *=
                glassSheetTransmission(
                    unpackAlbedoUnorm(payload.albedoPacked),
                    payload.normal,
                    currentDir
                );
        }
#else
        transmittance *=
            glassSheetTransmission(
                unpackAlbedoUnorm(payload.albedoPacked),
                payload.normal,
                currentDir
            );
#endif

        // W2: below 1% the remaining march cannot contribute visible
        // sun energy; stop tracing through stacked glass sheets.
        if(max(transmittance.r, max(transmittance.g, transmittance.b)) < 0.01)
        {
            return vec3(0.0);
        }
    }


    vec3 glassHitPos =
        currentOrigin +
        currentDir *
        payload.hitDistance;

#if GLASS_CAUSTIC_LEVEL >= 1
    // 2026-11-13②：染色玻璃光学焦散 —— 位移从"清玻璃专属 + 3cm 薄壁钳制"
    // 升级为"全部玻璃族 + 真实板厚"。
    //   物理依据：平板玻璃浓度 ≤1（能量守恒），室外透玻璃光 ≤ 直射阳光
    //   ⇒ 焦散的可见形态 = 位移后的透光/染色光斑在**影子内部**偏移
    //   （亮斑一侧出现、另一侧留出暗带）。位移量由真实板厚决定：玻璃块
    //   （1 格）在 45° 太阳下横移 ≈0.3~0.4 格；玻璃板（1/8 格）≈3cm。
    //   此前的 min(0.05/cos, 0.03) 是相机路径的伪影钳制，对太阳 march 的
    //   块玻璃把位移钳到不可见。
    //   板厚解析公式与 glass_dielectric_common.glsl 的 glassSlabThicknessFromFace
    //   同口径（rgen TU 在本 include 点没有那个头，内联）：
    //   2·|dot(fract(hitPos)-0.5, N)|，钳 [0.01, 1.0]（满方块最厚 1 格）。
    //   染色玻璃的图案由 tint（sprite RGB × 桶装染料）携带，无需覆盖率项
    //   ——它的 alpha 是整片透射率（102/255），不是二值覆盖率。
    //   附带修正：位移后起点落在板/块出射面之外 ⇒ 跳过该面 ⇒ 竖直面玻璃
    //   的"进出两面各 tint 一次"双计随之消失（每块/每板恰好一次，与 T1
    //   注释的既定口径一致）。
    // 折射位移**不受 UI 开关控制**（2026-11-13⑥ 用户澄清：开关只管焦散图案，
    // 关掉也要有折射）。位移是玻璃的物理属性，对全部玻璃族常开。
    if(hitMaterialId < 3.5)
    {
        const vec3 causticFaceNormal =
            normalize(payload.normal);
        const int causticProfile =
            int(getTransparentProfile(payload.state));
        const float causticIor =
            causticProfile == 2 ? 1.309 : 1.5;
        const vec3 causticInsideDir =
            refract(
                currentDir,
                causticFaceNormal,
                1.0 / max(causticIor, 1.0e-6));
        if(dot(causticInsideDir, causticInsideDir) > 1.0e-8)
        {
            const vec3 causticInsideDirN =
                normalize(causticInsideDir);
            const float causticInsideCosine =
                clamp(
                    dot(causticInsideDirN, -causticFaceNormal),
                    0.05,
                    1.0);
            const float causticSlabThickness =
                clamp(
                    2.0 * abs(
                        dot(
                            fract(glassHitPos) - vec3(0.5),
                            causticFaceNormal)),
                    0.01,
                    1.0);
            const float causticInsideTravel =
                min(causticSlabThickness / causticInsideCosine, 1.0);
            currentOrigin =
                glassHitPos +
                causticInsideDirN *
                causticInsideTravel +
                currentDir *
                RAY_BIAS;
            continue;
        }
    }
#endif

    currentOrigin =
        glassHitPos +
        currentDir *
        RAY_BIAS;


    continue;
}

        if(
            hitMaterialId <= 0.5
        )
        {
            // Opaque geometry still blocks the sun.
            return vec3(0.0);
        }


        // =================================================
        // Water only from this point onward.
        //
        // 1.0 = Air -> Water
        // 2.0 = Water -> Air
        // =================================================
        // Water -> Air state.
        bool exitingWater =
            hitMaterialId > 1.5;

        // The segment before this interface was travelled inside water.
        // Keep absorption as Beer-Lambert attenuation and add a separate
        // in-scattering term instead of replacing the transmitted light.
        if(exitingWater)
        {
            // 2026-11-15：统一随水深。旧 stable 档钳 min(hitDistance, 2.0)
            // ⇒ 水底直射太阳永远按 ≤2 格吸收，深水不清不浊。现在按真实
            // 水程 Beer-Lambert：红 0.24/格（默认浓度）下 3 格 0.49、
            // 10 格 0.09 —— 浅水清澈、深水自然压暗；<1% 由 W2 提前掐。
            float waterDistance =
                payload.hitDistance;

            vec3 extinction =
                exp(
                    -WATER_ABSORPTION *
                    waterDistance
                );

            transmittance *= extinction;

            float scatterDepth =
                1.0 -
                exp(
                    -waterDistance * 0.05
                );

            vec3 inScatter =
                WATER_SCATTER_COLOR *
                scatterDepth;

            transmittance +=
                inScatter;
        }

        vec3 hitPos =
            currentOrigin +
            currentDir *
            payload.hitDistance;

        // rchit 已经保证 normal 朝向入射射线。
        vec3 hitNormal =
            normalize(
                payload.normal
            );

        hitNormal =
            stabilizeWaterInterfaceNormal(
                hitNormal
            );

        // =================================================
        // FIX: Water -> Air classify stability
        //
        // The animated water-wave normal is desirable when entering water,
        // but on the exit interface it produces triangle-dependent classify
        // refraction and causes the reported flashing triangles.
        //
        // Keep full absorption/scatter on the underwater segment, but use a
        // stable planar exit normal for the Water -> Air interface.
        // =================================================
        // =====================================================
        // 2026-11-20：**恢复九月原档**（无条件水平出射法线）。
        //
        // 原档（见 .tmp/backup_20261026_pre_remix_align/shaders/rt_test.rgen
        // 8889-8911）是：
        //     interfaceNormal = exitingWater
        //         ? vec3(0, hitNormal.y >= 0 ? 1 : -1, 0)
        //         : hitNormal;
        // 即从水出射到空气时**一律**用水平面法线，无任何门限。
        //
        // 2026-11-13 为了不把瀑布/水柱侧壁（法线 y ≈ 0）强制成 (0,±1,0)，
        // 给它加了 `abs(hitNormal.y) > 0.5` 的条件。副作用：被抖陡的浪面
        // （波浪法线的 y 分量掉到 0.5 以下）也一起退回波浪法线，于是从水底
        // 打向太阳、穿出水面到空气的这条 march 又在出射界面按**逐三角形**
        // 的折射/全反射判定 —— 太阳掠射时水底就闪成碎三角形。
        //
        // 这里把默认值改回原档（无条件），11-13 的竖直面保护降级为可选档：
        //   WATER_SUN_VERTICAL_INTERFACE_NORMAL_PROTECT = 0 → 九月原档（默认）
        //   WATER_SUN_VERTICAL_INTERFACE_NORMAL_PROTECT = 1 → 仅 |N.y| > 0.15 的面
        // 若瀑布/水柱侧壁的太阳判定重新出问题（水面反射漏日盘、侧壁高光被压），
        // 就是这一项的责任，把开关置 1。
        // =====================================================
#ifndef WATER_SUN_VERTICAL_INTERFACE_NORMAL_PROTECT
#define WATER_SUN_VERTICAL_INTERFACE_NORMAL_PROTECT 0
#endif
#if WATER_SUN_VERTICAL_INTERFACE_NORMAL_PROTECT
        const bool stabilizeExitNormal =
            exitingWater &&
            abs(hitNormal.y) > 0.15;
#else
        const bool stabilizeExitNormal =
            exitingWater;
#endif
        vec3 interfaceNormal =
            stabilizeExitNormal
                ? vec3(
                    0.0,
                    hitNormal.y >= 0.0 ? 1.0 : -1.0,
                    0.0
                )
                : hitNormal;

        float cosTheta =
            clamp(
                -dot(
                    currentDir,
                    interfaceNormal
                ),
                0.0,
                1.0
            );

        // Fresnel transmission。
        // 反射出去的能量在这里不允许继续作为透射太阳。
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

        transmittance *=
            1.0 -
            fresnel;

        // W2: a deep water crossing can absorb the sun below any
        // visible contribution; stop the march before its remaining
        // interface traces.
        if(max(transmittance.r, max(transmittance.g, transmittance.b)) < 0.01)
        {
            return vec3(0.0);
        }

float eta =
    1.0 / WATER_IOR;

        vec3 transmittedDir =
            refract(
                currentDir,
                interfaceNormal,
                eta
            );

        // Water -> Air 可能发生全反射。
        // 这部分属于反射路径，
        // 不能继续算作太阳透射。
        if(
            dot(
                transmittedDir,
                transmittedDir
            ) < 1.0e-6
        )
        {
            return vec3(0.0);
        }

        // 2026-11-15：本步完成了出射（TIR 已在上面 return），人在空中。
        exitedToAir = exitingWater;

        // interfaceNormal 始终朝向射线来源一侧，
        // 所以透射后统一向 -normal 偏移即可跨过界面。
        currentOrigin =
            hitPos -
            interfaceNormal *
            RAY_BIAS;

        currentDir =
            normalize(
                transmittedDir
            );
    }

    // 2026-11-15：步数耗尽语义。
    // 2026-11-26 (Batch V, Bug 2): When exitedToAir is true after marching out of water,
    // check whether a cave roof or solid block occludes the sun above the exit point.
    if(!exitedToAir)
    {
        return vec3(0.0);
    }
#ifndef ORION_K2I_COMPOSE_COMPUTE
    {
        rayQueryEXT waterSunRoofQuery;
        #ifdef ORION_RT_STAGE_COUNTERS
        rtCntRayIssued();
        #endif
        rayQueryInitializeEXT(
            waterSunRoofQuery,
            topLevelAS,
            gl_RayFlagsTerminateOnFirstHitEXT,
            0xcfu,
            currentOrigin + currentDir * 0.02,
            0.02,
            currentDir,
            256.0
        );
        int waterSunRoofGuard = 0;
        while(rayQueryProceedEXT(waterSunRoofQuery) && ++waterSunRoofGuard < 32)
        {
            if(rayQueryGetIntersectionTypeEXT(waterSunRoofQuery, false) ==
               gl_RayQueryCandidateIntersectionTriangleEXT)
            {
                const uint candSbtOff =
                    rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(
                        waterSunRoofQuery, false);
                if(candSbtOff != 2u && candSbtOff != 3u)
                {
                    rayQueryConfirmIntersectionEXT(waterSunRoofQuery);
                }
            }
        }
        if(rayQueryGetIntersectionTypeEXT(waterSunRoofQuery, true) !=
           gl_RayQueryCommittedIntersectionNoneEXT)
        {
            return vec3(0.0);
        }
    }
#endif
    return transmittance;
}
const float PI =
    3.14159265359;