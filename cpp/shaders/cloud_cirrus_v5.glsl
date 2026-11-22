#ifndef CLOUD_CIRRUS_V5_GLSL
#define CLOUD_CIRRUS_V5_GLSL

// 2026-11-21 统一风场：唯一宿主 volumetric_clouds_v5.comp 在 include 本文件前
// 已声明 cam（camera_ubo.glsl@9）。
#include "wind_common.glsl"

// High-altitude icy cirrus veil shader for volumetric_clouds_v5.comp.
// Fixes the Y/Z swizzle bug in legacy high_cloud_field.glsl, uses anisotropic
// curl-sheared ice filaments, distance-aware LOD to prevent horizon aliasing,
// and real sky LUT + twilight afterglow shading.

const float CIRRUS_PI = 3.14159265359;

float cirrusHG(float cosTheta, float g)
{
    float g2 = g * g;
    float denom = max(1.0 + g2 - 2.0 * g * cosTheta, 1.0e-4);
    return (1.0 - g2) / (4.0 * CIRRUS_PI * denom * sqrt(denom));
}

bool cirrusIntersectSphere(vec3 origin, vec3 dir, vec3 center, float radius, out float t)
{
    vec3 oc = origin - center;
    float b = dot(oc, dir);
    float c = dot(oc, oc) - radius * radius;
    float h = b * b - c;
    if(h < 0.0)
    {
        t = 1.0e6;
        return false;
    }
    float s = sqrt(h);
    float t0 = -b - s;
    float t1 = -b + s;
    t = (t0 > 0.0) ? t0 : t1;
    return t > 0.0;
}

