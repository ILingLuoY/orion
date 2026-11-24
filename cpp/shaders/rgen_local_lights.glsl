#ifndef RGEN_LOCAL_LIGHTS_GLSL
#define RGEN_LOCAL_LIGHTS_GLSL

// =====================================================
// 2026-11-23 M1: Pruned legacy local-light helpers.
//
// Retained strictly for the two active callers:
//   - dynamicLightBRDF: shared Cook-Torrance GGX + Lambert BRDF used by
//     block lights (evaluateVPTRISDirectLightingSingle / Pairwise MIS tail)
//     and dynamic moving lights (evaluateDynamicLightRISDirect).
//   - g2bSampleDiffuseDirectionMIS: called at primary/secondary GI bounce
//     points in rgen_main_body.glsl (LL_P3_ENABLE_G2B_LOCAL_GUIDE == false).
// =====================================================

vec3 dynamicLightBRDF(
    vec3 N,
    vec3 V,
    vec3 L,
    vec3 albedo,
    vec3 f0,
    float metallic,
    float roughness
)
{
    float NdotL = max(dot(N, L), 0.0);
    if(NdotL <= 0.0)
    {
        return vec3(0.0);
    }
    if(f0.x < 0.0)
    {
        return albedo * (BSDF_LOCAL_DIFFUSE_SCALE * NdotL);
    }

    float specularRoughness =
        max(roughness, BSDF_LOCAL_MIN_SPEC_ROUGHNESS);

    float NdotV = max(dot(N, V), 1.0e-4);

    BsdfTerms bsdf =
        evalBSDFTerms(
            N,
            V,
            L,
            f0,
            metallic,
            specularRoughness,
            NdotV,
            true
        );

    vec3 diffuseTerm =
        bsdf.kD * albedo * (NdotL * BSDF_LOCAL_DIFFUSE_SCALE);
    vec3 specularTerm =
        bsdf.specular * (NdotL * BSDF_LOCAL_SPECULAR_SCALE);
    return diffuseTerm + specularTerm;
}

void g2bSampleDiffuseDirectionMIS(
    vec3 surfacePos,
    vec3 surfaceNormal,
    inout uint rngState,
    out vec3 sampledDirection,
    out float bsdfToMixtureWeight,
    bool useLocalGuide
)
{
    vec3 N = normalize(surfaceNormal);
    sampledDirection = sampleHemisphereCosine(N, rngState);
    bsdfToMixtureWeight = 1.0;
}

#endif
