// =============================================================================
// D1（材质 callable 化，2026-11-05）：K2 的材质求值改走 callable。
//
// B① 时代这里内联了整套材质层（扩展 / 图集绑定 / 顶点家族 / K2LodRoles /
// makeHitState / macro 重命名 shim / 三段 helper / material_resolve.glsl）——
// 那是与 rt_test.rchit 并存的一份漂移副本（4 个时辰函数 + 2 常量 + 雨可见度
// 两边数值不同，全靠 #define shim 压住）。callable 化后材质层只剩
// rt_test_material.spv 一份（由 .tmp/rgen_split/gen_material_callable.py 从
// rt_test.rchit 前言生成），rgen 侧只留进出块契约。
//
// 契约见 material_callable.glsl：调用点在 rgen_main_body.glsl（landing 载入
// 之后、guide 计算之前），matCallableResolve() 借 executeCallableEXT 完成
// 一次 SBT callable，进出全在 payload 身份五元组 + 材质字段。
//
// 2026-11-05 ③/④：callable 本体硬崩（D1 二分定案，机制未明，parked）。K2 的
// 可编译稳定路径回到 B① 内联块（rgen_k2_material_inline.glsl，由
// gen_k2_block_inline.py 从 rchit 前言生成），编译期旗标 ORION_K2_MATERIAL_INLINE
// 选择；不加旗标保持 callable 路径（D1 调试用，现行部署的 rt_test_k2.rgen.spv）。
#ifdef ORION_K2_MATERIAL_INLINE
#include "rgen_k2_material_inline.glsl"
#else
#include "material_callable.glsl"
#endif
