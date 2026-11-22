#ifndef SUN_SHADOW_GLSL
#define SUN_SHADOW_GLSL

// ===========================================================================
// 2026-11-12：**无数据**（生产者未命中 / 采样点在盒外）时的取值。
//
// 之前的做法是按"离相机距离"或"离盒距离"渐隐到 0 —— 两者都是**相机相对**的，
// 抬头看天时会在头顶切出一个盖子，观感就是"总有一道体积光从头顶直射下来"
// （用户实测：关体积雾即消失，改半径也没用）。
//
// 正确判据是**采样点相对当地地表**：地表以上 = 露天（没有东西挡着，本该受光），
// 地表以下 = 洞内（没有几何数据就不该给光）。体积雾在每次调用前把它写进这个
// 变量；表面着色（rgen）不定义宏 ⇒ 不受影响、始终按 1.0。
// ===========================================================================
#ifdef SUN_SHADOW_TREAT_NO_HIT_AS_NO_DATA
float sunShadowNoDataVisibility = 1.0;
#endif

#ifndef SUN_SHADOW_Z_FADE
#define SUN_SHADOW_Z_FADE 1
#endif

const int SUN_SHADOW_RESOLUTION = 1024;
const float SUN_SHADOW_TEXEL = 1.0 / 1024.0;
const float SUN_SHADOW_CENTER_QUANTUM = 1.0 / 256.0;

const float SUN_SHADOW_EXTENT_XY = 160.0;

const float SUN_SHADOW_OUTSIDE_VISIBILITY = 0.08;
const float SUN_SHADOW_OUTSIDE_FADE_BLOCKS = 32.0;

#ifndef SUN_SHADOW_Z_EXTENT_BLOCKS
#define SUN_SHADOW_Z_EXTENT_BLOCKS 160.0
#endif
const float SUN_SHADOW_EXTENT_Z = SUN_SHADOW_Z_EXTENT_BLOCKS;

const float SUN_SHADOW_BIAS = 0.45;
const float SUN_SHADOW_BIAS_NEAR = 0.06;

const float SUN_SHADOW_ORIGIN_BIAS = 0.02;

// 0x08u hits opaque blocks, OMM foliage, and DH far LODs while excluding
// first-person hand (0x04), local player (0x02), and particles (0xF7).
const uint SUN_SHADOW_CULL_MASK = 0x08u;

// 光的方向：与两个体积 pass 的 sunDirection() 同一个公式（同一个
// cam.historyParams.w），并且同样在太阳落到地平线以下后翻到月亮方向。
//
// 这里用函数内常量而不是共享的 PI：各个着色器的 PI 声明位置不同，
// 引用全局 PI 会踩到"用到时还没声明"的坑。
vec3 sunShadowLightDirection()
{
    const float TWO_PI = 6.28318530718;
    float angle = fract(cam.historyParams.w) * TWO_PI;
    vec3 direction = normalize(vec3(cos(angle), 0.81915204429 * sin(angle),
        0.57357643635 * sin(angle)));
    if(direction.y >= 0.0)
    {
        return direction;
    }
    // 2026-11-16：夜间不再用 -direction（"月亮永远在太阳对面"），改为月亮
    // 沿同一条日轨推移 (月相 + 0.5) 圈。月相 0（满月）时本式与 -direction
    // 逐位等价；其余月相下阴影方向会跟着月亮走，与 rgen/rmiss 的可见月亮
    // 同一向量。⚠ 与 sky_model_common.glsl 的 moonEclipticDirectionAt 同式。
    float moonAngle = fract(
        fract(cam.historyParams.w) + cam.sssParams.z + 0.5) * TWO_PI;
    return normalize(vec3(cos(moonAngle), 0.81915204429 * sin(moonAngle),
        0.57357643635 * sin(moonAngle)));
}

