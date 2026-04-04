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

layout(set = 1, binding = 1) uniform CharMaterial {
    float opacity;
    int alphaTest;
    int colorKeyBlack;
    int unlit;
    float emissiveBoost;
    vec3 emissiveTint;
    float specularIntensity;
    int enableNormalMap;
    int enablePOM;
    float pomScale;
    int pomMaxSamples;
    float heightMapVariance;
    float normalMapStrength;
};

layout(set = 1, binding = 2) uniform sampler2D uNormalHeightMap;

layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in vec3 Tangent;
layout(location = 4) in vec3 Bitangent;

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
    return smoothstep(0.0001, 0.005, texelDensity);
}

// Parallax Occlusion Mapping with angle-adaptive sampling
vec2 parallaxOcclusionMap(vec2 uv, vec3 viewDirTS, float lodFactor) {
    float VdotN = abs(viewDirTS.z);

    if (VdotN < 0.15) return uv;

    float angleFactor = clamp(VdotN, 0.15, 1.0);
    int maxS = pomMaxSamples;
    int minS = max(maxS / 4, 4);
    int numSamples = int(mix(float(minS), float(maxS), angleFactor));
    numSamples = int(mix(float(minS), float(numSamples), 1.0 - lodFactor));

    float layerDepth = 1.0 / float(numSamples);
    float currentLayerDepth = 0.0;

    vec2 P = viewDirTS.xy / max(VdotN, 0.15) * pomScale;
    float maxOffset = pomScale * 3.0;
    P = clamp(P, vec2(-maxOffset), vec2(maxOffset));
    vec2 deltaUV = P / float(numSamples);

    vec2 currentUV = uv;
    float currentDepthMapValue = 1.0 - texture(uNormalHeightMap, currentUV).a;

    for (int i = 0; i < 64; i++) {
        if (i >= numSamples || currentLayerDepth >= currentDepthMapValue) break;
        currentUV -= deltaUV;
        currentDepthMapValue = 1.0 - texture(uNormalHeightMap, currentUV).a;
        currentLayerDepth += layerDepth;
    }

    vec2 prevUV = currentUV + deltaUV;
    float afterDepth = currentDepthMapValue - currentLayerDepth;
    float beforeDepth = (1.0 - texture(uNormalHeightMap, prevUV).a) - currentLayerDepth + layerDepth;
    float weight = afterDepth / (afterDepth - beforeDepth + 0.0001);
    vec2 result = mix(currentUV, prevUV, weight);

    float fadeFactor = smoothstep(0.15, 0.35, VdotN);
    return mix(uv, result, fadeFactor);
}

void main() {
    float lodFactor = computeLodFactor();

    vec3 vertexNormal = normalize(Normal);
    if (!gl_FrontFacing) vertexNormal = -vertexNormal;

    vec2 finalUV = TexCoord;

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

    vec4 texColor = texture(uTexture, finalUV);

    if (alphaTest != 0) {
        // Screen-space sharpened alpha for alpha-to-coverage anti-aliasing.
        // Rescales alpha so the 0.5 cutoff maps to exactly the texel boundary,
        // giving smooth edges when MSAA + alpha-to-coverage is active.
        float aGrad = fwidth(texColor.a);
        texColor.a = clamp((texColor.a - 0.5) / max(aGrad, 0.001) * 0.5 + 0.5, 0.0, 1.0);
        if (texColor.a < 1.0 / 255.0) discard;
    }
    if (colorKeyBlack != 0) {
        float lum = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
        float ck = smoothstep(0.12, 0.30, lum);
        texColor.a *= ck;
        if (texColor.a < 0.01) discard;
    }

    // Compute normal (with normal mapping if enabled)
    vec3 norm = vertexNormal;
    if (enableNormalMap != 0 && lodFactor < 0.99 && normalMapStrength > 0.001) {
        vec3 mapNormal = texture(uNormalHeightMap, finalUV).rgb * 2.0 - 1.0;
        mapNormal.xy *= normalMapStrength;
        mapNormal = normalize(mapNormal);
        vec3 worldNormal = normalize(TBN * mapNormal);
        if (!gl_FrontFacing) worldNormal = -worldNormal;
        float blendFactor = max(lodFactor, 1.0 - normalMapStrength);
        norm = normalize(mix(worldNormal, vertexNormal, blendFactor));
    }

    vec3 result;

    if (unlit != 0) {
        vec3 warm = emissiveTint * emissiveBoost;
        result = texColor.rgb * (1.0 + warm);
    } else {
        vec3 ldir = normalize(-lightDir.xyz);
        float diff = max(dot(norm, ldir), 0.0);

        vec3 viewDir = normalize(viewPos.xyz - FragPos);
        vec3 halfDir = normalize(ldir + viewDir);
        float spec = pow(max(dot(norm, halfDir), 0.0), 32.0) * specularIntensity;

        float shadow = 1.0;
        if (shadowParams.x > 0.5) {
            float normalOffset = SHADOW_TEXEL * 2.0 * (1.0 - abs(dot(norm, ldir)));
            vec3 biasedPos = FragPos + norm * normalOffset;
            vec4 lsPos = lightSpaceMatrix * vec4(biasedPos, 1.0);
            vec3 proj = lsPos.xyz / lsPos.w;
            proj.xy = proj.xy * 0.5 + 0.5;
            if (proj.x >= 0.0 && proj.x <= 1.0 &&
                proj.y >= 0.0 && proj.y <= 1.0 &&
                proj.z >= 0.0 && proj.z <= 1.0) {
                float bias = max(0.0005 * (1.0 - dot(norm, ldir)), 0.00005);
                shadow = sampleShadowPCF(uShadowMap, vec3(proj.xy, proj.z - bias));
            }
            shadow = mix(1.0, shadow, shadowParams.y);
        }

        result = ambientColor.rgb * texColor.rgb
               + shadow * (diff * lightColor.rgb * texColor.rgb + spec * lightColor.rgb);
    }

    float dist = length(viewPos.xyz - FragPos);
    float fogFactor = clamp((fogParams.y - dist) / (fogParams.y - fogParams.x), 0.0, 1.0);
    result = mix(fogColor.rgb, result, fogFactor);

    outColor = vec4(result, texColor.a * opacity);
}
