#version 460 core
#include "includes/screen_space_reconstruction.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

uniform sampler2D gDepth;
uniform sampler2D gPackedNormalRM;
uniform sampler2D gAlbedoAO;
uniform sampler2D randTex;
uniform sampler2D prevColor;

layout(rgba16f, binding = 0) writeonly uniform image2D outputImage;

uniform mat4 invProj;
uniform mat4 invView;
uniform mat4 view;
uniform mat4 proj;
uniform float maxRayLenVS;
uniform int numSteps;
uniform float thickness;
uniform vec2 screenSize;
uniform vec2 workSize;
uniform float cameraNear;
uniform float cameraFar;

float PixelSizeVS(float absViewZ) {
    float tanHalfFovy = 1.0 / proj[1][1];
    float viewHeight = 2.0 * absViewZ * tanHalfFovy;
    return viewHeight / max(workSize.y, 1.0);
}

float SampleDepthNearest(vec2 uv) {
    ivec2 size = textureSize(gDepth, 0);
    ivec2 texel = clamp(ivec2(uv * vec2(size)), ivec2(0), size - 1);
    return texelFetch(gDepth, texel, 0).r;
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 dstSize = imageSize(outputImage);
    if (any(greaterThanEqual(id, dstSize))) {
        return;
    }

    vec2 uvWork = (vec2(id) + 0.5) / workSize;
    float depthRaw = texture(gDepth, uvWork).r;
    if (depthRaw >= 0.999) {
        imageStore(outputImage, id, vec4(0.0));
        return;
    }

    vec3 originVS = ReconstructViewPosition(uvWork, depthRaw, invProj);
    if (originVS.z >= 0.0) {
        imageStore(outputImage, id, vec4(0.0));
        return;
    }

    vec3 normalVS = DecodeSceneNormalVS(texture(gPackedNormalRM, uvWork).rg, view, true);
    vec2 rands = texture(randTex, uvWork).rg;

    float r = sqrt(rands.x);
    float phi = 6.28318530718 * rands.y;
    vec3 diskSample = vec3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - rands.x)));

    vec3 up = abs(normalVS.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, normalVS));
    vec3 bitangent = cross(normalVS, tangent);
    vec3 directionVS = normalize(tangent * diskSample.x + bitangent * diskSample.y + normalVS * diskSample.z);

    if (dot(directionVS, normalVS) < 0.01) {
        imageStore(outputImage, id, vec4(0.0));
        return;
    }

    float pixelVS = PixelSizeVS(abs(originVS.z));
    float stepLenVS = max(maxRayLenVS / float(max(numSteps, 1)), pixelVS * 0.75);
    vec3 stepVS = directionVS * stepLenVS;
    vec3 rayPosVS = originVS + normalVS * max(thickness * 2.0, pixelVS * 1.5);
    vec3 hitPosVS = vec3(0.0);
    bool foundHit = false;
    float adaptiveThickness = max(thickness, pixelVS * 2.0);

    for (int i = 0; i < numSteps; ++i) {
        rayPosVS += stepVS;
        vec2 sampleUV = ProjectViewPositionToUV(rayPosVS, proj);

        if (any(lessThan(sampleUV, vec2(0.0))) || any(greaterThan(sampleUV, vec2(1.0)))) {
            break;
        }

        float sampleDepth = SampleDepthNearest(sampleUV);
        if (sampleDepth >= 0.999) {
            continue;
        }

        vec3 surfaceVS = ReconstructViewPosition(sampleUV, sampleDepth, invProj);
        float depthDifference = surfaceVS.z - rayPosVS.z;
        bool intersects = depthDifference >= 0.0 && depthDifference <= adaptiveThickness;

        if (intersects) {
            vec3 hitNormalVS = DecodeSceneNormalVS(texture(gPackedNormalRM, sampleUV).rg, view, true);
            if (dot(hitNormalVS, -directionVS) <= 0.05) {
                continue;
            }
            foundHit = true;
            hitPosVS = surfaceVS;
            break;
        }
    }

    vec3 indirectIrradiance = vec3(0.0);
    float validityMask = 0.0;

    if (foundHit) {
        vec2 hitUV = ProjectViewPositionToUV(hitPosVS, proj);
        if (all(greaterThanEqual(hitUV, vec2(0.0))) && all(lessThan(hitUV, vec2(1.0)))) {
            vec3 hitColor = texture(prevColor, hitUV).rgb;
            float hitDistance = length(hitPosVS - originVS);
            float attenuation = 1.0 - smoothstep(0.0, maxRayLenVS, hitDistance);
            attenuation *= attenuation;

            vec3 hitDirection = normalize(hitPosVS - originVS);
            float cosineAtReceiver = max(dot(normalVS, hitDirection), 0.0);
            vec3 irradiance = hitColor * attenuation * cosineAtReceiver * (1.0 / 3.14159265);

            if (dot(irradiance, vec3(0.2126, 0.7152, 0.0722)) > 0.0001) {
                indirectIrradiance = irradiance;
                validityMask = 1.0;
            }
        }
    }

    imageStore(outputImage, id, vec4(max(indirectIrradiance, vec3(0.0)), validityMask));
}
