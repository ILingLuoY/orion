// SPDX-License-Identifier: LicenseRef-Orion-Internal
// ============================================================================
// B①：K1/K2 拆分的 landing 平面（主射线前半 → 传输后半的显式交接）。
//
// K1（ORION_K1_PRIMARY）跑：主射线（K1 精简 rchit 只落**命中身份**）+ depth/MV
//   + 玻璃支路 + guide 写入；在不透明切点把身份写进 landing 后 continue。
// K2（ORION_K2_TRANSPORT）跑：landing 载入身份 → resolveHitMaterial() 在 rgen
//   侧解析材质 → RTAO/太阳/SSS/本地光/glossy/GI/litColor → aerial → 累积 → 输出。
//
// 记录 = **命中身份**（hitDistance + slot/primitive/bary + state），不是 payload
// 快照：材质字段（albedo/emission/roughness/POM…）由 K2 的 resolveHitMaterial
// 从身份重建（与主变体 rchit 逐位同值 —— 同一份 material_resolve.glsl、同一套
// 图集绑定）。primaryHitDistance 等标量从身份/相机 UBO 确定性重算；水/玻璃/
// miss 像素整个由 K1 独立完成，K2 对它们只写一个 valid=0 哨兵防陈旧记录。
//
// binding 88。变体构建见 _deploy_stale_spv.py：-DORION_K1_PRIMARY /
// -DORION_K2_TRANSPORT（+SER 旗标共 4 个新 spv）。
// ============================================================================

// D1：callable TU（rt_test_material.rcall）也挂这个 buffer —— 材质字段的
// 回传借用同一批槽位（契约见 material_callable.glsl）。增加第三个条件对既有
// 变体的预处理输出零影响 ⇒ spv 逐字节不变。
#if defined(ORION_K1_PRIMARY) || defined(ORION_K2_TRANSPORT) || \
    defined(ORION_MATERIAL_CALLABLE)

struct PTLandingRecord
{
    // a: x=valid(1/0) y=hitDistance z=identityBaryX w=identityBaryY
    vec4 a;
    // b: xyz=(资源槽位, primitiveIndex, baryX)，w=state（原位整数，不走 float 保真）
    uvec4 b;
    // c..e: 预留（B③ landing 扩容候选槽），当前恒 0
    uvec4 c;
    vec4 d;
    // e: zw = dbgMotion.xy（主变体在 depth/MV 写入处留下的调试图值）
    vec4 e;
};

layout(std430, binding = 88) buffer PTLandingSSBO
{
    PTLandingRecord ptLandingRecords[];
};

// 槽位数与主变体的 SAMPLES_PER_FRAME 同一公式（宿主 clamp(g_perfRtSpp,0.5,4)），
// 双方各自从 cam UBO 现算 ⇒ 不需要在 buffer 里传，spp 滑块变化也不需要回写。
uint ptLandingSlotCount()
{
    return uint(max(1, int(clamp(cam.rtSampleParams.x, 0.5, 4.0) + 0.5)));
}

// 记录下标在调用点算（pixelCoord/size 在 main 体内才可见）。
uint ptLandingRecordIndex(uint pixelLinear, int s)
{
    return pixelLinear * ptLandingSlotCount() + uint(s);
}

// 每个样本先盖哨兵：非延期路径（miss/水/玻璃）此后不再写这条记录，
// K2 靠 valid=0 跳过 —— 防止读到上一帧的陈旧记录。
void ptLandingInvalidate(uint index)
{
    ptLandingRecords[index].a.x = 0.0;
    ptLandingRecords[index].c.w = 0u;
}

void ptLandingMarkGlass(uint index, vec2 dbgMotion)
{
    ptLandingRecords[index].a.x = 0.25;
    ptLandingRecords[index].c.w = 0u;
    ptLandingRecords[index].e.zw = dbgMotion;
}

// 切点身份快照。payload 此刻 = K1 精简 rchit 播种的身份五元组：
//   hitDistance = gl_HitTEXT
//   normal.xyz  = (资源槽位, primitiveIndex, baryX)
//   roughness   = baryY
//   state       = 发射前的 makeTraceState 值（精简 rchit 不改写）
void ptLandingStore(uint index, vec2 dbgMotion)
{
    PTLandingRecord r;
    r.a = vec4(1.0, payload.hitDistance, payload.normal.z, payload.roughness);
    r.b = uvec4(
        floatBitsToUint(payload.normal.x),
        floatBitsToUint(payload.normal.y),
        0u,
        payload.state
    );
    r.c = uvec4(0u);
    r.d = vec4(0.0);
    r.e = vec4(0.0, 0.0, dbgMotion);
    ptLandingRecords[index] = r;
}

