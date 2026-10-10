#version 460 core
// Lens flare sprites (LensFlarePass). The sprite array holds linear premultiplied colour (alpha already
// applied once on load), so the additive blend is GL_ONE, GL_ONE. Output is scene-referred glare
// radiance scaled by this frame's pre-exposure, like every other emitter in the HDR image.

#include "includes/exposure_common.glsl"

in vec2 vUV;
flat in float vLayer;
flat in vec3 vColor;

layout(location = 0) out vec4 FragColor;

uniform sampler2DArray uFlares;

void main() {
	vec3 c = texture(uFlares, vec3(vUV, vLayer)).rgb;
	FragColor = vec4(c * vColor * NoxPreExposure(), 0.0);
}