// 正交盒中心：跟着相机，但量化到纹素尺寸以抑制移动时的"游泳"。
// 正交基随太阳缓慢转动，所以按世界轴量化不是严格的纹素对齐，
// 但足以压住绝大部分抖动。
//
// 参数化版本（2026-10-01）给**远场图**用：远场图的 extent / texel 由 CameraUBO
// 的 sunShadowFarParams 下发，不是编译期常量。近场那份（sunShadowCenter）保持
// 原样，行为逐字节不变。
vec3 sunShadowCenterFor(float extentXY, float texel)
{
    float texelWorld = 2.0 * extentXY * texel;
    vec3 cameraPosition = cam.viewInverse[3].xyz;
    return floor(cameraPosition / texelWorld) * texelWorld;
}

vec3 sunShadowCenter()
{
    return sunShadowCenterFor(SUN_SHADOW_EXTENT_XY, SUN_SHADOW_CENTER_QUANTUM);
}

// 太阳方向 → 正交基。生产者和消费者都调这一个，所以不会各算一套。
void sunShadowBasis(vec3 sunDirection, out vec3 basisRight, out vec3 basisUp)
{
    vec3 reference = abs(sunDirection.y) > 0.99
        ? vec3(1.0, 0.0, 0.0)
        : vec3(0.0, 1.0, 0.0);
    basisRight = normalize(cross(reference, sunDirection));
    basisUp = cross(sunDirection, basisRight);
}

// 生产者用：某个纹素对应的射线起点（盒的"上风侧"边界上）。
//
// =====================================================
// 2026-10-10 修正：起点必须落在**朝太阳**那一端，不是背太阳端
// =====================================================
// 这里原来是 `- sunDirection * SUN_SHADOW_EXTENT_Z`。sunDirection 是
// sunShadowLightDirection()，一个 y ≥ 0 的**朝太阳**单位向量，于是 "-" 把起点
// 放到了背太阳端（地下），射线朝太阳方向（向上）打。整套数学随之变成一个
// 镜像：stored 变成"从地下一路向上遇到的**最低**遮挡体距离"，而
// `depth < stored` 的含义就成了"采样点比最低遮挡体还低"—— 即**地下可见、
// 地面与空中的一切判为遮挡**。这正是 `rt_test.rgen` 里"远场图对表面采样点恒判
// 全遮挡"的根因（那次把它归给"远场图"并只停用了 rgen 的消费点，但同一份数学
// 也在近场图里，服务着体积光/雾）。
//
// 标准 shadow map 的做法是**从光源侧朝场景**打射线：起点在太阳端、方向朝下，
// stored = 沿太阳方向遇到的**最高**遮挡体距离，`depth < stored` = 采样点比最高
// 遮挡体更靠近太阳 = 中间没有东西挡着 = 受光。所以起点取 "+"。
// 配合消费者把 depth 改成从太阳端量起（见 sunShadowVisibilityIn），本文件回到
// 标准形式，体积光"穿墙"（背光侧被判可见）随之消失。
vec3 sunShadowRayOrigin(vec2 uv, vec3 sunDirection, vec3 center)
{
    vec3 basisRight;
    vec3 basisUp;
    sunShadowBasis(sunDirection, basisRight, basisUp);
    vec2 offset = (uv * 2.0 - 1.0) * SUN_SHADOW_EXTENT_XY;
    return center +
        basisRight * offset.x +
        basisUp * offset.y +
        sunDirection * SUN_SHADOW_EXTENT_Z;
}

vec3 sunShadowRayOriginFar(vec2 uv, vec3 sunDirection, vec3 center,
    float extentXY, float extentZ)
{
    vec3 basisRight;
    vec3 basisUp;
    sunShadowBasis(sunDirection, basisRight, basisUp);
    vec2 offset = (uv * 2.0 - 1.0) * extentXY;
    return center +
        basisRight * offset.x +
        basisUp * offset.y +
        sunDirection * extentZ;
}

