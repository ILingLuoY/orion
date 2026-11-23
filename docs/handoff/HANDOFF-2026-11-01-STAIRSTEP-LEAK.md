# HANDOFF 2026-11-01：阶梯角漏光亮线（机制实证 + 修复已转正默认开启）

## 0. 一句话

地下/室内**不同高度的阶梯凹角**处沿方块棱的日/月色亮线，根因是 2026-10-02 注释记录的
同一几何缺陷（"相邻块四边形只在棱上共边，精度下可以从棱缝擦过去"）的**日/月直射版本**；
修复 = 太阳可见性的体素保守门（`vptVoxelPathBlocked` 复检），**已默认开启**，
显式关闭臂 = 档 33（`ORION_PERF_ABLATION=alt:33`，运行时 A/B，无需重编）。

## 1. 排除项（都做过受控 A/B，别回头）

| 臂 | 开关 | 结论 |
|---|---|---|
| 1 | `ORION_INDEXED_TERRAIN=0` | 亮线仍在 → 索引化 BLAS/共享 uint16 索引缓冲非触发器 |
| 2 | `ORION_PERF_ABLATION=alt:18`（noPom） | 仍在 → POM 非触发器（用户另证：不装材质包也有） |
| 3 | `alt:3`（noGi） | 仍在 → diffuse GI 非触发器 |
| 4 | `ORION_OPAQUE_SHELL_OFFSET=0.002`（法线外扩壳） | 粗亮条消失但出现凸棱 V 槽细线（外扩的固有副作用） |
| 5 | 外扩壳改平面内膨胀（0.003） | 细线仍在（成因见 §3，非壳可治） |

壳/膨胀实验代码仍在：`cpp/VulkanCore.cpp` uploadChunk（env 门控，默认 0 = 关），
备份 `VulkanCore.cpp.bak_pre_shelloffset_20261031`；`material_resolve.glsl` 的
`VPT_LOCAL_FACE_AXIS_EPS` 1e-3→5e-3（配合壳的位移面分类修复，壳关着也无害，保留）。

## 2. 用户三判据（机制必须同时满足）

1. 只出现在**不同高度的阶梯凹角**（共面交界没有）；
2. 亮度/颜色随游戏内时间变化（= 日/月光载体）；
3. 不装材质包也有（= 纯几何，与 PBR/POM 无关）。

## 3. 机制

- 阶梯凹角的共享棱上，日/月可见性射线**贴棱擦过** 1 格厚的阶梯实体，从另一侧
  穿出 → 误报"直射无遮挡" → 全额日/月直射打在角部 → 沿棱亮线。
- 共面交界不可见的原因：擦进板内的射线被板另一侧的面挡住（材质着色 face-forward，
  :1906 的 fixedNormal 翻转使背面命中着色为暗），而阶梯角贴棱穿过后到达的是真正
  朝向天空的通道。
- 2026-10-02 已用 `vptLocalFaceSetCosineBound` + `vptVoxelPathBlocked`（体素保守
  DDA，带贴棱/贴角判挡）治过**本地光**版本；本次把同一 DDA 接到**太阳可见性**。

## 4. 修复（已部署，四处 SPV 哈希一致）

`cpp/shaders/rt_test.rgen` 太阳软影循环内（shadowTransmittance 累加前）：
硬件报"通"时 `vptVoxelPathBlocked(hitPos, hitPos+shadowDir*16, 哨兵, 16)` 复检，
判挡则该样本按被挡处理。默认开启；`!= 33u` 为条件（档 33 原空闲）。
已知副作用（10-02 同类、可接受）：户外贴体素棱/角通过的太阳射线被保守判挡，
棱线附近直射可能偏暗一档。

⚠ 本次 SPV 变更 → 下次启动**冷编 ~15 分钟**属正常。

## 5. 实验轮里的错误记录（别重复）

- 体素 DDA 朝太阳走 32 格曾怀疑越界挂 GPU —— 实为用户自己在菜单界面关的游戏
  （FG MENU open=1 + 0 CPU = 菜单空闲，不是挂死）。判"挂死"前先确认有没有人在操作。
- 消融档 32 宿主侧名字 glossyBudgetOff 是**遗留空操作**（kGlossyRayBudget 已是
  10000，rt_test.rgen:18825），与太阳门无冲突。
- `cmd` bat 写中文注释会被 GBK 解析毁掉行结构（`set` 静默失效），启动脚本一律 ASCII。

## 6. 环境

- 启动：`_launch_direct.bat`（修复已默认，无需任何 env）；A/B 关闭臂 `alt:33`。
- 消融系统：120 帧/窗，窗名 `PERF_L10_ABLATION_STAGE_NAMES[34]`（VulkanCore.cpp:1709）。
- 日志：`run/rt_debug.log`（native）、`run/_client_err.log`（stderr）。
- 帧导出探针：无现成深度导出；诊断用消融档 + 截图像素采样（python PIL）。
