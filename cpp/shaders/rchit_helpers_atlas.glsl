#ifndef ORION_ATLAS_DYNAMIC_INDEX
#define ORION_ATLAS_DYNAMIC_INDEX 1
#endif
// 2026-11-05 B①：从 rt_test.rchit 474-1038 行原样切出的图集解码/采样 helper 段。
// rchit 在原位置 include 回来；K2 的 rgen（ORION_K2_TRANSPORT）也 include 它。
vec4 sampleEntityAtlas(uint textureSlot, vec2 uv)
{
#if ORION_ATLAS_DYNAMIC_INDEX
    if(textureSlot == 0u || textureSlot > 128u)
    {
        return vec4(1.0);
    }
    return textureLod(entityAtlases[nonuniformEXT(textureSlot - 1u)], uv, 0.0);
#else
    switch(textureSlot)
    {
        case 1u: return textureLod(entityAtlases[0], uv, 0.0);
        case 2u: return textureLod(entityAtlases[1], uv, 0.0);
        case 3u: return textureLod(entityAtlases[2], uv, 0.0);
        case 4u: return textureLod(entityAtlases[3], uv, 0.0);
        case 5u: return textureLod(entityAtlases[4], uv, 0.0);
        case 6u: return textureLod(entityAtlases[5], uv, 0.0);
        case 7u: return textureLod(entityAtlases[6], uv, 0.0);
        case 8u: return textureLod(entityAtlases[7], uv, 0.0);
        case 9u: return textureLod(entityAtlases[8], uv, 0.0);
        case 10u: return textureLod(entityAtlases[9], uv, 0.0);
        case 11u: return textureLod(entityAtlases[10], uv, 0.0);
        case 12u: return textureLod(entityAtlases[11], uv, 0.0);
        case 13u: return textureLod(entityAtlases[12], uv, 0.0);
        case 14u: return textureLod(entityAtlases[13], uv, 0.0);
        case 15u: return textureLod(entityAtlases[14], uv, 0.0);
        case 16u: return textureLod(entityAtlases[15], uv, 0.0);
        case 17u: return textureLod(entityAtlases[16], uv, 0.0);
        case 18u: return textureLod(entityAtlases[17], uv, 0.0);
        case 19u: return textureLod(entityAtlases[18], uv, 0.0);
        case 20u: return textureLod(entityAtlases[19], uv, 0.0);
        case 21u: return textureLod(entityAtlases[20], uv, 0.0);
        case 22u: return textureLod(entityAtlases[21], uv, 0.0);
        case 23u: return textureLod(entityAtlases[22], uv, 0.0);
        case 24u: return textureLod(entityAtlases[23], uv, 0.0);
        case 25u: return textureLod(entityAtlases[24], uv, 0.0);
        case 26u: return textureLod(entityAtlases[25], uv, 0.0);
        case 27u: return textureLod(entityAtlases[26], uv, 0.0);
        case 28u: return textureLod(entityAtlases[27], uv, 0.0);
        case 29u: return textureLod(entityAtlases[28], uv, 0.0);
        case 30u: return textureLod(entityAtlases[29], uv, 0.0);
        case 31u: return textureLod(entityAtlases[30], uv, 0.0);
        case 32u: return textureLod(entityAtlases[31], uv, 0.0);
        case 33u: return textureLod(entityAtlases[32], uv, 0.0);
        case 34u: return textureLod(entityAtlases[33], uv, 0.0);
        case 35u: return textureLod(entityAtlases[34], uv, 0.0);
        case 36u: return textureLod(entityAtlases[35], uv, 0.0);
        case 37u: return textureLod(entityAtlases[36], uv, 0.0);
        case 38u: return textureLod(entityAtlases[37], uv, 0.0);
        case 39u: return textureLod(entityAtlases[38], uv, 0.0);
        case 40u: return textureLod(entityAtlases[39], uv, 0.0);
        case 41u: return textureLod(entityAtlases[40], uv, 0.0);
        case 42u: return textureLod(entityAtlases[41], uv, 0.0);
        case 43u: return textureLod(entityAtlases[42], uv, 0.0);
        case 44u: return textureLod(entityAtlases[43], uv, 0.0);
        case 45u: return textureLod(entityAtlases[44], uv, 0.0);
        case 46u: return textureLod(entityAtlases[45], uv, 0.0);
        case 47u: return textureLod(entityAtlases[46], uv, 0.0);
        case 48u: return textureLod(entityAtlases[47], uv, 0.0);
        case 49u: return textureLod(entityAtlases[48], uv, 0.0);
        case 50u: return textureLod(entityAtlases[49], uv, 0.0);
        case 51u: return textureLod(entityAtlases[50], uv, 0.0);
        case 52u: return textureLod(entityAtlases[51], uv, 0.0);
        case 53u: return textureLod(entityAtlases[52], uv, 0.0);
        case 54u: return textureLod(entityAtlases[53], uv, 0.0);
        case 55u: return textureLod(entityAtlases[54], uv, 0.0);
        case 56u: return textureLod(entityAtlases[55], uv, 0.0);
        case 57u: return textureLod(entityAtlases[56], uv, 0.0);
        case 58u: return textureLod(entityAtlases[57], uv, 0.0);
        case 59u: return textureLod(entityAtlases[58], uv, 0.0);
        case 60u: return textureLod(entityAtlases[59], uv, 0.0);
        case 61u: return textureLod(entityAtlases[60], uv, 0.0);
        case 62u: return textureLod(entityAtlases[61], uv, 0.0);
        case 63u: return textureLod(entityAtlases[62], uv, 0.0);
        case 64u: return textureLod(entityAtlases[63], uv, 0.0);
        case 65u: return textureLod(entityAtlases[64], uv, 0.0);
        case 66u: return textureLod(entityAtlases[65], uv, 0.0);
        case 67u: return textureLod(entityAtlases[66], uv, 0.0);
        case 68u: return textureLod(entityAtlases[67], uv, 0.0);
        case 69u: return textureLod(entityAtlases[68], uv, 0.0);
        case 70u: return textureLod(entityAtlases[69], uv, 0.0);
        case 71u: return textureLod(entityAtlases[70], uv, 0.0);
        case 72u: return textureLod(entityAtlases[71], uv, 0.0);
        case 73u: return textureLod(entityAtlases[72], uv, 0.0);
        case 74u: return textureLod(entityAtlases[73], uv, 0.0);
        case 75u: return textureLod(entityAtlases[74], uv, 0.0);
        case 76u: return textureLod(entityAtlases[75], uv, 0.0);
        case 77u: return textureLod(entityAtlases[76], uv, 0.0);
        case 78u: return textureLod(entityAtlases[77], uv, 0.0);
        case 79u: return textureLod(entityAtlases[78], uv, 0.0);
        case 80u: return textureLod(entityAtlases[79], uv, 0.0);
        case 81u: return textureLod(entityAtlases[80], uv, 0.0);
        case 82u: return textureLod(entityAtlases[81], uv, 0.0);
        case 83u: return textureLod(entityAtlases[82], uv, 0.0);
        case 84u: return textureLod(entityAtlases[83], uv, 0.0);
        case 85u: return textureLod(entityAtlases[84], uv, 0.0);
        case 86u: return textureLod(entityAtlases[85], uv, 0.0);
        case 87u: return textureLod(entityAtlases[86], uv, 0.0);
        case 88u: return textureLod(entityAtlases[87], uv, 0.0);
        case 89u: return textureLod(entityAtlases[88], uv, 0.0);
        case 90u: return textureLod(entityAtlases[89], uv, 0.0);
        case 91u: return textureLod(entityAtlases[90], uv, 0.0);
        case 92u: return textureLod(entityAtlases[91], uv, 0.0);
        case 93u: return textureLod(entityAtlases[92], uv, 0.0);
        case 94u: return textureLod(entityAtlases[93], uv, 0.0);
        case 95u: return textureLod(entityAtlases[94], uv, 0.0);
        case 96u: return textureLod(entityAtlases[95], uv, 0.0);
        case 97u: return textureLod(entityAtlases[96], uv, 0.0);
        case 98u: return textureLod(entityAtlases[97], uv, 0.0);
        case 99u: return textureLod(entityAtlases[98], uv, 0.0);
        case 100u: return textureLod(entityAtlases[99], uv, 0.0);
        case 101u: return textureLod(entityAtlases[100], uv, 0.0);
        case 102u: return textureLod(entityAtlases[101], uv, 0.0);
        case 103u: return textureLod(entityAtlases[102], uv, 0.0);
        case 104u: return textureLod(entityAtlases[103], uv, 0.0);
        case 105u: return textureLod(entityAtlases[104], uv, 0.0);
        case 106u: return textureLod(entityAtlases[105], uv, 0.0);
        case 107u: return textureLod(entityAtlases[106], uv, 0.0);
        case 108u: return textureLod(entityAtlases[107], uv, 0.0);
        case 109u: return textureLod(entityAtlases[108], uv, 0.0);
        case 110u: return textureLod(entityAtlases[109], uv, 0.0);
        case 111u: return textureLod(entityAtlases[110], uv, 0.0);
        case 112u: return textureLod(entityAtlases[111], uv, 0.0);
        case 113u: return textureLod(entityAtlases[112], uv, 0.0);
        case 114u: return textureLod(entityAtlases[113], uv, 0.0);
        case 115u: return textureLod(entityAtlases[114], uv, 0.0);
        case 116u: return textureLod(entityAtlases[115], uv, 0.0);
        case 117u: return textureLod(entityAtlases[116], uv, 0.0);
        case 118u: return textureLod(entityAtlases[117], uv, 0.0);
        case 119u: return textureLod(entityAtlases[118], uv, 0.0);
        case 120u: return textureLod(entityAtlases[119], uv, 0.0);
        case 121u: return textureLod(entityAtlases[120], uv, 0.0);
        case 122u: return textureLod(entityAtlases[121], uv, 0.0);
        case 123u: return textureLod(entityAtlases[122], uv, 0.0);
        case 124u: return textureLod(entityAtlases[123], uv, 0.0);
        case 125u: return textureLod(entityAtlases[124], uv, 0.0);
        case 126u: return textureLod(entityAtlases[125], uv, 0.0);
        case 127u: return textureLod(entityAtlases[126], uv, 0.0);
        case 128u: return textureLod(entityAtlases[127], uv, 0.0);
        default: return vec4(1.0);
    }
#endif
}

