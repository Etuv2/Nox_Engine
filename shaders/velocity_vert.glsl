#version 460 core

layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoords;

uniform mat4 model;
uniform mat4 prevModel;
uniform mat4 view;
uniform mat4 projection;
uniform mat4 prevView;
uniform mat4 prevProjection;

out vec2 TexCoord;
out vec4 currentPos;
out vec4 prevPos;

void main()
{
    TexCoord = aTexCoords;
    
    // Current frame position
    vec4 worldPos = model * vec4(aPos, 1.0);
    vec4 viewPos = view * worldPos;
    currentPos = projection * viewPos;
    
    // Previous frame position from the stable previous-world transform
    vec4 prevWorldPos = prevModel * vec4(aPos, 1.0);
    vec4 prevViewPos = prevView * prevWorldPos;
    prevPos = prevProjection * prevViewPos;
    
    gl_Position = currentPos;
}
