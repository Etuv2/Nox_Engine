#version 460 core
layout(location = 0) in vec3 aPos;

// Light-space matrix per-slice
uniform mat4 lightSpaceMatrix;

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

void main() {
#if BASE_INSTANCE_SUPPORT
    uint idx = gl_BaseInstance;
#else
    uint idx = uint(uObjectIndex);
#endif
    mat4 model = modelMatrices[idx];
    gl_Position = lightSpaceMatrix * model * vec4(aPos, 1.0);
}