// K2 载入身份并重建 payload（只到身份五元组；材质字段由随后的
// resolveHitMaterial(origin, dir) 填写）。返回 false = 该像素不走 K2
// （K1 已完整处理：miss/水/玻璃）。dbgMotion 带出主变体在 depth/MV 写入处
// 留下的调试图值（A1 时域重投影会原样传给 resolve，调试视图也读它）。
bool ptLandingLoad(uint index, out vec2 dbgMotion)
{
    PTLandingRecord r = ptLandingRecords[index];
    if(r.a.x <= 0.5)
    {
        return false;
    }

    payload.hitDistance = r.a.y;
    payload.normal = vec3(
        uintBitsToFloat(r.b.x),
        uintBitsToFloat(r.b.y),
        r.a.z
    );
    payload.roughness = r.a.w;
    payload.state = r.b.w;
    dbgMotion = r.e.zw;
    return true;
}

// ============================================================================
// ③ K2 传输段再分层（2026-11-05）：D→I 两级之间的镜面交接，借身份记录的
// 空闲字段 d。槽位在 ptLandingLoad 之后即无人再读（内联路径没有 callable
// 回写，D1 的 callable 也不使用 d 字段），复用无冲突。
//   d.xyz = glossyReflectionLight（**原始值**，×primaryPOMMicroOcclusion 由
//           K2-I 桩回流后的既有表达式照常施加 ⇒ 语义逐位同主变体）
//   d.w   = rrSpecularHitDistance（K2-D 在 VPT_STABLE_SPEC_HIT_DISTANCE 快照
//           处的值；水像素不走这对函数，水面 guide 在 K2-I 原地重算）
// 水像素不落这组字段：水的镜面已含在 k2DirectImage.rgb 里。
// ============================================================================
void ptLandingStoreGlossyExtra(uint index, vec3 glossyRaw, float specHitDist)
{
    ptLandingRecords[index].d.xyz = glossyRaw;
    ptLandingRecords[index].d.w = specHitDist;
}

void ptLandingLoadGlossyExtra(uint index, out vec3 glossyRaw, out float specHitDist)
{
    glossyRaw = ptLandingRecords[index].d.xyz;
    specHitDist = ptLandingRecords[index].d.w;
}

vec2 ptOctEncode(vec3 n)
{
    float l1 = abs(n.x) + abs(n.y) + abs(n.z);
    vec3 u = l1 > 1.0e-8 ? (n / l1) : vec3(0.0, 1.0, 0.0);
    if(u.z < 0.0)
    {
        vec2 sgn = vec2(u.x >= 0.0 ? 1.0 : -1.0, u.y >= 0.0 ? 1.0 : -1.0);
        u.xy = (1.0 - abs(u.yx)) * sgn;
    }
    return clamp(u.xy, vec2(-1.0), vec2(1.0));
}

uint ptEncodeAxisFace(vec3 face)
{
    if(dot(face, face) <= 0.25) return 7u;
    vec3 af = abs(face);
    if(af.x >= af.y && af.x >= af.z) return face.x >= 0.0 ? 0u : 1u;
    if(af.y >= af.z) return face.y >= 0.0 ? 2u : 3u;
    return face.z >= 0.0 ? 4u : 5u;
}

void ptLandingStoreSpatialMaterial(
    uint index,
    vec3 albedo,
    float roughness,
    vec3 f0,
    float metallic,
    vec3 shadingNormal,
    vec3 geomFace,
    bool pixelActive
)
{
    uint faceCode = ptEncodeAxisFace(geomFace);
    bool hasPbr = f0.x >= 0.0;
    vec3 safeN = dot(shadingNormal, shadingNormal) > 1.0e-8
        ? normalize(shadingNormal)
        : vec3(0.0, 1.0, 0.0);
    uvec4 packedC;
    packedC.x = packUnorm4x8(vec4(clamp(albedo, 0.0, 1.0), clamp(roughness, 0.0, 1.0)));
    packedC.y = packUnorm4x8(vec4(clamp(max(f0, vec3(0.0)), 0.0, 1.0), clamp(metallic, 0.0, 1.0)));
    packedC.z = packSnorm2x16(ptOctEncode(safeN));
    packedC.w = faceCode | (hasPbr ? 8u : 0u) | (pixelActive ? 16u : 0u) | (0x5au << 24u);
    ptLandingRecords[index].c = packedC;
}

