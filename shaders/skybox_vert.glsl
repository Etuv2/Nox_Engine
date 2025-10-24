#version 450 core
layout (location = 0) in vec3 aPos;
out vec3 TexCoords;

uniform mat4 projection;
uniform mat4 view;

void main()
{
    // Remove translation from the view matrix so the skybox stays centered.
    mat4 viewNoTranslation = mat4(mat3(view));
    TexCoords = aPos;
    vec4 pos = projection * viewNoTranslation * vec4(aPos, 1.0);
    // Set depth so that the skybox renders behind all geometry.
    gl_Position = pos.xyww ; 
 }
