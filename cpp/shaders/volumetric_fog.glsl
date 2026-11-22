#ifndef VFOG_SUN_HEIGHTVIS
// 高度场太阳遮挡（见下面 fogSunHeightVisibility 的说明）。
// 默认 0：只有 fog_screen_march.comp 显式打开（它绑定了 binding 7 的高度图）。
#define VFOG_SUN_HEIGHTVIS 0
#endif

#ifndef VFOG_FORCE_SUN
// 对照实验开关：1 = shadow 恒 1（旁路全部可见性），用于区分
// "太阳项是 0" 与 "可见性恒 0"。默认 0。
#define VFOG_FORCE_SUN 0
#endif

#ifndef VOLUMETRIC_FOG_GLSL
#define VOLUMETRIC_FOG_GLSL

// 2026-11-21 统一风场：本文件的两个宿主（fog_screen_march.comp / volumetric_clouds_v3.comp）
// 都在 include 本文件之前声明 cam（camera_ubo.glsl@18/@9）。
#include "wind_common.glsl"

// 2026-11-18 一次性诊断通道（编译期开关，默认 0 = 不编进去）。
// 把"雾的太阳散射项为什么是 0"直接画成颜色，而不是靠猜：
//   R = 太阳散射项长度（rayColor × 16）
//   G = 受光密度占比 litFraction（0 = 全程判阴影，1 = 全程受光）
//   B = 由 fog_screen_march 填 directLightEnergy × 0.5（0 = 太阳能量本身是 0）
#ifndef VFOG_DEBUG_OUT
#define VFOG_DEBUG_OUT 0
#endif
#if VFOG_DEBUG_OUT
vec3 gVfogDebug = vec3(0.0);
#endif

#include "fog_density_common.glsl"

#include "local_light_grid_common.glsl"

#ifdef SUN_SHADOW_FOG_UNION
// ===========================================================================
// 2026-11-15：雾专用的太阳可见性 —— **盒外一律保守判暗**。
//
// 原来的 sunShadowFogVisibility() 把近场/远场的"盒外可视度"都传成
// SUN_SHADOW_FAR_OUTSIDE_VISIBILITY = 1.0（完全受光）。近场盒只有 ±160 格，
// 而雾视距常见 416 格 ⇒ 160~416 格整段判"没有遮挡 ⇒ 全亮"，看起来就是从
// 太阳方位凭空漏下的光柱（用户："体积光依然漏"）。
//
// 这里传 0.0：没有遮挡数据 = 未知 = 保守判暗。牺牲的是远处雾的太阳散射
//（160 格外的雾不再被太阳点亮），换来结构上不可能凭空漏光。
// 放在 volumetric_fog.glsl 里而不是改 sun_shadow.glsl 的常量，是为了不改变
// rt_test.rgen 的 SPV（否则触发 RTPSO 冷编）。
// ===========================================================================
float fogSunShadowVisibility(vec3 position, vec3 sunDirection, int sampleIndex)
{
    float nearFade = 0.0;
    float visibility = sunShadowVisibilityIn(
        sunShadowMap, position, sunDirection, sampleIndex,
        SUN_SHADOW_EXTENT_XY, SUN_SHADOW_EXTENT_Z, SUN_SHADOW_TEXEL,
        SUN_SHADOW_CENTER_QUANTUM,
        0.0,
        SUN_SHADOW_BIAS_NEAR, nearFade);

    float noDataFade = nearFade;
    if(cam.sunShadowFarParams.w > 0.5)
    {
        float farFade = 0.0;
        visibility = min(visibility, sunShadowVisibilityIn(
            sunShadowMapFar, position, sunDirection, sampleIndex,
            cam.sunShadowFarParams.x, cam.sunShadowFarParams.y,
            cam.sunShadowFarParams.z, cam.sunShadowFarParams.z,
            0.0,
            SUN_SHADOW_BIAS, farFade));
        noDataFade = min(nearFade, farFade);
    }

    return mix(visibility, SUN_SHADOW_OUTSIDE_VISIBILITY, noDataFade);
}
#endif

#ifdef VFOG_SUN_RAY
// ===========================================================================
// 2026-11-15（L2）：太阳遮挡真值 —— 每个 march 样本朝太阳打一条短射线。
//
// 为什么换掉原先那套：原来的可见性是四层拼出来的（近场 1024² 阴影图 + 远场图
// + 64×64×32 的 S1 体积 + "该列无数据 ⇒ 露天"启发式），每层都有自己的边界与
// 判据，拼接处必然有缝。实测症状：头顶竖直光柱、地下月光漏进来、遮挡处朝太阳
// 方向凭空一道光束。其中 S1 那条更糟：binding 15 从未注册进 set layout，雾一直
// 在采样"未定义描述符"。
//
// 现在近场直接按几何判定：与阴影生产者同一个 cull mask（0x04 = 地形层），
// 命中即遮挡。没有分辨率/PCF/bias/盒子/无数据这些概念，也就没有缝。
// ===========================================================================
// 2026-11-15：128 → 256。阴影盒只有 ±160 格，射线覆盖到 256 后
// **绝大部分雾样本都走几何真值**，不再依赖阴影图的"无数据 ⇒ 露天"判据
//（那是遮挡处仍然发亮的主要来源）。
const float FOG_SUN_RAY_MAX_DISTANCE = 256.0;
// 2026-11-15 修正：原来照抄了 SUN_SHADOW_CULL_MASK = 0x04，但 WorldGeometry.cpp
// 里 0x04 是**第一人称手部**专用位（地形是 0xff，primaryMaskCull 时 0xfe）。
// 用 0x04 打 ray 时命中集合完全不对 ⇒ 几乎全 miss ⇒ shadow 恒 1 ⇒ "几乎没有遮挡、
// 全漏光"。这里改用 0xFF（与 rt_sun_volume.comp 一致，能命中全部实例）。
const uint FOG_SUN_RAY_CULL_MASK = 0xFFu;

// ===========================================================================
// 2026-11-18（回归修复）：**高度场太阳遮挡** —— 第三条独立可见性来源。
//
// 实测（诊断通道）：阴影图路径与 256 格几何射线路径对每个 march 样本都判
// "被遮挡"（红=0、绿=0）⇒ shadow 恒 0 ⇒ 雾的太阳项恒 0 ⇒ 任何角度都看不到
// 光柱，与密度/相位/抬头压制全部无关。
//
// 所以这里不再依赖那两条链：沿太阳方向在水平面上推进，逐点比较"射线高度"
// 与"该点地形高度"。被地形挡住即 0，否则 1。用的是已经绑在 binding 7 的
// 128² / ±256 格地形高度图（terrainGroundHeight），不经过任何加速结构或
// 阴影纹理，因此不可能被上面两路的问题污染。
// 产出正是"地形遮挡的柱子"：山脊、谷口、悬崖的剪影。
// 代价：每次调用最多 8 次 2D 纹理取样。回退：-DVFOG_SUN_HEIGHTVIS=0。
// ===========================================================================
const float FOG_SUN_HEIGHT_MAX_DISTANCE = 192.0;

#if VFOG_SUN_HEIGHTVIS
float fogSunHeightVisibility(vec3 position, vec3 sunDirection, float maxDistance)
{
    float advance = 4.0;
    for(int k = 0; k < 8; ++k)
    {
        const vec3 probe = position + sunDirection * advance;
        if(probe.y > TERRAIN_HEIGHT_WORLD_TOP)
        {
            break;   // 已高过地形高度范围，不可能再被地形挡住
        }

        float outside = 0.0;
        const float groundHeight = terrainGroundHeight(probe.xz, outside);
        if(outside <= TERRAIN_HEIGHT_OUTSIDE_FADE_BLOCKS &&
            groundHeight > TERRAIN_HEIGHT_EMPTY * 0.5 &&
            probe.y <= groundHeight + 0.35)
        {
            return 0.0;
        }

        advance *= 2.0;
        if(advance > maxDistance)
        {
            break;
        }
    }
    return 1.0;
}
#endif

// 在太阳方向周围的锥内抖动一条射线方向（锥心 = 真实太阳方向）。
vec3 fogSunRayJitter(vec3 base, float h1, float h2, float cone)
{
    const vec3 reference = abs(base.y) > 0.9
        ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    const vec3 tangent = normalize(cross(reference, base));
    const vec3 bitangent = cross(base, tangent);
    const float angle = h1 * 6.2831853;
    const float radius = sqrt(h2) * cone;
    return normalize(base + (tangent * cos(angle) + bitangent * sin(angle)) * radius);
}

// 2026-11-15：每样本 2 条抖动射线（锥角 ≈1.4°）取平均。
// 单条二值射线在 16 步 march 上会留下"羊毛状"颗粒（实测用户：天顶光束像羊毛），
// 抖动把它变成软边，配合体积雾的时域 resolve 收敛。
float fogSunRayVisibility(vec3 origin, vec3 sunDirection, float maxDistance, float seed)
{
    const float cone = 0.024;
    float blocked = 0.0;
    for(int k = 0; k < 2; ++k)
    {
        const float h1 = fract(sin((seed + float(k) * 19.19) * 12.9898) * 43758.5453);
        const float h2 = fract(sin((seed + float(k) * 37.71) * 78.233) * 12345.6789);
        const vec3 direction = fogSunRayJitter(sunDirection, h1, h2, cone);

        rayQueryEXT query;
        // 起点抬 0.05 格：样本可能正好压在地表/方块面上，贴面起射会退化。
        // 2026-11-15：flags 改成 Opaque|TerminateOnFirstHit。
        // 原来带着 SkipClosestHitShaderEXT —— 那是 traceRayEXT 的旗标，inline
        // ray query 里加上它之后命中可能不被 commit，表现为"射线永远 miss ⇒
        // 遮挡处照样受光"。Opaque 保证命中直接作为不透明遮挡。
        rayQueryInitializeEXT(query, fogTlas,
            gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
            FOG_SUN_RAY_CULL_MASK, origin + sunDirection * 0.05, 0.0,
            direction, maxDistance);
        // 循环上限：TLAS 未绑定/失效时 rayQueryProceedEXT 可能一直返回 true
        // ⇒ GPU 侧无限循环 ⇒ TDR ⇒ 驱动 AV（实测 nvoglv64 崩溃）。
        int proceedGuard = 0;
        while(rayQueryProceedEXT(query) && ++proceedGuard < 32) {}

        blocked += (rayQueryGetIntersectionTypeEXT(query, true) !=
            gl_RayQueryCommittedIntersectionNoneEXT) ? 1.0 : 0.0;
    }
    return 1.0 - blocked * 0.5;
}
#endif

