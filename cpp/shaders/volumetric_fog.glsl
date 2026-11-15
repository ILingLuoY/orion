#ifndef VOLUMETRIC_FOG_GLSL
#define VOLUMETRIC_FOG_GLSL

#include "fog_density_common.glsl"

#include "local_light_grid_common.glsl"

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
const float FOG_SUN_RAY_MAX_DISTANCE = 128.0;
// 2026-11-15 修正：原来照抄了 SUN_SHADOW_CULL_MASK = 0x04，但 WorldGeometry.cpp
// 里 0x04 是**第一人称手部**专用位（地形是 0xff，primaryMaskCull 时 0xfe）。
// 用 0x04 打 ray 时命中集合完全不对 ⇒ 几乎全 miss ⇒ shadow 恒 1 ⇒ "几乎没有遮挡、
// 全漏光"。这里改用 0xFF（与 rt_sun_volume.comp 一致，能命中全部实例）。
const uint FOG_SUN_RAY_CULL_MASK = 0xFFu;

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
        rayQueryInitializeEXT(query, fogTlas,
            gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
            FOG_SUN_RAY_CULL_MASK, origin + sunDirection * 0.05, 0.0,
            direction, maxDistance);
        while(rayQueryProceedEXT(query)) {}

        blocked += (rayQueryGetIntersectionTypeEXT(query, true) !=
            gl_RayQueryCommittedIntersectionNoneEXT) ? 1.0 : 0.0;
    }
    return 1.0 - blocked * 0.5;
}
#endif

// 2026-11-10：6 → 16。
// 太阳光程改成解析积分后，每样本从 6+8=14 次 exp2 降到约 16+1：仍然更便宜，
// 而视线步数从 6 提到 16 直接消掉了"6 步跨 320 格"造成的层状带与放射条纹。
const float VFOG_QUALITY = 16.0;

// 2026-11-15：0.35 → 0.15。这个地板是"环境光不被太阳直射遮挡完全消灭"的近似，
// 但 0.35 太高：射线判定为遮挡的洞穴/背光处，雾仍按 35% 发亮，被当成"漏光"。
// 0.15 留给天空光的底，同时让遮挡区明显暗下去。
const float FOG_AMBIENT_SHADOW_FLOOR = 0.15;

