#version 460 core

layout(location = 0) in vec3 aPosition;

uniform mat4 model;

out vec3 worldPosition;

void main() {
    vec4 worldPos = model * vec4(aPosition, 1.0);
    worldPosition = worldPos.xyz;
    gl_Position = worldPos; // Pass-through to geometry shader
}