// 2026-11-10：6 → 16。
// 太阳光程改成解析积分后，每样本从 6+8=14 次 exp2 降到约 16+1：仍然更便宜，
// 而视线步数从 6 提到 16 直接消掉了"6 步跨 320 格"造成的层状带与放射条纹。
// 2026-11-16：回到 16 步。抬头方向改走解析雾幕（见 evaluateAnalyticFog 里的
// FOG_SKY_ANALYTIC 分支），不再依赖"加密 march 压噪声"，所以步数回到 16 省回
// 那 +50% 的开销。
const float VFOG_QUALITY = 16.0;

// 2026-11-15：0.35 → 0.15。这个地板是"环境光不被太阳直射遮挡完全消灭"的近似，
// 但 0.35 太高：射线判定为遮挡的洞穴/背光处，雾仍按 35% 发亮，被当成"漏光"。
// 0.15 留给天空光的底，同时让遮挡区明显暗下去。
// 2026-11-15：0.15 → 0.03。
// 这个地板是"遮挡处仍按 X% 发光"的量，用户实测"隔着墙也能看到太阳方位的
// 体积光"—— 墙后的雾不该有 15% 的方向性亮度。压到 0.03 后遮挡区基本只剩
// 极弱的天光底，不再形成"透视墙"的白光。
const float FOG_AMBIENT_SHADOW_FLOOR = 0.03;

const int VFOG_SUNLIGHT_STEPS_MAX = 8;

// 2026-11-18：密度标定系数（每格消光 / 单位浓度）。
// 旧式把密度除以 cameraFar（视距），于是视距越大雾越淡、且同一套参数在不同
// 视距下完全不可比 —— baseDensity=1 时只有 2.1e-4/格，100 格光程 τ≈0.02，
// 结构上看不见（用户实测"几乎没有"）。
// 新口径：0.08*0.15 = 0.012/格 ⇒ baseDensity=1 时 100 格光程 τ≈1.2，
// 1/e 距离约 83 格，属「中等雾」。UI 的雾浓度旋钮在此基础上线性缩放。
// 2026-11-18：0.15 → 0.06。用户实测口径："这雾也太浓了"。
// 0.15 时 baseDensity=1 ⇒ 0.012/格、100 格光程 τ≈1.2、1/e≈83 格，属于浓雾；
// 0.06 ⇒ 0.0048/格、τ≈0.48、1/e≈208 格，是"远处起薄、近处仍看得见"的中轻雾。
// UI 的雾浓度滑杆（g_fogBaseDensity）仍在本常量之上线性缩放，要更淡直接拉滑杆。
const float VFOG_DENSITY_SCALE = 0.06;

// =============================================================================
// 2026-11-19（重构）：**把"雾"与"体积光"拆成两种独立介质**。
//
// 旧模型只有一个密度场，它同时决定两件互不相干的事：
//   ① 雾有多厚（挡视线、压暗天空、环境内散射）
//   ② 光柱有多亮（同一密度的单次散射）
// 结果必然是"要柱子就得浓雾、要淡雾就没柱子"，而且雾带从 170 格盖到 -28 格、
// 抬头整条视线都在带内 ⇒ 头顶得到一层与几何无关的亮幕（"糊在屏幕上的光柱"）。
// 用户的判断是对的：这不是参数问题，是模型问题。
//
// 新模型：
//   · 雾（haze）  ：只做消光 + 环境内散射，**不含任何太阳直射项**。
//                   滑条只控制"看得多远/多灰"，不再牵连光柱亮度。
//   · 体积光(shaft)：独立介质，**贴地、短标高**（FOG_SHAFT_SCALE_HEIGHT），
//                   振幅来自独立的强度档，只积分太阳直射。
// 短标高带来一个结构性保证：相机离地 >~3 个标高之后，视线里的光柱介质已经
// 衰减到 0 ⇒ **头顶开阔天空不可能出现任何光柱**（不是靠仰角去压，是那里没介质）。
// =============================================================================
// 2026-11-19：**体积光强度档（UI「太阳散射强度」滑条，0..4）**。
// 默认 0 会让柱子完全消失（用户滑到 0 就真看不见，符合滑条语义），
// 出厂值 1.0（见 Java DEFAULT_VOLUMETRIC_FOG_SETTINGS 的 sunStrength）。
// 2026-11-20（体积光重构）：22 → 40 格。
// 22 格在实测里只覆盖贴地一层，视线一抬就穿出介质 ⇒ 屏幕上"没有体积可积分"
//（用户："画面无变化"）。40 格让柱子在楼宇/树冠高度上仍有余量。
const float FOG_SHAFT_SCALE_HEIGHT = 40.0;
// 2026-11-20：0.030 → 0.055。配合标高加高，单根柱子的散射量约 3 倍。
const float FOG_SHAFT_MEDIUM = 0.055;
const float FOG_SHAFT_GAIN = 1.0;

// 2026-11-19：太阳项按仰角淡出的区间（视线仰角的 y 分量）。
// LO 以下 = 1.0（水平/地形方向，柱子完整）；HI 以上 = 0（天空方向一根直射光都不剩）。
const float FOG_SUN_UP_LO = 0.30;
const float FOG_SUN_UP_HI = 0.62;

// 2026-11-18：抬头解析雾幕与 march 的混合区间（视线仰角）。
// 2026-11-19：起点对齐到 FOG_SUN_UP_HI —— 太阳项在 0.62 已经归零，解析路径从
// 0.62 开始接管，两侧在交界处都"无太阳项"，因此不会出现 y≈0.8 那道圆形台阶。
const float FOG_UP_BLEND_LO = 0.62;
const float FOG_UP_BLEND_HI = 0.86;

// =============================================================================
// 2026-11-18：**把"雾有多厚"与"光柱有多亮"拆开**（用户诉求：浓的体积光 +
// 淡的雾，且不要把天空压暗）。
//
// 问题：消光（density）同时决定"挡不挡视线"和"能散射多少太阳光"，所以调淡雾
// 就必然把光柱一起调没了 —— 之前所有档位都卡在这个耦合里。
// 真实介质里这两者由单次散射反照率联系，艺术上可以 >1；这里就用一个增益
// 直接放大太阳散射项：消光降一半、光柱亮度不变。
//
//   FOG_SHAFT_GAIN —— 只乘**太阳散射项**（rayColor），不乘消光、不乘环境项。
//
// 2026-11-19（重构）：旧模型里的 8× 增益已删除 —— 见文件顶部常量区
// FOG_SHAFT_GAIN / FOG_SHAFT_MEDIUM 的说明（介质分离后不需要放大补偿）。
// 天空早退的仰角阈值（见 evaluateAnalyticFog 里的 skyWeight）：
// 只有 [LO,HI] 之上（接近天顶）才走"均匀薄霾"，地形/水平方向全部进 march。
const float FOG_SKY_DIR_LO = 0.62;
const float FOG_SKY_DIR_HI = 0.86;
//
// FOG_SKY_DENSITY_SCALE —— 视线没有命中几何（看天）时的密度折扣。
// 以及把密度场的等值线（雾带边界/地形锚定的闭合圈）投影成头顶的圆形斑。
// 打折后天空保持亮、头顶的结构性圆斑几乎不可见；地面与水平方向的光柱不受影响。
//
// 2026-11-18（回归修复）：0.30 → 0.75。
// 0.30 是为了"头顶别那么浓"加的，但它把**天空方向的体积光柱**一起削掉 70%
// ——而真正防"无源垂直光柱"的是 256 格几何遮挡射线与逐像素云影，不是密度折扣。
// 保留 25% 的抑制量（天空仍比地面薄），把结构还给太阳项。
// =============================================================================
const float FOG_SKY_DENSITY_SCALE = 0.45;

// 尾部淡出：最后这段占比的 march 距离内密度线性压到 0，替代旧档在 fogEnd 的
// 硬切（VOLUMETRIC_FOG.md 记录过的"浓度平台"取舍）。
const float FOG_TAIL_FADE_FRACTION = 0.18;

struct FogResult
{
    vec3 scattering;
    float transmittance;
    float firstDistance;
    float opticalDepth;
    float meanDistance;
};

// 参考 VolumetricFog.glsl:75-79 的抖动。
// 参考在 TAA 分支用 BlueNoiseTemporal().y，非 TAA 分支用 bayer64()。本工程
// 的 4 个 clouds 变体并不都带 temporalJitter()，所以这里用参考**非 TAA 分支**
// 的固定 4x4 Bayer（bayer64 的左上 4x4 子块）：同一个像素永远拿同一个相位，
// 不会引入时间性闪烁；雾本身没有时域累积，逐帧抖动只会变成噪点。
// 2026-10-23：逐帧低差异相位（自创）。与 volumetric_clouds.comp 的
// temporalPhase() 同一招：黄金比序列在任意连续窗口内近似均匀铺满 [0,1)，
// 于是"沿视线的步进相位"在 3~5 帧内被既有 resolve 收敛掉，而不是靠白噪声。
// 只取低 10 位（同 v1 的理由：fp32 精度，几小时后仍精确）。
//
float fogTemporalPhase(float strength)
{
    if(strength <= 0.0)
    {
        return 0.0;
    }
    return strength * float(cam.frameCount & 0x3ffu) * 0.6180339887498949;
}

// 2026-10-31：4x4 Bayer 换成 Interleaved Gradient Noise（自创，标准做法）。
// 病征是"整片雾面上细密均匀、相机静止时钉死在屏幕上的纱窗"：Bayer 是
// **空间固定**的低分辨率图案（4 个云像素 = 8 个输出像素的周期），而雾是
// 低频均匀场，一旦去噪/时间累积没把它抹平就必然被看成网格。IGN 每个像素
// 的相位都不同、无重复周期，时间相位叠加后只会收敛成均匀值，不会再被识别
// 成图案；同时比 const int[16] 数组的动态索引便宜（部分驱动会把常量数组
// 降到 local memory 再去索引）。
float fogDitherPhase(ivec2 pixel, float temporalPhase)
{
    const float ign = fract(52.9829189 * fract(
        dot(vec2(pixel) + 0.5, vec2(0.06711056, 0.00583715))));
    return fract(ign + temporalPhase);
}

const float FOG_ANCHOR_REFERENCE_Y = 63.0;
const float FOG_ANCHOR_MAX_SHIFT = 128.0;

