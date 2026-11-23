# HANDOFF 2026-10-25：第一人称手部渲染

> 背景调研见 `docs/first-person-and-overlays.md`（本篇是它的**执行版**，修掉了其中
> 一处基于仓库注释的错误推论，见 §2.1）。
>
> 一句话结论：**手走原版光栅不是缺陷，是常规做法，建议保留**；但仓库里那套"手部变换
> 诊断机"是**基于错误前提**写的，应当删除而不是修好。真正要做的是三条一致性打磨。

---

## 1. 先决策：要不要让手进光追

| 路线 | 做什么 | 收益 | 代价 |
|---|---|---|---|
| **A（推荐）** | 保留光栅手，修「手与 RT 场景不一致」（§4） | 观感一致：手不再比世界锐/不再无视雾与水下 | 小，纯 Java |
| **B** | 手进 RT（复用 ENTITY 层），关掉光栅手 | 手有 RT 阴影/GI/反射 | 中：MV 与 RR 尾影（见 §3），要能接受手持物品边缘在动画时糊 |
| **C** | B + 独立 viewmodel 相机（PerspectiveCorrection） | 边缘不变形、FOV 独立 | 大：等于自建一条 viewmodel 管线；Remix 用它是因为它把 viewmodel 也放进光追 |

**为什么 A 是常规解**：near-field、高细节、逐帧动画的 viewmodel，业界主流就是"在
上采样/降噪之后单独合成"，而不是丢进被降噪的场景里。我们现在的顺序
（世界 → RT 合成 → 手光栅 → GUI）恰好就是主流顺序。Remix 反过来（viewmodel 也光追），
是为了 showcase 画面里手持火把/铜信标的 RT 阴影，代价是它必须再配一套独立 viewmodel
相机 + 场景隔离。

## 2. 已核实事实

### 2.1 ⚠ 手部顶点**不需要任何旋转**（纠正仓库注释）

MC 26.3 `GameRenderer.renderItemInHand`（`GameRenderer.java:382-396`）在把手交给
`submitHandsWithItems` **之前**已经：

```java
PoseStack poseStack = new PoseStack();
poseStack.pushPose();
poseStack.mulPose(cameraState.viewRotationMatrix.invert(new Matrix4f()));  // view → world
...
this.firstPersonHandsAndItemsRenderer.submitHandsWithItems(partialTicks, poseStack, ...);
```

也就是说**提交出来的顶点已经是「世界朝向 + 相机相对」**——正好是实体层
（`EntityRtCapture.captureVertex` 1416 行注释："顶点是世界减相机，native 上传前才加回
相机"）的约定。所以：

- `handPhase` 里那套 `handRotation`（`VulkanCameraMixin:311` 写 `rotInv`）→
  `EntityRtCapture.captureVertex:1388-1408` 的 M·v / Mᵀ·v 旋转是**多余且有害**的；
- `handTransformMode` 的"三种候选按视线挑一个"启发式（`:1259-1265`、`:1218-1240`）
  建立在一个**不成立的前提**上（注释称"顶点是视图空间、没有相机旋转"）；
- 它现在没造成问题，只因为 `handPhase` 恒为 false（见 2.2）。

**结论：这套机件应当整段删除**，而不是"修好再启用"。

### 2.2 `handPhase` 是一条死链

- `beginHandPhase()` 定义在 `EntityRtCapture`，**全仓无调用者**；原调用它的
  `FirstPersonHandsAndItemsRendererMixin` 已从仓库删除（`grep` 可证）。
- 打开它会立刻产生两个副作用：
  1. **贴图被换成方块图集**（`beginModel` 的 handPhase 分支，注释自称"仅诊断用"）；
  2. 触发 2.1 那套有害旋转。
- `endHandPhase()` 仍在 `LevelRendererMixin` 的世界渲染 HEAD 被调用，但 handPhase
  恒 false ⇒ 空操作。2026-10-25 已在代码注释里整段标为死代码，**不要再接回来**。
- ⚠ 关键修正：`DynamicLightCapture` 原注释"捕获链路在本工程里其实是断的"**不准确** ——
  手一直都被 Java 侧捕获，断的是"上传"（见 §2.6）。

