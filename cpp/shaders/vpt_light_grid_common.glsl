#ifndef VPT_LIGHT_GRID_COMMON_GLSL
#define VPT_LIGHT_GRID_COMMON_GLSL

// Shared closed-form weights between vpt_light_grid_build.comp and rt_test.rgen.
// Pure scalar/vector math only — zero local arrays (SPIR-V Local Memory Spill free).

float vptGridBaseWeight(
    ivec3 emitterBlock,
    uint emission,
    uint typeCode,
    vec3 padMin,
    vec3 padMax)
{
    if(emission == 0u)
    {
        return 0.0;
    }
    vec3 emitterCenter = vec3(emitterBlock) + vec3(0.5);
    vec3 closestInCell = clamp(emitterCenter, padMin, padMax);
    float distToCell = length(emitterCenter - closestInCell);
    float emissionLevel = float(emission);
    if(distToCell >= vptLocalLightRange(typeCode, emissionLevel))
    {
        return 0.0;
    }
    vec3 lightCol = localLightProfileColor(typeCode);
    float lightLuma = max(dot(lightCol, vec3(0.2126, 0.7152, 0.0722)), 0.08);
    return max(
        lightLuma *
        localLightProfileBrightness(typeCode, emissionLevel) *
        vptLocalLightDistanceAttenuation(distToCell + 0.5) *
        max(vptLocalLightRangeFade(distToCell, typeCode, emissionLevel), 1.0e-3),
        1.0e-6
    );
}

float vptSourceDirectoryDistWeight(ivec3 offset)
{
    float dSec = sqrt(float(
        offset.x * offset.x +
        offset.y * offset.y +
        offset.z * offset.z
    )) * 16.0;
    float effDist = 0.5 * dSec;
    return 1.0 / (64.0 + effDist * effDist);
}

#endif
