# Orion A2/A4 Rust 移植规格（只读提取）

- 日期：2026-11-17。提取方式：逐行读码，只读，未改任何文件。
- **版本注记（重要）**：`SectionCompilerMixin.java` 在本次提取会话内被另一进程并发修改（2026-11-17 12:24:00，2447 → 2597 行，改动是 `onCompileReturn` 内新增 Rust VPT 分支：`RUST_VPT` 常量 :44、`accumulateVptAlbedoRust` 调用 :429-444 与包装方法 :1711，以及相关常量后移）。**本文全部行号以 12:24 修改后版本为准**；任务给出的 `:890-1000` / `:1359-1672` 对应修改前版本（旧 fill=890 / tag=930-1305、旧 cull=1359-1515），四个候选内核函数内容逐字未变，仅行号后移约 27–30 行。
- `EntityRtCapture.java` 未被修改（最后修改时间 2026-11-17 08:53:15，3428 行），本文行号即当前行号。
- 参照范式：`rust/orion-farfield/src/lib.rs`（`off_*` 前缀导出、`catch_unwind` → 错误码、ABI 版本门）、`HANDOFF-2026-11-17-DH-OPT-AND-RUST.md` §8、`src/client/java/com/example/vulkan/RustBridge.java`（FFM 桥 + 版本门 + DLL 资源提取）。
- 相对路径均省略前缀 `fabric-example-mod-26.2/`。

---

## 候选一（A2）玻璃内面剔除 + 半透明标记/填充

文件：`src/client/java/com/example/client/mixin/SectionCompilerMixin.java`

### 1.1 函数清单（当前行号）

| 函数 | 行号 | 作用 |
|---|---|---|
| `fillUntaggedTranslucentQuads` | 920-957 | 逐**顶点**兜底写 marker=2 |
| `tagTranslucentQuads` | 960-1332 | 逐 quad 决定 RT 标记 + 玻璃 pad 位（原地） |
| `cullInteriorGlassFaces` | 1389-1545（javadoc 1380-1388） | 拷进 scratch 后原地压缩玻璃内部面 |
| `isInteriorSharedGlassFace` | 1360-1379 | 内面剔除判定 |
| `glassInteriorCullEnabled` | 1340-1344 | `-Drtx.glassInteriorCull=false` 关闭，默认开 |
| `glassCullScratch` | 1346-1358 | ThreadLocal 直接内存 scratch，只增不减 |
| `SC_GLASS_CULLED_VERTS` | 1334-1335 | 剔除顶点数累计（当前**只写不读**，死统计） |
| `isGlassFamilyMarker` | 783-786 | `marker >= 2 && marker <= 19` |
| `getTranslucentMarker` | 667-670 | `rtStateCache` bits 15..19 |
| `resolveTranslucentMarker` / `resolveTrackedGlassMarker` | 675-720 / 721-760 | 方块 → 标记映射（玻璃 2、染色玻璃 3+dyeId 0..15、染色玻璃板同、TINTED_GLASS=19、ICE=20、SLIME=21、HONEY=22、NETHER_PORTAL=24） |
| `isFlatGlassOwner` / `isPlainGlassBlock` | 798-813 | 探针侧是 `GLASS`/`GLASS_PANE` |
| `computeGlassConnectMask` | 823-919 | 4 边连接位掩码（体素探针） |
| `isConnectableGlass` | 790-793 | trackedGlassMarker≥2 或 TINTED_GLASS |
| `rtStateCache` | 85-110 | BlockState 注入缓存（`RtStateCache` duck 类型，bits：glass<<0 \| fill<<5 \| translucent<<15 \| valid<<20，常量 :77-82） |
| 标记/位常量 | 135-149 | `RT_MARKER_GENERIC_TRANSLUCENT=2`、`GLASS_CONNECT_MASK_SHIFT=8`、`GLASS_CONNECT_MASK_BITS=0x0F`、`GLASS_FLAT_CUBE_BIT=0x10`、`GLASS_PAD_CLEAR=~(0x1F<<8)` |

### 1.2 输入缓冲精确布局