### 2.3 RT 合成点在手之前，且只写颜色

- 上膛：`GameRendererMixin.rtx$armRtInjectionBeforeHand`（`renderItemInHand` HEAD）。
  2026-10-25 修正：旧注释"明确**不把第一人称手放进光追**、让原版光栅画它"已不成立 ——
  手现在会进 TLAS，只是仍被随后绘制的光栅手盖住（§2.6）。
- 落笔：`VulkanCommandEncoderMixin.vulkan$injectRtBeforeGui` → 在下一个"颜色附件 == 主
  目标"的 `createRenderPass` 之前调 `NativeVulkan.recordFrameCommand(cmd, image, w, h, true)`。
- **只传 image、不传 depth** ⇒ RT 不写主目标深度。
- 手的光栅自带 `depthTextureView`（`GameRenderer.renderItemInHand` 形参）⇒ 手与
  **原版世界深度**正常遮挡；因为原版世界仍然光栅化过（只是颜色被 RT 覆盖），深度是完整的。

### 2.4 由此带来的可观测差异（这就是路线 A 要修的）

| 差异 | 原因 |
|---|---|
| 手比世界锐 | 世界是 707×400 内部渲染 + DLSS-RR 上采样，手是**输出分辨率**光栅 |
| 手的光照与世界不符 | 手用原版 lightmap（`submitArmWithItem` 传 `lightCoords`），世界是物理光 |
| 手无视体积雾/水下吸收 | 雾在 RT 里；手在合成之后画 |
| 手不受 RT 阴影影响 | 同上（贴墙时手仍亮） |

### 2.5 手部入口签名（26.3，已验证）

```java
public void submitHandsWithItems(float partialTicks, PoseStack poseStack,
        SubmitNodeCollector submitNodeCollector, PlayerRenderState playerState,
        FirstPersonHandsAndItemsRenderState state)
```
`net.minecraft.client.renderer.FirstPersonHandsAndItemsRenderer:325`。

### 2.6 ⚠ 已定案（2026-10-25 源码核对）：手一直被捕获，但**从未进过 RT**

结论：**手是有（几何、材质、LabPBR 解析都正常），只是完全不参与光追。**
不是遮挡，也不是没捕获 —— 是**帧窗口在手提交之前就关了**。

26.3 的实际顺序（反编译源码 `GameRenderer.java:628-669`）：

```
renderLevel() {
    levelRenderer.render(...)   // 世界：LevelRendererMixin HEAD 开窗
    render3dHud(...)            // → renderItemInHand(...)：手/手持物在这里提交
}
```

旧代码在 `LevelRenderer.render` 的 **RETURN** 就调 `NativeVulkan.endEntityFrame()`
（`LevelRendererMixin:373-379`），而手/手持物的 feature 批次要到之后的
`renderItemInHand` → `prepareFrame(handAndScreenSubmitNodeStorage)` 里才
`prepareGroup`（`RenderTypeFeatureRendererMixin` / `ModelFeatureRendererMixin`
的捕获点都在 prepareGroup 内）。于是：

- Java 侧照常捕获（slot 分配、`_n`/`_s` 解析、顶点打包全部执行）；
- `uploadEntityBatch` 撞上原生门禁 `if(!g_entityFrameOpen) return;`
  （`VulkanCore.cpp:58210-58214`）被**静默丢弃**；
- `g_entityFrameOpen` 只由 `beginEntityFrame` 置 true、`endEntityFrame` 置 false，
  而后者已经在世界阶段末尾执行过 ⇒ 手这一批必然被拒。

旁证：`[RTX-HAND]` 那套一次性诊断从来没打印过，因为 `beginHandPhase()` 无调用者
（`FirstPersonHandsAndItemsRendererMixin` 已从仓库删除），`handPhase` 恒 false。

#### 因此原推论全部作废