vec4 sampleCirrusV5(
    vec3 origin,
    vec3 direction,
    vec3 lightDir,
    float sunElevation,
    vec3 skyRayColor,
    vec3 skyZenithColor,
    vec3 directSunColor,
    float pixelAngle,
    float sunsetWarmth,
    float maxSceneDist,
    out float outDistance)
{
    outDistance = 1.0e6;

    vec4 hc0 = cam.highCloudParams0;
    if(hc0.x < 0.5 || abs(cam.weatherParams.z - 1.0) >= 0.25)
    {
        return vec4(0.0, 0.0, 0.0, 1.0);
    }

    float heightOffset = max(hc0.y, 800.0);
    float scale = max(hc0.z, 0.05);
    float speed = hc0.w;

    vec4 hc1 = cam.highCloudParams1;
    float densityKnob = max(hc1.x, 0.0);
    float coverage = clamp(hc1.y, 0.0, 1.0);
    int octaves = clamp(int(hc1.z + 0.5), 1, 6);
    float fadeDistance = max(hc1.w, 5000.0);
    float brightness = max(cam.highCloudParams2.x, 0.0);
    if(densityKnob <= 0.0 || coverage <= 0.005 || brightness <= 0.0)
    {
        return vec4(0.0, 0.0, 0.0, 1.0);
    }

    // Sphere centered strictly below player XZ anchor with vertical Y radius offset (fixed Y/Z swizzle).
    vec3 sphereCenter = vec3(cam.cloudParams3.x, cam.cloudParams0.x - TYNDALL_RADIUS, cam.cloudParams3.y);
    float cirrusAlt = cam.cloudParams0.y + heightOffset;
    float shellRadius = TYNDALL_RADIUS + cirrusAlt;

    float t;
    if(!cirrusIntersectSphere(origin, direction, sphereCenter, shellRadius, t))
    {
        return vec4(0.0, 0.0, 0.0, 1.0);
    }
    if(t > maxSceneDist || t > 240000.0)
    {
        return vec4(0.0, 0.0, 0.0, 1.0);
    }

    vec3 hitPos = origin + direction * t;
    vec3 localUp = normalize(hitPos - sphereCenter);

    // Horizon fade & rain attenuation (cirrus is subtle/obscured during heavy downpours)
    float rain = clamp(cam.weatherParams.x, 0.0, 1.0);
    float horizonAlpha = smoothstep(0.008, 0.085, abs(direction.y));
    float distFade = exp(-t / fadeDistance) * horizonAlpha * mix(1.0, 0.35, rain);
    if(distFade <= 0.003)
    {
        return vec4(0.0, 0.0, 0.0, 1.0);
    }

    float cTime = max(uintBitsToFloat(cam.padding), 0.0);
    // 2026-11-21 统一风场：方向改读 cam.windParams.x（windDirXZ），无效回落
    // ≈原方向（原 normalize(0.86,0.51) 与出厂 0.862/0.507 同族，差 <0.5°）。
    vec2 windDir = windDirXZ();
    vec2 perpDir = vec2(-windDir.y, windDir.x);
    vec2 wind = windDir * (speed * cTime * 16.0);

    vec2 xz = hitPos.xz + wind;
    // Rotate into anisotropic wind-aligned frame (elongate filaments along upper jet stream)
    vec2 pAligned = vec2(dot(xz, windDir) * 0.28, dot(xz, perpDir) * 1.15);
    float baseFreq = 0.000065 / scale;
    float baseLod = clamp(log2(max(t * max(pixelAngle, 1.0e-4) * baseFreq * 96.0, 1.0)), 0.0, 5.0);

    // Macro patch mask (avoids covering the entire sky uniformly)
    float macroMask = textureLod(cloudAtlas, (hitPos.xz + wind * 0.4) * (baseFreq * 0.14), 0.0).r;
    float regionMask = smoothstep(1.0 - coverage * 1.15, 1.0 - coverage * 0.35 + 0.20, macroMask);
    if(regionMask <= 0.005)
    {
        return vec4(0.0, 0.0, 0.0, 1.0);
    }

    // Domain-shear curl warp for wispy mare's tail tendrils
    vec2 curlSample = textureLod(cloudWorley, vec3(pAligned * (baseFreq * 0.85), 0.31), baseLod).rg * 2.0 - 1.0;
    vec2 pWarped = pAligned * baseFreq + curlSample * 0.065;

    // Fast 3-fetch dual-channel wispy filament FBM (preserves mare's tail silhouette at ~half cost)
    vec2 shearStep = vec2(curlSample.y, -curlSample.x) * 0.04 + vec2(0.17, 0.31);
    vec2 q0 = pWarped;
    float s0 = textureLod(baseTex, vec3(q0.x, 0.43, q0.y), baseLod).r;
    float f0 = 1.0 - abs(s0 * 2.0 - 1.0);
    float fbm = 0.55 * mix(s0, f0 * f0, 0.65);
    float wSum = 0.55;

    if(octaves >= 2)
    {
        vec2 q1 = q0 * 2.15 + shearStep;
        vec2 s1 = textureLod(cloudWorley, vec3(q1.x * 0.9, 0.31, q1.y * 0.9),
            clamp(baseLod + 0.5, 0.0, 5.0)).rg;
        float f1 = 1.0 - abs(s1.r * 2.0 - 1.0);
        fbm += 0.264 * mix(s1.r, f1 * f1, 0.65);
        wSum += 0.264;

        if(octaves >= 3)
        {
            vec2 q2 = q1 * 2.15 + shearStep;
            vec2 s2 = textureLod(cloudWorley, vec3(q2.x * 0.9, 0.57, q2.y * 0.9),
                clamp(baseLod + 1.1, 0.0, 5.0)).rg;
            float f2 = 1.0 - abs(s2.r * 2.0 - 1.0);
            fbm += 0.127 * mix(s2.r, f2 * f2, 0.65);
            wSum += 0.127;

            if(octaves >= 4)
            {
                float microS = mix(s1.g, s2.g, 0.65);
                float f3 = 1.0 - abs(microS * 2.0 - 1.0);
                fbm += 0.061 * mix(microS, f3 * f3, 0.65);
                wSum += 0.061;
            }
        }
    }
    fbm /= max(wSum, 1.0e-4);

    float thresh = mix(0.76, 0.32, coverage * regionMask);
    float cirrusAmount = smoothstep(thresh, thresh + 0.26, fbm) * regionMask * distFade;
    if(cirrusAmount <= 0.002)
    {
        return vec4(0.0, 0.0, 0.0, 1.0);
    }

    float opticalDepth = densityKnob * 0.24 * cirrusAmount / max(abs(direction.y), 0.08);
    float alpha = clamp(1.0 - exp(-opticalDepth), 0.0, 0.88);
    if(alpha <= 0.002)
    {
        return vec4(0.0, 0.0, 0.0, 1.0);
    }
    outDistance = t;

    // Cirrus stays sunlit even a few degrees after ground sunset -> rich peach/coral/crimson wisps
    float highSunVis = smoothstep(-0.13, 0.04, sunElevation);
    float twilightPeak = smoothstep(-0.13, -0.02, sunElevation) * (1.0 - smoothstep(0.02, 0.24, sunElevation));
    vec3 afterglowColor = mix(
        vec3(1.00, 0.42, 0.26),
        vec3(0.96, 0.34, 0.46),
        smoothstep(-0.12, 0.02, -sunElevation)
    );
    vec3 baseSunCol = max(directSunColor, vec3(0.02));
    vec3 cirrusSunColor = mix(baseSunCol, max(baseSunCol, afterglowColor * 1.35),
        twilightPeak * clamp(sunsetWarmth, 0.0, 2.0));

    float cosTheta = clamp(dot(direction, lightDir), -1.0, 1.0);
    float phase = mix(cirrusHG(cosTheta, 0.72), cirrusHG(cosTheta, -0.20), 0.25);
    float sunIncidence = smoothstep(-0.08, 0.25, dot(lightDir, localUp));

    // Self-extinction through thin ice crystals stays subtle so cirrus remains luminous
    float selfTrans = exp(-cirrusAmount * 0.45);
    vec3 directLit = cirrusSunColor * highSunVis * sunIncidence * (0.28 + phase * 1.45) * selfTrans * mix(1.0, 0.35, rain);
    vec3 ambientLit = mix(skyZenithColor, skyRayColor, 0.45) * (0.65 + 0.35 * selfTrans);

    vec3 radiance = (directLit + ambientLit) * brightness;
    // Premultiply alpha & preserve crisp atmospheric integration into distant sky
    vec3 finalRgb = radiance * alpha;
    float finalTrans = 1.0 - alpha;
    return vec4(finalRgb, finalTrans);
}

#endif
