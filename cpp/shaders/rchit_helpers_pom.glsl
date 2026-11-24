// 2026-11-05 B①：从 rt_test.rchit 1063-2100 行原样切出的 POM/实体/杂项 helper 段。
// rchit 在原位置 include 回来；K2 的 rgen（ORION_K2_TRANSPORT）也 include 它。

// =====================================================
// LabPBR 1.3
// Atlas-safe Parallax Occlusion Mapping
//
// Quality:
//     12 steps = near perpendicular
//     32 steps = grazing angle
//
// _n.a:
//     1.0 = zero depth
//     0.0 = 25% block depth
// =====================================================

float sampleLabPBRDepth(
    vec2 uv
)
{
    float a =
        clamp(
            textureLod(
                normalAtlas,
                uv,
                0.0
            ).a,
            0.0,
            1.0
        );


    // LabPBR `_n.a == 0` means "no height data"; we remap it to the
    // top of the field (`sh += saturate(1 - sh*1e20)`).  Read as maximum depth
    // it pushed flat regions to the bottom of the height field.
    a +=
        clamp(
            1.0 - a * 1.0e20,
            0.0,
            1.0
        );


    return
        1.0 -
        a;
}


// =====================================================
// POM sprite boundary
//
// A Minecraft quad owns one independent height field.  Do not wrap or clamp
// a traversed POM position after it leaves this sprite: retain the last valid
// in-sprite sample instead.  This prevents opposite-edge height data from
// appearing as a grazing-angle displacement or self-shadow blocker.
// =====================================================

bool isPOMSpriteUVInBounds(
    vec2 uv,
    vec2 safeMin,
    vec2 safeMax
)
{
    return
        all(greaterThanEqual(uv, safeMin)) &&
        all(lessThanEqual(uv, safeMax));
}


// LabPBR `_n.a` as a height, with the "no height data" remap:
// alpha 0 is treated as the top of the field, not the bottom.
float pomHeightAlpha(ivec2 texel, ivec2 lo, ivec2 hi)
{
    texel = clamp(texel, lo, hi);

    float a =
        clamp(
            texelFetch(normalAtlas, texel, 0).a,
            0.0,
            1.0
        );

    return
        a +
        clamp(
            1.0 - a * 1.0e20,
            0.0,
            1.0
        );
}


// =====================================================
// PARALLAX_MODE == 2 height sampling
// (BilinearHeightSample).
//
// Nearest-texel sampling snapped the POM hit to whole atlas texels, which
// stair-stepped the relief and broke the displaced albedo up with it.  A
// bilinear read of the same height field keeps the surface continuous.
//
// The 2x2 footprint is clamped to the owning sprite's texel range, so bilinear
// still cannot read the neighbouring atlas cell -- the seam the sprite guards
// exist to prevent.
// =====================================================

// 2026-10-18 视差高度场采样开关（设置界面按钮，CameraUBO.pomParams.x）：
//   0（默认 / 旧 DLL）＝ 平滑：双线性 2x2 足迹；
//   > 0.5               ＝ 锐利：单纹素 texelFetch。
// 只切换高度场采样；位移 / 深度 / 自阴影 / 边界哨兵口径都不变。
bool pomSmoothSampling()
{
    return cam.pomParams.x <= 0.5;
}


// =====================================================
// 2026-10-29：把 atlasSizeF / lo / hi 拆成显式参数。
//
// 自阴影循环（tracePOMSelfShadow）每 hit 调 4~16 次采样，而这三次量在整个
// hit 内**都是常量**：atlasSizeF 只来自 textureSize()，lo/hi 只由
// safeMin/safeMax 决定。原实现每次调用都重算它们（floor/ceil 各两条 vec2
// 运算 + 一次 max，约 10 条指令 × 4~16 次）。
//
// 本函数只做参数搬运：采样点、clamp 边界、2x2 足迹、锐利/平滑分档与拆分前
// **逐位相同**（safeMin/safeMax 不再传入，因为 lo/hi 已含其全部信息）。
// =====================================================
vec2 labPBRAtlasSizeF()
{
    return vec2(textureSize(normalAtlas, 0));
}