- ~~"手上剑没有 PBR 是遮挡问题"~~ ⇒ RT 里没有剑，没有东西被遮挡；
- ~~"看不见的手在挡 GI / 投暗影"~~ ⇒ 手从未进入 TLAS，不可能挡射线；
- ~~"B1 只需关掉光栅手"~~ ⇒ 只关光栅手会让手直接消失，必须先修帧窗口。

顶点空间（§2.1）与材质链的结论仍然成立，只是要等几何真正进 TLAS 之后才轮得到它们。

#### 已做的修复（2026-10-25）

- `EntityRtCapture.finishEntityFrame()`：Java 收窗 + `NativeVulkan.endEntityFrame()`
  一起收，幂等；
- `GameRendererMixin` 新增两处：
  - `renderItemInHand` 内 `FeatureRenderDispatcher.prepareFrame` 的 **INVOKE AFTER**
    ⇒ 手 batch 已准备完、且早于 `createRenderPass("Item in hand")`
    （RT 注入就发生在那个 render pass 的 `vkCmdBeginRendering` 之前）⇒ **同帧**进 TLAS；
  - `renderLevel` RETURN 兜底（第三人称 / 隐藏 HUD / 旁观帧）；
- `renderItemInHand` HEAD 补 `beginEarlyFrame()`（幂等），保证手提交前窗口是开的；
- `LevelRendererMixin` 的 render RETURN **不再**收实体帧（只保留粒子层）。

## 3. 若要做 B：真正的代价在 MV / RR，不在几何

手是**相机锁定**几何：屏幕位置只随动画（挥动/使用）变化。两种处理都有代价：

- **MV = 0**（相机锁定，符合 memory 14336369 的结论）：静止时完美；**挥手/吃东西时
  手在屏幕上移动而 MV 说没动 ⇒ DLSS-RR 会把手的边缘拖出尾影**。
- **给逐顶点 MV**：要拿到手上一帧的屏幕位置。手顶点是相机相对的，`delta = 0` 天然成立
  （相机相对坐标不变），所以要额外用"手的动画矩阵变化"造出 delta —— 现有
  `resolveBatchMotion` 是按实体世界位移算的，直接套会得到错误位移。

Remix 绕开这个问题的办法是 viewmodel 独立相机 + 在降噪之后合成 —— 也就是我们现在的
结构。**所以 B 的合理形态其实是"手进 RT、但只在静止镜头下开"，或者干脆不做。**

## 4. 路线 A 的具体改动（推荐，全部纯 Java，无 SPV）

1. **删死代码（防误启用）**：
   - 删 `handPhase` 分支里的贴图替换（`:1115-1128`）与 `handRotation`/`handTransformMode`
     旋转（`:1388-1408`）、相关字段与 `endModel` 的一次性判定（`:1218-1240`）、
     `VulkanCameraMixin:307-312` 的 `setHandRotation` 调用。
   - 保留 `beginHandPhase/endHandPhase` 作为**纯标记**（供将来 B 用），但清空副作用；
     或者直接删除并把 `DynamicLightCapture.java:31` 的注释一起更新。
2. **光照一致性**：在手部光栅之前，用本地光网格/太阳方向估一个乘子乘到手的光照上
   （用 `NativeVulkan` 已有查询接口或 CPU 侧 `DynamicLightCapture` 的采样结果）。
   注意这只是观感对齐，<u>不要</u>声称是物理正确。
3. **水下/雾一致性**：`Camera.isInWater` / `cameraState` 已知时，给手叠一层同色 tint
   （或在手 pass 之后做一次屏幕空间稀释）。判据：水里手不应像贴在镜头上。
4. **锐度一致性（可选）**：把手的光栅分辨率降到 DLSS 输出前的内部尺寸不现实（手画在
   主目标上），所以这条**只做记录不做改动**：用 F3 截图对比说明差异来源。

## 4b. 中间档：让手只出现在反射/倒影里（几乎免费，推荐在 A 之后考虑）

`instance.mask` 已经是一套成熟的分层机制（`cpp/VulkanCore.cpp:70499-70585`）：

| 位 | 含义 |
|---|---|
| `0x01` | 主相机射线 |
| `0x02` | **本地玩家代理：主射线看不到，但反射/GI/阴影（cull 0xff）都能打到** |
| `0x04` | 遮挡体积光（太阳阴影图的 cull 位） |
| `0x08` | 雨水可见度探针专用（粒子层清掉这一位） |