#endif // ORION_K1_PRIMARY || ORION_K2_TRANSPORT

// ============================================================================
// P2（2026-11-17）：K2 三腿之间的材质结果中转 buffer（binding 94）。
//
// 背景：④ 三腿模式下 K2SUN / K2LOC / K2I 是三次**独立 dispatch**，ray payload
// 不跨 dispatch 存活；三条腿从同一份 landing 身份各自跑一次 resolveHitMaterial。
// 四条等价前提（同 origin/worldDir、解析不消费随机流、无共享写副作用、读同一
// landing 记录）成立 ⇒ 同一像素三条腿解析出的材质字段逐位同值，只需 SUN 解析
// 一次，LOC/I 从中转 buffer 灌回。
//
// 运行期闸门：cam.matReuseAb.x（ORION_MAT_REUSE_LEGS，默认 0）。关闭时本 buffer
// 不被访问、宿主也不分配；打开后：
//   · SUN（ORION_K2_DIRECT_SUN_STAGE）= 生产者：解析后把整条 payload 材质快照落盘；
//   · LOC（ORION_K2_DIRECT_LOCAL_STAGE）/ I（ORION_K2_INDIRECT_STAGE）= 消费者：
//     读记录灌回 payload，跳过 resolveHitMaterial。
//
// 记录 = resolveHitMaterial 之后的整条 payload（11 个字段，全浮点按位序存），
// 不漏字段：下游传输段读 albedoPacked / normal / emission / roughness / f0Metal /
// state / parallaxRayTravel / pomSelfShadow / pomMicroOcclusion / glassAlpha /
// hitDistance 任一字段，漏存都会改画面。
//
// 校验臂：cam.matReuseAb.y（ORION_MAT_REUSE_ASSERT）。消费者腿既读缓存记录又原地
// 重跑一次解析，逐位比对；不一致计数进计数器缓冲 reservedLane6（lane 6）。
// 校验臂下 payload 保留"原地解析"的结果，不用缓存值扰动本帧画面。
// ============================================================================
#if defined(ORION_K2_TRANSPORT)

struct MatReuseRecord
{
    // a: hitDistance / roughness / f0Metal / glassAlpha（浮点按位序存）
    uvec4 a;
    // b: state / albedoPacked / parallaxRayTravelBits / pomSelfShadowBits
    uvec4 b;
    // c: normal.xyz 位序 / pomMicroOcclusionBits
    uvec4 c;
    // d: emission.xyz 位序 / valid(1=本帧生产者已写)
    uvec4 d;
};

layout(std430, binding = 94) buffer MatReuseSSBO
{
    MatReuseRecord matReuseRecords[];
};

// 下标与 landing 记录同式：pixelLinear * slots + s（同一像素同一样本）。
uint matReuseRecordIndex(uint pixelLinear, int s)
{
    return pixelLinear * ptLandingSlotCount() + uint(s);
}

// 本帧标记。0 = 无生产者（缓冲初值），所以用 frameCount + 1。
// 2026-11-17 修复：此前 d.w 恒写 1，消费者无法区分"本帧写的"与"上一帧留下的"，
// 于是 SUN 腿在某类像素上没有产出时，LOC/I 会灌回上一帧的旧材质（已知不正确项）。
// 现在必须标记相等才算有效，否则消费者退回原地解析 ⇒ 与无复用逐位同值。
uint matReuseFrameTag()
{
    return cam.frameCount + 1u;
}

// 把当前 payload 打进记录（浮点一律走位序，保证灌回时逐位同值）。
void matReusePackRecord(out MatReuseRecord r, uint frameTag)
{
    r.a = uvec4(
        floatBitsToUint(payload.hitDistance),
        floatBitsToUint(payload.roughness),
        floatBitsToUint(payload.f0Metal),
        floatBitsToUint(payload.glassAlpha));
    r.b = uvec4(
        payload.state,
        payload.albedoPacked,
        floatBitsToUint(payload.parallaxRayTravel),
        floatBitsToUint(payload.pomSelfShadow));
    r.c = uvec4(
        floatBitsToUint(payload.normal.x),
        floatBitsToUint(payload.normal.y),
        floatBitsToUint(payload.normal.z),
        floatBitsToUint(payload.pomMicroOcclusion));
    r.d = uvec4(
        floatBitsToUint(payload.emission.x),
        floatBitsToUint(payload.emission.y),
        floatBitsToUint(payload.emission.z),
        frameTag);
}

