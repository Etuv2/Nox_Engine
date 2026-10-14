#version 460 core
#include "includes/skinning_common.glsl"

// Editor selection mask (SelectionOutlinePass): position only, skinned like the G-buffer pass.
layout(location = 0) in vec3 aPos;
layout(location = 4) in ivec4 aBoneIDs;
layout(location = 5) in vec4 aBoneWeights;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

void main()
{
    vec4 localPos = vec4(aPos, 1.0);
    ApplySkinning(aBoneIDs, aBoneWeights, localPos);
    gl_Position = projection * view * model * localPos;
}
