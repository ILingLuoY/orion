// RT 阶段三计数器（GPU 侧）。
//
// 口径（与 cpp/backend/Perf.cpp 的 [RT STAGE CNT] 行、tools/perf_step0.py 的解析字段
// 严格同序）：
//   [0] raysIssued     traceRayEXT / serTraceRay funnel / inline rayQuery 各计一次。
//   [1] hitsAccepted   closest-hit / any-hit 被硬件派发执行一次（直写原子加，
//                      每次 invocation 至多一次）。
//   [2] materialEvals  "完整材质解析"被执行一次的次数。口径写死：
//                      只在真正走进 resolveHitMaterial* 入口的点位自增；
//                      早退（探针 / 消融档 17 / K1 主射线身份落位）不计入。
//   [3] evPrimary      主射线类材质解析：K2 内联落地重建（rgen 漏斗）与 K1 callable
//                      落地重建（rgen 漏斗）。与 lane[2] 同一次解析的分桶，不重复计数。
//   [4] evSecondary    次级射线不透明命中解析：GI / glossy / 阴影探针 / 水等次级用途在
//                      rt_test.rchit 里走完整解析的次数。
//   [5] evGlass        半透明/玻璃命中组解析：rt_test_translucent.rchit 的
//                      resolveHitMaterialGlass 次数。
//   [6] [7] 预留车道：后续再加用途桶时直接落位，前 5 个 lane 的顺序不动。
//
// 2026-11-17：分桶车道是为定位实测 mateval/rays≈1.105 里多出来的那部分而加；
// lane[2] 仍是总口径，三个分桶之和应当等于 lane[2]（rgen 漏斗 + 命中侧漏斗）。
//
// 并发纪律（沿用基线）：
//   · rgen 侧 invocation 内先用模块级局部变量累加，main 末尾 rtCntFlushRgen() 一次落盘；
//   · 命中 stage 的 mateval 同样先私有累加、rtCntFlushHit() 一次落盘；hitsAccepted
//     保持基线的直写原子加；
//   · 不在材质解析函数体里直接 atomicAdd（避免射线内层循环里的原子写）。
//
// 总闸：ORION_RT_STAGE_COUNTERS（编译期宏，与 C++ 侧 getenv 同名）。未定义时本文件
// 所有声明与函数为空，编出的 spv 与基线逐位可比（无 binding 93、无 OpAtomicIAdd）。
#ifndef RT_STAGE_COUNTERS_GLSL
#define RT_STAGE_COUNTERS_GLSL

#ifdef ORION_RT_STAGE_COUNTERS

layout(std430, binding = 93, set = 0) buffer RtStageCounters
{
    uint raysIssued;
    uint hitsAccepted;
    uint materialEvals;
    uint evPrimary;
    uint evSecondary;
    uint evGlass;
    uint reservedLane6;
    uint reservedLane7;
} rtStageCounters;

// ---- rgen 侧：invocation 私有模块级累加器（每 invocation 一份，跨本 invocation
// 调用的 helper 函数共享；不是跨 invocation 共享内存）。 ----
uint g_rtCntRays = 0u;
uint g_rtCntMateval = 0u;
uint g_rtCntEvPrimaryRgen = 0u;

// 命中 stage 侧私有累加器：rchit 解析入口自增，main 末尾 rtCntFlushHit() 一次落盘。
uint g_rtCntHitMateval = 0u;
uint g_rtCntEvPrimaryHit = 0u;
uint g_rtCntEvSecondary = 0u;
uint g_rtCntEvGlass = 0u;

// 每发出一条射线调用一次（局部自增，不写设备缓冲）。
void rtCntRayIssued()
{
    ++g_rtCntRays;
}

// rgen 漏斗的材质求值：K2 内联 / callable 落地重建，口径上都属主射线类 ⇒ evPrimary。
void rtCntMateval()
{
    ++g_rtCntMateval;
    ++g_rtCntEvPrimaryRgen;
}

// rgen main 末尾统一落盘：每个 invocation 至多三次 atomicAdd。
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
    if(g_rtCntEvPrimaryRgen != 0u)
    {
        atomicAdd(rtStageCounters.evPrimary, g_rtCntEvPrimaryRgen);
    }
}

// 命中 stage：main 入口计一次。每次 invocation 至多一次 atomicAdd（基线口径，直写）。
void rtCntHitAccepted()
{
    atomicAdd(rtStageCounters.hitsAccepted, 1u);
}

// 命中侧不透明解析。primary=true 归 evPrimary（兜底普通变体的主射线；K1 精简 rchit
// 主射线在身份落位后已早退，到这里的基本都是次级）；否则归 evSecondary。
void rtCntHitMateval(bool primary)
{
    ++g_rtCntHitMateval;
    if(primary)
    {
        ++g_rtCntEvPrimaryHit;
    }
    else
    {
        ++g_rtCntEvSecondary;
    }
}

// 半透明/玻璃命中组解析：单独 evGlass 桶。
void rtCntHitMatevalGlass()
{
    ++g_rtCntHitMateval;
    ++g_rtCntEvGlass;
}

void rtCntFlushHit()
{
    if(g_rtCntHitMateval != 0u)
    {
        atomicAdd(rtStageCounters.materialEvals, g_rtCntHitMateval);
    }
    if(g_rtCntEvPrimaryHit != 0u)
    {
        atomicAdd(rtStageCounters.evPrimary, g_rtCntEvPrimaryHit);
    }
    if(g_rtCntEvSecondary != 0u)
    {
        atomicAdd(rtStageCounters.evSecondary, g_rtCntEvSecondary);
    }
    if(g_rtCntEvGlass != 0u)
    {
        atomicAdd(rtStageCounters.evGlass, g_rtCntEvGlass);
    }
}

#endif // ORION_RT_STAGE_COUNTERS
// 关闭分支：本头整体为空。所有调用点同样由 #ifdef ORION_RT_STAGE_COUNTERS 包裹，
// 缺省编译时局部累加、atomicAdd、缓冲声明、桩函数与调用全部不进 TU，与基线可比。
#endif // RT_STAGE_COUNTERS_GLSL