所以"手出现在水面倒影 / 玻璃反射里、但不参与主射线"= 把手批次按
`localPlayerProxy` 分类（`pad1 = 1`），让它进 `LOCAL_PLAYER_RT_LAYER_KEY`
（`VulkanCore.cpp:15310`；mask 在逐实例分类块 `70499-70585` 里定，本地玩家代理那一档
在 `70556-70559` 设成 `0x02`）。

要点：
- 光栅手照旧画在最上层 ⇒ **不参与降噪，没有 RR 拖尾风险**；
- 手会出现在反射里（物理上也该出现），还会挡住 GI 射线（地面出现一圈手影）——
  观察一局再定是否接受；不接受就把 GI 的 cull 也排除掉（改那个 class 的 mask）；
- **MV 必须强制 0**：手是相机锁定几何，屏幕位置不随相机变化，补 Δcam 是错的
  （见 memory 14336369 的同一条结论）。2026-10-25 核对：**已经是 0，不需要额外跳过** ——
  手臂批次的 `submit.state()` 是 `Unit.INSTANCE`，`resolveBatchMotion` 在
  `instanceof EntityRenderState` 处直接 return；手持物走 custom 链根本不调用它；
- 待核实：`LOCAL_PLAYER` 动态层每帧重建 BLAS 的成本（手是动画几何，顶点每帧变）。

## 5. 若要 B/C：改动清单（仅在被明确要求时执行）

B（手进 RT，共用相机）：
1. 新增 `FirstPersonHandsAndItemsRendererMixin`（`@Inject` 到 `submitHandsWithItems`
   HEAD/RETURN，`ci.cancel()` 不需要）→ `EntityRtCapture.beginHandPhase()/endHandPhase()`；
   同时在 `rtx-remix-renderer.mixins.json`（client）里注册。
2. **不**做任何顶点旋转（见 2.1）。
3. 手批次强制 `motion = 0`（在 `resolveBatchMotion` 里按 `handPhase` 跳过）。
4. 关掉光栅手：**不要**在 `renderItemInHand` 上 `ci.cancel()`。RT 注入的触发点就在这个方法
   内 `createRenderPass("Item in hand", ...)` 的 `vkCmdBeginRendering` 之前；整个方法 cancel
   会连 render pass 一起 cancel ⇒ RT 永不合成 ⇒ 画面只剩光栅世界。
   正确做法是 `@Redirect` `FeatureRenderDispatcher.renderAllFeatures(RenderPass, PreparedFrame)`
   为空（2026-10-25 已实现，见 `GameRendererMixin.rtx$skipRasterHand`，开关 `orion.handRt`）。
   这样 submit → prepareFrame（捕获 + 收帧）→ createRenderPass（RT 注入）→ 空 pass，
   少掉的只有"把批次画到屏幕"这一步。
5. 验证皮肤贴图槽：在 `beginModel` 打一行 `texture/slot` 日志，确认不是 0 且已 publish。

C（+独立 viewmodel 相机）：
6. 透视校正 = 视空间 XY 统一缩放 `k = tan(fovMain/2) / tan(fovViewModel/2)`，在手顶点捕获
   处（**在 rotInv 之前**，即 PoseStack 已含 rotInv 的话则需在提交端反算）逐顶点乘 ——
   注意 2.1 的结论：PoseStack 已含 rotInv，所以这个缩放必须在**世界朝向下**的 XY 上做，
   不能直接乘，需先转回视空间。**这是 C 最容易做错的一步。**
7. `fovViewModel` 作为设置项（默认 = 主 FOV，即不启用）。

## 6. 判据

- A：站水里 / 贴墙 / 暗室三处，手的亮度与世界观感一致（人眼判定）；截图对比说明差异
  缩小；**不动任何 SPV、不动原生**。
- B：挥手时手边缘无可见尾影（录 3 秒挥手）；贴墙时手被正确遮挡；第三人称切回正常。
- C：视野边缘手不变形（FOV 90 对比）。