调用链传入 vanilla `MeshData.vertexBuffer()`（`onCompileReturn` :397 附近取自 `results.renderedLayers`），直接内存、native order（小端）；三个内核都以 `duplicate().order(ByteOrder.nativeOrder())` 读写（fill :935-941、tag :993-999、cull 预读 :1408-1410）。**stride 硬门 == 28**（fill :924、tag :998、cull :1402）。

| 偏移 | 类型 | 含义 | 证据 |
|---|---|---|---|
| +0/+4/+8 | f32×3 | 顶点 x,y,z（节本地坐标；cull 用 `\|center\|≤32` 兜底 :1534-1537） | fill/tag/cull 读点 :1050-1058、:1446-1450 |
| +12..+15 | 4B | 本 mod 代码未触碰（推测为 vanilla 颜色 RGBA，**仅推断**，代码无引用） | — |
| +16/+20 | f32×2 | u,v（tag 读用于连接掩码与 sprite 查表；cull 不读） | :1059-1066、:1248-1255 |
| +24..+27 | i32（LE 4B） | vanilla 光照打包，被带外复用：**byte+27（int 最高字节）= RT 标记字节**；**int 的 bits 8..12 = 玻璃 pad** | 写 pad :1319-1323；写 marker :1307-1310 |
| pad 写法 | | `putInt(base+24, (light & GLASS_PAD_CLEAR) \| (pad << 8))`，pad = connectMask(4bit) \| flat?0x10 | :1319-1323、:1262-1266 |

quad 组织：`vertexCount % 4 == 0` 是 tag/cull 硬门（fill :932 无此门，纯逐顶点）。所有"已标记"判定统一读 **quad 首顶点**的 byte+27（tag :1026-1029、cull 预扫 :1417、cull 主循环 :1457）。

### 1.3 逐函数伪代码（与 Java 逐行对应）

**A. fillUntaggedTranslucentQuads（:920-957）—— 原地、逐顶点、无 MC 耦合**
```
guards: buffer==null || !isDirect || stride!=28 || count<=0 → return 0
vertices = buffer.duplicate().order(native)
tagged = 0
for v in 0..count:                       # 注意：按顶点，不按 quad
    off = v*28 + 27
    if (vertices.get(off) & 0xFF) == 0:  # 只填 0，不覆盖任何已有标记
        vertices.put(off, 2)             # RT_MARKER_GENERIC_TRANSLUCENT
        tagged++
return tagged
```

**B. tagTranslucentQuads（:960-1332）—— 原地、逐 quad、MC 耦合最重**
```
guards: region==null || buffer==null || !isDirect || stride!=28 || count<=0 || count%4!=0 → return 0
vertices = buffer.duplicate().order(native)
(minX,minY,minZ) = sectionPos.minBlock*
for quad in 0..count/4:
    preMarker = vertices.get(quad*4*28+27) & 0xFF
    if preMarker!=0 && !isGlassFamilyMarker(preMarker): continue   # :1028-1029 非玻璃族已标记 → 跳过
    for v in 0..3: 读 x[v],y[v],z[v](base+0/4/8), u[v],v[v](base+16/20)   # :1048-1071
    center = (Σxyz)*0.25                                            # :1073-1092
    e1 = P1-P0; e2 = P2-P0; n = e1×e2                               # :1094-1119（float 域）
    if |n|² <= 1e-12: continue                                      # :1121-1124 退化 quad 原样保留，随后被 A 兜底标 2
    n̂ = n * (1/sqrt(|n|²))                                          # :1126-1140（float 域）
    A = min + floor(center - n̂*0.01); B = min + floor(center + n̂*0.01)   # :1142-1191 法线两侧体素
    if preMarker != 0: marker = preMarker; connectCandidate = true  # :1193-1203
    else:
        marker = getTranslucentMarker(state@A)                      # :1205-1211 rtStateCache bits15..19
        if marker < 2: marker = getTranslucentMarker(state@B)       # :1213-1219
        connectCandidate = marker >= 2
        if marker == 0:                                             # :1224-1241
            marker = PBRAtlasStorage.spriteIsSeeThrough(u0,v0) ? 2 : 0    # PBRAtlasStorage.java:127-131，UV→OMM 网格查表
            connectCandidate = false
    if marker >= 2:                                                 # :1252
        glassConnect    = connectCandidate && isGlassFamilyMarker(marker)  # :1266-1269
        flatGlassWindow = marker==2 && isPlainGlassBlock(state@A 或 @B 的 block)  # :1271-1276, :798-817
        connectMask = glassConnect ? computeGlassConnectMask(...) : 0   # :1278-1291
        #   computeGlassConnectMask :823-919：4 条边 edge=0..3，a=edge,b=(edge+1)&3
        #   mid = 边中点；按 |Δu|<=|Δv| 分：u 轴位 edgeU 距 minU 近→0x1 否则 0x2；v 轴位→0x4/0x8  # :838-856
        #   step = mid-center 吸附主轴 ±0.5（ax>=ay>=az 比较）              # :858-880
        #   probe = min + floor(mid+step)；probe==自身格跳过；isConnectableGlass(state@probe)→置位  # :882-911
        pad = connectMask | (flatGlassWindow ? 0x10 : 0)            # :1293-1296
        for v in 0..3:
            if !markerAlreadyWritten: vertices.put(vBase+27, (byte)marker)  # :1306-1311
            if pad != 0: light=vertices.getInt(vBase+24)
                         vertices.putInt(vBase+24, (light & GLASS_PAD_CLEAR) | (pad << 8))  # :1313-1324
        matched++
return matched
```
每 quad 方块态读上限：2(getTranslucentMarker) + 4(connect) + 2(isFlatGlassOwner) = 8 次 `region.getBlockState`。

