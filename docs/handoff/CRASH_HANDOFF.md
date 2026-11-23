# RTX Mod GPU 卡死崩溃 —— 排查交接文档（2026-11-14）

## 0. 一句话结论

游戏跑 2~20 分钟后 **GPU 硬挂死**，表现为 `5s VK semaphore timeout`，进程以 `0xC0000409` 被杀。
**卡死点是渲染线程在 `recordFrameCommand` 内部**（探针实测 `framePhase=3`：世界几何重建之后、本帧录制尚未返回），
**"原地不动不崩，一移动就崩"**。已排除 6 个假设，仍未定位到具体调用。

---

## 1. 环境与操作方式

| 项 | 值 |
|---|---|
| 工程根 | `d:/fabric-example-mod-26.2/fabric-example-mod-26.2` |
| 版本 | Minecraft 26.3 / Fabric / 自制 Vulkan 后端（`com.mojang.renderpearl`） |
| 我们的原生库 | `rtx_backend.dll`，源码 `cpp/backend/*.cpp`，着色器 `cpp/shaders/*.glsl` → SPV |
| 构建 DLL | `cmake --build cpp/build --config Release --target rtx_backend` |
| 部署 DLL（三处，缺一不可） | `run/`、`src/main/resources/natives/windows/`、`build/resources/main/natives/windows/` |
| 启动 | `gradlew.bat runClient -PquickPlay=world`（直接进 `world` 存档） |
| 客户端日志 | `run/_client_<HHmmss>.log` |
| RT 日志 | `run/rt_debug.log`（行格式 `[CPP] HH:MM:SS ...`） |
| 崩溃报告 | `run/crash-reports/crash-*.txt`、`run/hs_err_pid*.log` |

### 三个必须知道的坑（我已踩过，全是自己造成的）

1. **`extern "C"` 与 `JNIEXPORT` 之间绝对不能插声明**。
   我把 `std::atomic<int> g_rtFramePhase{0};` 插在两者之间 → 导出名变成 C++ 修饰名
   `?Java_com_example_vulkan_NativeVulkan_recordFrameCommand@@YAX...` → JNI 解析不到 →
   `UnsatisfiedLinkError` → 游戏在 `Initializing game` 直接崩（看起来像"游戏起不来"）。
   部署前务必验证：
   ```
   dumpbin /EXPORTS cpp/build/Release/rtx_backend.dll | findstr "?Java_com_example"
   ```
   期望 **0 条**；`Java_com_example_*` 应为 202 条。
2. **绝不能同时跑两个客户端**，它们写同一个 `rt_debug.log`，帧号会交错（见过 `frame=56` 与 `frame=31749` 混在一起），
   所有时序结论都会失效。启动前先 `Get-Process javaw,java` 确认没有 Minecraft 窗口进程，再拷 DLL、再启动。
3. **改 SPV 或改 RT 管线创建标志 → 8~17 分钟冷编**。
   注意：`ORION_DISABLE_OMM=1` 也会改管线 key（`VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT`），
   做 A/B 时误用了这个开关，白等了一次冷编。

---

## 2. 崩溃现象（原文）

`run/crash-reports/crash-2026-11-14_16.26.32-client.txt`（同类共 8+ 次）：

```
Description: Render Frame
java.lang.IllegalStateException: 5s timeout reached when waiting for VK semaphore: Queue 0x...
 TOP_OF_PIPE = BEGIN_RENDER_PASS Item in hand
 BOTTOM_OF_PIPE = BEGIN_RENDER_PASS Item in hand
  at VulkanCommandEncoder.submit(VulkanCommandEncoder.java:222)
  at FrontendCommandEncoder.submit(...)
  at Minecraft.renderFrame(Minecraft.java:1409)
```

注意：**`Item in hand` 是误导**。它永远是一帧的最后一个 render pass，提交超时自然标在它头上。
用 `-Dorion.handRt=0`（走光栅手，不注入 RT）测试，**照样崩** ⇒ hand pass 不是原因。

`hs_err_pid24068.log` / `hs_err_pid22628.log` 的 native 栈（另两次崩溃）：

