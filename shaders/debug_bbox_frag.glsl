#version 460 core

// Debug Bounding Box Visualization - Fragment Shader
// Renders wireframe bounding boxes with per-instance colors from vertex shader

in vec4 v_Color;  // Color from vertex shader (per-instance)

out vec4 FragColor;

void main()
{
    FragColor = v_Color;
}