## 7. 坑

1. **历史崩溃**：曾经"Item in hand 的 RT 注入"导致进世界 5 秒信号量超时，根因是
   `rt_test.rchit` 的 `#else` 分支没把 `gl_InstanceCustomIndexEXT/gl_PrimitiveID/gl_HitTEXT`
   写进 payload ⇒ 用上一条射线的残留索引查 `sectionVertexAddressBuffer`（无边界检查）⇒
   垃圾指针 ⇒ GPU 挂死。**任何把新几何塞进 RT 的改动都必须先确认 rchit 写了完整 payload。**
2. **`handPhase` 会 gate 本地玩家代理**（`:682` `if (handPhase) return false;`）——
   开窗前后的批次归属会变，B 里要一并复核第三人称的身体代理是否还在。
3. **第三人称/无手帧**：`rtx$armRtInjectionBeforeHand` 的兜底是 `GuiRendererMixin`
   （RT 在 GUI 前上膛）。给 `renderItemInHand` 加 `cancel` 不影响该兜底，但要确认
   无手帧不会出现"RT 没上膛"。
4. **手在 0.3~0.7 格**：接近近平面，RT 的 `tMin=0.001` 与 AABB 自遮蔽需要复核（B）。
5. **不要**用手部几何去试 POM/材质分层——手是实体材质路径，POM 只对地形 section 生效。

## 8. 回退

- 全部改动都是**纯 Java**（无 SPV、无原生）⇒ `git revert` / 还原 4 个文件即回。
- 运行时 A/B 开关：`-Dorion.handRt=0` 或环境变量 `ORION_HAND_RT=0` ⇒ 恢复光栅手
  （RT 里仍有手，只是被盖住）。默认 1 = 手进 RT + 关光栅手。
- ⚠ 旧计划里的"`ORION_HAND_RT` 默认 0 + `renderItemInHand` cancel"已废弃：
  ① 默认改为 1；② 关闭光栅手**不能**用 cancel（会丢 render pass ⇒ 丢 RT 注入），
  改用 `renderAllFeatures` 的 Redirect（见 §5.4）。
- `beginHandPhase/endHandPhase` 保留在代码里但**是死代码**（注释已封死）。
  不要在 B 里把它们接回来：顶点已是世界朝向，那套旋转/换贴图是错的（§2.1）。

## 9. 待核实（开工前先跑一遍）

1. **手现在到底有没有被捕获进 RT？** —— **已定案（2026-10-25）：捕获一直有，但在
   原生门禁 `if(!g_entityFrameOpen)` 处被丢弃**（详见 §2.6）。帧窗口已搬迁到
   `renderItemInHand` 的 `prepareFrame` 之后。现在这条改为验证"搬迁是否生效"：
   1) 进入世界后看 `[RTX-HAND]`/日志是否出现手批次（`handPhase` 恒 false，若仍等
      旧日志则永远看不到 —— 改为观察手在水边倒影里是否出现）；
   2) 站水边低头：倒影里应出现拿剑的手 + 剑的高光/法线（ENTITY mask `0xff`）；
   3) 视野里的地面若出现一小片跟随手移动的暗区 ⇒ 手已在 TLAS 里挡 GI（这是
      "搬迁生效"的第二个独立证据）；
   4) 关光栅手那一步**已于 2026-10-25 一并实现**（`renderAllFeatures` Redirect，
      `orion.handRt` 闸），所以默认状态下 RT 手应直接显形 —— 这一步同时暴露
      §2.1/§7 的验收项
      （皮肤槽、薄卡片双面、glint、共面层序）。
   **注意：第 2)~4) 任何一步"看不见"，先确认不是窗口又关早了，而不是去查贴图。**
2. `rtx-remix-renderer.mixins.json` 的确切文件名与 client 段结构（加新 mixin 用）。
3. `renderItemInHand` 的 descriptor（26.3 是
   `(CameraRenderState, PlayerRenderState, GpuTextureView)V`，见 `GameRenderer.java:375`），
   加 `cancellable` 时按字节码核对。
