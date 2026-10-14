#version 460 core
// Generic particle shading (ParticlePass). Premultiplied output into the pre-exposed HDR target; the
// pass selects additive or alpha blending. Without a texture a particle is a soft disc.

#include "includes/exposure_common.glsl"

in vec2 vCorner;
in vec4 vColor;
layout(location = 0) out vec4 FragColor;

uniform sampler2DArray uParticleTex;  // one layer; premultiplied linear colour (TextureArrayBuilder)
uniform int uHasTexture;

void main() {
	vec4 texel;
	if (uHasTexture != 0) {
		vec2 uv = vec2(vCorner.x * 0.5 + 0.5, 0.5 - vCorner.y * 0.5);
		texel = texture(uParticleTex, vec3(uv, 0.0));
		texel.rgb *= vColor.rgb * vColor.a;
		texel.a *= vColor.a;
	} else {
		float disc = 1.0 - smoothstep(0.5, 1.0, length(vCorner));
		float a = disc * vColor.a;
		texel = vec4(vColor.rgb * a, a);
	}
	if (texel.a <= 0.0) discard;
	FragColor = vec4(texel.rgb * NoxPreExposure(), texel.a);
}