void labPBRDepthSpriteBounds(
    vec2 safeMin,
    vec2 safeMax,
    vec2 atlasSizeF,
    out ivec2 lo,
    out ivec2 hi
)
{
    lo =
        ivec2(floor(safeMin * atlasSizeF + 1.0e-4));

    hi =
        ivec2(ceil(safeMax * atlasSizeF - 1.0e-4)) - ivec2(1);

    hi =
        max(hi, lo);
}

float sampleLabPBRDepthInSpritePrepared(
    vec2 uv,
    vec2 atlasSizeF,
    ivec2 lo,
    ivec2 hi
)
{
    vec2 texelSpace =
        uv * atlasSizeF - 0.5;

    ivec2 baseTexel =
        ivec2(floor(texelSpace));

    vec2 f =
        texelSpace - vec2(baseTexel);

    // 2026-10-18 用户开关：锐利档退回单纹素采样，
    // 默认平滑档继续走下面的双线性。
    if(!pomSmoothSampling())
    {
        ivec2 sharpTexel =
            ivec2(floor(uv * atlasSizeF));

        return
            1.0 -
            pomHeightAlpha(sharpTexel, lo, hi);
    }

    float a00 =
        pomHeightAlpha(baseTexel + ivec2(0, 0), lo, hi);

    float a10 =
        pomHeightAlpha(baseTexel + ivec2(1, 0), lo, hi);

    float a01 =
        pomHeightAlpha(baseTexel + ivec2(0, 1), lo, hi);

    float a11 =
        pomHeightAlpha(baseTexel + ivec2(1, 1), lo, hi);

    float a =
        mix(
            mix(a00, a10, f.x),
            mix(a01, a11, f.x),
            f.y
        );

    return
        1.0 -
        a;
}


// 原入口：算一次边界再转调，行为与拆分前逐位一致（其它调用点不受影响）。
float sampleLabPBRDepthInSprite(
    vec2 uv,
    vec2 safeMin,
    vec2 safeMax
)
{
    vec2 atlasSizeF =
        labPBRAtlasSizeF();

    ivec2 lo;
    ivec2 hi;

    labPBRDepthSpriteBounds(safeMin, safeMax, atlasSizeF, lo, hi);

    return
        sampleLabPBRDepthInSpritePrepared(
            uv,
            atlasSizeF,
            lo,
            hi
        );
}


// =====================================================
// POM sprite tiling
//
// A Minecraft block face spans exactly one sprite period, so the sprite IS the
// tile: the neighbouring block of the same material carries this identical
// sprite, and its relief is already present inside these bounds.  Wrapping a
// traversal position periodically therefore continues the height field across
// a block edge with no neighbour lookup at all.
//
// The returned point is always strictly inside the sprite period -- fract()
// yields [0,1) -- so it can never land on the far edge and fetch the next
// atlas cell.
//
// 1 = wrap (default).  The previous rule stopped the march at the sprite edge
//     and kept the last in-sprite sample, which left a real displacement step
//     at every edge shared by two same-material blocks.  Set to 0 to restore
//     it: the guarded branches below then compile back to the old behaviour.
// =====================================================

#define POM_TILE_SPRITE 1

vec2 wrapPOMSpriteUV(
    vec2 uv,
    vec2 spriteMin,
    vec2 spriteSpan
)
{
    vec2 local =
        (uv - spriteMin) /
        max(spriteSpan, vec2(1.0e-8));

    local -=
        floor(local);

    return
        spriteMin +
        local *
        spriteSpan;
}


