#ifndef WIND_COMMON_GLSL
#define WIND_COMMON_GLSL

// =====================================================
// 统一风场（2026-11-21）：一条风向喂云 / 雾 / 水波 / 植物。
// 数据源：cam.windParams（CameraUBO 末尾尾块，2026-11-21 追加，见 camera_ubo.glsl
// 尾注释）：
//   x = 风向角（世界 XZ 平面，度；dir = (cos a, sin a)，与原 high_cloud_field
//       的 lp2.w 风向角约定一致；出厂 30.5 ≈ HP 云原方向）
//   y = 风强倍率（0..2，1 = 出厂；植物摇摆/阵风的幅度乘数）
//   z = 阵风强度（0..1，0 = 恒定风）
//   w = 有效性标记（1 = 宿主已写入）。0（旧 DLL / 未写入）⇒ 一切读数回落
//       出厂常量，逐位等于改动前 —— 与 moonParams.z 同一套回落语义。
// 依赖：本文件只允许在已声明 cam（camera_ubo.glsl 之后）的 TU 里被 include。
// 自身不带任何 include —— camera_ubo.glsl 是块成员片段（没有 include guard），
// 绝不能从这里再 include 它。各 TU 的 include 顺序契约见水体交接 §16/§17。
//
// 速度不统一：云 / 雾 / 水波各自的推进速度都是调好的观感旋钮
//（HP 云 4.9 格/s、tyndall = cloudParams1.y、雾滚 = godRayParams2.z、
//  水 FFT = waterFftParams.w），本文件只供**方向**，速度一律留在原地。
// =====================================================

// 出厂方向 = HP 云 / 雾原硬编码 vec2(0.862, 0.507)（≈30.5°）。
const vec2 WIND_DEFAULT_DIR = vec2(0.86207, 0.50700);

vec2 windDirXZ()
{
    if(cam.windParams.w < 0.5)
    {
        return WIND_DEFAULT_DIR;
    }
    float a = radians(cam.windParams.x);
    return vec2(cos(a), sin(a));
}

float windStrength()
{
    return cam.windParams.w >= 0.5
        ? clamp(cam.windParams.y, 0.0, 2.0)
        : 1.0;
}

float windGust()
{
    return cam.windParams.w >= 0.5
        ? clamp(cam.windParams.z, 0.0, 1.0)
        : 0.0;
}

// 2D 整数 hash → [0,1)。写法照 tyndall_cloud_field.glsl 的 tyndallRegionHash
//（同一套 u32 混链），但砍到 2D 双 lane —— 阵风场只要一个标量。
vec2 windHash2(vec2 cell)
{
    uvec2 h = uvec2(ivec2(cell));
    h *= uvec2(1597334677u, 3812015801u);
    uint seed = h.x ^ h.y;
    uvec2 v = seed ^ uvec2(2246822519u, 3266489917u);
    v ^= v >> 16u;
    v *= 2246822519u;
    v ^= v >> 13u;
    return vec2(v & 0x00ffffffu) / 16777216.0;
}