const int VFOG_SUNLIGHT_STEPS_MAX = 8;

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

    // 风推移。云那条链用的是 vec2(0.862, 0.507) * (14.0*0.35*time)，
    // 地面雾更"重"，取约 1/3 的推进速度。
    const vec2 wind = vec2(0.862, 0.507) * (max(speed, 0.0) * time);

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

    const float baseDensity =
        max(baseDensityParam, 0.0) *
        (wetness * 0.15 + 0.08) /
        max(cam.cameraParams.y, 100.0);

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
    const float rollingExtinction = max(mistLane.z, 0.0) * 1.50;
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

    const vec3 start = worldOrigin + worldDirection * fogNear;
    const vec3 end = worldOrigin + worldDirection * fogFar;
    const vec3 rayVector = end - start;

    const float rSteps = 1.0 / VFOG_QUALITY;
    // 提到循环外：原来循环里对同一条 rayVector 反复求 length（雾浪与均值距离各一次）。
    const float rayVectorLength = length(rayVector);
    const float rayLength = rayVectorLength * rSteps * 0.23979;

    const float noise = fogDitherPhase(
        ivec2(gl_GlobalInvocationID.xy), fogTemporalPhase(temporalJitter));

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

    // 薄雾激活时也要采地面高度场（薄雾锚在地形上）。
    const bool needGroundSample = groundAnchor > 0.0 || rollingStrength > 0.0;

    float transmittance = 1.0;
    float fogDensityAccum = 0.0;
    float rayDensityAccum = 0.0;
    float fogMeanDistanceAccum = 0.0;
    float fogMeanWeightAccum = 0.0;
    float fogLitAccum = 0.0;

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
            fogInnerBase) * tailFade;

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

        const float absorption = exp2(-density * stepDensity);

        // =====================================================================
        // 2026-11-10：太阳方向的光学厚度改为**解析积分**。
        //
        // 原实现沿太阳方向走最多 8 步（步长 5 → 7.5 → … → 85 格），每步一次
        // fogDensityAt（各 1 次 exp2）。两个问题：
        //   ① 每样本 8 次 exp2 ⇒ 与视线 6 步相乘 = 48 次，是雾段的主要开销；
        //   ② 步长这么粗，自阴影项被欠采样，残差沿太阳方向成条带 ⇒ 正对太阳时
        //      呈现"从太阳射出的放射状光柱"（用户实测：关掉体积雾即消失）。
        //
        // 高度带雾的密度剖面在带外是纯指数，光程积分有闭式解，直接解析求：
        // 代价 2~4 次 exp2，且与步数无关。省下的预算给视线步数（6 → 16）。
        // 薄雾（mist）从自阴影里略去：它只有 1 格厚，对太阳光程的贡献可忽略。
        // =====================================================================
        // 2026-11-13（A2）：pow(1.5, n) → exp2(n·log2(1.5))。
        // exp2 在全部目标 GPU 上都是一条指令，pow 是 10~20 周期。
        // （更彻底的做法是把它提出 march 循环，但那要动循环结构，留待下一批。）
        const float sunPathLength =
            10.0 * (exp2(float(sunSteps) * 0.5849625007211562) - 1.0);
        float lightingDensity = fogSunOpticalDepth(
            fogPos.y - groundOffset,
            sunDirection.y,
            sunPathLength,
            (2.0 / VFOG_FALLOFF) * (1.0 - wetness * 0.5),
            baseDensity,
            heightMax,
            heightMin) * fogTimeFactor;

        // 2026-11-09 P4：单次散射 + 多次散射一阶近似。
        // 薄雾时 boost≈0（逐位等价旧档），厚雾里补最多 40% 能量 ⇒ 光柱深处不死黑。
        float stepRayDensity =
            (exp2(-lightingDensity * VFOG_SUNLIGHT_ABSORPTION) +
             fogMultiScatterBoost(lightingDensity)) * phases;

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
        const vec3 shadowSunDirection =
            sunDirection.y >= 0.0 ? sunDirection : -sunDirection;

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

        float shadow = sunShadowFogVisibility(fogPos, shadowSunDirection, i) * cloudGap;

#ifdef VFOG_SUN_RAY
        // 2026-11-15（L2）：太阳遮挡真值。近场（<= ~128 格）完全按几何判定；
        // 128~192 格平滑过渡回阴影图（更远处雾已经很稀，且那边阴影图的
        // "盒外保守处理"是安全的）。于是洞穴顶 / 树冠 / 山体侧影都不会再漏光，
        // 也不会出现"朝太阳方向凭空一道光束"。
        {
            const float rayVis = fogSunRayVisibility(
                fogPos, shadowSunDirection, FOG_SUN_RAY_MAX_DISTANCE,
                float(gl_GlobalInvocationID.x) +
                    float(gl_GlobalInvocationID.y) * 7919.0 + float(i) * 131.0);
            const float rayWeight =
                1.0 - smoothstep(FOG_SUN_RAY_MAX_DISTANCE * 0.70,
                    FOG_SUN_RAY_MAX_DISTANCE, sampleDistance);
            shadow = mix(shadow, rayVis, rayWeight);
        }
#endif

        const float integral = transmittance - absorption * transmittance;
        stepRayDensity *= integral;

        fogDensityAccum += integral;
        fogLitAccum += integral * shadow;
        rayDensityAccum += stepRayDensity * shadow;

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

    const vec3 rayColor =
        sunRadiance * rayDensityAccum *
        (1.2 * max(sunStrength, 0.0));

    // 2026-11-13：波长相关透射（fogOpticsParams.yzw）。
    // 真实介质里蓝光衰减得比红光慢，所以远处/厚雾下的太阳散射偏蓝。
    // 这里对**太阳散射项**做逐通道加权（环境项不动，否则整片雾会被染色）。
    // 权重恒为 1 时与原档逐位等价。
    result.scattering = fogColor +
        rayColor * vec3(cam.fogOpticsParams.y, cam.fogOpticsParams.z,
            cam.fogOpticsParams.w) +
        lightDensityAccum;
    result.transmittance = transmittance;
    result.opticalDepth = -log(max(transmittance, 1.0e-6));

    return result;
}

#endif