// 一次高度场取样，同时服务两件事：密度带的 Y 平移、雾浪的贴地剖面。
struct FogGround
{
    float height;
    float weight;
};
FogGround fogGroundAt(vec3 position)
{
    FogGround g;
    g.height = FOG_ANCHOR_REFERENCE_Y;
    g.weight = 0.0;

    float outsideDistance = 0.0;
    const float ground = terrainGroundHeight(position.xz, outsideDistance);
    if(ground < TERRAIN_HEIGHT_EMPTY * 0.5)
    {
        return g;
    }

    g.height = ground;
    g.weight = 1.0 - smoothstep(
        0.0, TERRAIN_HEIGHT_OUTSIDE_FADE_BLOCKS, outsideDistance);
    return g;
}

float fogGroundAnchorOffset(FogGround g, float strength)
{
    const float shift = clamp(
        g.height - FOG_ANCHOR_REFERENCE_Y,
        -FOG_ANCHOR_MAX_SHIFT,
        FOG_ANCHOR_MAX_SHIFT);
    return shift * clamp(strength * g.weight, 0.0, 1.0);
}

// 2026-10-31：雨天地面雾浪（cloudParams4.y/.z/.w + cloudParams9.w 那条链）
// 整段删除。用户实测"跟没有一样"，留着只是每步多 2 次 textureLod 与一次
// 3D 噪声形状计算。相关 UBO 车道保留（std140 布局不动），但已无消费者。
const float FOG_LIGHT_PHASE_SCALE = 0.62;
const float FOG_LIGHT_PHASE_FLOOR = 0.02;
const float FOG_LIGHT_PHASE_G = 0.40;

// 2026-11-09 P4：双叶相位（与 vol_restir_integrate.comp 同口径）。
// 单叶 HG 在灯很近时前向瓣峰会偏窄 ⇒ 光斑"中间一团、边缘一刀切"。
// 混入 25% 后向瓣（g=-0.2）补近场底光。两瓣各自球面积分为 1，混合仍能量守恒。
const float FOG_LIGHT_PHASE_BACKWARD_G = -0.20;
const float FOG_LIGHT_PHASE_BACKWARD_MIX = 0.25;

float fogDualLobePhase(float cosine, float g)
{
    return mix(hgPhase(cosine, g), hgPhase(cosine, FOG_LIGHT_PHASE_BACKWARD_G),
        FOG_LIGHT_PHASE_BACKWARD_MIX);
}

float fogLightPhase(vec3 toLight, vec3 viewDirection)
{
    const float distanceToLight = max(length(toLight), 1.0e-4);
    const float cosine = dot(-toLight / distanceToLight, viewDirection);
    return FOG_LIGHT_PHASE_FLOOR +
        FOG_LIGHT_PHASE_SCALE * fogDualLobePhase(cosine, FOG_LIGHT_PHASE_G);
}

// 2026-11-09 P4：多次散射的一阶近似（Hillaire 2020 两八度简化版）。
//
// 单次散射在厚雾里按 exp2(-od) 掉得很快，光柱一进深处就"死黑"。补一档衰减更慢
// 的能量：薄雾时 primary≈1 ⇒ 本项≈0（行为完全不变）；厚雾时最多补 a=0.40。
// 代价 2 次 exp2，相对整个 march 可忽略。
const float FOG_MULTI_SCATTER_STRENGTH = 0.40;
const float FOG_MULTI_SCATTER_TAU_SCALE = 0.25;

float fogMultiScatterBoost(float opticalDepth)
{
    const float primary = exp2(-opticalDepth * VFOG_SUNLIGHT_ABSORPTION);
    const float secondary = exp2(
        -opticalDepth * VFOG_SUNLIGHT_ABSORPTION * FOG_MULTI_SCATTER_TAU_SCALE);
    return FOG_MULTI_SCATTER_STRENGTH * secondary * (1.0 - primary);
}

// ===========================================================================
// 2026-11-10 新增：高度带雾沿某方向的光学厚度**解析积分**（替代太阳光步 march）。
//
// fogDensityAt 的形状（不含时间因子）：
//   density(y) = exp2(-a * dh(y)) * (VFOG_DENSITY * 0.15) + baseDensity
//   dh(y)      = max(y - heightMax, 0) + max(heightMin - y, 0)
//   a          = (2 / VFOG_FALLOFF) * (1 - wetness * 0.5)
//
// 沿 y(s) = y0 + sunY * s 对 s ∈ [0, distance] 积分：
//   · 常数项 baseDensity * distance 直接可积；
//   · 指数项按 y 穿越 heightMax / heightMin 切成至多 3 段，每段内 dh 线性
//     ⇒ ∫exp2(-a*dh) ds 有闭式解 ((exp2(-a*dhA) - exp2(-a*dhB)) / (a*slope*ln2))。
//
// 量纲与原来的 lightingDensity 一致（密度 × 距离，**不含** fogTimeFactor，
// 由调用方乘）。
// ===========================================================================
// ===========================================================================
// 2026-11-10 新增：平流雾（rolling advection ground fog）的体密度。
//
// 旧的贴地薄雾只有"常数密度 × 高度指数 × 距离淡出"，结果是一层均匀灰膜：
// 没有形态、不会动，糊在山谷里像一块玻璃。本次按开源做法重写：
//   · 形态：两层 3D 噪声叠加（粗 64 格 = 雾团、细 16 格 = 边缘细节），
//     smoothstep 阈值化成成团的白雾，而不是整片均匀密度；
//   · 滚动：采样点随天气风平移（与云同一个风向常量），细层速度是粗层的
//     1.6 倍 ⇒ 层间错动，看起来是"雾在地面上翻卷/滚动"；
//   · 高度：exp2(-离地高度 / 折叠高度)，贴地但不糊住天空；
//   · 距离：rollingRange 近场淡出，避免远处出现悬空的一层。
// 参考：MirzaBeig/GPU-Fog-Particles（Unlicense，多层噪声按高度衰减）
//       + phlippos/unity-urp-volumetric-fog（froxel 体积里叠加密度项的结构）。
// 返回"每格消光"，单位与 fogDensityAt 一致，直接加进主 march 的 density。
// ===========================================================================
float rollingFogDensity(
    vec3 fogPos,
    float groundHeight,
    float heightScale,
    float strength,
    float range,
    float time,
    float speed,
    vec3 cameraPosition)
{
    if(strength <= 0.0)
    {
        return 0.0;
    }

    const float rangeFade =
        1.0 - smoothstep(range * 0.4, range, length(fogPos - cameraPosition));
    if(rangeFade <= 0.0)
    {
        return 0.0;
    }

    const float heightAboveGround = max(fogPos.y - groundHeight, 0.0);
    // 2026-11-13：下限 0.2 → 1.0。0.2~0.5 格的层在 16 步 march 下采不到
    //（第 5 步跨度已 18 格），只有掠射视线勉强可见。1.0 格仍是很薄的贴地雾。
    const float safeHeightScale = max(heightScale, 1.0);
    const float heightFade = exp2(-heightAboveGround / safeHeightScale);
    if(heightFade <= 1.0e-4)
    {
        return 0.0;
    }

    // 风推移。2026-11-21 统一风场：方向改读 cam.windParams.x（windDirXZ），
    // 无效回落 HP 云原方向；速度仍由调用方 speed 决定（贴地雾更"重"，约云的 1/3）。
    const vec2 wind = windDirXZ() * (max(speed, 0.0) * time);

    // 2026-11-12：竖向坐标随层厚缩放。原来固定除以 26 / 10 格，层厚压到半格后
    // 竖向几乎恒定 ⇒ 雾团会变成"一柱到底"。现在按层厚归一，薄层内部仍有起伏。
    const float verticalUv = heightAboveGround / max(heightScale, 1.0) * 0.25;

    // 2026-11-13：尺度 64/16 → 28/9 格。64 格在玩家视角下只有三四个起伏，
    // 边界又平又直（用户："方形"）；28 格才看得出"一团一团"。
    // 同时加一层 curl 涡流位移：原来只有整体平移，像一块板在滑；
    // 真实贴地雾是翻滚的，位移场用云 pass 已经在用的 curl 噪声（binding 2）。
    // 2026-11-13 修正：curl 噪声是 R16G16B16A16_SFLOAT，值域**本来就是 [-1,1]**，
    // 不能再做 `*2-1`（那会映射成 [-3,1]，位移被系统性拉向一侧）。
    // 标准做法（curl noise advection）就是直接把向量场当偏移量用。
    const vec2 curlSample = textureLod(cloudCurlNoise,
        vec3((fogPos.xz + wind * 0.6) / 48.0, 0.35), 0.0).xy;
    // 2026-11-13：涡流强度接到 UI（fogOpticsParams.x，0 = 只整体平移，2 = 剧烈翻滚）
    const vec2 curlOffset = curlSample *
        (3.5 * clamp(cam.fogOpticsParams.x, 0.0, 2.0));

    const vec3 coarsePos = vec3((fogPos.xz + wind + curlOffset) / 28.0, verticalUv);
    const vec3 finePos = vec3((fogPos.xz + wind * 1.6 + curlOffset * 1.4) / 9.0,
        verticalUv * 1.7);

    const float coarse = textureLod(cloudShapeNoise, coarsePos, 0.0).r;
    const float fine = textureLod(cloudShapeNoise, finePos, 0.0).r;
    const float shape = clamp(coarse * 0.75 + fine * 0.45, 0.0, 1.0);
    // 2026-11-13：去掉 0.25 保底，阈值收紧到 0.42~0.78。
    // 那个保底是之前为了"打开了不至于看不见"加的，但它让整片地面永远有一层底雾，
    // 噪声只在 75% 幅度上起伏 ⇒ 观感就是一整坨（用户："又黑又方，不该是一团一团吗"）。
    // 现在雾团之间是真正的空白，密度靠 GUI 浓度控制（上限已从 4.0 收到 2.0）。
    const float clumping = smoothstep(0.42, 0.78, shape);

    return strength * clumping * heightFade * rangeFade;
}

