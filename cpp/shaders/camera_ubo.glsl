
    mat4 viewInverse;
    mat4 projInverse;
    uvec2 vertexBufferAddr;
    uint frameCount;
    uint padding;

    vec4 cameraParams;

    uint historyReadIndex;
    uint historyValid;
    uint historyPadding0;
    uint historyPadding1;

    uvec4 perfPrimary;
    uvec4 perfSecondary;

    mat4 prevViewInverse;
    mat4 prevProjInverse;

    vec4 historyParams;

    vec4 playerPositionAndWater;
    vec4 playerVelocityAndSpeed;

    vec4 waterSimSurfaceAndOrigin;
    uvec4 waterSimState;
    vec4 waterSimImpulse;

    vec4 weatherParams;

    vec4 cloudParams0;
    vec4 cloudParams1;
    vec4 cloudParams2;
    vec4 cloudParams3;
    vec4 cloudParams4;
    vec4 cloudParams5;
    vec4 cloudParams6;
    vec4 cloudParams7;
    vec4 cloudParams8;
    vec4 cloudParams9;
    vec4 skyParams0;
    vec4 skyParams1;

    // godRayParams1 = 贴地薄雾四车道（2026-11-04 复活，消费方 volumetric_fog.glsl）：
    // x=总开关 y=高度标尺(×6=e 折叠格数) z=消光(×0.05=每格) w=距相机淡出半径(格)
    vec4 godRayParams0;
    vec4 godRayParams1;
    vec4 godRayParams2;

    vec4 rainWallParams0;
    vec4 rainWallParams1;
    vec4 waterStyleParams0;
    vec4 waterStyleParams1;

    uvec4 perfSerAb;

    vec4 dhFarFieldParams;
    mat4 dhFarFieldInvMvp;
    vec4 dhFarFieldLight;
    mat4 dhFarFieldMvp;

    vec4 sunShadowFarParams;

    vec4 highCloudParams0;
    vec4 highCloudParams1;
    vec4 highCloudParams2;
    vec4 highCloudParams3;
    vec4 highCloudParams4;

    vec4 cloudMsParams;
    vec4 cloudMarchParams;
    vec4 waterParams;

    vec4 cloudPhiParams;
    vec4 cloudLightParams;
    vec4 cloudShapeParams;
    vec4 cloudMoonParams;
    vec4 cloudShape2Params;

    vec4 cloudAmbientParams;
    vec4 cloudMsParams2;
    vec4 cloudLightParams2;
    vec4 cloudProfileParams;
    vec4 cloudDetailParams;
    vec4 waterWaveParams;
    vec4 waterWaveShapeParams;
    vec4 waterSssParams;
    vec4 waterFftParams;
    vec4 waterFftParams2;

    vec4 auroraParams;
    vec4 rtSampleParams;
    uvec4 perfAblationStage;
    vec4 galaxyParams;

    vec4 fogExtrasParams;
    vec4 cloudTemporalParams;

    vec4 cloudAerialParams;

    uvec4 sharcBuffersLo;
    uvec4 sharcBuffersHi;

    uvec4 sharcCacheParams;

    vec4 cloudSkyHueParams;

    vec4 cloudPhysicsParams;

    vec4 pomParams;

    vec4 sssParams;

    vec4 ueCloudA;
    vec4 ueCloudB;

    vec4 giHalfParams;

    vec4 giRayParams;

    vec4 hpShapeA;
    vec4 hpShapeB;
    vec4 hpShapeC;
    vec4 hpShapeD;
    vec4 hpLightA;
    vec4 hpLightB;
    vec4 hpMiscA;

    vec4 sparseParams;

    // ── skyModelParams ──（2026-11-01 追加在末尾：std140 允许块提前结束，
    // 但中间插入会让后面所有字段整体平移，所以只能追加）
    //   x = 天空散射模型：0 = Bruneton 预计算 LUT（默认），1 = Hillaire 结构
    //       （逐像素 march + 解析多次散射，见 sky_hillaire_common.glsl）
    //   y = Hillaire 分支的辐亮度标定倍率（1 = 出厂；两模型量纲不完全一致，
    //       留这个旋钮做 A/B 对齐，不必重编）
    //   z/w = 保留（写 0）
    vec4 skyModelParams;

    // ── ssHybridParams ──（2026-11-02 追加在末尾，理由同上：中间插入会平移）
    //   x = SSPT 射程（区块，0 = 关；UI 滑条 0..12）
    //   y = SSR 射程上限（区块，0 = 关；UI 硬性下限 12 ⇒ 只要 >0 就按 12 起算）
    //   z/w = 保留（写 0；内部渲染分辨率走 ss_hybrid 自己的 push constant，
    //         不用 UBO 是因为 UBO 在 updateCamera 里写，重尺寸那一帧会滞后）
    // 消费方：cpp/shaders/ss_hybrid.comp
    vec4 ssHybridParams;

    // ── 派生矩阵 ──（2026-11-09 追加在末尾，理由同上：中间插入会平移）
    // raygen 每个像素原本要跑 4 次 inverse(mat4)：inverse(viewInverse)、
    // inverse(projInverse)、inverse(prevViewInverse*prevProjInverse)、
    // inverse(viewInverse*projInverse)。四个都是帧常量，宿主每帧算好后
    // 写在这里；列主序 [col*4 + row]，与 GLSL mat4 一致。
    mat4 viewMatrix;        // = inverse(viewInverse)
    mat4 projectionMatrix;  // = inverse(projInverse)
    mat4 viewProj;          // = projectionMatrix * viewMatrix
    mat4 prevViewProj;      // = inverse(prevProjInverse) * inverse(prevViewInverse)

    // ── 本地光源档案表 ──（2026-11-09 追加在末尾，理由同上）
    // 原本是 local_light_profile.glsl 里的四张 const 表；glslang 对"动态索引
    // const 数组"会生成 indexable 局部数组（768 B/线程）⇒ 逐像素 local memory。
    // 移到 UBO 后动态索引就是一次 OpAccessChain。
    //   A：rgb = 发射色（已归一到峰值 1.0），a = lumens（emission=15 时的相对亮度）
    //   B：x = 基础传播距离（格），y = pointLike（1 = 点状光源），zw = 保留（0）
    vec4 localLightProfileA[32];
    vec4 localLightProfileB[32];

    // ── pomSecondaryParams ──（2026-11-09 追加在末尾，理由同上）
    //   x = 次级射线（反射/水体/玻璃后）跳过 POM 的开关（1 = 跳过，出厂开）
    //   y/z/w = 保留（写 0）
    // 消费方：rgen_trace_state.glsl 的 secondaryRayPomSkipEnabled()，
    //         rgen_glossy.glsl 播种 payload.state 的 bit17（GI_NO_POM）。
    vec4 pomSecondaryParams;

    // ── fogOpticsParams ──（2026-11-13 追加在末尾，不动既有偏移）
    //   x = 平流雾的 curl 涡流强度（0 = 只整体平移，1 = 默认，2 = 剧烈翻滚）
    //   y/z/w = 雾的波长相关透射权重（红/绿/蓝，1 = 不偏色；蓝>红 ⇒ 远处偏蓝）
    // 消费方：volumetric_fog.glsl（rollingFogDensity 的涡流位移、雾散射的色偏）
    vec4 fogOpticsParams;

    // ── glassCausticParams ──（2026-11-13 追加在末尾，不动既有偏移）
    //   2026-11-13（用户拍板）：玻璃光学焦散**整体回退**，该设置已删除
    //   （UI/Config/JNI/atomic 全清）。字段保留占位以维持 CameraUBO 布局
    //   不变；shader 侧消费宏（GLASS_CAUSTIC_LEVEL /
    //   GLASS_VISIBILITY_COVERAGE_CAUSTIC）已置 0，无任何读者。
    vec4 glassCausticParams;

    // ── perfBandParams ──（2026-11-15 追加在末尾）
    //   x = 「性能模式」的 RT 射程（区块，0 = 非性能模式）
    //   y = 弹射分界（区块）：≤ y 用用户设定的弹射数，> y 只 1 弹（普通模式同样生效）
    //   z = 性能模式开关（1 = 开）
    //   w = 远场贴图降档起点（格，0 = 关）：material_resolve.glsl 起点起每
    //       32 格抬 1 级 mip（封顶 +3），8 区块以外材质贴图慢慢降档
    // 消费方：cpp/shaders/ss_hybrid.comp（距离分档）、rgen_main_body.glsl（弹射分档）、
    //         material_resolve.glsl（远场贴图 mip）
    vec4 perfBandParams;

    // ── moonParams ──（2026-11-16 追加在末尾，与 OrionInternal.h 顺序一致）
    //   x = 月光能量地板（新月夜 = 满月的这一比例；0 = 纯物理，只剩环境光）
    //   y = 白天月亮亮度（占夜间基准的比例；1 = 不压暗）
    //   z = 有效性标记（1 = 宿主已写入本块）。消费方必须走 z > 0.5 判定：
    //       旧 DLL 不写这一块 ⇒ 全 0 ⇒ 回落编译期默认值，否则新月夜全黑。
    //   w = 保留
    vec4 moonParams;

    // ── fogMoodParams ──（2026-11-16 追加在末尾，与 OrionInternal.h 顺序一致）
    //   x = 雾形态强度 0..2（0 = 均匀灰膜旧档，1 = 出厂团块）
    //   y = 晨雾峰值倍率 1..3（1 = 不增强，1.75 = 出厂）
    //   z/w = 保留
    // 消费方：fog_froxel_density.comp
    vec4 fogMoodParams;

    // ── visQueryAb ──（2026-11-16 追加在末尾，与 OrionInternal.h 顺序一致）
    // 可见性射线 inline ray query 可选臂的按位闸门。默认全 0 = 走既有 RT pipeline
    // traceRayEXT（逐位旧行为）；本机跑不了客户端、未做画面 A/B，等价是代码证出来
    // 的，所以默认关。每 lane 一个点位，互不影响：
    //   x = bit0 AO 可见性点位（rgen_main_body.glsl 的 RTAO 探针）
    //   y = bit1 水面太阳可见性点位（rgen_glossy.glsl）
    //   z = bit2 水体入口太阳可见性点位（rgen_water_receiver.glsl）
    //   w = 保留（宿主写 0）
    // 宿主：环境变量 ORION_VIS_QUERY（4 位掩码），每帧写入。
    // 消费方：上述三个 rgen 点位各自的 query 臂（#if RTAO_VISIBILITY_EARLYOUT 包着）。
    uvec4 visQueryAb;
