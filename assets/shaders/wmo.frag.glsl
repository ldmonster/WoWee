#version 450

layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
    mat4 lightSpaceMatrix;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambientColor;
    vec4 viewPos;
    vec4 fogColor;
    vec4 fogParams;
    vec4 shadowParams;
};

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(set = 1, binding = 1) uniform WMOMaterial {
    int hasTexture;
    int alphaTest;
    int unlit;
    int isInterior;
    float specularIntensity;
    int isWindow;
    int enableNormalMap;
    int enablePOM;
    float pomScale;
    int pomMaxSamples;
    float heightMapVariance;
    float normalMapStrength;
    int isLava;
    float wmoAmbientR;
    float wmoAmbientG;
    float wmoAmbientB;
};

layout(set = 1, binding = 2) uniform sampler2D uNormalHeightMap;

layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in vec4 VertColor;
layout(location = 4) in vec3 Tangent;
layout(location = 5) in vec3 Bitangent;

layout(location = 0) out vec4 outColor;

const float SHADOW_TEXEL = 1.0 / 4096.0;

float sampleShadowPCF(sampler2DShadow smap, vec3 coords) {
    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            shadow += texture(smap, vec3(coords.xy + vec2(x, y) * SHADOW_TEXEL, coords.z));
        }
    }
    return shadow / 9.0;
}

// LOD factor from screen-space UV derivatives
float computeLodFactor() {
    vec2 dx = dFdx(TexCoord);
    vec2 dy = dFdy(TexCoord);
    float texelDensity = max(dot(dx, dx), dot(dy, dy));
    // Low density = close/head-on = full detail (0)
    // High density = far/steep = vertex normals only (1)
    return smoothstep(0.0001, 0.005, texelDensity);
}

// Parallax Occlusion Mapping with angle-adaptive sampling
vec2 parallaxOcclusionMap(vec2 uv, vec3 viewDirTS, float lodFactor) {
    float VdotN = abs(viewDirTS.z);  // 1=head-on, 0=grazing

    // Fade out POM at grazing angles to avoid distortion
    if (VdotN < 0.15) return uv;

    float angleFactor = clamp(VdotN, 0.15, 1.0);
    int maxS = pomMaxSamples;
    int minS = max(maxS / 4, 4);
    int numSamples = int(mix(float(minS), float(maxS), angleFactor));
    numSamples = int(mix(float(minS), float(numSamples), 1.0 - lodFactor));

    float layerDepth = 1.0 / float(numSamples);
    float currentLayerDepth = 0.0;

    // Direction to shift UV per layer — clamp denominator to prevent explosion at grazing angles
    vec2 P = viewDirTS.xy / max(VdotN, 0.15) * pomScale;
    // Hard-clamp total UV offset to prevent texture swimming
    float maxOffset = pomScale * 3.0;
    P = clamp(P, vec2(-maxOffset), vec2(maxOffset));
    vec2 deltaUV = P / float(numSamples);

    vec2 currentUV = uv;
    float currentDepthMapValue = 1.0 - texture(uNormalHeightMap, currentUV).a;

    // Ray march through layers
    for (int i = 0; i < 64; i++) {
        if (i >= numSamples || currentLayerDepth >= currentDepthMapValue) break;
        currentUV -= deltaUV;
        currentDepthMapValue = 1.0 - texture(uNormalHeightMap, currentUV).a;
        currentLayerDepth += layerDepth;
    }

    // Interpolate between last two layers for smooth result
    vec2 prevUV = currentUV + deltaUV;
    float afterDepth = currentDepthMapValue - currentLayerDepth;
    float beforeDepth = (1.0 - texture(uNormalHeightMap, prevUV).a) - currentLayerDepth + layerDepth;
    float weight = afterDepth / (afterDepth - beforeDepth + 0.0001);
    vec2 result = mix(currentUV, prevUV, weight);

    // Fade toward original UV at grazing angles for smooth transition
    float fadeFactor = smoothstep(0.15, 0.35, VdotN);
    return mix(uv, result, fadeFactor);
}