// ===========================================================================
// 2026-11-11 新增：云隙光（crepuscular rays）的逐点云层透射率。
//
// 上一版把"云遮挡太阳"做成了一个**全局标量**（按覆盖率解析求）⇒ 雨天整片雾
// 一起变暗，看不到任何从云缝里漏下来的光柱。云隙光的本质是**空间变化**的遮挡：
// 云层像一块有洞的遮光板，光只能从洞里下来。
//
// 做法（与 photon 的 draw_crepuscular_rays 同一思路：每个 march 步查云影）：
//   · 把云层当成一层遮光板，沿太阳方向把采样点投影到云层中面；
//   · 用两层 3D 噪声在**玩家尺度**（620 / 210 格）生成"隙/云"图案 —— 天气图是
//     96k 格尺度的宏观图，在视野内几乎常数，产不出云隙，所以不能用它；
//   · 图案随风平移（与云同一风向、约 0.35 倍速度），使光柱跟着云一起动；
//   · 覆盖率越高，阈值越低（云越多洞越小），再按光程算 τ ⇒ exp(-τ)。
// 只有 2 次 2D 纹理取样，且只在"雾在云层下方 + 太阳在地平线上"时执行。
// ===========================================================================
float cloudGapTransmittance(vec3 fogPos, vec3 sunDirection, vec4 cloudLayer, float time)
{
    // 2026-11-13：**暂时停用云对太阳体积光的遮挡**。
    //
    // 用户口径："我只要地形遮挡的体积光，先不管云"。之前云影图是以乘性因子
    // 作用在太阳项上，厚云时会把整片太阳散射按到 0，白天几乎看不到体积光。
    // 这里直接返回 1.0（不遮挡），需要恢复时把下一行删掉即可 —— 下面的实现
    // 与 UI 旋钮都保留。
    return 1.0;

    if(cloudLayer.z <= 0.002 || sunDirection.y <= 0.02)
    {
        return 1.0;
    }

    // UI 三件套（cloudParams4.y/.z/.w，2026-11-11）：
    //   y = 开关，z = 云层光学厚度倍率（强度），w = 缝宽偏置
    const float gapStrength = max(cam.cloudParams4.z, 0.0);
    if(cam.cloudParams4.y < 0.5 || gapStrength <= 0.0)
    {
        return 1.0;
    }
    const float gapWidth = clamp(cam.cloudParams4.w, 0.0, 1.0);

    // 云层中面；从采样点沿太阳走到该高度，落到哪一点就取哪里的云隙图案
    const float midHeight = mix(cloudLayer.x, cloudLayer.y, 0.5);
    const float toLayer = (midHeight - fogPos.y) / max(sunDirection.y, 0.05);
    if(toLayer <= 0.0)
    {
        return 1.0;   // 采样点已在云层之上
    }

    // 2026-11-12：改为查**云影图**，不再用自造噪声。
    //
    // 之前用 620/210 格的两层噪声充当"云隙"，光柱位置与真正的云隙无关
    //（用户实测："云隙光也不和云隙对齐"）。云影图由 rt_cloud_shadow.comp 用
    // 主云 pass 同一个密度函数烘出来，按世界 XZ 一一对应 ⇒ 天然对齐。
    //
    // 纹理覆盖以相机为中心 ±512 格，与 rt_cloud_shadow.comp 的
    // CLOUD_SHADOW_EXTENT 必须一致。
#ifndef VFOG_CLOUD_SHADOW_MAP
    // v3 旧档没有这条 binding，保持"不遮挡"的旧行为。
    return 1.0;
#else
    const vec3 shadowCameraPos = cam.viewInverse[3].xyz;
    const vec2 shadowUv = (fogPos.xz - shadowCameraPos.xz) / (2.0 * 512.0) + 0.5;

    // 出界时钳到边缘（贴边的云隙延续），避免出现一圈空白。
    float transmittance = texture(cloudShadowMap,
        clamp(shadowUv, vec2(0.0), vec2(1.0))).r;

    // UI 两根旋钮在这里复用它原本的语义：
    //   强度 = 云层光学厚度的倍率 ⇒ 对透射率取幂（越大云越黑、光柱越亮）
    //   缝宽 = 抬升透射率的下限（越大洞越大、光柱越粗）
    transmittance = pow(clamp(transmittance, 0.0, 1.0), max(gapStrength, 0.001));
    transmittance = mix(transmittance, 1.0, clamp(gapWidth, 0.0, 1.0) * 0.35);

    return transmittance;
#endif
}

float fogSunOpticalDepth(
    float y0,
    float sunY,
    float distance,
    float bandA,
    float baseDensity,
    float heightMax,
    float heightMin)
{
    const float bandPeakDensity = VFOG_DENSITY * 0.15;
    float integral = max(baseDensity, 0.0) * max(distance, 0.0);

    if(distance <= 0.0 || bandPeakDensity <= 0.0)
    {
        return integral;
    }

    if(abs(sunY) < 1.0e-4)
    {
        const float dh = max(y0 - heightMax, 0.0) + max(heightMin - y0, 0.0);
        return integral + bandPeakDensity * exp2(-bandA * dh) * distance;
    }

    float breaks[4];
    breaks[0] = 0.0;
    breaks[1] = clamp((heightMax - y0) / sunY, 0.0, distance);
    breaks[2] = clamp((heightMin - y0) / sunY, 0.0, distance);
    breaks[3] = distance;

    // 4 元素插入排序（固定下标，避免动态索引把常量数组降到 local memory）。
    for(int i = 0; i < 3; ++i)
    {
        for(int j = 0; j < 3 - i; ++j)
        {
            if(breaks[j + 1] < breaks[j])
            {
                const float tmp = breaks[j];
                breaks[j] = breaks[j + 1];
                breaks[j + 1] = tmp;
            }
        }
    }

    const float lnTwo = 0.6931471805599453;

    for(int i = 0; i < 3; ++i)
    {
        const float s0 = breaks[i];
        const float s1 = breaks[i + 1];
        const float ds = s1 - s0;

        if(ds <= 1.0e-5)
        {
            continue;
        }

        const float yA = y0 + sunY * s0;
        const float yB = y0 + sunY * s1;
        const float dhA = max(yA - heightMax, 0.0) + max(heightMin - yA, 0.0);
        const float dhB = max(yB - heightMax, 0.0) + max(heightMin - yB, 0.0);
        const float slope = (dhB - dhA) / ds;

        if(abs(slope) < 1.0e-6)
        {
            integral += bandPeakDensity * exp2(-bandA * dhA) * ds;
        }
        else
        {
            const float decay = bandA * slope * lnTwo;
            integral += bandPeakDensity *
                (exp2(-bandA * dhA) - exp2(-bandA * dhB)) / decay;
        }
    }

    return integral;
}

// 主入口。前 17 个参数与 2026-10-19 B 方案一致；末尾三个是 2026-10-23 追加的
// 自创项（地面锚定 / 时域抖动 / 动态光源），全部"0 = 逐位旧档"。
// ===========================================================================
// 2026-10-23 修正：环境项**不能**取"视线方向的晴空辐亮度"
//
// 症状（用户报告）："有时候可以隔墙看到太阳所在位置的白光"。
// 根因：clearSkyInscatter() 里含 Mie 前向瓣，正对太阳时比天顶亮一到两个数量级。
// 我先前直接把它当环境入散射用，于是：
//   1) 雾变成有方向的 —— 朝太阳看时整层雾发白，白斑钉在太阳的屏幕位置上；
//   2) 环境项只吃 sunShadow 的"环境压暗地板"（FOG_AMBIENT_SHADOW_FLOOR = 0.35），
//      所以躲在墙后 / 洞里照样按 35% 发光 ⇒ "隔墙可见"。
// 物理上环境项是**半球积分**，不能用单方向辐亮度。这里改成对三个固定方向
// （天顶 / 太阳侧水平 0.2 仰角 / 反太阳侧水平 0.2 仰角）取 HG 加权平均 ——
// 与 v2/v4 的天光（D6）同一套口径，**不包含指向太阳的那一瓣**，
// 因此不会在太阳位置形成亮斑；同时保留"日出日落雾自己泛色"的效果。
// ===========================================================================
vec3 fogSkyAmbient(
    vec3 viewDirection,
    float sunElevation,
    vec3 lightDirection,
    vec3 directLightColor,
    float radianceScale,
    float weatherFactor)
{
    const vec3 sunSideDir = normalize(vec3(lightDirection.x, 0.20, lightDirection.z));
    const vec3 antiSunDir = normalize(vec3(-lightDirection.x, 0.20, -lightDirection.z));
    const vec3 radianceZenith = clearSkyInscatter(
        vec3(0.0, 1.0, 0.0), sunElevation, lightDirection,
        directLightColor, radianceScale, weatherFactor, vec3(1.0));
    const vec3 radianceSunSide = clearSkyInscatter(
        sunSideDir, sunElevation, lightDirection,
        directLightColor, radianceScale, weatherFactor, vec3(1.0));
    const vec3 radianceAntiSun = clearSkyInscatter(
        antiSunDir, sunElevation, lightDirection,
        directLightColor, radianceScale, weatherFactor, vec3(1.0));
    const float weightZenith = hgPhase(dot(viewDirection, vec3(0.0, 1.0, 0.0)), 0.25);
    const float weightSunSide = hgPhase(dot(viewDirection, sunSideDir), 0.25);
    const float weightAntiSun = hgPhase(dot(viewDirection, antiSunDir), 0.25);
    return (radianceZenith * weightZenith +
        radianceSunSide * weightSunSide +
        radianceAntiSun * weightAntiSun) /
        max(weightZenith + weightSunSide + weightAntiSun, 1.0e-4);
}
// =============================================================================
// 2026-11-16 雾的时段倍率（**单一真源**，纯参数版：不引用 cam 的新字段）
//
// ⚠ 当前生效的雾是 volumetric_clouds_v5.comp 的内嵌 march
//   （Clouds.cpp:3809 kEnableFogFroxel = false），它走本文件的 evaluateAnalyticFog
//   —— 所以时段项必须在这里生效，加在 fog_froxel_density.comp 里是无效的。
//   fog_froxel_density.comp 调用同一个函数，两者天然同口径。
//
// 辐射雾在日出前后最浓、太阳升高后蒸发；入夜后地面辐射仍在，但比清晨薄。
//   dawnBoostPeak = 晨雾峰值倍率（UI 车道 cam.fogMoodParams.y，默认 1.75）
//   sunHeight     = 太阳仰角（不是月亮，也不是光源方向）
// =============================================================================
float fogTimeOfDayScale(float sunHeight, float dawnBoostPeak)
{
    const float dawnDuskMist = smoothstep(-0.10, 0.02, sunHeight) *
        (1.0 - smoothstep(0.14, 0.55, sunHeight));
    const float nightMist = smoothstep(0.05, -0.30, sunHeight);
    return 1.0 + max(dawnBoostPeak - 1.0, 0.0) * dawnDuskMist +
        0.15 * nightMist;
}

