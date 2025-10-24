#version 330 core

layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoords;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform mat4 prevView;
uniform mat4 prevProjection;

uniform vec2 jitter;
uniform vec2 prevJitter;
uniform vec2 screenSize;

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
    
    // Apply current jitter
    currentPos.xy += jitter * currentPos.w;
    
    // Previous frame position (using same model matrix for simplicity)
    vec4 prevViewPos = prevView * worldPos;
    prevPos = prevProjection * prevViewPos;
    
    // Apply previous jitter
    prevPos.xy += prevJitter * prevPos.w;
    
    gl_Position = currentPos;
}