float sampleLabPBRDepthTiled(
    vec2 uv,
    vec2 spriteMin,
    vec2 spriteSpan,
    vec2 safeMin,
    vec2 safeMax
)
{
    // Texel fetch floors this coordinate, so hold it off the far sprite edge.
    // For an already in-bounds uv this is identical to the direct fetch.
    return
        sampleLabPBRDepthInSprite(
            min(
                wrapPOMSpriteUV(uv, spriteMin, spriteSpan),
                safeMax
            ),
            safeMin,
            safeMax
        );
}


// 2026-10-29：自阴影专用入口 —— 边界已算好，循环内不再重算。
// wrap / safeMax 的处理与上面的 sampleLabPBRDepthTiled 完全一致。
float sampleLabPBRDepthTiledPrepared(
    vec2 uv,
    vec2 spriteMin,
    vec2 spriteSpan,
    vec2 safeMax,
    vec2 atlasSizeF,
    ivec2 lo,
    ivec2 hi
)
{
    return
        sampleLabPBRDepthInSpritePrepared(
            min(
                wrapPOMSpriteUV(uv, spriteMin, spriteSpan),
                safeMax
            ),
            atlasSizeF,
            lo,
            hi
        );
}


vec2 snapPOMSpriteUVToTexelCenter(
    vec2 uv,
    vec2 safeMin,
    vec2 safeMax
)
{
    ivec2 atlasSize = textureSize(normalAtlas, 0);
    vec2 texel = floor(uv * vec2(atlasSize));
    return clamp(
        (texel + vec2(0.5)) / vec2(atlasSize),
        safeMin,
        safeMax
    );
}


float getPOMDepthScale()
{
    // 2026-10-25 A/B：GI 射线跳过 POM。返回 0 会让 traceParallax() 与
    // tracePOMSelfShadow() 走它们**既有**的 "POM off" 早退分支（见各自的
    // pomDepthScale <= 1e-5 判定），所以这里不需要改 march 本身。
    if(g_giRaySkipPOM)
    {
        return 0.0;
    }

    // 2026-10-29 消融档 18「noPom」：POM 主 march 与自阴影的**价格上界**。
    // 返回 0 让 traceParallax()/tracePOMSelfShadow() 走各自既有的 pomDepthScale<=1e-5
    // 早退分支，march 本身一个字不改 ⇒ 两者行为与"材质没开视差"逐位相同。
    // 注意口径：POM 关掉后 displaced uv 也消失，后续采样与 borderScale 一起变简单，
    // 所以 18 的价格是「POM + 由它派生的一切」，是上界。
    if(cam.perfAblationStage.x == 18u)
    {
        return 0.0;
    }

    // CameraUBO.historyParamsPadding0 -> historyParams.z
    return
        clamp(
            cam.historyParams.z,
            0.0,
            0.20
        );
}


struct ParallaxHit
{
    vec2 uv;

    float depth01;
    float borderScale;

    float depthWorld;
    float rayTravelWorld;

    vec3 worldPosition;

    bool valid;
};


