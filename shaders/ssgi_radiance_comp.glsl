#version 460 core

layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0) uniform sampler2D gAlbedoAO;
layout(binding = 1) uniform sampler2D gSpecularF0;
layout(binding = 2) uniform sampler2D gEmissive;
layout(binding = 3) uniform sampler2D historyColor;
layout(binding = 4) uniform sampler2D linearDepthQuarter;
layout(binding = 5, rgba16f) writeonly uniform image2D outRadiance;

uniform vec2 invRadianceSize;
uniform vec3 envColor;

float Luma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 outSize = imageSize(outRadiance);
    if (any(greaterThanEqual(id, outSize))) {
        return;
    }

    vec2 uv = (vec2(id) + 0.5) * invRadianceSize;

    vec4 albedoAO = textureLod(gAlbedoAO, uv, 0.0);
    vec4 specF0 = textureLod(gSpecularF0, uv, 0.0);
    vec3 emissive = max(textureLod(gEmissive, uv, 0.0).rgb * specF0.a, vec3(0.0));

    float ao = clamp(albedoAO.a, 0.0, 1.0);
    vec3 albedo = max(albedoAO.rgb, vec3(0.0));

    // Keep source radiance independent from the final lit frame to avoid recursive energy feedback.
    vec3 diffuseBase = albedo * mix(0.03, 0.10, ao) * max(envColor, vec3(0.02));

    // Lightly blend history color only as chromatic guidance, bounded to avoid ambient double counting.
    vec3 hist = max(textureLod(historyColor, uv, 0.0).rgb, vec3(0.0));
    float histLuma = Luma(hist);
    vec3 histChroma = (histLuma > 1e-4) ? (hist / histLuma) : vec3(1.0);
    vec3 boundedHistory = histChroma * min(histLuma, 1.2) * 0.06;

    float linDepth = textureLod(linearDepthQuarter, uv, 0.0).r;
    if (linDepth > 65000.0) {
        imageStore(outRadiance, id, vec4(0.0));
        return;
    }

    vec3 radiance = diffuseBase + emissive + boundedHistory;
    radiance = clamp(radiance, vec3(0.0), vec3(12.0));

    imageStore(outRadiance, id, vec4(radiance, 1.0));
}
