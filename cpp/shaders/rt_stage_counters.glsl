#ifndef RT_STAGE_COUNTERS_GLSL
#define RT_STAGE_COUNTERS_GLSL

// =============================================================================
// RT 阶段计数器（Perf 诊断仪器，2026-11-16）
//
// 三个计数（车道顺序与 C++ 侧 g_rtStageCntSums / 日志行严格同序）：
//   [0] raysIssued    真正发出的射线数：traceRayEXT / serTraceRay()  funnel /
//                      inline rayQueryInitializeEXT 各计一次。主射线与次级弹射
//                      合并记同一个总数，不在着色器里拆 lane。
//   [1] hitsAccepted  命中着色真正被硬件派发执行的次数：rchit（实体/水体/半透明
//                      命中组）+ rahit（any-hit）main 入口各计一次。
//                      注意：硬件不暴露遍历步数，本计数**不是** traversal steps。
//   [2] materialEvals 材质解析经 K2 callable 层执行的次数：在调用方 TU 的
//                      executeCallableEXT 之前计一次（rgen_k2_material.glsl 的
//                      matCallableResolveToLanding 漏斗内）。K1 行内解析与
//                      K2-MATERIAL_INLINE 内联解析不走 callable，不计入本 lane。
//
// 开关：编译期宏 ORION_RT_STAGE_COUNTERS。
//   · 缺省（不定义）= 关闭：本头整段不进 TU，缓冲声明、局部累加、atomicAdd
//     全部被预处理裁掉，编出的 spv 与改动前基线逐位可比。
//   · 采集时由部署脚本在 ORION_RT_STAGE_COUNTERS=1 时追加 -D 定义；C++ 侧另有
//     同名 getenv 总闸，关闭时不建缓冲、不清零、不回读、不打 [RT STAGE CNT] 行。
//
// 并发纪律（硬约束）：
//   · rgen 侧 invocation 内先用模块级局部变量累加，main 末尾各做一次 atomicAdd
//     （rays / mateval 至多两次设备写）；不在射线内层循环里 atomicAdd。
//   · 命中 stage 每次 invocation 只有一次机会，按"每次命中一次 atomicAdd"记账，
//     其自身的原子写开销计入该 lane 的 distort 上限；无内层循环重复写。
// =============================================================================

#ifdef ORION_RT_STAGE_COUNTERS

// 字段用 uint32：与现有 RayStats 缓冲一致，避免依赖 64 位原子扩展。单窗 60 帧
// 量级按 1080p × 每像素数十条射线估算仍在 2^32 内；溢出口径写进交付说明。
layout(std430, binding = 93, set = 0) buffer RtStageCounters
{
    uint raysIssued;
    uint hitsAccepted;
    uint materialEvals;
} rtStageCounters;

// ---- rgen 侧：invocation 私有模块级累加器（每 invocation 一份，跨本 invocation
// 调用的 helper 函数共享；不是跨 invocation 共享内存）。 ----
uint g_rtCntRays = 0u;
uint g_rtCntMateval = 0u;

// 每发出一条射线调用一次（局部自增，不写设备缓冲）。
void rtCntRayIssued()
{
    ++g_rtCntRays;
}

// 每经 callable 解析一次材质调用一次（局部自增）。
void rtCntMateval()
{
    ++g_rtCntMateval;
}

// rgen main 末尾统一落盘：每个 invocation 至多两次 atomicAdd。
void rtCntFlushRgen()
{
    if(g_rtCntRays != 0u)
    {
        atomicAdd(rtStageCounters.raysIssued, g_rtCntRays);
    }
    if(g_rtCntMateval != 0u)
    {
        atomicAdd(rtStageCounters.materialEvals, g_rtCntMateval);
    }
}

// 命中 stage：main 入口计一次。每次 invocation 至多一次 atomicAdd。
void rtCntHitAccepted()
{
    atomicAdd(rtStageCounters.hitsAccepted, 1u);
}

#endif // ORION_RT_STAGE_COUNTERS
// 关闭分支：本头整体为空。所有调用点同样由 #ifdef ORION_RT_STAGE_COUNTERS 包裹，
// 缺省编译时局部累加、atomicAdd、缓冲声明、桩函数与调用全部不进 TU，与基线可比。
#endif // RT_STAGE_COUNTERS_GLSL