// 平滑 value noise（0..1），bilinear + smoothstep 权重。
float windValueNoise(vec2 p)
{
    vec2 cell = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = windHash2(cell).x;
    float b = windHash2(cell + vec2(1.0, 0.0)).x;
    float c = windHash2(cell + vec2(0.0, 1.0)).x;
    float d = windHash2(cell + vec2(1.0, 1.0)).x;
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// 阵风标量场 0..1：沿风向行进的大尺度 value noise + 分钟级全局起伏。
//   波长 ~55 格（frequency 0.018/格）、行进 1.1 格/s（风推阵风团）。
//   阵风强度 0 ⇒ 返回 0（恒定风），>0 时场值在 0..1 间流动。
// 消费方自行映射，例如摇摆幅度 ×(1 - gust + gust × field × 1.5)。
float windGustField(vec2 worldXZ, float timeSeconds)
{
    float gust = windGust();
    if(gust <= 0.001)
    {
        return 0.0;
    }
    vec2 dir = windDirXZ();
    vec2 travel = worldXZ * 0.018 - dir * (timeSeconds * 1.1);
    float g = windValueNoise(travel);
    float global = 0.75 + 0.25 * sin(timeSeconds * 0.13);
    return clamp(g * global, 0.0, 1.0);
}

// ===========================================================================
// ⑤ 批3：植物 UV 摇摆（BSL/Crysis 混合模型，适配 RT 着色空间）
//
// 路径追踪不重建 BLAS ⇒ 三角形轮廓不动；可见摇晃来自 **alpha 剪影的纹理图样
// 随风位移**：resolve（着色）与 any-hit（alpha test）用同一函数同一输入 ⇒
// 镂空轮廓在两类射线间逐位一致地摆（官方 MC RTX 的植物动画同为此路线）。
//
// 模型（BSL 多向行进波 + Crysis 阵风门控混合）：
//   · 主摆：沿风传播的正弦（位置相位 ⇒ 阵风跨树冠行进，波长 ~11 格）
//   · 侧摆：垂直风向的拍频（×0.45，叶丛的横向摆动）
//   · 叶颤：高频小幅度微颤（位置哈希相位，叶与叶不同步）
//   · 阵风门控：windGustField（55 格行进噪声）调幅，风强滑条整体乘
//   · 位移以 **世界格** 为单位（资源包分辨率无关），经 spriteSpan 换算成 UV
//   · Sprite 矩形钳制（半纹素内缩，与 material_resolve 的 safe 口径一致）：
//     图样贴边时不会串进相邻 sprite
//
// 调用方（material_resolve 与 rt_test.rahit）必须给同一 worldPos（插值顶点
// 位置）与同一 baseUv/顶点 UV 三元组 ⇒ 剪影逐位一致。
// ===========================================================================
vec2 windFoliageUvSway(
    vec2 baseUv,
    vec2 uv0, vec2 uv1, vec2 uv2,
    vec3 worldPos,
    vec2 atlasTexel)
{
    vec2 wdir = windDirXZ();
    vec2 wperp = vec2(-wdir.y, wdir.x);
    float t = uintBitsToFloat(cam.padding);

    // 位置相位：波沿风跨树冠行进
    float travel = dot(worldPos.xz, wdir);
    float side = dot(worldPos.xz, wperp);

    // 阵风门控（0.45 基线 = 无阵风时也有微风）+ 三分量波形
    float gustGate = 0.45 + 0.55 * windGustField(worldPos.xz, t);
    float mainWave = sin(t * 1.9 + travel * 0.55);
    float sideWave = sin(t * 3.1 + side * 0.62);
    float flutter = sin(
        t * 5.3 + worldPos.x * 2.1 + worldPos.y * 1.3 + worldPos.z * 1.7);

    // 位移（格）：闲置 ~0.020，阵风峰值 ~0.095；风强滑条 0..2 整体乘
    float disp = (0.020 + 0.075 * gustGate) * windStrength();

    // sprite 矩形（顶点 UV 包围盒）—— 位移换算成 UV 的世界尺度基准
    vec2 spriteMin = min(min(uv0, uv1), uv2);
    vec2 spriteMax = max(max(uv0, uv1), uv2);
    vec2 spriteSpan = max(spriteMax - spriteMin, atlasTexel);

    vec2 uvJitter =
        (wdir * mainWave + wperp * (sideWave * 0.45)) * (disp * spriteSpan)
        + vec2(0.0, flutter * 0.30) * (disp * spriteSpan.y);

    // Sprite 内钳制（半纹素内缩，防 bilinear 串相邻 sprite）
    vec2 sMin = spriteMin + atlasTexel * 0.5;
    vec2 sMax = spriteMax - atlasTexel * 0.5;
    vec2 sCenter = (sMin + sMax) * 0.5;
    sMin = min(sMin, sCenter);
    sMax = max(sMax, sCenter);
    return clamp(baseUv + uvJitter, sMin, sMax);
}

#endif
