// shadow_fragment.glsl
#version 460 core
#include "includes/pbr_common.glsl"
#include "includes/material_common.glsl"

in vec2 TexCoords;
in vec2 TexCoords1;
in vec4 VertColor;

uniform sampler2D texture_diffuse;
uniform vec4 baseColorFactor = vec4(1.0);
uniform bool hasBaseColorTexture = false;
uniform int baseColorUVSet = 0;
// KHR_texture_transform per slot (MaterialDesc::TextureSlot order)
uniform mat3 uvXform[10];
uniform float alphaCutoff = 0.5;
uniform int alphaMode = 0; // 0=OPAQUE, 1=MASK, 2=BLEND

void main() {
    if (alphaMode == 0) {
        return;
    }

    vec4 baseColor = baseColorFactor * VertColor;
    if (hasBaseColorTexture) {
        baseColor *= texture(texture_diffuse, SelectUVSetXform(baseColorUVSet, TexCoords, TexCoords1, uvXform[0]));
    }

    float cutoff = (alphaMode == 1) ? alphaCutoff : 0.001;
    if (baseColor.a < cutoff) {
        discard;
    }
}
