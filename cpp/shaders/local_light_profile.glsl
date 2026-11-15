#ifndef LOCAL_LIGHT_PROFILE_GLSL
#define LOCAL_LIGHT_PROFILE_GLSL

// =============================================================================
// 本地光源档案表（Local Light Profile）
//
// 2026-10-31：取消"所有发光方块共用 emission/15 同一口径"的做法。
// 每个档案自带三件东西：发射色（色温或直接给定）、满级亮度、基础传播距离。
// 设置界面里的两个滑杆从此只当**整体系数**：
//   「本地光源照射强度」→ 乘在 localLightProfileBrightness() 上（0..4）
//   「本地光范围系数」  → 乘在 localLightProfileRange() 上（0.25..2.0），
//                         乘积最后仍被 @VPT_LOCAL_MAX_DISTANCE 封顶 30 格。
//
// id 由 SectionCompilerMixin.classifyLocalLightType() 写进体素字的
// bits 6..10（5 bit，0..31）。**本表顺序必须与那边的映射逐项一致。**
//
// color：已按峰值归一到 1.0。色温档来自黑体辐射（Tanner Helland 拟合，
//        < 2600K 往白方向去饱和 8%~22%，否则火焰的蓝分量过低、上屏发暗红）；
//        非热辐射光源（灵魂火/红石/传送门/紫水晶…）没有色温，直接给发射色。
// lumens：emission = 15 时的相对发光强度（1.0 ≈ 旧口径的满级亮度）。
//        实际亮度 = lumens × (emission / 15) × 用户滑杆。
// range：基础传播距离（格），已按各光源的物理尺度取过；全部 ≤ 30。
// pointLike：1 = 点状光源（火把/蜡烛，用半径 0.5 的球面近似），
//            0 = 整块光源（荧石/岩浆/海晶灯，用 2x2 立方体面近似）。
// =============================================================================

const uint LOCAL_LIGHT_PROFILE_COUNT = 32u;
const uint LOCAL_LIGHT_PROFILE_MASK = 31u;

// 四张平行表（不合成 struct：glslang 不支持"struct 数组 + 数组构造器"这种
// 常量初始化，而分离的 vec3[]/float[] 构造器是 GLSL 里最稳的写法）。
// 每行注释给出该档对应的光源与色温，改表时请连注释一起改。

// =============================================================================
// 2026-11-09：四张表已移到宿主（CameraUBO.localLightProfileA/B）。
//
// 原因（可复核）：glslang 对"const 数组 + 运行时索引"会生成 indexable 局部数组。
// 本文件的四张表在索引是 `typeCode & 31`（运行时值）时，会在 rt_test.rgen.spv 里
// 变成 `%indexable` vec3[32] + 3 x float[32] = 768 B/线程的 local memory
// （2026-11-09 的 SPIR-V 普查实测）。那是逐像素的栈读写，且与寄存器无关。
// UBO 数组的动态索引编译成 OpAccessChain，走常量缓存，不产生局部副本。
//
// 数据源：cpp/backend/Memory.cpp 的 kLocalLightProfile[]（createCameraUBO 填写）。
// 顺序仍必须与 SectionCompilerMixin.classifyLocalLightType() 的映射逐项一致。
//
// 前置条件：本文件必须在 CameraProperties（camera_ubo.glsl）声明**之后** include，
// 因为它直接用 `cam`。当前四个消费者都满足（rt_test.rgen 547/776、
// rt_vpt_irradiance.comp 7/11、vol_restir*.comp 9..10 / 12..13）。
// =============================================================================

uint localLightProfileIndex(uint typeCode)
{
    return typeCode & LOCAL_LIGHT_PROFILE_MASK;
}

// 发射色（已含色温/色调）。0 = 未分类 ⇒ 暖白兜底。
vec3 localLightProfileColor(uint typeCode)
{
    return cam.localLightProfileA[localLightProfileIndex(typeCode)].rgb;
}

// emission = 15 时的相对发光强度。
float localLightProfileLumens(uint typeCode)
{
    return cam.localLightProfileA[localLightProfileIndex(typeCode)].w;
}

// 基础传播距离（格）。未分类档没有档案语义，按原版光级线性给。
float localLightProfileBaseRange(uint typeCode, float emissionLevel)
{
    if(localLightProfileIndex(typeCode) == 0u)
    {
        return clamp(emissionLevel * 2.0, 2.0, 30.0);
    }
    return cam.localLightProfileB[localLightProfileIndex(typeCode)].x;
}

// 亮度因子：替代原来的 (emission / 15)。
// 每档的"满级有多亮"由档案决定，emission 只负责档内的比例（红石火把 7 级
// 就该比营火 15 级暗）。未分类档退回旧口径，模组方块不会突然变死黑或爆亮。
float localLightProfileBrightness(uint typeCode, float emissionLevel)
{
    float level01 = clamp(emissionLevel / 15.0, 0.0, 1.0);
    if(localLightProfileIndex(typeCode) == 0u)
    {
        return level01;
    }
    return localLightProfileLumens(typeCode) * level01;
}

// 光源几何：true = 点状（球面近似），false = 整块（立方体面近似）。
bool localLightProfilePointLike(uint typeCode)
{
    return cam.localLightProfileB[localLightProfileIndex(typeCode)].y > 0.5;
}

#endif
