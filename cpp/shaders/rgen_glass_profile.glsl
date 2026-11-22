
// Orion glass filter: base RGB is transmission_color directly.  The
// profile argument is retained only to avoid changing the existing call
// ABI; exact glass markers always pass zero.

int getTransparentProfileFromPayloadMaterialId(float materialId)
{
    return int(
        round(
            clamp(
                fract(materialId) * 10.0,
                0.0,
                4.0
            )
        )
    );
}

// =====================================================
// Orion transmission_weight / subsurface_weight port.
//
// SectionCompilerMixin's resolveTranslucentMarker labels the TRANSLUCENT mesh
// and VulkanCore maps marker N to materialId 2.0 + 0.1 * (N - 18), so the
// profile arrives here as 1..4:
//
//   0  clear / stained glass and the generic translucent body
//   1  tinted glass       (vanilla blocks light almost completely)
//   2  ice family         (hazy, still clearly see-through)
//   3  slime block
//   4  honey block
//
// Orion keeps transmission_weight ORTHOGONAL to transmission_color:
//   transmission_color = base_color RGB            (pure tint, never scaled)
//   thinTransmissionWeight = transmission_weight   (strength, 0..1)
// The weight is multiplied OUTSIDE the thin-sheet geometric series, never
// folded into the tint.  The previous code multiplied the profile scale into
// the chromatic filter itself, which darkened the dye of ice / honey / tinted
// glass instead of only reducing their transmitted energy.
// =====================================================

// Orion transmission_weight: how much light the body transmits.  This is a
// strength, not a colour, so it must never touch the tint RGB.
float getTransparentProfileTransmission(int transparentProfile)
{
    if(transparentProfile == 1)
    {
        return 0.06;
    }
    if(transparentProfile == 2)
    {
        return 0.55;
    }
    if(transparentProfile == 3)
    {
        return 0.45;
    }
    if(transparentProfile == 4)
    {
        return 0.32;
    }
    return 1.0;
}

// Orion subsurface_weight: the share of transmitted light that leaves the
// back face as a Lambert diffuse lobe (frosted / translucent-body scattering)
// rather than a straight sheet transmission.  Ice, slime and honey are the
// bodies that should scatter; clear and stained glass stay thin-sheet.
//
// Reserved for a follow-up that adds a real back-face Lambert scattering lobe.
// The straight camera path has no BSDF sampling framework, so a correct
// subsurface lobe needs an extra hemisphere trace per translucent pixel; that
// cost is deferred until the transport rewrite lands.
#if 0
float getTransparentProfileSubsurfaceWeight(int transparentProfile)
{
    if(transparentProfile == 2)
    {
        return 0.55;   // ice: frosted, strong back-scatter
    }
    if(transparentProfile == 3)
    {
        return 0.70;   // slime: dense translucent gel
    }
    if(transparentProfile == 4)
    {
        return 0.80;   // honey: thickest, most diffuse
    }
    return 0.0;        // clear/stained/tinted glass: pure thin-sheet
}
#endif

// =====================================================
// Glass optical model (straight-through).
//
// glassThinSheetFactors() is the thin-sheet dielectric energy
// split: one air-glass boundary whose internal reflections form a
// geometric series.  glassSheetTransmission() is the straight-shadow
// connection.  The closest-hit shader flips the glass normal toward
// the ray origin, so the boundary cosine takes abs(dot).
//
// Minecraft's base texture RGB is transmission_color directly.  Texture
// alpha neither punches coverage holes nor selects optical transmission
// strength, so it no longer participates in any glass filter.
// =====================================================

vec3 fresnelSchlick(
    float cosTheta,
    vec3 F0
);

// Disabled (no call sites): kept as a future interface option.  The
// scalar-F variants below are arithmetically identical for the uniform
// 0.04 glass F0 but cost a third of the Fresnel ALU and skip the unused
// reflection term on transmission-only paths.
#if 0
void glassThinSheetFactors(
    vec3 fresnel,
    vec3 transmittance,
    out vec3 reflection,
    out vec3 transmission
)
{
    vec3 f = clamp(fresnel, vec3(0.0), vec3(1.0));
    vec3 a = clamp(transmittance, vec3(0.0), vec3(1.0));
    vec3 oneMinusF = vec3(1.0) - f;
    vec3 denominator = max(vec3(1.0) - f * f * a * a, vec3(1.0e-6));
    reflection = f + oneMinusF * oneMinusF * f * a * a / denominator;
    transmission = oneMinusF * oneMinusF * a / denominator;
}
#endif

// The glass F0 (0.04) is uniform across RGB, so Schlick Fresnel collapses
// to one scalar.  The x^5 power is expanded into multiplies: exact
// (slightly more accurate than the GPU pow approximation) and off the
// SFU pipe.
float fresnelSchlickScalar(
    float cosTheta,
    float F0
)
{
    float x = 1.0 - clamp(cosTheta, 0.0, 1.0);
    float x2 = x * x;
    return F0 + (1.0 - F0) * (x2 * x2) * x;
}

