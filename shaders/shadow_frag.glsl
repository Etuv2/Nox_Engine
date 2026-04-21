// shadow_fragment.glsl
#version 460 core
#include "includes/pbr_common.glsl"
#include "includes/material_common.glsl"

in vec2 TexCoords;
in vec2 TexCoords1;

uniform sampler2D texture_diffuse;
uniform vec4 baseColorFactor = vec4(1.0);
uniform bool hasBaseColorTexture = false;
uniform int baseColorUVSet = 0;
uniform float alphaCutoff = 0.5;
uniform int alphaMode = 0; // 0=OPAQUE, 1=MASK, 2=BLEND

void main() {
    if (alphaMode == 0) {
        return;
    }

    vec4 baseColor = baseColorFactor;
    if (hasBaseColorTexture) {
        baseColor *= texture(texture_diffuse, SelectUVSet(baseColorUVSet, TexCoords, TexCoords1));
    }

    float cutoff = (alphaMode == 1) ? alphaCutoff : 0.001;
    if (baseColor.a < cutoff) {
        discard;
    }
}
