#version 450 core
in vec2 TexCoord;
out vec4 FragColor;

uniform sampler2D tex;
uniform vec4 tintColor;
void main() {
    FragColor = texture(tex, TexCoord) * tintColor;
}