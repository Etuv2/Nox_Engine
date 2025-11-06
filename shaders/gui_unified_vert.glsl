#version 460 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;

out vec2 TexCoord;
out vec2 FragPos;

uniform mat4 mvp;

void main() {
    TexCoord = aUV;
    FragPos = aPos; // Pass normalized position for SDF calculations
    gl_Position = mvp * vec4(aPos, 0.0, 1.0);
}