#ifdef SUN_SHADOW_PRODUCER
float sunShadowTraceDepth(vec3 origin, vec3 sunDirection, float maxDistance)
{
    rayQueryEXT query;
    // Closest-hit traversal (NO TerminateOnFirstHit) so we record the highest
    // occluder along -sunDirection rather than an arbitrary BVH triangle.
    rayQueryInitializeEXT(
        query,
        sceneTlas,
        0u,
        SUN_SHADOW_CULL_MASK,
        origin,
        SUN_SHADOW_ORIGIN_BIAS,
        sunDirection,
        maxDistance);
    int guard = 0;
    while(rayQueryProceedEXT(query) && ++guard < 64) {}
    if(rayQueryGetIntersectionTypeEXT(query, true) ==
        gl_RayQueryCommittedIntersectionNoneEXT)
    {
        return maxDistance;
    }
    return rayQueryGetIntersectionTEXT(query, true);
}
#endif
#ifndef SUN_SHADOW_FRAME_SEED
// 兜底默认值：逐帧相位会让阴影终止线的可见性每帧在 0/0.25/0.5… 之间跳，
// RR 在高对比处会 clamp 掉历史 ⇒ 静止时噪带累积。定成 0 后偏移只按 sampleIndex
// 哈希，是固定的几个方向。
// 注意：真正生效的是**消费端**在 include 之前写的定义 ——
// rt_test.rgen:1233 已经 `#define SUN_SHADOW_FRAME_SEED 0`，所以这个兜底对
// 主射线那条路不参与编译（改它不会改变 rt_test.rgen 的产物字节）。
// A/B：-DSUN_SHADOW_FRAME_SEED=cam.frameCount 回到逐帧抖动（需两端一致）。
#define SUN_SHADOW_FRAME_SEED 0
#endif
vec2 sunShadowJitter(int sampleIndex)
{
    uint hash = uint(SUN_SHADOW_FRAME_SEED) * 747796405u +
        uint(sampleIndex) * 2246822519u + 2891336453u;
    hash ^= hash >> 16;
    hash *= 0x7feb352du;
    hash ^= hash >> 15;
    hash *= 0x846ca68bu;
    hash ^= hash >> 16;
    return vec2(float(hash & 0xffffu), float((hash >> 16) & 0xffffu)) /
        65535.0 - 0.5;
}