// Scalar-F Orion thin-sheet split.  Same geometric series as the
// shielded vec3 version; only the reflection term keeps the per-channel
// algebra that a mirror-glass lobe actually needs.
void glassThinSheetFactorsScalarF(
    float fresnel,
    vec3 transmittance,
    out vec3 reflection,
    out vec3 transmission
)
{
    float f = clamp(fresnel, 0.0, 1.0);
    vec3 a = clamp(transmittance, vec3(0.0), vec3(1.0));
    float oneMinusF = 1.0 - f;
    vec3 denominator = max(
        vec3(1.0) - (f * f) * (a * a),
        vec3(1.0e-6)
    );
    reflection =
        vec3(f) + ((oneMinusF * oneMinusF) * f) * (a * a) / denominator;
    transmission =
        (oneMinusF * oneMinusF) * a / denominator;
}

vec3 glassSheetTransmissionScalarF(
    vec3 transmissionColor,
    float cosTheta
)
{
    float fresnel = fresnelSchlickScalar(cosTheta, 0.04);
    float oneMinusF = 1.0 - fresnel;
    vec3 a = clamp(transmissionColor, vec3(0.0), vec3(1.0));
    vec3 denominator = max(
        vec3(1.0) - (fresnel * fresnel) * (a * a),
        vec3(1.0e-6)
    );
    return (oneMinusF * oneMinusF) * a / denominator;
}

vec3 glassSheetTransmission(
    vec3 transmissionColor,
    vec3 faceNormal,
    vec3 directionToLight
)
{
    // Both inputs are unit length at every call site: payload.normal is
    // normalised in the closest-hit shaders and directionToLight is the
    // maintained-unit march direction.
    float cosine = clamp(
        abs(dot(faceNormal, directionToLight)),
        0.0,
        1.0
    );
    return glassSheetTransmissionScalarF(transmissionColor, cosine);
}

// Orion transmission_color: the pure chromatic filter.  It is base_color
// RGB clamped, with NO profile scaling folded in.  The transmission strength
// is applied separately as getTransparentProfileTransmission() OUTSIDE the
// thin-sheet series, matching Orion's surfaceShadowTransmission():
//   transmission = thinSheetFactors(fresnel, transmission_color) * weight.
// =====================================================
// 2026-10-01：染料浓度按"薄片"折减（用户报「所有彩色玻璃都偏暗」）
//
// 这里的 glassTint 不是**玻璃纹理**上的颜色，而是宿主写进顶点色的**桶装染料色**
// （VulkanCore.cpp 的 kStainedGlassDyeRgbById：0xF0F0F0 白、0xC354CD 品红……
// 与 vanilla 染料逐位一致）。把它当线性滤光片直接乘在被透射的辐射上，等于假设
// 这一格玻璃里灌满了整桶浓缩染料 ⇒ 彩色玻璃普遍偏暗、饱和到发闷。
//
// 1 格厚的薄片只该滤掉一部分：用 pow 折减浓度 —— 色相不变、亮度提上来。
//   0.5 ⇒ 红染料 (0.69,0.18,0.15) → (0.83,0.42,0.39)；白 (1,1,1) → (1,1,1) 不变。
// 1.0 = 旧行为（原样用染料色）。
// A/B：-DGI_GLASS_DYE_STRENGTH=1.0 回到旧行为；想更浓就调高（最大 1.0）。
// =====================================================
#ifndef GI_GLASS_DYE_SOFTEN
#define GI_GLASS_DYE_SOFTEN 1
#endif
#ifndef GI_GLASS_DYE_STRENGTH
#define GI_GLASS_DYE_STRENGTH 0.5
#endif

vec3 computeGlassBlockTransmission(
    vec3 glassTint,
    float glassAlpha,
    int transparentProfile
)
{
    // transmission_color = base_color RGB, clamped.  glassAlpha and the
    // profile strength deliberately do NOT enter the tint; folding either in
    // would darken the dye instead of only attenuating transmitted energy.
    vec3 dye =
        clamp(glassTint, vec3(0.0), vec3(1.0));

#if GI_GLASS_DYE_SOFTEN
    dye =
        pow(dye, vec3(GI_GLASS_DYE_STRENGTH));
#endif

    return dye;
}

