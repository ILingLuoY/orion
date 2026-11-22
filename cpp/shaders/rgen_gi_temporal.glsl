// ============================================================================
// 2026-11-04 A1：GI 第一反弹时域重投影（HANDOFF 见对话记录）
//
// 背景：GI 距离梯形（rgen_main_body 的 rtLodDiffuseGiEnabled / rtLodDiffuseGiWeight）
// 是"时间交错"采样 —— 64 格外按 1/2~1/32 抽稀、跑到的帧乘权重补偿。时间平均无偏，
// 但被跳过的帧 indirectLight = 0，方差全交给 DLSS-RR 对整色累积。
//
// 这套时域历史把"补偿"升级成真正的 EMA 累积：
//   · 本帧跑了 GI 的像素：resolved = mix(H, T, α)，α = 1/(1+梯形权重)
//     （近场 α=0.5 追得快；384 格外 α≈0.03 累积深 —— 与旧交错的时间常数对齐）
//   · 本帧被跳过的像素：resolved = H（重投影校验通过时），否则 0（与旧行为一致）
//   · 两分支下 diffuseGILodWeight（纯速率补偿，见 4719-4720）都置 1 ——
//     补偿的职责已由历史接管，再乘一次就是双份。
//   · 校验：稠密 MV（UV 单位，rtDebugPrimaryMotion 与写进 MV 图的同一值）取上帧
//     像素 + 视深度相对差 10%；历史无效（a=0）/重投影出屏/深度不符 ⇒ 视为无历史。
//   · 主射线 miss 的像素写 a=0 失效标记（clear 后天然 a=0 = 无效，无需哨兵值）。
//
// ping-pong：binding 86/87 两张渲染分辨率 rgba16f，都常驻描述符集，
//   读 half = cam.historyReadIndex，写 half = 1 - 读 half（与 vptDirectHistory
//   同一约定，见 rgen_main_body 6673/6702）。跨帧读写依赖帧间信号量的内存依赖。
//
// 开关：cam.giRayParams.y > 0.5（宿主 ORION_GI_TEMPORAL，默认开）。
//   giRayParams.y/z/w 此前恒 0（Frame.cpp:370-372），借用 .y 不动 UBO 布局。
//
// 变体约束：SHARC_UPDATE（(w/5)² 稀疏派发，读自己的像素会张冠李戴）与
//   GI_HALFRES（写 giHalfImage 后提前 return）都不得碰这两张图 —— 见各 #if。
// ============================================================================

#if !SHARC_UPDATE && !GI_HALFRES

// 2026-11-05 诊断探针（DBG_GI_TEMPORAL_PROBE）：把 A1 的内部判定画成颜色，
// 输出尾部覆盖 currentColor（rgen_main_body 的 MV 调试图同型）。
//   未调用（不该发生）= 品红
//   R = historyValid（历史接受=1 绿/红）
//   G = |motionUv| × 40（运动矢量幅度）
//   B = 深度失配 / 容差（>1 = 深度判据必拒）
#ifdef DBG_GI_TEMPORAL_PROBE
vec3 g_dbgGiTemporal =
    vec3(-1.0);
#endif

layout(binding = 86, set = 0, rgba16f)
uniform image2D giTemporalHistoryImage0;

layout(binding = 87, set = 0, rgba16f)
uniform image2D giTemporalHistoryImage1;

bool giTemporalEnabled()
{
    return cam.giRayParams.y > 0.5;
}

// GLSL 不允许局部 opaque 句柄变量 ⇒ 读/写哪一半只能用显式分支表达。
ivec2 giTemporalHistorySize()
{
    if(cam.historyReadIndex == 0u)
    {
        return imageSize(giTemporalHistoryImage0);
    }
    return imageSize(giTemporalHistoryImage1);
}

vec4 giTemporalHistoryLoad(ivec2 pixel)
{
    if(cam.historyReadIndex == 0u)
    {
        return imageLoad(giTemporalHistoryImage0, pixel);
    }
    return imageLoad(giTemporalHistoryImage1, pixel);
}

void giTemporalHistoryStore(ivec2 pixel, vec4 value)
{
    if(cam.historyReadIndex == 0u)
    {
        imageStore(giTemporalHistoryImage1, pixel, value);
    }
    else
    {
        imageStore(giTemporalHistoryImage0, pixel, value);
    }
}

// 解析本像素的 GI 时域值并把结果写进下一帧的那一半。
//   traced        本帧是否产出了新鲜 GI（回路跑了，或半分辨率上采样兜了底）
//   hasPrimaryHit 主射线是否命中（miss 只写失效标记）
//   motionUv      写进 MV 图的那份 UV 运动矢量（rtDebugPrimaryMotion）
//   curViewDepth  -(viewMatrix * vec4(hitPos,1)).z，与 giHalfViewDepth 同公式
//   tracedGi      本帧 traced 的原始 indirectLight（乘 diffuseGILodWeight 之前）
//   rateWeight    rtLodDiffuseGiWeight（梯形速率补偿），只用来定 α
// 返回解析后的 GI（raw 域；调用方照旧乘 1.8/rtao/远场项，补偿已置 1）。
vec3 giTemporalResolveAndStore(
    ivec2 pixel,
    bool traced,
    bool hasPrimaryHit,
    vec2 motionUv,
    float curViewDepth,
    vec3 tracedGi,
    float rateWeight)
{
    // 读 half = cam.historyReadIndex，写 half = 1 - 读 half（helper 内分支）。
    if(!hasPrimaryHit)
    {
        giTemporalHistoryStore(pixel, vec4(0.0, 0.0, 0.0, 0.0));
        return vec3(0.0);
    }

    const ivec2 historySize = giTemporalHistorySize();
    const vec2 prevPixelF =
        vec2(pixel) + motionUv * vec2(historySize);
    const ivec2 prevPixel = ivec2(round(prevPixelF));
    const bool prevInBounds =
        all(greaterThanEqual(prevPixel, ivec2(0))) &&
        all(lessThan(prevPixel, historySize));

    vec4 history = vec4(0.0);
    if(prevInBounds)
    {
        history = giTemporalHistoryLoad(prevPixel);
    }

    // 视深度相对差 10%（0.35m 下限）：挡遮挡/换面，放过掠射角的轻微数值差。
    const bool historyValid =
        history.w > 0.0 &&
        abs(history.w - curViewDepth) <=
            max(0.35, 0.1 * curViewDepth);

#ifdef DBG_GI_TEMPORAL_PROBE
    g_dbgGiTemporal = vec3(
        historyValid ? 1.0 : 0.0,
        clamp(length(motionUv) * 40.0, 0.0, 1.0),
        clamp(
            abs(history.w - curViewDepth) /
                max(0.35, 0.1 * curViewDepth),
            0.0,
            1.0));
#endif

    vec3 resolved;
    if(traced)
    {
        if(historyValid)
        {
            // α 跟梯形档位走：近场快追，远场深累积。
            const float alpha =
                1.0 / (1.0 + max(rateWeight, 1.0));
            resolved = mix(history.rgb, tracedGi, alpha);
        }
        else
        {
            resolved = tracedGi;
        }
    }
    else
    {
        resolved = historyValid ? history.rgb : vec3(0.0);
    }

    resolved = max(resolved, vec3(0.0));

    giTemporalHistoryStore(
        pixel,
        vec4(min(resolved, vec3(60000.0)), curViewDepth));

    return resolved;
}

#endif // !SHARC_UPDATE && !GI_HALFRES