#ifdef SUN_SHADOW_CONSUMER
float sunShadowVisibilityIn(
    sampler2D shadowMap,
    vec3 position,
    vec3 sunDirection,
    int sampleIndex,
    float extentXY,
    float extentZ,
    float texel,
    float centerQuantum,
    float outsideVisibility,
    float bias,
    out float outsideFade01)
{
    vec3 basisRight;
    vec3 basisUp;
    sunShadowBasis(sunDirection, basisRight, basisUp);
    vec3 local = position - sunShadowCenterFor(extentXY, centerQuantum);
    vec2 uv = vec2(dot(local, basisRight), dot(local, basisUp)) /
        (2.0 * extentXY) + 0.5;
    float outsideDistance = max(
        max(-uv.x, uv.x - 1.0),
        max(-uv.y, uv.y - 1.0)) * (2.0 * extentXY);
#if SUN_SHADOW_Z_FADE
    float depthSigned = dot(local, sunDirection);
    outsideDistance = max(outsideDistance,
        max(-depthSigned - extentZ,
            depthSigned - extentZ));
#endif
    if(outsideDistance > 0.0)
    {
        outsideFade01 = smoothstep(
            0.0, SUN_SHADOW_OUTSIDE_FADE_BLOCKS, outsideDistance);
#ifdef SUN_SHADOW_TREAT_NO_HIT_AS_NO_DATA
        // 2026-11-11：盒外不再"硬判 0"。
        //
        // 上一版直接 return 0，靠 volumetric_fog.glsl 里 128~157 格的
        // shadowCoverage 渐隐遮住硬边。但那个渐隐是**以相机为圆心**的球壳：
        // 抬头看天时它正好在头顶切出一个"盖子"，亮区被截断 ⇒ 观感就是
        // "头顶总有一道体积光直射下来"（用户实测：关体积雾即消失）。
        //
        // 现在改成**夹取边界列**：盒外用最近一列的真实遮挡值（空间上连续），
        // 再按离盒距离平滑淡出到 0，既没有硬边、也不会出现相机相对的盖子。
        const float outsideDepth = extentZ - dot(local, sunDirection);
        const vec2 clampedUv = clamp(uv, vec2(0.0), vec2(1.0));
        const float clampedStored = texture(shadowMap, clampedUv).r;

        outsideFade01 = 0.0;

        if(clampedStored >= 2.0 * extentZ - 1.0)
        {
            // 最近一列也没有几何 ⇒ 无数据 ⇒ 露天取 1、洞内取 0
            return sunShadowNoDataVisibility;
        }

        const float borderVisible =
            outsideDepth < clampedStored + bias ? 1.0 : 0.0;
        const float smearFade = 1.0 - smoothstep(
            SUN_SHADOW_OUTSIDE_FADE_BLOCKS * 2.0,
            SUN_SHADOW_OUTSIDE_FADE_BLOCKS * 8.0,
            outsideDistance);

        return borderVisible * smearFade * sunShadowNoDataVisibility;
#else
        return mix(
            1.0,
            outsideVisibility,
            outsideFade01);
#endif
    }
    outsideFade01 = 0.0;
    float depth = extentZ - dot(local, sunDirection);
    vec2 sampleUv = uv + sunShadowJitter(sampleIndex) * texel;
#ifdef SUN_SHADOW_TREAT_NO_HIT_AS_NO_DATA
    // 2026-11-09：生产者 miss 时写的是 `2 * extentZ`（见 rt_sun_shadow.comp 的
    // sunShadowTraceDepth(origin, -sunDirection, 2 * SUN_SHADOW_EXTENT_Z)）。
    //
    // 在闭合世界里，从盒的朝阳面沿 -sun 走满 2*extentZ 不可能什么都碰不到（地形
    // 从基岩到地表连续），所以 miss 的唯一现实含义是：**这一列没有几何**
    // —— 阴影盒（±160）超出了已加载/已进 TLAS 的地形范围。
    //
    // 以前它被当成"没有遮挡体 ⇒ 受光"（depth < 320 恒成立），于是雾在没有几何
    // 覆盖的地方整片被太阳点亮 —— 就是"太阳体积光穿墙、从地表灌到地下"。
    // 这里改判为"无数据"，交给 outsideFade 走 SUN_SHADOW_OUTSIDE_VISIBILITY（暗）。
    if(texture(shadowMap, sampleUv).r >= 2.0 * extentZ - 1.0)
    {
        // 返回 **0**（完全遮蔽），不要返回 outsideVisibility：
        // 体积雾那条调用给的 outsideVisibility 是
        // SUN_SHADOW_OUTSIDE_VISIBILITY = 0.08，是给"盒外远处"用的软边界；
        // 而"盒内这一列没有几何"没有理由保留 8% 的太阳 —— 在漆黑洞穴里
        // 8% 仍然可见，表现为残留光柱（实测 user：上一版"几乎不穿墙"后还剩一层）。
        //
        // 不清 outsideFade01：sunShadowFogVisibility 会按它做
        // mix(visibility, 0.08, noDataFade)，置 1 就被拉回 0.08。
        // 2026-11-12：不再硬判 0（那会给露天天空切出一个跟随相机的盖子），
        // 改由调用方按"是否高于当地地表"决定（露天 1 / 洞内 0）。
        return sunShadowNoDataVisibility;
    }
#endif
#ifdef SUN_SHADOW_PCF_TAPS_4
#ifdef SUN_SHADOW_TREAT_NO_HIT_AS_NO_DATA
    // 2026-11-13（A3）：体积雾的判据是"4 个 tap 全部受光才算受光"，
    // 所以可以**遇到第一个被挡就返回 0** —— 与原来的 step(3.5, lit) 完全等价，
    // 但平均只需 1.x 次采样（遮挡处通常第 1 个 tap 就命中）。
    for(int tapY = 0; tapY < 2; ++tapY)
    {
        for(int tapX = 0; tapX < 2; ++tapX)
        {
            vec2 tapUv = sampleUv +
                (vec2(float(tapX), float(tapY)) - 0.5) * texel;

            if(depth >= texture(shadowMap, tapUv).r + bias)
            {
                return 0.0;
            }
        }
    }
    return 1.0;
#else
    float lit = 0.0;
    for(int tapY = 0; tapY < 2; ++tapY)
    {
        for(int tapX = 0; tapX < 2; ++tapX)
        {
            vec2 tapUv = sampleUv +
                (vec2(float(tapX), float(tapY)) - 0.5) * texel;
            lit += depth < texture(shadowMap, tapUv).r + bias ? 1.0 : 0.0;
        }
    }
    // 表面着色（rgen，未定义 TREAT 宏）：保持原来的平均语义。
    return lit * 0.25;
#endif
#else
    float stored = texture(shadowMap, sampleUv).r;
    return depth < stored + bias ? 1.0 : 0.0;
#endif
}