// 生产者：解析完成后落盘整条材质快照。
void matReuseStore(uint index)
{
    MatReuseRecord r;
    matReusePackRecord(r, matReuseFrameTag());
    matReuseRecords[index] = r;
}

// 消费者：读记录。返回 false = 本帧没有有效生产者记录（防御路径，退回原地解析）。
bool matReuseLoad(uint index, out MatReuseRecord r)
{
    r = matReuseRecords[index];
    return r.d.w == matReuseFrameTag();
}

// 消费者：把记录字段灌回 payload（逐位还原）。
void matReuseApply(in MatReuseRecord r)
{
    payload.hitDistance = uintBitsToFloat(r.a.x);
    payload.roughness = uintBitsToFloat(r.a.y);
    payload.f0Metal = uintBitsToFloat(r.a.z);
    payload.glassAlpha = uintBitsToFloat(r.a.w);
    payload.state = r.b.x;
    payload.albedoPacked = r.b.y;
    payload.parallaxRayTravel = uintBitsToFloat(r.b.z);
    payload.pomSelfShadow = uintBitsToFloat(r.b.w);
    payload.normal = vec3(
        uintBitsToFloat(r.c.x),
        uintBitsToFloat(r.c.y),
        uintBitsToFloat(r.c.z));
    payload.pomMicroOcclusion = uintBitsToFloat(r.c.w);
    payload.emission = vec3(
        uintBitsToFloat(r.d.x),
        uintBitsToFloat(r.d.y),
        uintBitsToFloat(r.d.z));
}

// 校验臂：把"原地解析后的当前 payload"与缓存记录逐位比对；不一致累加 reservedLane6。
// 仅在计数器编译档（ORION_RT_STAGE_COUNTERS）下才有落盘缓冲；非计数器档本函数
// 只做打包比对、不写任何缓冲。
//
// 2026-11-17 晚：再加一条诊断通道 —— reservedLane7 当**字段位图**累积（atomicOr）。
// 位序：0-3 = a（hitDistance/roughness/f0Metal/glassAlpha）、4-7 = b（state/
// albedoPacked/parallaxRayTravel/pomSelfShadow）、8-11 = c（normal.xyz/pomMicroOcclusion）、
// 12-14 = d（emission.xyz）。跑完一局看这个 mask 就知道分歧落在哪个字段，
// 不用再猜"是不是读到上一帧"。
uint matReuseFieldDiffMask(in MatReuseRecord fresh, in MatReuseRecord cached)
{
    uint mask = 0u;

    for(uint i = 0u; i < 4u; ++i)
    {
        if(fresh.a[i] != cached.a[i])
        {
            mask |= (1u << i);
        }
    }

    for(uint i = 0u; i < 4u; ++i)
    {
        if(fresh.b[i] != cached.b[i])
        {
            mask |= (1u << (4u + i));
        }
    }

    for(uint i = 0u; i < 4u; ++i)
    {
        if(fresh.c[i] != cached.c[i])
        {
            mask |= (1u << (8u + i));
        }
    }

    for(uint i = 0u; i < 3u; ++i)
    {
        if(fresh.d[i] != cached.d[i])
        {
            mask |= (1u << (12u + i));
        }
    }

    return mask;
}

void matReuseAssertCompare(in MatReuseRecord cached)
{
    MatReuseRecord fresh;
    matReusePackRecord(fresh, matReuseFrameTag());
    const bool same =
        fresh.a == cached.a &&
        fresh.b == cached.b &&
        fresh.c == cached.c &&
        fresh.d.xyz == cached.d.xyz;
#ifdef ORION_RT_STAGE_COUNTERS
    if(!same)
    {
        uint mask = matReuseFieldDiffMask(fresh, cached);
        // 15 = 不一致出现在 LOC 腿，16 = 出现在 I 腿（哪个消费者腿先撞上）。
#if defined(ORION_K2_DIRECT_LOCAL_STAGE)
        mask |= (1u << 15);
#elif defined(ORION_K2_INDIRECT_STAGE)
        mask |= (1u << 16);
#endif
        atomicAdd(rtStageCounters.reservedLane6, 1u);
        atomicOr(rtStageCounters.reservedLane7, mask);
    }
#endif
}

#endif // ORION_K2_TRANSPORT
