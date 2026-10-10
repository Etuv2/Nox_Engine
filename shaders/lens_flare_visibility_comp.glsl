#version 460 core
// Lens flare source visibility (LensFlarePass). One thread per source: tests a small disc of scene
// depth around the projected source (soft partial occlusion by nearby geometry, including the
// emitter's own housing) and eases the stored visibility toward it, so flares fade in and out
// smoothly instead of popping. Sources behind the camera or off screen fade to zero.

layout(local_size_x = 64) in;

#include "includes/lens_flare_common.glsl"

layout(std430, binding = 46) buffer NoxFlareVisibility {
	float flareVisibility[];
};

uniform uint uCount;
uniform mat4 uViewProj;      // jittered projection: matches this frame's depth buffer
uniform mat4 uProj;
uniform sampler2D uDepth;
uniform vec2 uScreenSize;
uniform float uBlend;        // 1 - exp(-dt * rate)
uniform bool uReset;         // snap (scene load)

const int kTaps = 13;
const vec2 kDisc[kTaps] = vec2[](
	vec2(0.0, 0.0),
	vec2(0.5, 0.0), vec2(-0.5, 0.0), vec2(0.0, 0.5), vec2(0.0, -0.5),
	vec2(0.71, 0.71), vec2(-0.71, 0.71), vec2(0.71, -0.71), vec2(-0.71, -0.71),
	vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0));

float ViewDepth(float d) {
	return uProj[3][2] / ((d * 2.0 - 1.0) + uProj[2][2]);
}

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= uCount) {
		return;
	}
	NoxFlareSource s = flareSources[i];
	vec4 clip = uViewProj * vec4(s.posStyle.xyz, 1.0);
	float target = 0.0;
	if (clip.w > 0.05) {
		vec2 px = (clip.xy / clip.w * 0.5 + 0.5) * uScreenSize;
		float srcDepth = clip.w;
		// Sources are expected slightly in front of their emitting surface; the tolerance also absorbs
		// depth precision at range.
		float tolerance = 0.1 + 0.004 * srcDepth;
		float lensPx = s.tintSize.w * 0.3 * uProj[1][1] * uScreenSize.y * 0.5 / srcDepth;
		float radius = clamp(lensPx, 1.5, 8.0);
		float visible = 0.0;
		for (int k = 0; k < kTaps; ++k) {
			vec2 p = px + kDisc[k] * radius;
			if (any(lessThan(p, vec2(0.0))) || any(greaterThanEqual(p, uScreenSize))) {
				continue;   // off-screen taps count as hidden: flares fade at the frame edge
			}
			float d = texelFetch(uDepth, ivec2(p), 0).r;
			float sceneDepth = d >= 1.0 ? 1e9 : ViewDepth(d);
			visible += sceneDepth > srcDepth - tolerance ? 1.0 : 0.0;
		}
		target = visible / float(kTaps);
	}
	float previous = flareVisibility[i];
	flareVisibility[i] = uReset ? target : mix(previous, target, uBlend);
}