#ifndef SUN_SHADOW_NO_NEAR_MAP
#ifdef SUN_SHADOW_FAR_MAP
float sunShadowFarVisibility(vec3 position, vec3 sunDirection, int sampleIndex);
#endif
float sunShadowVisibility(vec3 position, vec3 sunDirection, int sampleIndex)
{
    float outsideFade = 0.0;
    float nearVis = sunShadowVisibilityIn(sunShadowMap, position, sunDirection, sampleIndex,
        SUN_SHADOW_EXTENT_XY, SUN_SHADOW_EXTENT_Z, SUN_SHADOW_TEXEL,
        SUN_SHADOW_CENTER_QUANTUM,
        SUN_SHADOW_OUTSIDE_VISIBILITY, SUN_SHADOW_BIAS_NEAR, outsideFade);
#ifdef SUN_SHADOW_FAR_MAP
    if(outsideFade > 0.0)
    {
        return mix(
            nearVis,
            sunShadowFarVisibility(position, sunDirection, sampleIndex),
            outsideFade);
    }
#endif
    return nearVis;
}
#endif

const float SUN_SHADOW_FAR_OUTSIDE_VISIBILITY = 1.0;

#ifdef SUN_SHADOW_FAR_MAP
float sunShadowFarVisibility(vec3 position, vec3 sunDirection, int sampleIndex)
{
    float ignoredOutsideFade;
    return sunShadowVisibilityIn(sunShadowMapFar, position, sunDirection, sampleIndex,
        cam.sunShadowFarParams.x, cam.sunShadowFarParams.y, cam.sunShadowFarParams.z,
        cam.sunShadowFarParams.z,
        SUN_SHADOW_FAR_OUTSIDE_VISIBILITY, SUN_SHADOW_BIAS, ignoredOutsideFade);
}
#endif

#if defined(SUN_SHADOW_FOG_UNION) && defined(SUN_SHADOW_FAR_MAP) && \
    !defined(SUN_SHADOW_NO_NEAR_MAP)
float sunShadowFogVisibility(vec3 position, vec3 sunDirection, int sampleIndex)
{
    float nearFade = 0.0;
    float visibility = sunShadowVisibilityIn(
        sunShadowMap, position, sunDirection, sampleIndex,
        SUN_SHADOW_EXTENT_XY, SUN_SHADOW_EXTENT_Z, SUN_SHADOW_TEXEL,
        SUN_SHADOW_CENTER_QUANTUM,
        SUN_SHADOW_FAR_OUTSIDE_VISIBILITY,
        SUN_SHADOW_BIAS_NEAR, nearFade);

    float noDataFade = nearFade;
    if(cam.sunShadowFarParams.w > 0.5)
    {
        float farFade = 0.0;
        visibility = min(visibility, sunShadowVisibilityIn(
            sunShadowMapFar, position, sunDirection, sampleIndex,
            cam.sunShadowFarParams.x, cam.sunShadowFarParams.y,
            cam.sunShadowFarParams.z, cam.sunShadowFarParams.z,
            SUN_SHADOW_FAR_OUTSIDE_VISIBILITY,
            SUN_SHADOW_BIAS, farFade));
        noDataFade = min(nearFade, farFade);
    }

    return mix(visibility, SUN_SHADOW_OUTSIDE_VISIBILITY, noDataFade);
}
#endif
#endif
#endif