ParallaxHit traceParallax(
    vec2 baseUV,
    vec2 spriteMin,
    vec2 safeMin,
    vec2 safeMax,
    vec2 spriteSpan,
    mat3 tbn,
    vec3 worldRayDirection
)
{
    float pomDepthScale =
        getPOMDepthScale();


    // 0.00 is a real "POM off" value.
    // Avoid the entire 32-96 step march and self-shadow path.
    if(pomDepthScale <= 1.0e-5)
    {
        ParallaxHit result;

        result.uv =
            baseUV;

        result.depth01 =
            0.0;

        result.borderScale =
            1.0;

        result.depthWorld =
            0.0;

        result.rayTravelWorld =
            0.0;

        result.worldPosition =
            materialRayOrigin +
            materialRayDirection *
            materialHitT;

        result.valid =
            false;

        return result;
    }


    // =====================================================
    // POM-C
    // Stable view-space setup.
    // =====================================================

    // Ray direction points toward the surface.
    // POM needs surface -> ray origin.
    vec3 viewDirTS =
        normalize(
            transpose(tbn) *
            (-worldRayDirection)
        );


    // Make the tangent-space view vector front-facing.
    // Keep this behavior identical for primary / reflection / GI.
    if(viewDirTS.z < 0.0)
    {
        viewDirTS =
            -viewDirTS;
    }


    float viewZ =
        clamp(
            viewDirTS.z,
            0.0,
            1.0
        );


    // =====================================================
    // POM-C1
    // Extreme-grazing safety.
    //
    // Old code divided by max(viewZ, 1e-4), which allowed
    // extremely large UV movement when the ray became almost
    // parallel to the surface.  Only the final few degrees are
    // attenuated here; normal viewing angles remain unchanged.
    // =====================================================

    const float POM_GRAZING_MIN_Z =
        0.04;

    const float POM_GRAZING_FULL_Z =
        0.10;


    float grazingFade =
        smoothstep(
            POM_GRAZING_MIN_Z,
            POM_GRAZING_FULL_Z,
            viewZ
        );


    float stableViewZ =
        max(
            viewZ,
            POM_GRAZING_MIN_Z
        );


    float facing =
        viewZ;


    // =====================================================
    // Pixel POM keeps the confirmed continuous 32 -> 48 step range.
    // The loop count is fixed at 48; only the per-ray depth increment varies.
    // =====================================================

    float layerDensity =
        mix(
            48.0,
            32.0,
            facing
        );


    layerDensity =
        clamp(
            layerDensity,
            32.0,
            48.0
        );


    float layerStep =
        1.0 /
        layerDensity;


    // LabPBR displacement in atlas UV space.
    vec2 totalUVOffset =
        (
            viewDirTS.xy /
            stableViewZ
        ) *
        spriteSpan *
        pomDepthScale *
        grazingFade;

#if POM_TILE_SPRITE
    // A block face spans exactly one sprite period, so one tile of tile-space
    // offset is "at most the immediately adjacent block".  Without this cap a
    // grazing-angle traversal reaches two or more tiles -- the offset in tiles
    // is |viewDirTS.xy / stableViewZ| * pomDepthScale, which passes 1.0 from
    // about 0.2 block depth at 0.1 view Z -- and would then fetch relief from
    // blocks that are nowhere near the hit.  That runaway is exactly what the
    // sprite confinement used to prevent; this reproduces the bound while
    // still allowing the single tile of continuation a block edge needs.
    vec2 totalTileOffset =
        totalUVOffset /
        max(spriteSpan, vec2(1.0e-8));

    totalTileOffset =
        sign(totalTileOffset) *
        min(
            abs(totalTileOffset),
            vec2(1.0)
        );

    totalUVOffset =
        totalTileOffset *
        spriteSpan;
#endif


    vec2 currentUV =
        baseUV;


    float currentLayerDepth =
        0.0;


    float currentMapDepth =
        sampleLabPBRDepthInSprite(
            currentUV,
            safeMin,
            safeMax
        );


    // Absolute reconstruction is retained; traversal stops before leaving
    // the current Minecraft sprite.

    for(
        int i = 0;
        i < 48;
        ++i
    )
    {
        if(
            currentLayerDepth >=
            currentMapDepth
        )
        {
            break;
        }


        if(currentLayerDepth >= 1.0)
        {
            break;
        }


        float nextLayerDepth =
            min(
                currentLayerDepth +
                    layerStep,
                1.0
            );


        vec2 nextUV =
            baseUV -
            totalUVOffset *
            nextLayerDepth;

#if POM_TILE_SPRITE
        // Continue the traversal into the next tile of the same sprite rather
        // than stopping at the block edge.  The neighbouring same-material
        // block owns this identical sprite, so this is its relief too, and the
        // now shared height field is what removes the displacement step.
        nextUV =
            wrapPOMSpriteUV(
                nextUV,
                spriteMin,
                spriteSpan
            );
#else
        // Do not wrap, clamp or blend a traversal position outside the
        // owning sprite.  Keep currentUV/currentMapDepth, the last valid
        // height-field point on this compiled Minecraft quad.
        if(!isPOMSpriteUVInBounds(nextUV, safeMin, safeMax))
        {
            break;
        }
#endif

currentUV =
    nextUV;

currentLayerDepth =
    nextLayerDepth;

currentMapDepth =
    sampleLabPBRDepthTiled(
        currentUV,
        spriteMin,
        spriteSpan,
        safeMin,
        safeMax
    );

    }


    ParallaxHit result;


    // Keep the continuous final POM coordinate.  Snapping the result to the
    // centre of the outer texel made neighbouring block faces select visibly
    // different lighting samples and exposed a bright seam.
    result.uv =
        clamp(
            currentUV,
            safeMin,
            safeMax
        );


    // Only the normal/self-shadow terms converge in the outermost atlas
    // texel.  POM UV displacement, depth and world-space ray travel remain
    // untouched, so this seam guard cannot pull the surface back to the
    // original triangle plane.
    vec2 pomAtlasSize =
        max(
            vec2(textureSize(normalAtlas, 0)),
            vec2(1.0)
        );

    vec2 edgeDistanceTexels =
        min(
            (result.uv - safeMin) * pomAtlasSize,
            (safeMax - result.uv) * pomAtlasSize
        );

    float nearestEdgeTexels =
        max(
            min(edgeDistanceTexels.x, edgeDistanceTexels.y),
            0.0
        );

    result.borderScale =
        smoothstep(
            0.0,
            2.0,
            nearestEdgeTexels
        );


    // Use the actual height of the final selected texel.  The previous
    // quantized march layer made the POM self-shadow jump whenever the
    // view crossed a 32..48-step boundary.
    result.depth01 =
        currentMapDepth *
        grazingFade;


    result.depthWorld =
        result.depth01 *
        pomDepthScale;


    result.rayTravelWorld =
        result.depthWorld /
        stableViewZ;


    vec3 originalWorldHit =
        materialRayOrigin +
        materialRayDirection *
        materialHitT;


    result.worldPosition =
        originalWorldHit +
        normalize(
            worldRayDirection
        ) *
        result.rayTravelWorld;


    result.valid =
        true;


    return result;
}