// =====================================================
// 2026-10-17：多格染色玻璃的逐段染料累计（**零新增 trace**）
//
// 相机透射射线是 PASSTHROUGH，会忽略一切玻璃 ⇒ 它拿不到"墙里第二格及以后"
// 的染料；MC 又会剔除相邻同种玻璃的内部面，几何上本来也没有那些界面。
// 但体素字里有每一格的染料（Java 写的 RGB555 + VPT_STAINED_GLASS_BIT），
// 所以这里沿体内方向读一遍页表，把**入射格之后**的染色玻璃格按格内实际行程
// 做 Beer-Lambert：
//     段透射 = dye ^ 段长
// 与 glassSegmentTransmittance 同口径（σ = -ln(dye)）。
//
// 只处理染色玻璃（bit24）：遮光玻璃/冰/粘液/蜂蜜的染料在顶点材质里、不在体素
// 字里，它们的多格累计要等界面走查（相机路径本轮不做）。
//
// 成本：每个染色玻璃像素最多 GI_GLASS_RUN_MAX_CELLS 次页表读取，没有射线。
// A/B：-DGI_GLASS_RUN_DYE=0 ⇒ 逐位回到"只算入射格"。
// =====================================================
#ifndef GI_GLASS_RUN_DYE
#define GI_GLASS_RUN_DYE 1
#endif
#ifndef GI_GLASS_RUN_MAX_CELLS
// 8 格：再厚的染色玻璃墙视觉上已经接近不透明，继续走没有收益。
#define GI_GLASS_RUN_MAX_CELLS 8
#endif

// entryPoint 必须落在**入射格内**（相机命中点即可）；函数跳过该格，只累计
// 它后面的染色玻璃格。方向必须是玻璃**体内**的方向（折射后）。
vec3 glassRunDyeAfterFirstCell(vec3 entryPoint, vec3 insideDirection)
{
    vec3 direction = normalize(insideDirection);
    vec3 origin = entryPoint + direction * 1.0e-3;
    ivec3 cell = ivec3(floor(origin));
    ivec3 stepDir = ivec3(sign(direction));
    vec3 invAbs = 1.0 / max(abs(direction), vec3(1.0e-6));
    vec3 boundary = vec3(cell) + vec3(max(stepDir, ivec3(0)));
    vec3 tMax = (boundary - origin) / direction;
    if(stepDir.x == 0) { tMax.x = 1.0e30; }
    if(stepDir.y == 0) { tMax.y = 1.0e30; }
    if(stepDir.z == 0) { tMax.z = 1.0e30; }

    vec3 dyeProduct = vec3(1.0);
    float travelled = 0.0;
    for(int i = 0; i <= GI_GLASS_RUN_MAX_CELLS; ++i)
    {
        float next = min(tMax.x, min(tMax.y, tMax.z));
        if(i > 0)
        {
            uint source;
            if(!vptReadSourceVoxel(cell, source) ||
               (source & VPT_STAINED_GLASS_BIT) == 0u)
            {
                break;
            }
            vec3 cellTransmission =
                computeGlassBlockTransmission(
                    glassDecodeVoxelAlbedo(source),
                    1.0,
                    0
                );
            dyeProduct *= pow(
                max(
                    cellTransmission,
                    vec3(GLASS_SEGMENT_TRANSMITTANCE_FLOOR)
                ),
                vec3(max(next - travelled, 0.0))
            );
        }
        travelled = next;

        if(next == tMax.x)
        {
            cell.x += stepDir.x;
            tMax.x += invAbs.x;
        }
        else if(next == tMax.y)
        {
            cell.y += stepDir.y;
            tMax.y += invAbs.y;
        }
        else
        {
            cell.z += stepDir.z;
            tMax.z += invAbs.z;
        }
    }
    return dyeProduct;
}

// =====================================================
// Local-light stained-glass tint: judgement-experiment hooks
//
// The model itself (dye saturation, tint strengthening, exposure guard) lives in
// glass_tint_common.glsl, shared with rt_restir_final.rgen.  What stays here are
// the readouts that need the raw incident radiance before filtering.
//
// The 2026-09-22 note is preserved inside glassStainedPoolRadiance(): it is the
// reason LOCAL_GLASS_TINT_ABSORPTION is 0.0, i.e. the reason the coloured pool
// and the white pool are the same order of magnitude.
// =====================================================
vec3 applyLocalGlassTint(vec3 radiance, vec3 transmittance)
{
    // 诊断（R 通道）：本地光直射项的入射能量（过滤之前），用于与二级弹射
    // 项、无过滤假发光项比较能量占比。
    if(DBG_LOCAL_TERM_MAP)
    {
        return vec3(dbgTermLuma(radiance), 0.0, 0.0);
    }
    if(DBG_GLASS_DYE_VS_ENERGY)
    {
        // R = 直射的染料浓度（经玻璃过滤，应当有色）
        // G = 该项能量
        return vec3(
            dbgDyeConcentration(strengthenLocalGlassTint(
                clamp(transmittance, vec3(0.0), vec3(1.0))
            )),
            dbgTermLuma(radiance),
            0.0
        );
    }

    // 判决实验 I 探针直通：探针值已经是最终读数，不能再被「染色增强」
    // 与中性早退改写，否则读出的不是 DDA 覆盖率本身。
    if(DBG_VPT_DDA_PROBE)
    {
        return vec3(0.0, 1.0, 0.0);
    }

    if(DBG_GLASS_PEAK_READOUT)
    {
        // 同上：比值型读数，读直射路径染色后的色调纯度
        return dbgHuePurity2(glassStainedPoolRadiance(radiance, transmittance));
    }

    return glassApplyStainedTint(radiance, transmittance);
}