```
C [nvoglv64.dll+0xeaa3a6] ← vulkan-1.dll ← rtx_backend.dll+0x2f2ac
  ← recordFrameCommand ← VulkanCommandEncoder.createRenderPass ← GameRenderer.renderItemInHand
```

---

## 3. 已建立的探针（都在树里，可直接用）

### 3.1 GPU 卡死看门狗（`Platform.cpp` / `OrionInternal.h` / `Frame.cpp`）

- `void rtHangHeartbeatTick()`：每帧调用一次（在相机 UBO 的 JNI 入口，`Frame.cpp` 里 `if (!g_cameraUboMapped) return;` 之前）。
  记录心跳与录制线程 tid；首次调用时起一个 detached 线程。
- 看门狗线程：每 250ms 检查心跳，**停滞 ≥3s** 打印一次快照（`g_rtHangReported` 保证只打一次）。
- 快照内容（`rtDumpHangState`）：
  ```
  [GPU HANG] watchdog stalledMs=...
  [GPU HANG] heartbeat stalled frame=<帧号> vramUsedMB=.. vramBudgetMB=.. vramRatio=..
  [GPU HANG] freed[-1] addr=0x.. bytes=.. tag=sectionBLAS|sectionVB frame=<释放帧号>   ×12（新→旧）
  [GPU HANG] <栈回溯（当前不可用，见 3.4）>
  [GPU HANG] framePhase=<0|2|3>
  [GPU HANG] vli capacity=.. nextPage=.. livePages=.. freePages=.. pending=..
             governorEvicted=.. governorIgnored=.. limit=.. poolBytes=..
  [GPU HANG] instances=.. danglingRefs=..（TLAS 实例表指针可用时才打）
  [GPU FAULT] ...（VK_EXT_device_fault，引擎未启用，通常只有一行 "not enabled"）
  ```
- `void rtGpuFreedRangeNote(uint64_t addr, uint64_t bytes, const char* tag)`：
  在 `destroySectionASImmediate`（`Sections.cpp`）里调用，记录 512 条环形缓冲。
- **`g_rtFramePhase`**：`0`=帧外、`2`=几何重建前、`3`=几何重建后（`Frame.cpp` 两处 `rebuildWorldGeometry(...)` 前后打点）。

### 3.2 交互延迟探针（Java，另一条线，与本次崩溃无关但已完成）

`src/client/java/com/example/client/latency/InteractionLatencyProbe.java`
+ mixin：`MultiPlayerGameModeProbeMixin`、`ClientPacketListenerProbeMixin`、
`SectionRenderDispatcherProbeMixin`、`LevelExtractorProbeMixin`、`SectionUpdateRenderStateProbeMixin`、`ClientLevelProbeMixin`
输出 `[LATENCY]`（use→ack / ack(applied) / action→dirtyMark / action→meshDone）与 `[LATENCY-Q]`（队列长度、空闲 pack）。
**结论（已定案）**：交互链路健康（`action→dirtyMark` 0~2ms、`action→meshDone` 4~12ms、`ack(applied)` 19~69ms）。
方块几秒才出现的原因在 **RT 侧**，不是原版网格链路。

### 3.3 其他打点

`[SL FRAME SPLIT] tokenAvgMs/sleepAvgMs`（`Streamline.cpp`）：**in-world `sleepAvgMs=5.79 / max 17.65`**，
证明每帧 5~6ms 的 `beginStreamlineMs` 就是 **Reflex 限帧器在睡**（`frameLimitUs=6060`）。
`[RT OMM GATE] deferred=/created=/ommMsThisFrame=`（`WorldGeometry.cpp`）。

### 3.4 已知失效的探针

`rtDumpThreadStack`（StackWalk64 + DbgHelp）**没用起来**：
`SymInitialize` 返回 `ERROR_INVALID_PARAMETER(87)`（试过 `GetCurrentProcess()` 与 `OpenProcess`，都失败）。
→ **下一步最值得做的就是把它修通**（或改用 MiniDumpWriteDump），卡死线程的调用栈能一锤定音。
`llvm-symbolizer.exe` 在本机可用（`C:\Program Files\Microsoft Visual Studio\...\Hostx64\x64\llvm-symbolizer.exe`），
且 `rtx_backend.pdb` 与 dll 同目录，所以只要能打出 `模块+RVA` 就能离线解符号。