// =====================================================
// POM-B
// Height-field self-shadow toward the current fixed sun.
// 4-8 height samples, no additional ray tracing.
// =====================================================

// 2026-11-10 遮蔽判据（世界尺度 + 纹素锚定）的三档 A/B，说明见
// tracePOMSelfShadow 内的长注释：
//   =0.15   浮雕影更明显
//   =0.25   默认
//   =0.5    更弱
//   =0.016  约等于 2026-10-19 旧口径（一碰就全黑 ⇒ 豹纹）
#ifndef POM_SELF_SHADOW_FULL_TEXELS
#define POM_SELF_SHADOW_FULL_TEXELS 0.25
#endif
#ifndef POM_RES_ADAPTIVE_SHADOW
#define POM_RES_ADAPTIVE_SHADOW 1
#endif
#ifndef POM_SELF_SHADOW_MIN_DEPTH_FRAC
#define POM_SELF_SHADOW_MIN_DEPTH_FRAC 0.32
#endif

float tracePOMSelfShadow(
    vec2 hitUV,
    float hitDepth01,
    vec2 spriteMin,
    vec2 safeMin,
    vec2 safeMax,
    vec2 spriteSpan,
    mat3 tbn,
    vec3 worldLightDirection
)
{
    float pomDepthScale =
        getPOMDepthScale();

    // 2026-10-29 消融档 19「noPomSelfShadow」：只跳过自阴影的 24 次采样 march
    // （VL_POM_SELF_SHADOW_MAX_SAMPLES），主 POM march 与 displaced uv 全部保留。
    // 返回 1.0 = 无遮蔽，与"没有浮雕"那条早退分支同义。
    if(cam.perfAblationStage.x == 19u)
    {
        return 1.0;
    }


    if(
        hitDepth01 <= 0.01 ||
        pomDepthScale <= 1.0e-5
    )
    {
        return 1.0;
    }


    vec3 lightDirTS =
        normalize(
            transpose(tbn) *
            normalize(worldLightDirection)
        );


    // Light behind / almost parallel to the geometric plane:
    // leave direct-light rejection to the normal/NdotL path.
    if(lightDirTS.z <= 1.0e-3)
    {
        return 1.0;
    }


    float facing =
        clamp(
            lightDirTS.z,
            0.0,
            1.0
        );


    float lightZ =
        max(
            lightDirTS.z,
            0.05
        );


    // =====================================================
    // 2026-10-14 掠射角自阴影（实机症状："日落把视差浮雕点亮"）
    // =====================================================
    // 取证：正午（仰角大）浮雕自阴影正常，日落（仰角 ~7.5°）整片消失。
    //
    // 机制：march 的水平步幅 = lightDirTS.xy / lightZ * pomDepthScale（见下面的
    // selfShadowTileOffset）—— **太阳越低步幅越大**。而样本数是固定 4~8 个 ⇒
    // 掠射时相邻样本在贴图上的水平间隔可达 0.1 格以上（≈2 texel），直接**跨过**
    // 浮雕的凸起 ⇒ blockDepth 一路判不到 ⇒ 返回 1.0（毫无自遮蔽）⇒ 浮雕在日落
    // 被压平点亮。正午 lightZ ≈ 1 ⇒ 同一段路密得多 ⇒ 正常。
    //
    // 修法：让采样密度跟着 1/lightZ 一起涨（上限 4 倍）。**射程不变、只把同一段
    // 路走细**，所以遮蔽的**位置**不会被改变，只有"抓不抓得到凸起"变了。
    //
    // ⚠ 正午 lightZ ≥ 0.25 ⇒ boost = 1 ⇒ 采样数与旧档**逐位相同**（不动正午画面）。
    //   boost = 0.25 / lightZ：15° 起才 > 1，7.5° 时约 1.9，2.9° 封顶 4 倍。
    // 回退：把 grazingBoost 恒置 1.0 即逐位旧档。
    // =====================================================
    float grazingBoost =
        clamp(
            0.25 / lightZ,
            1.0,
            4.0
        );


    // 2026-10-14：掠射角下的样本上限（旧档固定 8 —— 那正是日落丢自阴影的原因，
    // 见上面 grazingBoost 的取证）。24 与 4 倍密度上限配对，成本只在低太阳角发生。
    const int VL_POM_SELF_SHADOW_MAX_SAMPLES = 24;

    const vec2 selfShadowAtlasSizeF =
        labPBRAtlasSizeF();

    const float selfShadowSpriteRes =
        max(spriteSpan.x * selfShadowAtlasSizeF.x, 1.0);

#if POM_RES_ADAPTIVE_SHADOW
    const float resLog2 =
        clamp(log2(max(selfShadowSpriteRes / 16.0, 1.0)), 0.0, 5.0);

    const float resSampleBoost =
        clamp(1.0 + 0.35 * resLog2, 1.0, 2.2);
#else
    const float resSampleBoost = 1.0;
#endif

    int sampleCount =
        int(
            round(
                mix(
                    8.0,
                    4.0,
                    facing
                ) *
                grazingBoost *
                resSampleBoost
            )
        );


    sampleCount =
        clamp(
            sampleCount,
            4,
            VL_POM_SELF_SHADOW_MAX_SAMPLES
        );


    // =====================================================
    // 2026-11-10 判据改回**世界尺度**，并锚在纹素尺寸上
    //
    // 病灶（用户截图：太阳直射时墙面出现豹纹状黑斑）：
    //   2026-10-19 起口径是"归一化高度差满遮蔽 = 0.02"，即高度范围的 2%。
    //   高度范围 = pomDepthScale ≈ 0.05 格 ⇒ 满遮蔽只需要 0.001 格 ≈ 1 mm。
    //   而 _n.a 是 8 bit：1 LSB = pomDepthScale/255 ≈ 0.2 mm ⇒ **相邻纹素差
    //   5 级就直接全黑**，0.004 的偏置（= 1 LSB）压不住任何东西。
    //   ⇒ 遮蔽退化成"采到的那个纹素比射线高 5 级吗"的 0/1 判定，逐纹素成块，
    //     而且完全确定性 ⇒ 逐帧不闪的豹纹黑斑（与 material_resolve_glass.glsl
    //     里记录的"正视干净、斜看固定块状噪点"同源）。
    //
    // 新口径：满遮蔽 = 埋得比 **0.25 个纹素的世界边长**更深。
    //   横向采样误差本来就是纹素量级（采样点在贴图上走一格 = 1 纹素），
    //   所以只有"埋得比纹素尺度更深"才可能是真遮挡。
    //   16x16 贴图 ⇒ 0.25 texel = 0.0156 格 = 80 个 LSB：
    //     · 5 LSB 的纹素间噪声 → 6% 遮蔽（看不出来 ⇒ 黑斑消失）；
    //     · 深浮雕（旧口径下真能全遮蔽的那种高度差）→ 仍然能全遮蔽。
    //   与"太阳越贴近面法线，浮雕影越淡"也一致：影长 = 深度 × tanθ，
    //   高太阳时本来就落在亚纹素量级，而不是在阈值附近乱翻。
    //
    // 与 pomDepthScale 的耦合（有意）：POM 深度调大 ⇒ 高度范围变深 ⇒
    // 同样 0.25 texel 的门槛只占范围的一小部分 ⇒ 浮雕影变强。
    //   采样数、早退时机、循环体指令数都不变（只换一个乘法与比较口径）。
    //   回退见文件顶部 POM_SELF_SHADOW_FULL_TEXELS 的三档。
    // =====================================================
    float selfShadowTexelWorld =
        1.0 / selfShadowSpriteRes;

#if POM_RES_ADAPTIVE_SHADOW
    float selfShadowFullDepthWorld =
        max(
            max(
                selfShadowTexelWorld *
                    POM_SELF_SHADOW_FULL_TEXELS,
                pomDepthScale *
                    POM_SELF_SHADOW_MIN_DEPTH_FRAC
            ),
            1.0e-6
        );

    float selfShadowBias01 =
        !pomSmoothSampling()
            ? mix(0.008, 0.024, clamp(resLog2 * 0.25, 0.0, 1.0))
            : 0.004;
#else
    float selfShadowFullDepthWorld =
        max(
            selfShadowTexelWorld *
                POM_SELF_SHADOW_FULL_TEXELS,
            1.0e-6
        );

    const float selfShadowBias01 = 0.004;
#endif


    float maxBlockDepthWorld =
        0.0;


    // Hoisted from the loop and, under POM_TILE_SPRITE, held to the same one
    // tile bound as the main traversal so the blocker search cannot reach
    // height data from blocks that are not adjacent to the hit either.
    vec2 selfShadowTileOffset =
        (
            lightDirTS.xy /
            lightZ
        ) *
        pomDepthScale;

#if POM_TILE_SPRITE
    selfShadowTileOffset =
        sign(selfShadowTileOffset) *
        min(
            abs(selfShadowTileOffset),
            vec2(1.0)
        );
#endif


    // 2026-10-29：atlasSizeF 与纹素边界在整个 hit 内不变 ⇒ 循环外算一次。
    // 循环体每步只做 wrap + 采样，不再重复 textureSize / floor / ceil。
    ivec2 selfShadowLo;
    ivec2 selfShadowHi;

    labPBRDepthSpriteBounds(
        safeMin,
        safeMax,
        selfShadowAtlasSizeF,
        selfShadowLo,
        selfShadowHi
    );


    for(int i = 1; i <= VL_POM_SELF_SHADOW_MAX_SAMPLES; ++i)
    {
        if(i > sampleCount)
        {
            break;
        }


        // Exclude both endpoints to avoid immediately
        // self-testing the exact POM hit or the base plane.
        float progress =
            float(i) /
            float(sampleCount + 1);


        float depthRise =
            hitDepth01 *
            progress;


        float rayDepth =
            hitDepth01 -
            depthRise;


        vec2 sampleUV =
            hitUV +
            selfShadowTileOffset *
            spriteSpan *
            depthRise;


#if POM_TILE_SPRITE
        // The height field tiles with the sprite, so the blocking relief of
        // the neighbouring same-material block is the wrapped sample here.
        // Without this the self-shadow still broke at the block edge even
        // though the traversal above no longer did.
        float blockerDepth =
            sampleLabPBRDepthTiledPrepared(
                sampleUV,
                spriteMin,
                spriteSpan,
                safeMax,
                selfShadowAtlasSizeF,
                selfShadowLo,
                selfShadowHi
            );
#else
        // A light ray leaving the owning sprite has no in-sprite height
        // blocker.  Do not wrap it to the opposite edge.
        if(!isPOMSpriteUVInBounds(sampleUV, safeMin, safeMax))
        {
            break;
        }

        float blockerDepth =
            sampleLabPBRDepthInSprite(
                sampleUV,
                safeMin,
                safeMax
            );
#endif


        // Positive means the light ray is underneath a
        // neighbouring height-field surface and is blocked.
        //
        // 2026-11-10：换算成**世界单位**再判定（乘 pomDepthScale），阈值见上。
        // 归一化偏置 0.004 保留 —— 它是 1 个 8 bit LSB，只用来压住双线性
        // 高度场与 march 层的数值抖动，不再是"吃掉阈值 60%"的那个偏置。
        float blockDepthWorld =
            (
                rayDepth -
                blockerDepth -
                selfShadowBias01
            ) *
            pomDepthScale;


        maxBlockDepthWorld =
            max(
                maxBlockDepthWorld,
                blockDepthWorld
            );


        // Early-out once the ray is buried deeper than the texel-scaled bar.
        if(
            maxBlockDepthWorld >=
            selfShadowFullDepthWorld
        )
        {
            return 0.0;
        }
    }


    return
        1.0 -
        smoothstep(
            0.0,
            selfShadowFullDepthWorld,
            maxBlockDepthWorld
        );
}


// =============================================================================
// 2026-10-23：延迟材质整段删除后，本 TU 是唯一的材质求值点——命中着色器通过
// resolveHitMaterial() 就地求完整个材质。
// =============================================================================
// 材质体（共享体在 material_resolve.glsl）。放在这里，确保 include 点之前
// 它调用的每个 helper 都已声明。
//
// LOD role indirection: lod_coverage.glsl 的 lodInstanceRole() 读 gl_InstanceCustomIndexEXT，
// 那是命中着色器的内建；rgen 没有它。共享体因此只认 materialLodRole()，本 TU 直接转调
// 原生实现（参数与内建同值，逐位等价）。
uint materialLodRole(uint instanceIndex)
{
    if(instanceIndex >= LOD_COVERAGE_SLOT_LIMIT)
    {
        return LOD_ROLE_TERRAIN;
    }

    return lodRoles.roles[instanceIndex];
}

// 共享解析体写入它；本 TU 的材质 helper 仍直接读 materialHitT，这里只是让共享体
// 能编译。三个全局量都在 resolveHitMaterial() 开头被写入，初值只为满足声明，
// 不参与求值。