// 2026-10-09：实体 LabPBR 一次取两图（`_n` + `_s`）。与 sampleEntityAtlas 同样
// 必须静态索引（本设备没开 descriptor indexing），所以是 switch 而不是动态下标。
// 槽 0（雨/粒子/非实体）与越界槽走 default ⇒ 返回中性值 ⇒ 调用方判为"无贴图"。
void sampleEntityPbr(
    uint textureSlot,
    vec2 uv,
    out vec4 normalTexel,
    out vec4 specularTexel
)
{
#if ORION_ATLAS_DYNAMIC_INDEX
    if(textureSlot == 0u || textureSlot > 128u)
    {
        normalTexel = vec4(0.0, 0.0, 0.0, 1.0);
        specularTexel = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    const uint entityPbrIdx = nonuniformEXT(textureSlot - 1u);
    normalTexel = textureLod(entityNormalAtlases[entityPbrIdx], uv, 0.0);
    specularTexel = textureLod(entitySpecularAtlases[entityPbrIdx], uv, 0.0);
    return;
#else
    switch(textureSlot)
    {
        case 1u: normalTexel = textureLod(entityNormalAtlases[0], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[0], uv, 0.0); return;
        case 2u: normalTexel = textureLod(entityNormalAtlases[1], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[1], uv, 0.0); return;
        case 3u: normalTexel = textureLod(entityNormalAtlases[2], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[2], uv, 0.0); return;
        case 4u: normalTexel = textureLod(entityNormalAtlases[3], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[3], uv, 0.0); return;
        case 5u: normalTexel = textureLod(entityNormalAtlases[4], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[4], uv, 0.0); return;
        case 6u: normalTexel = textureLod(entityNormalAtlases[5], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[5], uv, 0.0); return;
        case 7u: normalTexel = textureLod(entityNormalAtlases[6], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[6], uv, 0.0); return;
        case 8u: normalTexel = textureLod(entityNormalAtlases[7], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[7], uv, 0.0); return;
        case 9u: normalTexel = textureLod(entityNormalAtlases[8], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[8], uv, 0.0); return;
        case 10u: normalTexel = textureLod(entityNormalAtlases[9], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[9], uv, 0.0); return;
        case 11u: normalTexel = textureLod(entityNormalAtlases[10], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[10], uv, 0.0); return;
        case 12u: normalTexel = textureLod(entityNormalAtlases[11], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[11], uv, 0.0); return;
        case 13u: normalTexel = textureLod(entityNormalAtlases[12], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[12], uv, 0.0); return;
        case 14u: normalTexel = textureLod(entityNormalAtlases[13], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[13], uv, 0.0); return;
        case 15u: normalTexel = textureLod(entityNormalAtlases[14], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[14], uv, 0.0); return;
        case 16u: normalTexel = textureLod(entityNormalAtlases[15], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[15], uv, 0.0); return;
        case 17u: normalTexel = textureLod(entityNormalAtlases[16], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[16], uv, 0.0); return;
        case 18u: normalTexel = textureLod(entityNormalAtlases[17], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[17], uv, 0.0); return;
        case 19u: normalTexel = textureLod(entityNormalAtlases[18], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[18], uv, 0.0); return;
        case 20u: normalTexel = textureLod(entityNormalAtlases[19], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[19], uv, 0.0); return;
        case 21u: normalTexel = textureLod(entityNormalAtlases[20], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[20], uv, 0.0); return;
        case 22u: normalTexel = textureLod(entityNormalAtlases[21], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[21], uv, 0.0); return;
        case 23u: normalTexel = textureLod(entityNormalAtlases[22], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[22], uv, 0.0); return;
        case 24u: normalTexel = textureLod(entityNormalAtlases[23], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[23], uv, 0.0); return;
        case 25u: normalTexel = textureLod(entityNormalAtlases[24], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[24], uv, 0.0); return;
        case 26u: normalTexel = textureLod(entityNormalAtlases[25], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[25], uv, 0.0); return;
        case 27u: normalTexel = textureLod(entityNormalAtlases[26], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[26], uv, 0.0); return;
        case 28u: normalTexel = textureLod(entityNormalAtlases[27], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[27], uv, 0.0); return;
        case 29u: normalTexel = textureLod(entityNormalAtlases[28], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[28], uv, 0.0); return;
        case 30u: normalTexel = textureLod(entityNormalAtlases[29], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[29], uv, 0.0); return;
        case 31u: normalTexel = textureLod(entityNormalAtlases[30], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[30], uv, 0.0); return;
        case 32u: normalTexel = textureLod(entityNormalAtlases[31], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[31], uv, 0.0); return;
        case 33u: normalTexel = textureLod(entityNormalAtlases[32], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[32], uv, 0.0); return;
        case 34u: normalTexel = textureLod(entityNormalAtlases[33], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[33], uv, 0.0); return;
        case 35u: normalTexel = textureLod(entityNormalAtlases[34], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[34], uv, 0.0); return;
        case 36u: normalTexel = textureLod(entityNormalAtlases[35], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[35], uv, 0.0); return;
        case 37u: normalTexel = textureLod(entityNormalAtlases[36], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[36], uv, 0.0); return;
        case 38u: normalTexel = textureLod(entityNormalAtlases[37], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[37], uv, 0.0); return;
        case 39u: normalTexel = textureLod(entityNormalAtlases[38], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[38], uv, 0.0); return;
        case 40u: normalTexel = textureLod(entityNormalAtlases[39], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[39], uv, 0.0); return;
        case 41u: normalTexel = textureLod(entityNormalAtlases[40], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[40], uv, 0.0); return;
        case 42u: normalTexel = textureLod(entityNormalAtlases[41], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[41], uv, 0.0); return;
        case 43u: normalTexel = textureLod(entityNormalAtlases[42], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[42], uv, 0.0); return;
        case 44u: normalTexel = textureLod(entityNormalAtlases[43], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[43], uv, 0.0); return;
        case 45u: normalTexel = textureLod(entityNormalAtlases[44], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[44], uv, 0.0); return;
        case 46u: normalTexel = textureLod(entityNormalAtlases[45], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[45], uv, 0.0); return;
        case 47u: normalTexel = textureLod(entityNormalAtlases[46], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[46], uv, 0.0); return;
        case 48u: normalTexel = textureLod(entityNormalAtlases[47], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[47], uv, 0.0); return;
        case 49u: normalTexel = textureLod(entityNormalAtlases[48], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[48], uv, 0.0); return;
        case 50u: normalTexel = textureLod(entityNormalAtlases[49], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[49], uv, 0.0); return;
        case 51u: normalTexel = textureLod(entityNormalAtlases[50], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[50], uv, 0.0); return;
        case 52u: normalTexel = textureLod(entityNormalAtlases[51], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[51], uv, 0.0); return;
        case 53u: normalTexel = textureLod(entityNormalAtlases[52], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[52], uv, 0.0); return;
        case 54u: normalTexel = textureLod(entityNormalAtlases[53], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[53], uv, 0.0); return;
        case 55u: normalTexel = textureLod(entityNormalAtlases[54], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[54], uv, 0.0); return;
        case 56u: normalTexel = textureLod(entityNormalAtlases[55], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[55], uv, 0.0); return;
        case 57u: normalTexel = textureLod(entityNormalAtlases[56], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[56], uv, 0.0); return;
        case 58u: normalTexel = textureLod(entityNormalAtlases[57], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[57], uv, 0.0); return;
        case 59u: normalTexel = textureLod(entityNormalAtlases[58], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[58], uv, 0.0); return;
        case 60u: normalTexel = textureLod(entityNormalAtlases[59], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[59], uv, 0.0); return;
        case 61u: normalTexel = textureLod(entityNormalAtlases[60], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[60], uv, 0.0); return;
        case 62u: normalTexel = textureLod(entityNormalAtlases[61], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[61], uv, 0.0); return;
        case 63u: normalTexel = textureLod(entityNormalAtlases[62], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[62], uv, 0.0); return;
        case 64u: normalTexel = textureLod(entityNormalAtlases[63], uv, 0.0); specularTexel = textureLod(entitySpecularAtlases[63], uv, 0.0); return;
        default:
            normalTexel = vec4(0.0, 0.0, 0.0, 1.0);
            specularTexel = vec4(0.0, 0.0, 0.0, 1.0);
            return;
    }
#endif
}

vec4 sampleRTAlbedo(uint textureSlot, vec2 uv)
{
    return textureSlot == 0u
        ? textureLod(blockAtlas, uv, 0.0)
        : sampleEntityAtlas(textureSlot, uv);
}

vec4 sampleParticleAtlas(uint textureSlot, vec2 uv)
{
    switch(textureSlot)
    {
        case 1u: return textureLod(particleAtlases[0], uv, 0.0);
        case 2u: return textureLod(particleAtlases[1], uv, 0.0);
        case 3u: return textureLod(particleAtlases[2], uv, 0.0);
        default: return vec4(0.0);
    }
}

// 2026-10-03（用户要求）：雨可见度统一 2× 增益（rgen 里同名常量，两边必须一起改）。
// 这里作用于雨条本体：albedo ×2 ⇒ 雨条辐射度 ×2（radiance = albedo × 天空辐照度）。
const float RAIN_VISIBILITY_GAIN = 2.0;

vec4 sampleRainStreakAtlas(vec2 uv)
{
    return textureLod(rainStreakAtlas, uv, 0.0);
}

// =====================================================
// Voxy LOD atlas sampling (v11)
//
// The bridge bakes one 16x16 tile per face texture into a
// 256x256 atlas (9-level mip chain; a tile stays isolated
// down to mip 4 = one texel) and emits one quad per LOD cell
// face in world block coordinates.  Voxy's rasterizer picks
// the mip from screen-space UV derivatives; a hit shader has
// none, so texel density is reconstructed analytically:
//
//   cellPx = cellW * screenH / (2 tan(fov/2) * dist) * facing
//   mip    = clamp(log2(16 / cellPx), 0, 4)
//
// facing = |dot(normal, rayDir)| reproduces the hardware
// max-derivative mip choice at grazing angles (stable
// distance terrain instead of shimmer).  Mip 4 is the hard
// cap: below it a tile is sub-texel and neighbouring tiles
// would bleed.  The sample UV is inset-clamped to the owning
// tile so bilinear/trilinear filtering never crosses a tile
// border at any mip.
//
// pad2 = bridgeMip + shade/256; the bridge mip is superseded
// by the estimate above, shade stays the light modulation.
// pad2 < 0 keeps the pre-modulated vertex-colour fallback.
//
// Colour space: this pipeline shades with gamma-encoded values.
// Near terrain is texelFetch(blockAtlas) * interpColor with no
// decode anywhere (VulkanCore uploadChunk writes byte/255 raw),
// and rr_sdr_post applies the single global pow(1/2.2).  Voxy
// must use the same convention or the LOD/vanilla seam shows a
// brightness step, so the atlas sample is NOT decoded here; the
// bridge pre-encodes its vertex bytes to cancel the sRGB decode
// that appendVoxyLodQuads applies, leaving tint in gamma space.
// =====================================================
// v15: 256 -> 1024 (bridge tiles 16 -> 64 per axis, capacity 255 -> 4095).
// VOXY_TILE_PIXELS stays 16 so texel density is unchanged; only the number of
// addressable tiles grows.  Must track ATLAS_SIZE in the voxy bridge.
const float VOXY_ATLAS_SIZE = 1024.0;
const float VOXY_TILE_PIXELS = 16.0;

// Disabled: decoding the atlas sample to linear is physically right but
// inconsistent with every other material in this pipeline, which shades in
// gamma space.  Kept as a future interface option for a linear-space pass.
#if 0
vec3 voxySrgbToLinear(vec3 srgb)
{
    vec3 high = pow((srgb + 0.055) * (1.0 / 1.055), vec3(2.4));
    vec3 low = srgb * (1.0 / 12.92);
    return mix(high, low, lessThanEqual(srgb, vec3(0.04045)));
}
#endif

float voxyLodMip(vec3 pos0, vec3 pos1, vec3 geoNormal)
{
    // Both quad triangles expose the cell edge length: the first
    // triangle's v0->v1 is a square side, the second's is the
    // diagonal whose largest |component| equals the same side.
    vec3 edge = abs(pos1 - pos0);
    float cellW = max(edge.x, max(edge.y, edge.z));
    float projScale = float(gl_LaunchSizeEXT.y)
        / (2.0 * max(tan(cam.cameraParams.z * 0.5), 0.05));
    float facing = abs(dot(geoNormal, materialRayDirection));
    float cellPx = cellW * projScale * max(facing, 0.02)
        / max(materialHitT, 1.0e-3);
    return clamp(
        log2(VOXY_TILE_PIXELS / max(cellPx, 1.0e-4)),
        0.0,
        4.0
    );
}

vec2 voxyTileSafeUV(vec2 uv, vec2 uv0, vec2 uv1, vec2 uv2, float mip)
{
    // Encoded corner UVs are mip0 texel centres; recover the true
    // tile origin, then inset by half a texel of the selected mip
    // (ceil keeps the coarser trilinear level inside the tile).
    vec2 tileOrigin = min(uv0, min(uv1, uv2)) - vec2(0.5 / VOXY_ATLAS_SIZE);
    float inset = exp2(ceil(mip)) * (0.5 / VOXY_ATLAS_SIZE);
    vec2 tileSpan = vec2(VOXY_TILE_PIXELS / VOXY_ATLAS_SIZE);
    return clamp(uv, tileOrigin + inset, tileOrigin + tileSpan - inset);
}

vec3 voxyLodAlbedo(float mipShade, vec2 uv, vec2 uv0, vec2 uv1, vec2 uv2,
    float mip, vec3 tintOrFallback)
{
    if(mipShade < 0.0)
    {
        return tintOrFallback;
    }
    float shade = mipShade - floor(mipShade);
    vec2 safeUV = voxyTileSafeUV(uv, uv0, uv1, uv2, mip);
    return textureLod(voxyLodAtlas, safeUV, mip).rgb * tintOrFallback * shade;
}

// =====================================================
// DH far field: per-block texture (binding 49 = DH 的 blockRatioAtlas)
//
// 顶点字节 6-9 带的是 (textureTileId, normalIndex)（见 VulkanCore.cpp:
// appendDhLodQuads）。这里按 DH 自己的 terrain/blaze/frag.fsh 复现：
//   tileOrigin = vec2(id % 256, id / 256) * 16
//   uv         = (tileOrigin + blockFaceUv() * 16) / atlasSize
//   albedo     = mix(顶点色, clamp(顶点色 * tile.rgb * 2), tile.a)
// blockFaceUv 用命中点的方块小数位置 + normalIndex（0..5 = down/up/north/
// south/west/east），于是侧墙也永远是"一格一张贴图"，不会被拉成帘幕 ——
// 这替换掉了早期那版"把顶点自带的一格 tile 铺满整个 quad"的写法。
//
// mip：tile 是 16px，正好对应一个方块面 ⇒ 用 dhBlockMip（方块在屏幕上的
// 大小）而不是 quad 的跨度，否则外圈大格会采到过粗的 mip 糊成一片。
// =====================================================
// Same derivation as voxyLodMip, but the cell is always exactly one block:
// once the UV is per-block the mip must follow the block's on-screen size,
// not the quad's span, otherwise the outer ring's large cells sample an
// over-coarse mip and the texture dissolves into a blur.
float dhBlockMip(vec3 hitPos, vec3 normal)
{
    return voxyLodMip(hitPos, hitPos + vec3(1.0), normal);
}

vec2 dhFaceUv(vec3 hitPos, float normalIndex)
{
    vec3 pos = fract(hitPos);
    if(normalIndex < 0.5) return vec2(pos.x, 1.0 - pos.z);        // down
    if(normalIndex < 1.5) return vec2(pos.x, pos.z);              // up
    if(normalIndex < 2.5) return vec2(1.0 - pos.x, 1.0 - pos.y);  // north
    if(normalIndex < 3.5) return vec2(pos.x, 1.0 - pos.y);        // south
    if(normalIndex < 4.5) return vec2(pos.z, 1.0 - pos.y);        // west
    return vec2(1.0 - pos.z, 1.0 - pos.y);                        // east
}

// =====================================================
// DH far field: fake-texture "grit" (DH's own applyNoise)
// =====================================================
// 照 .tmp 里 DH 的 terrain/blaze/frag.fsh 逐行复现：只对 tileId == 0 的
// **无纹理**方块施加 hash 噪声假纹理（noiseDropoff=1024，取自
//  Config$Client$Advanced$Graphics$NoiseTexture 的 javap，enableNoiseTexture 默认 true）。
//
// ⚠ 强度/步数是**刻意超出 DH 默认值**的（DH: noiseSteps=4 / noiseIntensity=0.05）。
//   原因：DH 只对"粗到没纹理的层"用噪声——细层（d6~d8）走它的 blockRatioAtlas 真纹理。
//   而我们这边 rendererMode=DISABLED ⇒ DH 的图集永远不创建 ⇒ 我方顶点全部 tileId=0，
//   连近处细层也落进这条无纹理分支。照搬 0.05 会让近处平得像塑料，故调高。
//   图集一旦接上（见 DhTextureHandles），这两项应当调回 DH 默认值。
//
// 与 DH 的两处刻意偏差：
//   1) 噪声格点锚在**世界坐标**而不是 section 局部坐标。DH 用 section 局部坐标
//      ⇒ 每个 section 一个噪声相位，跨 section 的假纹理对不上；锚世界网格则连成
//      一片，观感更统一（且不会随 section 边界闪）。
//   2) rchit 没有 dFdx/dFdy，DH 用它们重建面法线再沿法线推 0.001 只是为了修正
//      vPos 的浮点误差 —— 这里已经有真实几何法线，直接用它做同样的偏移。
//   DH 还会乘 fragColor.a，但 DH LOD 顶点色的 a 恒为 1，故省略（见 vert.vsh）。
float dhNoiseRand(float co)
{
    return fract(sin(co * 91.3458) * 47453.5453);
}

float dhNoiseRand(vec2 co)
{
    return fract(sin(dot(co.xy, vec2(12.9898, 78.233))) * 43758.5453);
}

float dhNoiseRand(vec3 co)
{
    return dhNoiseRand(co.xy + dhNoiseRand(co.z));
}

vec3 dhFakeTextureNoise(vec3 albedo, vec3 hitPos, vec3 geoNormal)
{
    const float DH_NOISE_INTENSITY = 0.18;
    const float DH_NOISE_STEPS = 8.0;
    const float DH_NOISE_DROPOFF = 1024.0;

    vec3 fixedPos = hitPos + normalize(geoNormal) * 0.001;
    vec3 grid = floor(fixedPos * DH_NOISE_STEPS) / DH_NOISE_STEPS;

    float luminance = (albedo.r + albedo.g + albedo.b) * (1.0 / 3.0);
    // -(2x-1)^2+1：越暗或越亮的地方噪声越弱，中间调最强。
    float amplification =
        (1.0 - pow(luminance * 2.0 - 1.0, 2.0)) * DH_NOISE_INTENSITY;

    float randomValue =
        dhNoiseRand(grid) * 2.0 * amplification - amplification;
    vec3 newColor = clamp(albedo + (1.0 - albedo) * randomValue, 0.0, 1.0);

    // 越远越淡（DH: min(viewDist / dropoff, 1)）。viewDist 是到相机的距离。
    float viewDistance = length(hitPos - cam.viewInverse[3].xyz);
    float distanceFade = min(viewDistance / DH_NOISE_DROPOFF, 1.0);
    return mix(newColor, albedo, distanceFade);
}

// =====================================================
// DH 平价色补偿（"LOD 那一片为什么亮一点"）
// =====================================================
// DH 顶点色的那个"平面色"是**在线性空间求的平均**：ClientBlockStateColorCache
// .calculateColorFromTexture 用 srgbToLinearTable 把每个不透明 texel 转线性、
// 加权求和，再用 linearToSrgb 编码回 8 位（见其字节码）。
// 而近场是**逐 texel 采样贴图**、把 sRGB 字节当线性用，于是在 LOD 接缝处被
// 降噪器平均出来的就是"同一个面所有 texel 的 sRGB 均值"。
// encode 是凹函数 ⇒ encode(E[L]) ≥ E[encode(L)]：
//   DH 的平面色 **永不比** 同一批 texel 的 sRGB 均値暗，贴图反差越大差得越多
//   （树叶/石砖 >> 纯色方块）。这就是接缝两侧"同一块地，远的那半分更亮"的直接来源。
//
// 我们没有贴图分布，无法精确反演，只能按典型方块贴图的统计差补一个增益：
//   反差中等的贴图（草/泥土）实测比值 ≈ 0.93，反差大的（树叶/圆石）≈ 0.81~0.84，
//   取中间偏保守的 0.85。
// ⚠ 2026-10-01：这个偏差**正比于合并层级**。一个 LOD quad 越粗，它平均掉的
//   texel 越多，encode(E[L]) - E[encode(L)] 越大 ⇒ 越远越亮越灰。所以补偿不能
//   是一个常数，必须随距离爬升。
//
// 实测（2026-10-01 夕阳，DH 远场草地）：远场 ≈ RGB(185,220,198)，Y=0.82、S=0.15；
// 同屏近场 ≈ RGB(81,102,88) Y=0.38 与 RGB(54,60,34) Y=0.22 —— 亮 2~3 倍。
//
// 旧写法只在 tileId == 0 的平价色分支里乘一个 0.85。一旦 DH 的 blockRatioAtlas
// 接上（publishTextures=true + 重启），顶点全部带真 tileId ⇒ 那段补偿**整段
// 失效**，亮度爬升就回来了。现在把补偿提到两条分支**之后**统一做。
//
// DH_LOD_NEAR_GAIN 沿用旧值 0.85（近场档的档位不变），DH_LOD_FAR_GAIN 是最粗层
// 的档位。刻意不压到与近场同亮 —— 远处本来就该淡一点，这里只削掉大部分爬升。
// 观感偏亮就调小 DH_LOD_FAR_GAIN，偏暗就调大；要整体关掉把两个都设回 1.0。
const float DH_LOD_NEAR_GAIN = 0.85;
const float DH_LOD_FAR_GAIN = 0.65;
const float DH_LOD_NEAR_SAT = 1.0;
const float DH_LOD_FAR_SAT = 1.45;
const float DH_LOD_NEAR_BLOCKS = 96.0;
const float DH_LOD_FAR_BLOCKS = 768.0;

// 两个距离端点之间线性过渡。补偿量本就正比于"合并了多少 texel"，而合并量在
// LOD 层级上是台阶式涨的，所以这里用直的，不用 smoothstep。
vec3 dhLodColorCompensation(vec3 color, vec3 hitPos)
{
    float viewDistance = length(hitPos - cam.viewInverse[3].xyz);
    float t = clamp(
        (viewDistance - DH_LOD_NEAR_BLOCKS) /
        max(DH_LOD_FAR_BLOCKS - DH_LOD_NEAR_BLOCKS, 1.0),
        0.0,
        1.0);
    vec3 gained = color * mix(DH_LOD_NEAR_GAIN, DH_LOD_FAR_GAIN, t);
    // ⚠ 增益是**乘法**，只能压亮度，动不了彩度。而线性空间平均除了抬亮，还会把
    //   每个通道往均值拉 ⇒ 饱和度一起被吃掉（实测远场草地 S=0.15，近场 S=0.21，
    //   比 0.71）。所以必须另补一步：以亮度为轴等比放大彩度。
    //   远端 1.45 ≈ 0.21/0.15 的倒数；近场档 1.0 = 不动，保证近场逐字节不变。
    float luma = dot(gained, vec3(0.2126, 0.7152, 0.0722));
    return clamp(
        mix(vec3(luma), gained, mix(DH_LOD_NEAR_SAT, DH_LOD_FAR_SAT, t)),
        0.0,
        1.0);
}

vec3 dhLodAlbedo(float tileId, float normalIndex, vec3 hitPos, vec3 geoNormal,
    vec3 vertexColor)
{
    // tileId 0 = DH 自己的"无纹理"约定，同时也是"图集还没就绪"的占位
    // （Java 侧只在真句柄推送成功后才写 tileId，见 DhTextureHandles.isPublished）。
    // DH 对这类方块补一层假纹理噪声（applyNoise），远场因此才有颗粒感。
    if(tileId < 0.5)
    {
        return dhLodColorCompensation(
            dhFakeTextureNoise(vertexColor, hitPos, geoNormal), hitPos);
    }
    float mip = clamp(dhBlockMip(hitPos, geoNormal), 0.0, 4.0);
    ivec2 atlasSize = textureSize(dhBlockRatioAtlas, 0);
    vec2 tileOrigin = vec2(mod(tileId, 256.0), floor(tileId / 256.0)) * 16.0;
    vec2 uv = (tileOrigin + dhFaceUv(hitPos, normalIndex) * 16.0) / vec2(atlasSize);
    vec4 tile = textureLod(dhBlockRatioAtlas, uv, mip);
    vec3 tinted = clamp(vertexColor * (tile.rgb * 2.0), 0.0, 1.0);
    return dhLodColorCompensation(
        mix(vertexColor, tinted, tile.a), hitPos);
}

// =====================================================
// DH far field: baked block light (DH vertex byte 13)
// =====================================================
// Java 侧把每一段的 (skyLight << 4) | blockLight 打进 DH 顶点字节 13，C++ 的
// appendDhLodQuads 原样搬进 Vertex.a ⇒ 这里从 interpColor.a 解回来。一个 quad 的
// 四顶点同值，重心插值精确，不需要新 attribute、不动 payload 结构。
//
// 为什么需要它：DH 远场在 MC 视距之外，我们的本地光（ReSTIR DI / VPT 体素光）
// 在那片区域**没有数据**，于是夜里远场只剩 ambient + GI ⇒ 村庄/街灯那一带全黑。
// blockLight 是唯一知道"那里有灯"的信息源。
//
// ⭐ 用法：作为**加性自发光**写进 payload.emission，刻意**不**乘进 albedo。
//   1) 乘 albedo = 把光照二次计入（ambient / 直射 / GI 已经各自乘过一次），而且
//      会把 16 级量化台阶烙在几何上 —— voxy 的 shade 就是这么被关掉的；
//   2) emission 是加性项，且会作为命中辐射进 GI ⇒ 远场街区顺带照亮自己的邻居，
//      与近场本地光的行为一致。
// skyLight 刻意不参与：天空/太阳本来由 shadow ray + GI 实时算，再乘一次就是双重
// 计光。"屋檐下的暗部"属于 RTAO/GI 的活，不该由这里补。
const float DH_BAKED_BLOCK_GAIN = 0.25;
const vec3 DH_BAKED_BLOCK_TINT = vec3(1.0, 0.82, 0.60);

float dhBlockLightLevel(float packedLight)
{
    float code = floor(clamp(packedLight, 0.0, 1.0) * 255.0 + 0.5);
    return mod(code, 16.0);
}

vec3 dhBakedBlockLight(vec3 albedo, float packedLight)
{
    float level = dhBlockLightLevel(packedLight) * (1.0 / 15.0);
    // 三次方曲线：0..4 级几乎不出光，12..15 级明确发亮（对齐"火把一两格内"的观感）。
    return albedo * DH_BAKED_BLOCK_TINT * (DH_BAKED_BLOCK_GAIN * level * level * level);
}

// v10 fixed-mip sampling (bridge selected mip = LOD ring level):
// superseded by the analytical density estimate above.  Disabled,
// kept as a future interface option.
#if 0
vec3 voxyLodAlbedo(float mipShade, vec2 uv, vec3 fallback)
{
    if(mipShade < 0.0)
    {
        return fallback;
    }
    float mip = floor(mipShade);
    float shade = mipShade - mip;
    return textureLod(voxyLodAtlas, uv, mip).rgb * shade;
}
#endif