**C. cullInteriorGlassFaces（:1389-1545）—— 拷贝后原地压缩**
```
out[0] = null
guards: !glassInteriorCullEnabled() || region==null || buffer==null || !isDirect
        || stride!=28 || count<=0 || count%4!=0 → return -1        # :1399-1406（-1 = 未动，调用方沿用原缓冲）
预扫: 统计玻璃族 quad（首顶点 byte+27），>=2 才继续，否则 return -1     # :1414-1424
byteCount = count*28
out = glassCullScratch(byteCount); out.put(source); out.position(0)    # :1426-1434 整块拷贝
for quad in 0..count/4:
    marker = out.get(quad*4*28+27) & 0xFF
    drop = false
    if isGlassFamilyMarker(marker):                                   # :1457
        center/edge/normal 同 B（float 域）                            # :1459-1489
        if |n|² > 1e-12:
            A = min + floor(center - n̂*0.01); B = min + floor(center + n̂*0.01)  # :1491-1513
            if |cx|<=32 && |cy|<=32 && |cz|<=32:                        # :1515-1518
                drop = isInteriorSharedGlassFace(region, A, B)          # :1519-1520
    if drop: culled++; continue                                       # :1525-1528
    if writeQuad != quad:
        memCopy(out + quad*4*28, out + writeQuad*4*28, 4*28)          # :1530-1535 write<=read 同缓冲前向拷贝安全
    writeQuad++
if culled == 0: return -1                                             # :1540
newCount = writeQuad*4
out.limit(newCount*28); out[0] = out
SC_GLASS_CULLED_VERTS += count - newCount                             # :1542
return newCount
```
`isInteriorSharedGlassFace`（:1360-1379）：`A==B → false`；`stateA==stateB || stateA.block != stateB.block → false`；`return stateA.isCollisionShapeFullBlock(region,A) && stateB.isCollisionShapeFullBlock(region,B)`。即**只剔除「同方块、不同 BlockState、双方完整方块」的共享面**（:1372、:1376-1377）。

### 1.4 调用点、线程、频度、量级