---

## 4. 实测数据（关键几条）

最后一次崩溃（20:34:10）完整快照：

```
[GPU HANG] heartbeat stalled frame=6044 vramUsedMB=4618 vramBudgetMB=7180
[GPU HANG] freed[-1] addr=0x33418862080 bytes=635392 tag=sectionBLAS frame=6044   ← 同一帧
[GPU HANG] freed[-2] addr=0x33417068544 bytes=1793520 tag=sectionVB  frame=6044   ← 同一帧
[GPU HANG] freed[-3..-12] frame=6040/6036/6034/6032/6028（每 2~4 帧释放 1.8MB VB + 0.6MB BLAS）
[GPU HANG] framePhase=3
[GPU HANG] vli capacity=3253 livePages=2482 freePages=0 governorEvicted=96 governorIgnored=222 limit=3253
```

其它局：`framePhase` 恒为 `3`；VLI 恒为 `freePages=0`；释放记录恒与挂死帧相差 0~15 帧。

---

## 5. 已排除的假设（全部实测，不要重复试）

| # | 假设 | 实验 | 结果 |
|---|---|---|---|
| 1 | 手部 RT 注入（`Item in hand` pass） | `-Dorion.handRt=0` | **仍崩** |
| 2 | 存档几何太大 | 换小存档 `craftcollection2` | **仍崩**（1 分钟） |
| 3 | VLI 页池驱逐 | `ORION_VLI_NO_EVICT=1`（governorEvicted=0 确认生效） | **仍崩** |
| 4 | VLI 页池预算不足 | `ORION_VLI_BUDGET_FRACTION=0.12 / 0.20` | **仍崩** |
| 5 | 即时销毁 AS（无围栏证明） | `destroySectionAS` 改为延迟释放 + `[RT RETIRE]` 计数 | 该路径触发 **0 次**，仍崩 |
| 6 | 退休帧龄不足（GPU 在飞帧引用） | 帧龄 3 → **8**，环 6 → 12 → 256 → **2048** | 仍崩；释放记录仍与挂死同帧 |
| 7 | section 显存生命周期（终极验证） | `ORION_NO_SECTION_FREE=1`（**完全不释放，只记录**） | **仍崩** ⇒ 生命周期彻底排除 |
| 8 | OMM 闸门（我曾改的控制流） | `ORION_OMM_MS_PER_FRAME=0` 关闸门 | **仍崩** |
| 9 | 原版 `rchit` payload 播种缺失（2026-10-21 旧根因） | 读码 | 五行播种仍在（`rt_test.rchit:334-345`），**未回归** |

`ORION_NO_SECTION_FREE=1` 这一条最关键：**不释放也崩 ⇒ 不是"释放了还在用的内存"**。
所以 `freed[-1] frame=挂死帧` 只是"移动时频繁重建/释放"的相关性，不是因果。

---

## 6. 目前仍成立的线索（下一步从这里查）

1. **必须移动才崩**（用户实测：原地不动稳定，走几步必崩）⇒ 依赖 chunk 流式加载/卸载 + 重捕获 + TLAS 重建。
2. **`framePhase=3`**：卡在 `rebuildWorldGeometry()` 之后、`recordFrameCommand` 返回之前的某处。
   这段里的顺序（`Frame.cpp` 1900~2700 行）：
   `rebuildWorldGeometry` → `writePerfL10GpuBoundary(VLI)` → `recordVLIUpdates(cmd)` →
   `writePerfL10GpuBoundary(FAR)` → setup/RR（`slDLSSD*`）→ … → 函数返回（**本函数不提交，命令缓冲由引擎提交**）。
   → 建议在 `recordVLIUpdates` 前后、RR 之前各加一个 phase 号（4/5/6），把 `framePhase=3` 再切一刀。
3. **VLI 满容量**（`capacity=3253 livePages≈2300~2500 freePages=0`）在每次挂死时都出现，
   但 `NO_EVICT` 也崩 ⇒ 可能是"页池满 → 某些段走 ignore/降级路径"引入的不一致，而不是驱逐本身。
   注意 `livePages < capacity` 却有 900+ 页既不在 live 也不在 free（疑似泄漏/账本不同步），值得单独查。