void main() {
    float lodFactor = computeLodFactor();

    vec3 vertexNormal = normalize(Normal);
    if (!gl_FrontFacing) vertexNormal = -vertexNormal;

    // Compute final UV (with POM if enabled)
    vec2 finalUV = TexCoord;

    // Lava/magma: scroll UVs for flowing effect
    if (isLava != 0) {
        float time = fogParams.z;
        // Scroll both axes — pools get horizontal flow, waterfalls get vertical flow
        // (UV orientation depends on mesh, so animate both)
        finalUV += vec2(time * 0.04, time * 0.06);
    }

    // Build TBN matrix
    vec3 T = normalize(Tangent);
    vec3 B = normalize(Bitangent);
    vec3 N = vertexNormal;
    mat3 TBN = mat3(T, B, N);

    if (enablePOM != 0 && heightMapVariance > 0.001 && lodFactor < 0.99) {
        mat3 TBN_inv = transpose(TBN);
        vec3 viewDirWorld = normalize(viewPos.xyz - FragPos);
        vec3 viewDirTS = TBN_inv * viewDirWorld;
        finalUV = parallaxOcclusionMap(TexCoord, viewDirTS, lodFactor);
    }

    vec4 texColor = hasTexture != 0 ? texture(uTexture, finalUV) : vec4(1.0);
    if (alphaTest != 0 && texColor.a < 0.5) discard;

    // Compute normal (with normal mapping if enabled)
    vec3 norm = vertexNormal;
    if (enableNormalMap != 0 && lodFactor < 0.99 && normalMapStrength > 0.001) {
        vec3 mapNormal = texture(uNormalHeightMap, finalUV).rgb * 2.0 - 1.0;
        mapNormal = normalize(mapNormal);
        vec3 worldNormal = normalize(TBN * mapNormal);
        if (!gl_FrontFacing) worldNormal = -worldNormal;
        // Linear blend: strength controls how much normal map detail shows,
        // LOD fades out at distance. Both multiply for smooth falloff.
        float blend = clamp(normalMapStrength, 0.0, 1.0) * (1.0 - lodFactor);
        norm = normalize(mix(vertexNormal, worldNormal, blend));
    }

    vec3 result;

    // Sample shadow map for all WMO groups (interior groups with 0x2000 flag
    // include covered outdoor areas like archways/streets that should receive shadows)
    float shadow = 1.0;
    if (shadowParams.x > 0.5) {
        vec3 ldir = normalize(-lightDir.xyz);
        float normalOffset = SHADOW_TEXEL * 2.0 * (1.0 - abs(dot(norm, ldir)));
        vec3 biasedPos = FragPos + norm * normalOffset;
        vec4 lsPos = lightSpaceMatrix * vec4(biasedPos, 1.0);
        vec3 proj = lsPos.xyz / lsPos.w;
        proj.xy = proj.xy * 0.5 + 0.5;
        if (proj.x >= 0.0 && proj.x <= 1.0 &&
            proj.y >= 0.0 && proj.y <= 1.0 &&
            proj.z >= 0.0 && proj.z <= 1.0) {
            float bias = max(0.0005 * (1.0 - abs(dot(norm, ldir))), 0.00005);
            shadow = sampleShadowPCF(uShadowMap, vec3(proj.xy, proj.z - bias));
        }
        shadow = mix(1.0, shadow, shadowParams.y);
    }

    if (isLava != 0) {
        // Lava is self-luminous — bright emissive, no shadows
        result = texColor.rgb * 1.5;
    } else if (unlit != 0) {
        result = texColor.rgb * shadow;
    } else if (isInterior != 0) {
        // WMO interior: vertex colors (MOCV) are pre-baked lighting from the artist.
        // The MOHD ambient color tints/floors the vertex colors so dark spots don't
        // go completely black, matching the WoW client's interior shading.
        vec3 wmoAmbient = vec3(wmoAmbientR, wmoAmbientG, wmoAmbientB);
        // Clamp ambient to at least 0.3 to avoid total darkness when MOHD color is zero
        wmoAmbient = max(wmoAmbient, vec3(0.3));
        vec3 mocv = max(VertColor.rgb, wmoAmbient);
        result = texColor.rgb * mocv * shadow;
    } else {
        vec3 ldir = normalize(-lightDir.xyz);
        float diff = max(dot(norm, ldir), 0.0);

        vec3 viewDir = normalize(viewPos.xyz - FragPos);
        vec3 halfDir = normalize(ldir + viewDir);
        float spec = pow(max(dot(norm, halfDir), 0.0), 32.0) * specularIntensity;

        result = ambientColor.rgb * texColor.rgb
               + shadow * (diff * lightColor.rgb * texColor.rgb + spec * lightColor.rgb);

        result *= max(VertColor.rgb, vec3(0.5));
    }

    float dist = length(viewPos.xyz - FragPos);
    float fogFactor = clamp((fogParams.y - dist) / (fogParams.y - fogParams.x), 0.0, 1.0);
    result = mix(fogColor.rgb, result, fogFactor);

    float alpha = texColor.a;

    // Window glass: opaque but simulates dark tinted glass with reflections.
    if (isWindow != 0) {
        vec3 viewDir = normalize(viewPos.xyz - FragPos);
        float NdotV = abs(dot(norm, viewDir));
        float fresnel = 0.08 + 0.92 * pow(1.0 - NdotV, 4.0);

        vec3 ldir = normalize(-lightDir.xyz);
        vec3 reflectDir = reflect(-viewDir, norm);
        float sunGlint = pow(max(dot(reflectDir, ldir), 0.0), 32.0);

        float baseBrightness = mix(0.3, 0.9, sunGlint);
        vec3 glass = result * baseBrightness;

        vec3 reflectTint = mix(ambientColor.rgb * 1.2, vec3(0.6, 0.75, 1.0), 0.6);
        glass = mix(glass, reflectTint, fresnel * 0.8);

        vec3 halfDir = normalize(ldir + viewDir);
        float spec = pow(max(dot(norm, halfDir), 0.0), 256.0);
        glass += spec * lightColor.rgb * 0.8;

        float specBroad = pow(max(dot(norm, halfDir), 0.0), 12.0);
        glass += specBroad * lightColor.rgb * 0.12;

        result = glass;
        if (isWindow == 2) {
            // Instance/dungeon glass: mostly transparent to see through
            alpha = mix(0.12, 0.35, fresnel);
        } else {
            alpha = mix(0.4, 0.95, NdotV);
        }
    }

    outColor = vec4(result, alpha);
}
