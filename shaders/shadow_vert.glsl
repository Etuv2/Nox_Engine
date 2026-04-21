#version 460 core
#include "includes/skinning_common.glsl"
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aTexCoords;
layout(location = 10) in vec2 aTexCoords1;
layout(location = 4) in ivec4 aBoneIDs;
layout(location = 5) in vec4 aBoneWeights;

// Light-space matrix per-slice
uniform mat4 lightSpaceMatrix;
uniform mat4 model;
uniform bool uUseModelMatrixUniform = false;

// Per-draw model matrices uploaded by MDIBatch at binding=3
layout(std430, binding = 3) buffer ModelMatrices {
    mat4 modelMatrices[];
};

// Define availability of gl_BaseInstance (requires GLSL 460)
#if __VERSION__ >= 460
#define BASE_INSTANCE_SUPPORT 1
#else
#define BASE_INSTANCE_SUPPORT 0
#endif

#if BASE_INSTANCE_SUPPORT == 0
uniform int uObjectIndex;
#endif

out vec2 TexCoords;
out vec2 TexCoords1;

void main() {
    vec4 localPos = vec4(aPos, 1.0);
    ApplySkinning(aBoneIDs, aBoneWeights, localPos);
    TexCoords = aTexCoords;
    TexCoords1 = aTexCoords1;

    mat4 resolvedModel = model;
    if (!uUseModelMatrixUniform) {
#if BASE_INSTANCE_SUPPORT
        uint idx = gl_BaseInstance;
#else
        uint idx = uint(uObjectIndex);
#endif
        resolvedModel = modelMatrices[idx];
    }

    gl_Position = lightSpaceMatrix * resolvedModel * localPos;
}