// 函数名 evaluateAnalyticFog 是旧名，当前内部已是参考的 march，不是解析闭式雾。
// fogDensityAt() 来自共享头 fog_density_common.glsl。
FogResult evaluateAnalyticFog(
    vec3 worldOrigin,
    vec3 worldDirection,
    float sceneDistance,
    vec3 sunRadiance,
    vec3 sunDirection,
    vec3 ambientRadiance,
    float rain,
    float enabled,
    float baseDensityParam,
    float heightA,
    float heightB,
    float anisotropyG,
    float sunStrength,
    float ambientStrength,
    float maxDistance,
    float startDistance,
    float sunStepsRaw,
    float rainBoost,
    float groundAnchor,
    float temporalJitter,
    float dynamicLightStrength,
    vec4 cloudLayer,
    // 2026-11-12：当前像素的屏幕 uv，用于查"太阳可见性体积"（相机视锥对齐）。
    vec2 screenUv)
{
    FogResult result;
    result.scattering = vec3(0.0);
    result.transmittance = 1.0;
    result.firstDistance = 0.0;
    result.opticalDepth = 0.0;
    result.meanDistance = 0.0;

    if(enabled < 0.5)
    {
        return result;
    }

    const float wetness = clamp(rain, 0.0, 1.0);

    // 2026-11-13：湿度的**时间包络**。
    // 直接用瞬时 rain 会让"雾顶硬边"和"平流雾出现/消失"在雨停那一帧跳变。
    // C++ 侧每帧做一次 EMA（约 3 秒时间常数），写在 godRayParams0.z
    //（这条车道原本恒写 0，是空闲的）。为 0 时退回瞬时值。
    const float wetnessRaw = clamp(cam.godRayParams0.z, 0.0, 1.0);
    const float wetnessEnv = wetnessRaw > 0.0 ? wetnessRaw : wetness;

    // 2026-11-16 时段倍率（清晨最浓、正午蒸发）。太阳高度从相位直接算，
    // 不用 sunDirection()：本文件被多个 TU include，避免引入 include 顺序依赖。
    const float fogSunHeight = 0.81915204429 *
        sin(fract(cam.historyParams.w) * 6.28318530718);
    // 2026-11-18：密度标定改为**物理口径**（每格消光），不再除以视距。
    //
    // 旧式：baseDensity * (wetness*0.15+0.08) / max(cam.cameraParams.y, 100)
    // cam.cameraParams.y 是 cameraFar（视距，见 OrionInternal.h:416-419 的
    // CameraUBO 字段顺序：cameraNear/Far/Fov/Padding），于是视距 376 时
    // baseDensity=1 只给出 2.1e-4/格 —— 100 格光程消光 0.02、透射率 0.98，
    // 结构上不可能看到雾，更不可能看到光柱（用户实测："几乎没有"）。
    // 而且视距一改密度就变，同一套雾参数在不同视距下完全不同，无法调参。
    //
    // 新式：baseDensity=1.0 ⇒ 0.08*0.15 = 0.012/格 ⇒ 100 格光程 τ≈1.2。
    // 这是「中等雾」的量级（1/e 约 83 格）；UI 的雾浓度旋钮在此基础上线性缩放。
    // 2026-11-19（密度基准重标定）：(wetness*0.15 + 0.08) → (wetness*0.06 + 0.035)。
    //
    // 用户反馈："把密度基准滑条搞正常一点，现在雾随随便便就很浓"。
    // 旧标定下雾浓度滑条 1.0 就已经是 0.0048/格（100 格 τ≈0.48），滑到 1.5~2
    // 直接进"糊屏"区间，可用范围全挤在滑条最底部。新标定把整条曲线压到约 0.6 倍，
    // 让 1.0 落在"能看出来的薄雾"、3.0 才接近浓雾，滑条才有可用的中段。
    const float baseDensity =
        max(baseDensityParam, 0.0) *
        (wetness * 0.06 + 0.035) *
        VFOG_DENSITY_SCALE *
        fogTimeOfDayScale(fogSunHeight, cam.fogMoodParams.y);

    const float fogTimeFactor =
        mix(1.0, max(rainBoost, 0.0), wetness);

    // ===========================================================================
    // 2026-11-04 贴地薄雾重构（godRayParams1 四车道复活）
    //
    // 病征（用户）："雨天的薄雾现在也有问题"。根因：10-31 删雾浪时把 shader 消费者
    // 整段删除，而 C++ populateGroundMistParams() 一直在写 godRayParams[4..7] ——
    // 四个原子量永远停在出厂值 (enabled/height=1.0/extinction=0.15/distance=48.0)，
    // 等于"开着但从未生效"。
    //
    // 新口径：在主 march 密度场上叠加一个"距地面高度"的指数衰减项，与雾共享同一套
    // 步进 / 太阳自阴影 / 相位 / 环境项 / 本地光衰减（不需要第二次 march；火把光
    // 穿薄雾时也按积分衰减）。地面高度取 fogGroundAt（出地形范围回落到海平面 63，
    // 与 FOG_ANCHOR_REFERENCE_Y 一致 ⇒ 海面上薄雾正好贴着水面）。
    //   单位 = 每格消光。满雨带内雾 ≈3.9e-3/格；出厂 extinction 0.15 × 0.05 =
    //   7.5e-3/格，落在旧雾浪峰值 FOG_WISP_EXTINCTION(0.0090) 附近。
    //   height 1.0 × 6 = 6 格 e 折叠高度；distance 48 = 距相机开始淡出的半径（格）。
    //   触发 = max(湿ness, 拂晓曲线 × 0.75)：雨天全开；晴天清晨/黄昏也有一层
    //   （太阳仰角 [-0.12, 0.30] 的钟形，夜里与正午都是 0）。
    // ===========================================================================
    // 2026-11-10 重写：贴地薄雾 → 平流雾（rolling advection ground fog）。
    // 上面 11-04 那段"常数密度 + 高度衰减"的实现已被本次替换（它没有形态、
    // 不会动，用户评价"很糟糕"）。四车道含义保持不变，C++ 侧无需改：
    //   x = 开关, y = 折叠高度(×6 格), z = 消光(×0.05/格), w = 近场淡出半径
    const vec4 mistLane = cam.godRayParams1;
    const float rollingGate = step(0.5, mistLane.x);
    // 2026-11-12：层厚标定 6.0 → 0.5。
    // 用户要求的是"很矮的一层，一格或半格高"，原来 1.0 × 6 = 6 格 e 折叠高度
    // 是山谷晨雾的量级，不是贴地雾。现在滑条 0.25~8 映射到 0.125~4 格，
    // 默认 1.0 ⇒ **0.5 格**，即半个方块的厚度。
    // 2026-11-13：0.5 → 1.5。0.5 格那版在 16 步 march 下根本采不到
    //（第 5 步跨度就超过 18 格，远大于层厚），只有掠射视线勉强可见。
    // 1.5 格 e 折叠仍是一条很薄的贴地雾，但落在采样分辨率内。
    // 滑条 0.25~8 ⇒ 0.375~12 格。
    const float rollingHeightScale = max(mistLane.y, 0.1) * 1.5;
    // 2026-11-11：0.05 → 0.15。原标定下 extinction=0.15 只有 0.0075/格，
    // 走 20 格 τ≈0.15，肉眼几乎看不见；新标定下 τ≈0.45，才是一层能看出来的雾。
    // 2026-11-12：层厚从 6 格压到 0.5 格后，穿过薄层的光程也少了 12 倍，
    // 必须同比提高消光才看得见。0.05 → 1.50（滑条 0~1 ⇒ 每格 0~1.5 消光，
    // 薄层在掠射视线下的 τ 才够）。
    // 2026-11-19：1.50 → 0.60（同一次重标定）。1.50 是"0.5 格薄层"时代的补偿值，
    // 层厚已是 1.5 格后它显得过冲：滑条 0.8 就给到 1.2/格，掠射视线下直接糊屏。
    const float rollingExtinction = max(mistLane.z, 0.0) * 0.60;
    const float rollingRange = max(mistLane.w, 8.0);
    const float rollingDawnCurve =
        smoothstep(-0.12, -0.02, sunDirection.y) *
        (1.0 - smoothstep(0.05, 0.30, sunDirection.y));
    // 2026-11-11 修正：原来 `max(wetness, dawn*0.75)` 在晴天正午恒为 0，
    // 于是"打开了却完全看不见"（用户实测）。改成以开关为准、天气为调制：
    //   晴 60% 基准 → 雨天 100%，拂晓/黄昏再抬到 90% 以上。
    // 2026-11-12：只在**雨天**出现（用户明确要求）。
    // 之前为了"看得见"加了 60% 晴天基线 + 拂晓曲线，结果晴天也有，
    // 与"雨天贴地滚动"的设计意图不符。现在完全由湿度驱动：
    // 雨强 0.02~0.35 之间渐入，雨停后自然消失。
    const float rollingWeatherDrive = smoothstep(0.02, 0.35, wetnessEnv);
    const float rollingStrength =
        rollingGate * rollingExtinction * rollingWeatherDrive;
    // 与云共用同一个时间轴（v5 里 cfg.time = uintBitsToFloat(cam.padding)）。
    const float rollingTime = max(uintBitsToFloat(cam.padding), 0.0);
    // 2026-11-10：滚动速度（格/秒）由 UI 下发到 godRayParams2.z；
    // <=0（旧版 UBO 或未设置）回落到 1.35。
    const float rollingSpeed = cam.godRayParams2.z > 0.0
        ? cam.godRayParams2.z
        : 1.35;

    if(fogTimeFactor <= 0.01)
    {
        return result;
    }

    const float heightMax = max(heightA, heightB);
    const float heightMin = min(heightA, heightB);

    const float fogFar = min(max(sceneDistance, 0.0), max(maxDistance, 1.0));
    const float fogNear = clamp(startDistance, 0.0, fogFar);

    const float fogTailFadeStart = fogFar * (1.0 - FOG_TAIL_FADE_FRACTION);

    result.firstDistance = fogNear;

    if(fogFar - fogNear <= 0.01)
    {
        return result;
    }

    // =====================================================================
    // 2026-11-18：**天空早退 —— 只吃一层与方向无关的均匀薄霾**。
    //
    // 症状：相机飞得低（正好在雾带顶 y=98 下方）时，抬头会看到一块"圆形区域"，
    // 飞高了就没有。原因不是几何，而是雾在天空方向的**径向结构**：视线穿出雾带
    // 的路径长度 = (带顶 - 相机Y)/仰角正弦，随仰角连续变化；这些等值线在透视
    // 投影下就是一圈圈以天顶为中心的圆 ⇒ 头顶被看成圆形的明暗台阶。
    //
    // 天空方向没有"被雾挡住的东西"，那里的雾只该是一层薄霾。所以直接给常数：
    // 同一 τ、同一散射，与仰角/路径长度无关 ⇒ 结构上不可能出现同心圆。
    // 顺带把天空像素的 16 步 march 全部省掉（原来是纯浪费）。
    // =====================================================================
    // 2026-11-19（根因修复）：天空判据**改由仰角决定**，不再看 sceneDistance。
    //
    // 原判据是 smoothstep(0.85,0.99, sceneDistance/fogFar)。但 sceneDistance 在
    // 这个雾 pass 里长期是坏的（深度图读出 0），于是**整屏**被判成天空、全部走
    // 下面这条"均匀薄霾"早退 ⇒ march 一次都不执行 ⇒ 体积光柱结构上不可能出现。
    //
    // 2026-11-19（第二次修复）：**删掉硬 return**。
    // 只把 skyWeight 抬进 upBlend，由下面那条软过渡去混：
    //   · 硬 return 会在 y=FOG_SKY_DIR_HI 处与 march 结果直接跳变 ⇒ 圆洞复现；
    //   · 天空方向的解析结果只含环境项（没有太阳项）⇒ 结构上不可能再出现
    //     "从头顶垂直射下来"的假光柱。
    // 于是同一个机制同时解决"圆洞"和"头顶无源光柱"，且不再牺牲 march。
    const float skyWeight =
        smoothstep(FOG_SKY_DIR_LO, FOG_SKY_DIR_HI, worldDirection.y);

    // =====================================================================
    // 2026-11-16：抬头方向（worldDirection.y > 0.8）走**解析雾幕**，不 march。
    //
    // 三条硬要求：不漏光 / 保性能 / 头顶一点光柱都不能有。
    // 头顶那片雾的太阳项已按仰角压到 2%（见 sunUpAtten），剩下的只是"均匀的
    // 环境亮雾"；而 16 步 march 在 198 格厚雾里步长 20+ 格，逐像素抖动落点不同，
    // 就在这片亮雾上叠出细密竖条（用户："头顶还有残留"）。
    //
    // 解析版：只采一次中线密度，按视线到雾顶的光程算透射率，散射全给环境项
    //（无方向性 ⇒ 结构上不可能有光柱），并且**跳过 march** ⇒ 性能反而更好。
    // =====================================================================
    // =====================================================================
    // 2026-11-18：**硬切换 → 软过渡**（头顶"圆形空洞"的根因）。
    //
    // 症状：抬头看天空时，某一块圆形区域里雾的浓淡与亮度明显不同
    //（用户："头顶有时候有圆形空洞"）。根因不是密度，而是下面这道门槛：
    // `y > 0.8` 走解析雾幕、其余走 16 步 march，两条路径的亮度与透射率本来就
    // 不相等；而"仰角 0.8"在透视投影下是天空里的一条**圆锥截线 —— 一个圆**。
    // 于是分界被看成圆形的明暗台阶（同处 2026-11-16 的注释记的是同一现象）。
    //
    // 改法：解析结果先算出来但不立即 return，按仰角在 [LO, HI] 与 march 结果
    // 混合；只有 y ≥ HI 才走纯解析快路径（保持"头顶不 march"的性能收益）。
    // =====================================================================
    const float upBlendRaw =
        max(smoothstep(FOG_UP_BLEND_LO, FOG_UP_BLEND_HI, worldDirection.y),
            smoothstep(FOG_SKY_DIR_LO, FOG_SKY_DIR_HI, worldDirection.y));
    float upBlend = upBlendRaw;
    FogResult upResult;
    upResult.scattering = vec3(0.0);
    upResult.transmittance = 1.0;
    upResult.opticalDepth = 0.0;
    upResult.meanDistance = fogNear;
    upResult.firstDistance = fogNear;

    if(upBlendRaw > 0.0)
    {
        const float topY = max(heightMax, heightMin);
        const float camY = worldOrigin.y;
        const float dirY = max(worldDirection.y, 1.0e-3);
        const float a = (2.0 / VFOG_FALLOFF) * (1.0 - wetness * 0.5);

        // 中点采一次地表（innerBase 沿视线变化缓慢，用中点代表整条视线）。
        const float upPathToTop = (topY - camY) / dirY;
        const vec3 midPoint =
            worldOrigin + worldDirection * (fogNear + upPathToTop * 0.5);

        FogGround upGround;
        upGround.height = FOG_ANCHOR_REFERENCE_Y;
        upGround.weight = 0.0;
        if(groundAnchor > 0.0 || rollingStrength > 0.0)
        {
            upGround = fogGroundAt(midPoint);
        }
        const float upOffset = fogGroundAnchorOffset(upGround, groundAnchor);
        const float upInnerBase =
            ((upGround.weight > 0.0) ? upGround.height : FOG_ANCHOR_REFERENCE_Y)
            - upOffset;

        // 带内剖面（上疏下密）的**闭式积分**：
        //   density(y) = exp2(-(y-ib)*a)*C + base
        //   ∫[yLo,topY] = (exp2(-(yLo-ib)*a) - exp2(-(topY-ib)*a)) / (a·ln2)
        //                 + base·(topY-yLo)
        // 相机在带底之下时从带底起算；在雾顶之上时 span=0 ⇒ 无雾（正确）。
        // 之前用"中线单点密度 × 路程"，剖面让高处密度趋近 0 ⇒ 整段雾被算没了，
        // 表现为"头顶一大块圆形无雾空气"。
        const float yLo = max(camY, upInnerBase);
        const float span = max(topY - yLo, 0.0);
        const float vertical = span > 0.001
            ? (exp2(-(yLo - upInnerBase) * a) -
               exp2(-(topY - upInnerBase) * a)) / (a * 1.442695) +
              baseDensity * span
            : 0.0;

        // 沿视线 τ = ∫density ds = (∫density dy) / dir.y
        // 2026-11-18：抬头吃的也是"天空方向折扣"，否则头顶仍是厚霾。
        const float tau =
            vertical / dirY * fogTimeFactor * FOG_SKY_DENSITY_SCALE;
        const float upPath = min(upPathToTop, fogFar - fogNear);
        const float upTransmittance = exp2(-tau * 1.442695);

        if(upPath > 0.01)
        {
            // =================================================================
            // 2026-11-19（第二次修复）：**天空方向不再有任何太阳项**。
            //
            // 症状（用户）："有光柱了，但从头顶垂直射下来的，且看起来没有遮挡
            // 判断"。根因就是我 11-18 给这条解析快路径补的那份太阳项：
            //   · 它用的是"射线到地面是否被地形挡住"的判据，而抬头视线的样本
            //     一路往上走，地形永远在下面 ⇒ 判据恒 1（结构上不可能有遮挡）；
            //   · 散射又是解析求的(1-T)，与仰角一一对应 ⇒ 于是整片天空按仰角
            //     分层、在天顶汇聚，看起来就是"从头顶垂直射下来的柱子"。
            // 天空方向本来就没有"被雾遮挡的光路"，它只该是一层无方向性的薄霾。
            // 所以这里回到纯环境项：结构上不可能出现头顶光柱。
            // 真正的地形遮挡柱只留在地面/水平方向的 march 里。
            // =================================================================
            upResult.scattering = ambientRadiance * (1.0 - upTransmittance) *
                (0.3 * max(ambientStrength, 0.0));
            upResult.transmittance = upTransmittance;
            upResult.opticalDepth = -log(max(upTransmittance, 1.0e-6));
            upResult.meanDistance = fogNear + upPath * 0.5;

            // 2026-11-19：上面这份解析太阳项已删除（它产生了"头顶垂直光柱"）。
            // 天空方向保持无方向性薄霾，见本段开头的说明。

            // 纯抬头（y ≥ HI）：解析口径足够，省掉 march。
            if(upBlendRaw >= 0.999)
            {
                return upResult;
            }
        }
        else
        {
            // 相机在雾顶之上：解析口径下就是"无雾"，拿它去混 march 会把雾抹掉。
            upBlend = 0.0;
        }
    }

    const vec3 start = worldOrigin + worldDirection * fogNear;
    const vec3 end = worldOrigin + worldDirection * fogFar;
    const vec3 rayVector = end - start;

    const float rSteps = 1.0 / VFOG_QUALITY;
    // 提到循环外：原来循环里对同一条 rayVector 反复求 length（雾浪与均值距离各一次）。
    const float rayVectorLength = length(rayVector);
    const float rayLength = rayVectorLength * rSteps * 0.23979;

    const float noise = fogDitherPhase(
        ivec2(gl_GlobalInvocationID.xy), fogTemporalPhase(temporalJitter));

    // 2026-11-18：天空方向折扣（见 FOG_SKY_DENSITY_SCALE 的注释）。
    // sceneDistance 是这条视线上的最近几何距离；没有命中（>= fogFar）说明看的是天。
    // 用 smoothstep 过渡，避免物体轮廓处出现硬边。
    const float skyDensityMul = mix(
        1.0,
        FOG_SKY_DENSITY_SCALE,
        smoothstep(fogFar * 0.85, fogFar * 0.99, sceneDistance));

    const float VdotL = dot(worldDirection, sunDirection);
    // 2026-11-09 P4：单叶 → 双叶。55° 侧光下光柱边缘不再"一刀切"，
    // 背光侧靠后向瓣保留一层底光，雾的体积感更强。
    const float phases = fogDualLobePhase(VdotL, anisotropyG) * 0.7 + 0.035;

    const int sunSteps = clamp(int(sunStepsRaw + 0.5), 1, VFOG_SUNLIGHT_STEPS_MAX);

    vec4 fogLightPosEmission[FOG_LIGHT_MAX];
    uint fogLightType[FOG_LIGHT_MAX];
    int fogLightCount = 0;
    float fogLightEnergyScale = 0.0;
    if(dynamicLightStrength > 0.0)
    {
        const vec3 gatherPoint = worldOrigin + worldDirection * min(fogFar, 24.0);
        fogLightCount = fogGatherLocalLights(
            gatherPoint, fogLightPosEmission, fogLightType);
        fogLightEnergyScale = 3.2 * LOCAL_LIGHT_ENERGY_SCALE *
            localLightIntensity() * max(dynamicLightStrength, 0.0);
    }
    vec3 lightDensityAccum = vec3(0.0);

    // =====================================================================
    // 2026-11-19（重构）：体积光介质的两个循环不变量。
    //
    // ① shaftAmp：光柱强度档。独立于雾密度 —— 这是"浓光柱 + 淡雾"能同时成立的
    //    前提。雨天直接归零（用户明确要求：雨天的平流雾不该算体积光）。光强
    //    只影响介质里散射多少太阳光，不影响任何消光。
    // ② shadowSunDirection：阴影查表必须与生产者 rt_sun_shadow 用同一方向。
    //    太阳落到地平线以下时生产者把方向翻上来（正交盒永远在场景上方），
    //    这里必须同式，否则查表落进镜像的纹素、整片误判受光。
    // =====================================================================
    const float advectionFogRain = smoothstep(0.35, 0.75, rain);
    const float shaftAmp = FOG_SHAFT_MEDIUM *
        clamp(max(sunStrength, 0.0), 0.0, 4.0) *
        (1.0 - advectionFogRain);

    const float moonPhaseAngle =
        fract(fract(cam.historyParams.w) + cam.sssParams.z + 0.5) * 6.28318530718;
    const vec3 moonShadowDirection = normalize(vec3(
        cos(moonPhaseAngle), 0.81915204429 * sin(moonPhaseAngle),
        0.57357643635 * sin(moonPhaseAngle)));
    const vec3 shadowSunDirection =
        sunDirection.y >= 0.0 ? sunDirection : moonShadowDirection;

    // 薄雾激活时也要采地面高度场（薄雾锚在地形上）。
    const bool needGroundSample = groundAnchor > 0.0 || rollingStrength > 0.0;

    float transmittance = 1.0;
    float fogDensityAccum = 0.0;
    float rayDensityAccum = 0.0;
    float shaftScatterAccum = 0.0;
    float fogMeanDistanceAccum = 0.0;
    float fogMeanWeightAccum = 0.0;
    float fogLitAccum = 0.0;

#if VFOG_DEBUG_OUT
    // 诊断：分开统计"阴影图判的可见度"与"几何射线判的可见度"，
    // 以便定位是哪一路把 shadow 按成 0。按密度加权归一化。
    float vfogDbgMapVisAccum = 0.0;
    float vfogDbgRayVisAccum = 0.0;
#endif

    for(int i = 0; i < int(VFOG_QUALITY); ++i)
    {
        const float exponential = pow(11.0, (float(i) + noise) * rSteps);
        const float stepLength = exponential * 0.1 - 0.1;
        const float stepDensity = exponential * rayLength * 1.442695;

        const vec3 fogPos = start + stepLength * rayVector;

        FogGround ground;
        ground.height = FOG_ANCHOR_REFERENCE_Y;
        ground.weight = 0.0;
        if(needGroundSample)
        {
            ground = fogGroundAt(fogPos);
        }
        // =================================================================
        // 2026-11-15：**恢复整条带随地形平移**（锚定）。
        //
        // 上一版为了消掉"头顶竖直光柱"把锚定去掉了，结果漏光全回来了：
        // 锚定同时承担了"山体内部/地下少雾"的作用 —— 带被地形抬起来后，
        // 山体里的样本落到带底以下，密度按带外指数掉下去；去掉锚定后整条带
        // 固定在世界高度，山体内部和地下也全在带内、满密度 ⇒ 阴影判据的
        // 边角（无数据列、S1 体积近场）一漏就是一条光柱。
        // 结论：锚定不能动。头顶光柱只能靠"带内上疏下密"这条剖面去压，
        // 不能靠改坐标。
        // =================================================================
        const float groundOffset = fogGroundAnchorOffset(ground, groundAnchor);

        const float sampleDistance = fogNear + stepLength * rayVectorLength;

        // 尾部淡出作用于视向距离：远处雾先变薄再归零，不再在 fogEnd 处硬切。
        const float tailFade =
            1.0 - smoothstep(fogTailFadeStart, fogFar, sampleDistance);

        // 2026-11-14：带内剖面的基准（上疏下密，见 fog_density_common.glsl）。
        // 密度场已经随地形整体平移过 groundOffset，所以地表在这里也要减一次，
        // 否则平移后的基准会和 y 不同坐标。weight=0（地形图无数据）时用海平面
        // 参考高度，与 FogGround 的默认值同口径。
        const float fogInnerBase =
            ((ground.weight > 0.0) ? ground.height : FOG_ANCHOR_REFERENCE_Y)
            - groundOffset;

        float density = fogDensityAtGround(
            vec3(fogPos.x, fogPos.y - groundOffset, fogPos.z),
            baseDensity,
            fogTimeFactor,
            wetness,
            heightMax,
            heightMin,
            fogInnerBase) * tailFade * skyDensityMul;

        // 2026-11-13：雾顶逆温层（雨天）。
        // 真实雨雾/层云有一个相当清晰的上边界（其上空气更干、温度更高），
        // 观感就是"雾顶一条平线"，而不是指数式慢慢淡出。
        // 用已有的 wetness 驱动、不新增车道：晴天 wetness≈0 ⇒ mix(...,0) 恒等，
        // 行为与改动前逐位一致；雨天阈值收到 10 格，边界变硬。
        {
            const float wetForTop = clamp(wetnessEnv, 0.0, 1.0);
            const float topSoftness = mix(64.0, 10.0, wetForTop);
            const float topMask = 1.0 - smoothstep(
                heightMax - topSoftness, heightMax, fogPos.y);
            density *= mix(1.0, topMask, wetForTop);
        }

        // 平流雾：不参与尾部淡出（自带按 rollingRange 的近场淡出）。离地高度用
        // 本地地面 ground.height，**不**叠加锚定偏移——偏移是让雾带跟着地形整体
        // 平移的，平流雾要的是"贴着当地地表滚动"，山谷积雾、山脊裸露才对。
        if(rollingStrength > 0.0)
        {
            density += rollingFogDensity(fogPos, ground.height, rollingHeightScale,
                rollingStrength, rollingRange, rollingTime, rollingSpeed, worldOrigin);
        }

        // =====================================================
        // 2026-11-16 雾形态：主体积雾的**厚薄不均**。
        //
        // 上面两个来源都长不出它：fogDensityAtGround 是纯解析指数（均匀灰膜），
        // rollingFogDensity 只服务 1.5 格厚的贴地雾且**只在下雨时出现**。
        // 于是 30~200 格厚的主体积雾在晴天没有任何内部结构。
        //
        // 做法：用已绑定的 cloudCurlNoise（低频、平滑、已随风平移）在 120 格
        // 尺度上乘性调制。乘性 ⇒ 不凭空造雾；clamp 0.45~1.85 ⇒ 不撕出空洞；
        // 120 格比平流雾的 28/9 格粗得多，不会叠成麻点。
        //   cam.fogMoodParams.x = 0 ⇒ 逐位回到改动前（均匀灰膜）。
        // 与 fog_froxel_density.comp 同式（那边有完整注释）。
        // =====================================================
        const float fogShapeStrength = clamp(cam.fogMoodParams.x, 0.0, 2.0);
        if(fogShapeStrength > 0.0 && density > 1.0e-7)
        {
            // 2026-11-21 统一风场：滚动方向改读 windDirXZ()，速度（×0.35）不动。
            const vec2 shapeWind = windDirXZ() *
                (max(rollingSpeed, 0.0) * rollingTime * 0.35);
            const vec2 shapeCurl = textureLod(cloudCurlNoise,
                vec3((fogPos.xz + shapeWind) / 120.0, 0.72), 0.0).xy;
            density *= clamp(
                1.0 + shapeCurl.x * 0.85 * fogShapeStrength, 0.45, 1.85);
        }

        const float absorption = exp2(-density * stepDensity);

        // =====================================================================
        // 2026-11-19（重构）：太阳项不再由雾密度驱动，改由**独立的体积光介质**。
        // 见常量区 FOG_SHAFT_* 的说明。这里只做三件事：
        //   ① 求本步的介质密度（贴地指数，短标高）
        //   ② 解析求太阳方向的光学厚度（闭式，2 次 exp2）
        //   ③ 按本步实际光程累积散射能量
        // =====================================================================
        // stepDensity = dt_blocks × 1.442695，所以 dt = stepDensity / 1.442695。
        // 2026-11-19（修正）：stepDensity 里的 exponential·rayLength 只是消光积分的
        // 近似尺度，**不是**本步的实际世界长度。实际步长是相邻两个 stepLength 之差：
        //   dx = (exponential(i+1) - exponential(i)) · 0.1 · rayLength
        // 直接用它，光柱能量才和物理光程对得上（旧式大了约 60 倍）。
        const float nextExponential =
            pow(11.0, (float(i) + 1.0 + noise) * rSteps);
        const float stepBlocks =
            max(nextExponential - exponential, 0.0) * 0.1 * rayLength;

        const float localGroundY =
            (ground.weight > 0.0) ? ground.height : FOG_ANCHOR_REFERENCE_Y;
        const float shaftHeight = max(fogPos.y - localGroundY, 0.0);
        const float shaftDensity = shaftAmp *
            exp2(-shaftHeight / FOG_SHAFT_SCALE_HEIGHT * 1.4426950408889634);

        // 沿太阳方向对贴地指数剖面做闭式积分：
        //   ∫₀^∞ A·e^{-(h+s·sinθ)/H} ds = A·H/sinθ
        // 这就是"这个样本上方还有多少介质挡着阳光"。太阳越低，路径越长、越暗。
        const float shaftSunTau = shaftDensity *
            FOG_SHAFT_SCALE_HEIGHT / max(shadowSunDirection.y, 0.05);
        const float shaftSunTransmittance =
            1.0 - exp2(-shaftSunTau * 1.4426950408889634);

        // 相函数：前向散射（朝太阳看才亮），双瓣近似。
        const float shaftStep = shaftDensity * stepBlocks * shaftSunTransmittance *
            phases;

        // 2026-11-09：阴影查表必须用**生产者同一个方向**。
        //
        // 生产者 rt_sun_shadow.comp 用的是 sunShadowLightDirection()，它有一条
        // `return direction.y >= 0.0 ? direction : -direction;` —— 太阳在地平线
        // 以下时把方向**翻上来**（保证正交盒永远落在场景上方）。
        // 而这里传进来的 sunDirection 来自 sunDirection()（sky_model_common.glsl），
        // **没有**这个翻转。于是太阳在地平线以下时两者相差 180°：基向量
        // basisRight/basisUp 和 depth = extentZ - dot(local, sunDir) 全部镜像，
        // 查表落到完全错误的纹素，读回的 stored 绝大多数是 320（未命中）⇒
        // depth < 320 ⇒ 判定"受光" ⇒ 雾在洞里整片被太阳点亮（光柱穿墙）。
        //
        // 这里只翻阴影查表这一个用途；上面第 364 行的太阳光步 march 仍用真实方向。
        // 2026-11-16：与 sunShadowLightDirection() 同步 —— 月亮按月相沿日轨
        // 移动，不再是 -sunDirection。本文件不依赖 sky_model_common.glsl 的
        // include 顺序，故内联同式（改一处必须改另一处）。
        // 2026-11-19（重构）：提到循环外（原本每个 march 步都重算一遍）。

        // 2026-11-10：阴影盒覆盖渐隐。
        // 盒外已经判 0（没有遮挡数据就不给光柱，见 sun_shadow.glsl 的宏分支），
        // 但如果只做硬切，会在 ±160 格处留下一个明显的"环"。这里在
        // 0.80~0.98 × EXTENT（约 128~157 格）内把光柱线性收到 0：
        // 洞穴里光柱都在 5~30 格内，完全不受影响；开阔天空里的远处光柱
        // 变成自然的大气衰减。
        // 2026-11-11：删除 128~157 格的 shadowCoverage 渐隐。
        // 它是以相机为圆心的球壳，抬头看天时会在头顶切出一个"盖子"，
        // 表现为"总有一道体积光从头顶直射下来"。盒外连续性改由
        // sun_shadow.glsl 的"夹取边界列 + 远距离淡出"负责。

        // 2026-11-11：云隙光。逐 march 步采样云层透射率（而不是全局标量），
        // 于是云薄处亮、云厚处暗 ⇒ 从云缝里漏下来的光柱自然出现。
        const float cloudGap = cloudGapTransmittance(
            fogPos, shadowSunDirection, cloudLayer, rollingTime);

        // 2026-11-12：把"是否露天"告诉阴影查表，供它处理**无数据**的情况。
        // 高于当地地表 ⇒ 露天 ⇒ 1.0（本该受光，不能被相机相对的渐隐切掉）；
        // 低于地表 ⇒ 洞内 ⇒ 0.0（没有几何数据就不给光）。
#ifdef SUN_SHADOW_TREAT_NO_HIT_AS_NO_DATA
        sunShadowNoDataVisibility =
            (ground.weight > 0.0 && fogPos.y < ground.height + 1.0) ? 0.0 : 1.0;
#endif

        // 2026-11-15：走"盒外保守判暗"的雾专用版本（见文件顶部
        // fogSunShadowVisibility 的说明）。
        const float shadowMapVis = fogSunShadowVisibility(fogPos, shadowSunDirection, i);
        float shadow = shadowMapVis * cloudGap;
        float dbgRayVis = 1.0;   // 诊断用：几何射线自己判的可见度

#if VFOG_FORCE_SUN
        // 对照实验：完全旁路可见性。用来区分"太阳项本身是否是 0"与
        // "可见性链路是否恒为 0"。这一档画面会整片发亮（没有柱子），
        // 只要能看到雾变亮，就证明太阳项活着。
        shadow = 1.0;
#else
#if VFOG_SUN_HEIGHTVIS
        // 2026-11-19（回退到可用的判据）：**地形高度场为准**。
        //
        // 上一版把太阳阴影图（rt_sun_shadow）当主判据，实测它几乎对每个样本都
        // 报"被遮挡" ⇒ shadow 恒 0 ⇒ 一根柱子都没有。那张图本身是坏的（另案修），
        // 不能拿它当"严丝合缝"的前提。
        // 高度场能给出山脊/谷口/悬崖的真实轮廓，是当前唯一可用且稳定的遮挡源。
        // 树冠/建筑暂缺 —— 见 FOG_SUN_HEIGHT_MAX_DISTANCE 的说明，后续用可达的
        // 几何源补（不是靠这张坏图）。
        // 阴影图路径保留在 VFOG_SHADOWMAP_OCCLUSION 下（默认关），供它修好后启用。
        {
            const float heightVis = fogSunHeightVisibility(
                fogPos, shadowSunDirection, FOG_SUN_HEIGHT_MAX_DISTANCE);
            shadow = heightVis * cloudGap;
#if VFOG_SHADOWMAP_OCCLUSION
            shadow = shadow * shadowMapVis;
#endif
        }
#endif

#if defined(VFOG_SUN_RAY) && !VFOG_SUN_HEIGHTVIS
        // 2026-11-15（L2）：太阳遮挡真值。近场（<= ~128 格）完全按几何判定；
        // 128~192 格平滑过渡回阴影图（更远处雾已经很稀，且那边阴影图的
        // "盒外保守处理"是安全的）。于是洞穴顶 / 树冠 / 山体侧影都不会再漏光，
        // 也不会出现"朝太阳方向凭空一道光束"。
        {
            const float rayVis = fogSunRayVisibility(
                fogPos, shadowSunDirection, FOG_SUN_RAY_MAX_DISTANCE,
                float(gl_GlobalInvocationID.x) +
                    float(gl_GlobalInvocationID.y) * 7919.0 + float(i) * 131.0);
            dbgRayVis = rayVis;
            const float rayWeight =
                1.0 - smoothstep(FOG_SUN_RAY_MAX_DISTANCE * 0.70,
                    FOG_SUN_RAY_MAX_DISTANCE, sampleDistance);
            // 2026-11-18 修正：**近场以几何射线为唯一真值源**。
            //
            // 原来是 min(阴影图, 射线)。实测（诊断通道）：阴影图路径对每个样本
            // 都判"被遮挡"（红=0），于是 min 把本来正确的射线（绿）一起否掉，
            // 雾的太阳项恒为 0 ⇒ 任何角度都看不到体积光。
            //
            // 现在按距离过渡：近场（rayWeight→1）纯用射线，远场（超出射线射程）
            // 才回退阴影图。这也正是"严丝合缝"要求的判据 —— 柱子边界只由真实
            // 几何决定，不受阴影图的盒边界/清屏值影响。
            shadow = mix(shadow, rayVis, rayWeight);
        }
#endif
#endif

        const float integral = transmittance - absorption * transmittance;

#if VFOG_DEBUG_OUT
        vfogDbgMapVisAccum += integral * shadowMapVis;
        vfogDbgRayVisAccum += integral * dbgRayVis;
#endif

        fogDensityAccum += integral;
        fogLitAccum += integral * shadow;
        // 2026-11-19（重构）：光柱能量按**光柱介质自己的**光学厚度累积，
        // 不掺雾的 density。衰减用当前 transmittance（这一段样本到眼睛的遮蔽）。
        shaftScatterAccum += shaftStep * shadow * transmittance;

        fogMeanDistanceAccum += integral * sampleDistance;
        fogMeanWeightAccum += integral;

        for(int li = 0; li < FOG_LIGHT_MAX; ++li)
        {
            if(li >= fogLightCount)
            {
                break;
            }

            const vec3 toLight = fogLightPosEmission[li].xyz - fogPos;
            const float emissionLevel = clamp(fogLightPosEmission[li].w, 1.0, 15.0);
            // 2026-10-31：每盏灯有自己的档案（色温/亮度/距离），不再共用
            // "emission / 15 + emission × 2 格"的统一口径。
            const uint lightProfile = fogLightType[li];
            const float distanceToLight = length(toLight);
            if(distanceToLight > vptLocalLightRange(lightProfile, emissionLevel))
            {
                continue;
            }

            const float lightScale = fogLightEnergyScale *
                localLightProfileBrightness(lightProfile, emissionLevel) *
                vptLocalLightDistanceAttenuation(distanceToLight) *
                vptLocalLightRangeFade(distanceToLight, lightProfile, emissionLevel);

            lightDensityAccum += decodeLocalLightColor(fogLightType[li]) * lightScale *
                (fogLightPhase(toLight, worldDirection) * integral);
        }

        transmittance *= absorption;

        if(transmittance < 1.0e-4)
        {
            break;
        }
    }

    result.meanDistance = fogMeanWeightAccum > 1.0e-6
        ? fogMeanDistanceAccum / fogMeanWeightAccum
        : fogFar;

    const float litFraction = fogDensityAccum > 1.0e-6
        ? clamp(fogLitAccum / fogDensityAccum, 0.0, 1.0)
        : 1.0;
    const float ambientVisibility =
        mix(FOG_AMBIENT_SHADOW_FLOOR, 1.0, litFraction);

    const vec3 fogColor =
        ambientRadiance * fogDensityAccum *
        (0.3 * max(ambientStrength, 0.0)) * ambientVisibility;

    // =====================================================================
    // 2026-11-19（重构）：合成口径分成三项，互不牵连。
    //
    //   fogColor   —— 环境内散射：来自雾介质的密度，不含任何太阳项。
    //                 "雾有多浓"只体现在这里（画面被压灰多少），与光柱无关。
    //   shaftColor —— 太阳直射在**光柱介质**里的单次散射：强度、贴地标高、
    //                 几何遮挡全部独立于雾密度；头顶没有介质 ⇒ 恒为 0。
    //   lightDensityAccum —— 局部光源在雾里的散射（未改动）。
    //
    // 太阳项不再乘任何"抬头压制/仰角淡出"：那类系数是旧模型（一根雾柱盖满
    // 整个天空）的补丁，介质分离后既不需要，也会误伤低角度的长光柱。
    // =====================================================================
    const vec3 shaftColor =
        sunRadiance * shaftScatterAccum * FOG_SHAFT_GAIN *
        vec3(cam.fogOpticsParams.y, cam.fogOpticsParams.z,
            cam.fogOpticsParams.w);

    result.scattering = fogColor + shaftColor + lightDensityAccum;
    result.transmittance = transmittance;
    result.opticalDepth = -log(max(transmittance, 1.0e-6));

    // 2026-11-18：软过渡收口。仰角带内把 march 结果与抬头解析结果按 upBlend 混合，
    // 消掉原来 y=0.8 那条圆形硬边；带外 upBlend 为 0（或已早退），这里是无操作。
    if(upBlend > 0.0)
    {
        result.scattering = mix(result.scattering, upResult.scattering, upBlend);
        result.transmittance =
            mix(result.transmittance, upResult.transmittance, upBlend);
        result.opticalDepth = -log(max(result.transmittance, 1.0e-6));
        result.meanDistance =
            mix(result.meanDistance, upResult.meanDistance, upBlend);
    }

    // 2026-11-19 诊断（重构后）：三个量都改成新口径，供 fog_screen_march 上色。
#if VFOG_DEBUG_OUT
    {
        //   R = litFraction                 —— 光柱介质的受光比例
        //   G = shaftScatterAccum×40        —— 光柱介质累积的散射能量
        //   B = |shaftColor|×3              —— 真正加进画面的那部分
        gVfogDebug = vec3(
            clamp(litFraction, 0.0, 1.0),
            clamp(shaftScatterAccum * 40.0, 0.0, 1.0),
            clamp(length(shaftColor) * 3.0, 0.0, 1.0));
    }
#endif

    return result;
}

#endif