4. **每 2~4 帧就有 1.8MB VB + 0.6MB BLAS 的段在重建** ⇒ 移动时 section 重建churn 极高，
   可查是否重复标脏（同一 key 反复重建）。若属实，减少 churn 本身可能绕过崩溃。
5. `vramRatio` 65~78%，未触顶；`oomSkips=0`。

---

## 7. 当前代码状态（今天改过、已编译部署的部分）

| 文件 | 改动 |
|---|---|
| `cpp/backend/OrionInternal.h` | `SECTION_RETIREMENT_RING_SIZE 3 → 2048`；`SectionRetirementSlot` 增加 `retireFrameSerial`；新增 `rtHangHeartbeatTick` / `rtGpuFreedRangeNote` / `rtLogVliStateForHang` / `g_rtFramePhase` 声明 |
| `cpp/backend/Sections.cpp` | `destroySectionAS` → 延迟释放包装（待销毁表 + 3 帧龄 + 512 上限），原函数改名 `destroySectionASImmediate`；`kRetirementMinFrameAge = 8`；`flushPendingSectionDestroys` 在 `reapSignaledRetirements` / `releaseAllRetirementSlots` 调用；`ORION_NO_SECTION_FREE` 实验开关 |
| `cpp/backend/Platform.cpp` | 看门狗线程、释放地址环形记录、TLAS 悬垂检查、`rtDumpThreadStack`（**不可用**） |
| `cpp/backend/Frame.cpp` | `g_rtFramePhase` 定义（**必须在 `extern "C"` 之外**）+ phase 打点 + 心跳调用 |
| `cpp/backend/WorldGeometry.cpp` | OMM 闸门从"每帧 N 段"改为"每帧累计 OMM 耗时预算"（`ORION_OMM_MS_PER_FRAME` 默认 3.0）；**控制流已退回** `requeueSectionsFrom` + `sectionAbortedMidway`；`[RT OMM GATE]` 打点 |
| `cpp/backend/Streamline.cpp` | `[SL FRAME SPLIT]`：拆分 `slGetNewFrameToken` 与 `slReflexSleep` 耗时 |
| `build.gradle` | 新增 `-Dorion.handRt=${handRt}` 桥接 |

**注意**：这些改动**都没有解决崩溃**，但 `[GPU HANG]` 快照 + `framePhase` 是有效资产，别删。

---

## 8. 建议的下一步（按性价比排序）

1. **修通卡死线程的栈回溯**（最高价值）。可选方案：
   - `SymInitialize(process, searchPath, FALSE)` + `SymLoadModuleEx`，或改用 `MiniDumpWriteDump` 落 `.dmp` 后离线看；
   - 或直接在崩溃前用 `EnumProcessModules` + 手动 `RtlVirtualUnwind` 只打印 `rtx_backend.dll+RVA`（不需要 dbghelp）。
   拿到 RVA 后用 `llvm-symbolizer --obj=rtx_backend.dll 0x<RVA>` 解出函数名。
2. 若拿不到栈：在 `framePhase=3` 区间内继续细分（`recordVLIUpdates` 前后、RR 之前、返回之前）。
3. 查"每 2~4 帧就重建同一批 section"的 churn 来源（`[RT PHASE SLOW]` / `[RT GEOMETRY]` 里 captured/sections 计数），
   很可能是移动时反复标脏 + VLI 降级共同造成的状态不一致。
4. 复现脚本（用户已确认可用）：`/time set day` 可瞬间复现；或走动 10~20 秒。

---

## 9. 复现与取证命令（Windows PowerShell）

```powershell
# 启动（确保没有 Minecraft 进程在跑）
cd d:/fabric-example-mod-26.2/fabric-example-mod-26.2
$ts=Get-Date -Format "HHmmss"
Start-Process cmd.exe -ArgumentList "/c set ORION_RT_PIPE_HOT_RELOAD=1&& gradlew.bat runClient -PquickPlay=world > run\_client_$ts.log 2>&1" -WindowStyle Hidden

# 崩溃后看快照
Select-String -Path run/rt_debug.log -Pattern "GPU HANG" | Select-Object -Last 20
Get-ChildItem run/crash-reports | Sort-Object LastWriteTime -Descending | Select-Object -First 1
```