- 入口：`onCompileReturn`（:356-357，`@Inject(method="compile", at=@At("RETURN"))`，注入 `SectionCompiler.compile(SectionPos, RenderSectionRegion, VertexSorting, SectionBufferBuilderPack)`）。
- 时机：**每节重建执行一次**（区块初次上传、方块/光照变更触发的重编），非每帧。`FarFieldGate.shouldSuppress(sectionPos)` 为真时整段跳过（:362-365）。
- 次数：遍历 `results.renderedLayers`（:394），**每个层名含 "TRANSLUCENT" 的层各调一轮** tag(:412-419) → fill(:420-426) → cull(:470-481)（通常即半透明层 1 次；26.2 层枚举中有几个名字含 TRANSLUCENT 未核对）。
- cull 结果 >=0 时：`vertexCount = scCulledVerts`，上传指针改指 scratch 缓冲（:483-489），随后走 `SectionUploadDedup` 去重 + `NativeVulkan.uploadChunk(...)`（:521-528）。**tag/fill/cull 的输出直接决定上传内容**。
- 线程：节重建 worker 线程（证据：`SECTION_SCRATCH` :123-124 与 `GLASS_CULL_SCRATCH` :1337-1338 均为 ThreadLocal；"线程本地 scratch"注释 :1382-1383。vanilla 侧 compile 在渲染工作线程执行——此点基于 vanilla 行为，未对 26.2 反编译核对）。
- 量级：单节半透明层顶点数一般数千以内（16³ 节），单次扫描 O(vertexCount)；tag/cull 耗时由 `scTagNanos` 计时（:410-427）。

### 1.5 MC 对象耦合与前置方案（能否前置成数组：能）

| 耦合点 | 位置 | 每 quad 上限 | 前置方案 |
|---|---|---|---|
| `region.getBlockState`（读 marker/full-block/glass 位） | tag ≤8 次、cull 2 次/玻璃 quad + 2 次 `isCollisionShapeFullBlock` | 见上 | 重建前把节周边体素（探针全部落在节内 ±1 格；quad 越界由 `\|center\|≤32` 兜底）打成 4 张平坦数组：`translucentMarker`(5bit)、`connectableGlass`(1bit)、`fullBlock`(1bit)、`blockId`(u16)，Rust 侧全部化为下标访问 |
| `rtStateCache`（BlockState 注入缓存） | :85-110 | — | 前置时照常走（Java 侧算好写入体素数组） |
| `PBRAtlasStorage.spriteIsSeeThrough(u,v)` | :1248-1255；access/PBRAtlasStorage.java:127-131 | 1 次（仅 marker==0 分支） | UV→布尔可前置为 32×32 网格快照或调用方预计算 per-quad 布尔 |
| `SectionPos.minBlock*` | :1000-1005、:1437-1439 | — | 3 个 i32 入参 |
| `BlockPos.MutableBlockPos`/`SectionScratch` | 局部便签 | — | Rust 侧不需要 |
| `isCollisionShapeFullBlock`（vanilla 形状查询） | :1376-1377 | 2 次/玻璃 quad | 前置为 fullBlock 位；结果可按 BlockState 缓存 |

结论：几何（叉积/归一化/floor/位包）全部纯计算；MC 耦合 100% 可前置。与既有 `off_vpt_plan`/`off_vpt_build`（lib.rs:516/:669，28B stride 地形缓冲）同型，且本文件已实接 Rust VPT（`RUST_VPT` :44、`accumulateVptAlbedoRust` :429-444/:1711，`-Dorion.rustVpt`，失败回退 Java）——A2 可按同一形状加 `off_glass_tag` / `off_glass_cull`。

### 1.6 错误/边界行为

- 全守卫早退，无异常路径依赖：stride≠28、count≤0、count%4≠0（fill 无 %4 门）、非直接缓冲、region null → tag/fill 返回 0（无害）、cull 返回 -1（沿用原缓冲）。
- cull 的返回值语义：`-1` = 未动；`>=0` = `outBuffer[0]` 有效，调用方**必须**换指针与新顶点数上传。
- scratch（:1346-1358）按需扩容、永不缩水，ThreadLocal 保活；无越界可能（读写偏移均由 count*28 界定）。
- 次序耦合：**tag 先于 cull 执行，cull 读的 marker 正是 tag/fill 写的字节**（:412-481 同层顺序）；Rust 移植必须保持该顺序与逐位写序。
- 多线程安全：所有可变状态为 ThreadLocal 或方法局部；`SC_GLASS_CULLED_VERTS` 原子累加（当前无消费者）。

### 1.7 可移植性判定与收益（A2）

