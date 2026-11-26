


// =====================================================
// T11 - Deterministic scene reflection for glass
//
// Exactly one first-surface reflection ray.  It follows the geometric mirror
// direction and returns either the real opaque-scene radiance at its first
// hit, or the existing miss-shader sky radiance.  Water and later transparent
// hits deliberately contribute nothing here.
// =====================================================


vec3 traceGlassSceneReflectionOneRay(
    vec3 glassSurfacePosition,
    vec3 glassNormal,
    vec3 viewDir,
    vec3 sunDirection
)
{
    vec3 N = normalize(glassNormal);
    vec3 V = normalize(viewDir);
    float NdotV = max(dot(N, V), 0.0);
    if(NdotV <= 0.0)
    {
        return vec3(0.0);
    }

    vec3 reflectionDirection =
        normalize(reflect(-V, N));

    const float glassReflCamDist =
        length(glassSurfacePosition - cam.viewInverse[3].xyz);
    const int maxGlassReflectSteps =
        (glassReflCamDist < 24.0) ? 3 : ((glassReflCamDist < 64.0) ? 2 : 1);

    vec3 currentOrigin =
        glassSurfacePosition + N * 0.002 + reflectionDirection * 0.001;
    vec3 currentDirection = reflectionDirection;
    vec3 throughput = vec3(1.0);
    vec3 reflectedSceneRadiance = vec3(0.0);
    float accumulatedDistance = 0.0;
    bool recordedFirstGuide = false;
    bool appliedTranslucentTint = false;
    bool reachedTerminus = false;

    for(int step = 0; step < maxGlassReflectSteps; ++step)
    {
        hit.hitDistance = 1.0;
        hit.albedoPacked = packAlbedoUnorm(vec3(1.0));
        hit.state = makeTraceState(RT_PURPOSE_FULL) |
            RT_STATE_NO_SUN_DISK_BIT |
            RT_STATE_NO_SUN_GLOW_BIT;
        if(secondaryRayPomSkipEnabled())
        {
            hit.state |= RT_STATE_GI_NO_POM_BIT;
        }

#ifdef RT_ENABLE_SER
        // Perf-L10.8: same ray, same flags; SER only reschedules the hit shader.
        if(cam.perfSerAb.z != 0u)
        {
            serTraceRay(
                currentOrigin,
                0.01,
                currentDirection,
                10000.0,
                secondaryTraverseFlags(),
                0xefu,
                0u
            );
        }
        else
#endif
        {
            #ifdef ORION_RT_STAGE_COUNTERS
            rtCntRayIssued();
            #endif
            traceRayEXT(
                topLevelAS,
                secondaryTraverseFlags(),
                0xefu,
                0,
                0,
                0,
                currentOrigin,
                0.01,
                currentDirection,
                10000.0,
                0
            );
        }

        if(payload.hitDistance < 0.0)
        {
            rrGlassReflectionHitDistance = 65504.0;
            reflectedSceneRadiance +=
                throughput * unpackAlbedoUnorm(payload.albedoPacked);
            reachedTerminus = true;
            break;
        }

        float hitDist = payload.hitDistance;
        vec3 hitPos = currentOrigin + currentDirection * hitDist;
        vec3 hitNormal = normalize(payload.normal);
        vec3 hitAlbedo = unpackAlbedoUnorm(payload.albedoPacked);
        float hitMatId = getLegacyMaterialId(payload.state);
        float hitRoughness = payload.roughness;
        float hitF0Metal = payload.f0Metal;
        vec3 hitEmission = payload.emission;
        float hitPomSelfShadow = payload.pomSelfShadow;

        accumulatedDistance += hitDist;

        if(hitMatId <= 0.5)
        {
            rrGlassReflectionHitDistance = accumulatedDistance;

#if GI_GLASS_DOMINANT_GUIDE
            if(!recordedFirstGuide)
            {
                glassReflectionReceiverValid = true;
                glassReflectionReceiverNormal = hitNormal;
                glassReflectionReceiverAlbedo =
                    clamp(hitAlbedo, vec3(0.0), vec3(1.0));
                glassReflectionReceiverRoughness =
                    clamp(hitRoughness, 0.0, 1.0);
                recordedFirstGuide = true;
            }
#endif

            vec3 reflectionHitEmission =
                hitEmission +
                (GI_GLASS_REFLECTION_LEGACY != 0
                    ? evaluateVLIEmitterRadianceAtHit(
                        hitPos,
                        hitNormal,
                        hitAlbedo
                    )
                    : vec3(0.0));

            reflectedSceneRadiance +=
                throughput * (
                    evaluateOpaqueSurfaceRadiance(
                        hitPos,
                        hitNormal,
                        -currentDirection,
                        hitAlbedo,
                        hitRoughness,
                        hitF0Metal,
                        reflectionHitEmission,
                        hitPomSelfShadow,
                        sunDirection
                    ) +
                    ((GI_GLASS_REFLECTION_LEGACY != 0 && glassReflCamDist < 32.0)
                        ? evaluateVPTExactFaceDirectLighting(
                            hitPos,
                            hitNormal,
                            hitAlbedo
                        )
                        : vec3(0.0)) +
                    hitAlbedo * getSurfaceAmbientRadiance()
                );

            reachedTerminus = true;
            break;
        }
        else if(hitMatId < 2.5)
        {
            vec3 waterNormal =
                dot(hitNormal, currentDirection) > 0.0
                    ? -hitNormal
                    : hitNormal;
            float waterCos =
                clamp(-dot(currentDirection, waterNormal), 0.0, 1.0);
            float waterF = fresnelSchlickScalar(waterCos, 0.02);

            reflectedSceneRadiance +=
                throughput * (1.0 - waterF) * hitAlbedo * (
                    getSurfaceAmbientRadiance() +
                    evaluateDirectSunLighting(
                        hitPos,
                        waterNormal,
                        -currentDirection,
                        vec3(1.0),
                        0.9,
                        0.0,
                        1.0,
                        sunDirection
                    ) * 0.18
                );

            rrGlassReflectionHitDistance = accumulatedDistance;
            if(waterF < 0.02)
            {
                reachedTerminus = true;
                break;
            }

            throughput *= waterF;
            currentOrigin = hitPos + waterNormal * 0.002;
            currentDirection = normalize(reflect(currentDirection, waterNormal));
            continue;
        }
        else
        {
            reflectedSceneRadiance += throughput * hitEmission;

            if(hitDist < 0.02)
            {
                currentOrigin = hitPos + currentDirection * 0.002;
                continue;
            }

            vec3 faceNormal =
                dot(hitNormal, currentDirection) > 0.0
                    ? -hitNormal
                    : hitNormal;
            float faceCos =
                clamp(-dot(currentDirection, faceNormal), 0.0, 1.0);
            float faceF = fresnelSchlickScalar(faceCos, 0.04);
            vec3 faceTint =
                appliedTranslucentTint
                    ? vec3(1.0)
                    : clamp(hitAlbedo, vec3(0.0), vec3(1.0));
            appliedTranslucentTint = true;

            throughput *= (1.0 - faceF) * faceTint;
            if(max(throughput.r, max(throughput.g, throughput.b)) < 0.02)
            {
                rrGlassReflectionHitDistance = accumulatedDistance;
                reachedTerminus = true;
                break;
            }

            currentOrigin = hitPos + currentDirection * 0.002;
            continue;
        }
    }

    if(!reachedTerminus)
    {
        rrGlassReflectionHitDistance =
            accumulatedDistance > 0.0 ? accumulatedDistance : 65504.0;
        reflectedSceneRadiance += throughput * getSurfaceAmbientRadiance();
    }

    return reflectedSceneRadiance;
}