- **判定：可移植（plan/build 两段式，前置体素属性数组后 Rust 内核零 MC 耦合），但收益分化，整体置信：高（代码层）/低（收益层）。**
- tag（:960-1332）是三者中最重：每 quad 全量读 7 个 float + 叉积/归一化/floor + ≤8 次方块态读；前置数组本身有 Java 侧一次性成本，收益需对拍实测。
- cull（:1389-1545）门槛预扫每节只读 1 byte/quad（:1414-1424），非玻璃节近乎零成本；真正重活仅在玻璃密集节。**关键疑点**：剔除条件要求「同 block 不同 state 且双方完整方块」（:1372、:1376-1377），原版玻璃族基本是单 state 方块（且原版 `TransparentBlock.skipRendering` 已剔除同方块共享面——此句为推断，未对 26.2 反编译核对），该条件在原版世界**可能恒 false** → `culledQuads==0` → 恒 return -1，内核长期空转。**建议先用运行时统计（SC_GLASS_CULLED_VERTS，目前只写不读，需加打印）验证后再定优先级。**
- 风险：marker 字节与 light pad 是 MeshData 原缓冲上的带外复用；Rust 版必须逐位复刻写序（byte+27 与 light int bits 8..12），否则污染原版光照。

---

## 候选二（A4）实体批次 MV 循环（历史差分 / 3×4 变换 + 历史回写）

文件：`src/client/java/com/example/client/EntityRtCapture.java`（未被并发修改）

### 2.1 位置与调用链

- 内核位于 `uploadBatch(ActiveBatch batch)`（:1230）内，:1260-1292 为前置手部位姿增量块，:1295-1344 为逐顶点 MV 循环本体（任务给的 :1300-1336 即循环主体），:1467-1473 为上传。
- `uploadBatch` 调用方：`flushTextBatch`（:479→486，文字批）与 `endModel`（:1031→1040）；`endModel` 由 `ModelFeatureRendererMixin.java:55` 在每个模型提交后调用（`beginModel` :45 配对）→ **渲染线程、每模型批一次、每帧数十次**。
- 帧号：`beginEarlyFrame` 每帧开头递增 `entityMotionFrame`（:239/:243；调用点 GameRendererMixin.java:80、LevelRendererMixin.java:200）。
- 下游：`NativeVulkan.uploadEntityBatch(textureSlot, memAddress(batch.buffer), vertexCount, 48, motionPointer)`（:1469）——motion 向量供原生渲染器做运动矢量。

### 2.2 输入/输出布局

- `batch.buffer`：直接内存、native order（:3118），顶点 48 字节 = 12×f32（`BYTES_PER_VERTEX` :46）。由 `emit`（:3331-3350）写入：

| f32 序号 | 含义 |
|---|---|
| 0/1/2 | x,y,z（**本内核唯一读取的字段**，:1309-1311） |
| 3/4 | u,v |
| 5/6/7/8 | r,g,b,a（0..1 归一化） |
| 9 | 恒 0.0 |
| 10 | localPlayerProxy=1 / emissive=-1 / 其他 0 |
| 11 | textureSlot + materialClass/MATERIAL_CLASS_SCALE（`encodedTextureSlot` :3352-3367） |

- motion 输出：直接内存、native order，每顶点 3×f32（`motionScratchFor` :3208-3218，capacity 只增不减）。
- 历史数组：`float[vertexCount*3]`，挂在 `ActiveBatch.vertexHistory`（`HashMap<ModelBatchKey, VertexHistory>`，`attachVertexHistory` :912-929，由 `LIMB_MV_ENABLED` :625-632 门控；每批 reset 时置 null :3168）。
- `poseDelta float[12]`：行主序 3×4 `[R | t]`，语义 `p_prev = R·p_cur + t`。

### 2.3 内核前的两个前置块（均 Java 侧，产物即内核输入）

1. **实体运动记录**：`resolveBatchMotion`（:810-910）/`applyRecordedMotion`（:782-787）把 double 域运动记录写进 `batch.poseDelta`（float）并置 `hasPoseDelta`。
2. **手部相机 MV**（:1260-1292）：条件 `HAND_CAMERA_MV`(:1179，`-DORION_HAND_CAMERA_MV=0` 关) && `batch.diagHand` && `!hasPoseDelta` && `prevCameraRotationValid`(:1175)。计算（全程 **float 域**）：
   - `R = (R⁻¹)ᵀ`：`m[i*3+j] = Σk prevHandRotation[i*4+k] * handRotation[j*4+k]`（3×3×3 乘加，:1265-1272）；
   - `t = prevCameraWorld − R·cameraWorld`（:1274-1280）；
   - 写 `poseDelta[0..11] = [m | t]`（:1281-1291），`hasPoseDelta = true`。

### 2.4 内核循环伪代码（:1295-1344，与 Java 逐行对应）

```
motionPointer = 0
if batch.vertexCount > 0:                                 # :1298
    motionFloats = vertexCount*3
    useVertexHistory = vertexHistoryIsPreviousFrame(motionFloats)
        # :3183-3193: entry.frame == entityMotionFrame-1 && positions.length == motionFloats
    positionStore = vertexHistoryPositions(motionFloats)
        # :3171-3181: 历史数组长度匹配则复用同一数组（原地滚动），否则 new float[motionFloats]
    needMotion = useVertexHistory || batch.hasPoseDelta   # :1302
    if needMotion || positionStore != null:               # :1303
        motion = needMotion ? batch.motionScratchFor(motionFloats) : null   # :1304
        positions = batch.buffer; delta = batch.poseDelta # :1305-1306
        anyMotion = false
        for vertex in 0..vertexCount:                     # :1308
            off = vertex*48                               # BYTES_PER_VERTEX
            px,py,pz = positions.getFloat(off), (+4), (+8)
            if needMotion:                                # :1313
                if useVertexHistory:                      # 历史差分路径 :1317-1321
                    mx = store[i]-px; my = store[i+1]-py; mz = store[i+2]-pz   # p_prev − p_cur
                else:                                     # 3×4 位姿路径 :1322-1326
                    mx = delta[0]*px + delta[1]*py + delta[2]*pz + delta[3] - px
                    my = delta[4]*px + delta[5]*py + delta[6]*pz + delta[7] - py
                    mz = delta[8]*px + delta[9]*py + delta[10]*pz + delta[11] - pz
                anyMotion |= (mx|my|mz) != 0
                motion.putFloat(mx).putFloat(my).putFloat(mz)
            if positionStore != null:                     # 历史回写（与 needMotion 无关）:1328-1336
                store[i]=px; store[i+1]=py; store[i+2]=pz # 写回同一数组，原地滚动
        if positionStore != null:
            batch.storeVertexHistory(positionStore)       # :1337-1339 → :3195-3206
            # entry.positions = 数组; entry.frame = entityMotionFrame（本帧）
            # 键数超 ENTITY_VERTEX_HISTORY_MAX_KEYS=32 时清表（:641、:3198）
        if needMotion && anyMotion:
            motion.flip(); motionPointer = memAddress(motion)   # :1340-1343
```
语义要点：motion 向量 = **上一帧位置 − 本帧位置**；零向量顶点也写入，`anyMotion` 只控制是否上传指针；历史回写无条件发生在 `positionStore != null` 时（即使本帧无位姿增量也刷新历史，供下一帧差分）。

### 2.5 频度与数据量级

- 每帧批次数十（每个渲染的实体/物品/文字批一次）；单批顶点 = `batch.vertexCount`（初始缓冲 768 顶点、倍增扩容 :3118/:3370-3381）。
- 总量级：HANDOFF §8 实测**实体/物品捕获链 223.8 万 verts/帧**（`ORION_ENTITY_CAPTURE=0` 省 124.1ms）；帧累计读口已有：`takeCapturedVertexCount`（:2085/:2092-2095，读后清零，LevelPhaseProbe.java:461 消费）。
- 内核每顶点工作量：读 3 float + ~15 flop + 写 3 float（motion）+ 写 3 float（history），内存带宽型。

### 2.6 耦合点（全部可参数化）

需传入 Rust 的仅原始缓冲/数组：src 指针 + vertexCount、模式位（历史/位姿）、history 数组（inout）、`entityMotionFrame`、`poseDelta[12]`、motion 输出指针。`handRotation/prevHandRotation/cameraWorld*` 等静态量已在前置块消化；history 键控（`ModelBatchKey`）、32 键上限与 prune（:927-948）全留 Java。

### 2.7 错误/边界

- `vertexCount==0` → 整段跳过；`needMotion=false && positionStore==null` → 零工作。
- `anyMotion=false` → `motionPointer=0`（原生侧按无 motion 处理——Rust 桥必须保留该语义）。
- 历史数组长度不匹配（批顶点数变化）→ 新数组替代且 `useVertexHistory=false` → 回退位姿路径；`entry.frame != entityMotionFrame-1` → 同样回退（帧号不连续不信任下标对应，:3183-3193 注释）。
- `ensureCapacity`（:3370-3381）`Math.multiplyExact` 溢出会抛——在捕获阶段，与内核无关。

### 2.8 可移植性判定与收益（A4）

- **判定：可移植性全项目最高**——零 MC 对象耦合、纯 float 域、三个缓冲全部调用方分配，与天气内核 `off_weather_plan/build` 同型。建议 ABI：`off_entity_motion(src: *const u8, vertexCount: u32, mode: u32, frame: u32, history: *mut f32, delta: *const f32, out: *mut f32) -> i32`（返回 anyMotion 位/错误码，catch_unwind 包裹）。
- **但收益上限已被实测钉住**：HANDOFF §8 目标选择第 1 条原文——实体捕获 Δ124ms 的卡点是**图元数**（§12.12 已证），正解是**模型级批量化设计**，"不是逐顶点移植 ⇒ 留给设计路线"。本内核每顶点 ~15 flop，单独移植摊薄 FFI 后收益有限。**建议**：要么与 captureVertex/emit 捕获链合并成一个大内核，要么直接投入批量化设计（§8 待办 4）；单独移植 A4 仅在对拍管线练手时有价值。
- 边界风险（对拍门）：需逐位复刻 float 运算顺序（R 行主序乘加序、`anyMotion` 位语义、`motion.flip` 时机、历史回写与帧号绑定）。

---

## 对拍与 ABI 对齐（两候选通用）

- 版本门：`off_abi_version`（lib.rs:22-23）；每个导出 `catch_unwind` → 错误码，不跨 FFI 展开（lib.rs:48/:88/:351/:522 模式）；Java 侧 RustBridge.java FFM 版本门 + `/natives/windows/orion_farfield.dll` 资源提取（HANDOFF §8）。
- 对拍门建议：A2 用满玻璃/空/混合三场景的节缓冲逐位对拍（marker 字节 + pad int + 压缩后顶点流三样都比对）；A4 对拍 motion 缓冲逐位 + anyMotion 标志 + history 数组终态。
- A/B 开关沿用既有形态：`-Dorion.rustGlassTag` / `-Dorion.rustGlassCull` / `-Dorion.rustEntityMotion`（参照 `-Dorion.rustVpt` :44、`-Dorion.rustWeather`）。

## 待证项

1. `isInteriorSharedGlassFace` 在原版方块上是否恒 false（决定 A2-cull 优先级）。查法：运行时打印 `SC_GLASS_CULLED_VERTS`（:1334，当前只写不读）于玻璃密集存档；或反编译 26.2 `TransparentBlock.skipRendering` 核对同方块共享面是否已被原版剔除。
2. 26.2 `ChunkSectionLayer` 有几个层名含 "TRANSLUCENT"（决定每节调几轮 tag/fill/cull）。
3. 实体每帧顶点总量的稳定测量（HANDOFF §8 待证①；`takeCapturedVertexCount` :2092 已有读口，接 PERF 统计即可）。
4. vanilla 26.2 `MeshData.vertexBuffer` 的字节序/复用约束（代码假设 native order 且 cull 注释声明"原缓冲仍属 MeshData 可能被别处使用" :1382-1384——移植时不可原地改原缓冲，A2-cull 的拷贝语义必须保留）。

## 建议下一步

1. A4 若做：按 §2.8 ABI 出 `off_entity_motion` + 孪生 bench（对拍 motion 逐位）；收益叙事上明确它是练手/合并前置，不是独立提速项。
2. A2 若做：先做待证项 1；tag 内核先在 Java 侧前置体素属性数组测速（对比 `scTagNanos`），确认预算后再进 Rust；cull 内核与 `off_vpt_*` 同型，直接复用 plan/build 管线。
3. 更高优先级仍是 HANDOFF §8 待办 4 的实体捕获模型级批量化设计评审（Δ124ms 大头）。
