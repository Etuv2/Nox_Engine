#version 460 core
// Lens flare sprites (LensFlarePass). Up to NOX_FLARE_MAX_LAYERS camera-facing layers per source, built
// in screen space on the projected source (UV 0.5 pivot shared by every layer, screen-aligned so a
// horizontal streak stays horizontal). Which textures, gains, widths and tints a source uses comes from
// its style (SceneRenderExtras LensFlareStyle); unused layers collapse to an off-screen point.
//
// Glare strength: 0 for sources at or below the maximum base size, rising to 1 as the projected source
// fills it (close sources). Layers can require or scale with it (glareMin / glareWeight).

#include "includes/lens_flare_common.glsl"

layout(std430, binding = 46) readonly buffer NoxFlareVisibility {
	float flareVisibility[];
};

uniform mat4 uViewProj;     // unjittered: drawn after TAA, so the sprite must not jitter
uniform mat4 uProj;
uniform vec3 uCameraPos;
uniform vec2 uScreenSize;
uniform float uMinPixels;   // base glow size clamp, already scaled to this resolution
uniform float uMaxPixels;
uniform float uGain;        // glare cd/m^2 per source cd/m^2 (set gain folded in)
uniform float uFadeStart;   // m
uniform float uFadeEnd;     // m
uniform int uStyleCount;

out vec2 vUV;
flat out float vLayer;
flat out vec3 vColor;

const vec2 kCorners[4] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));

void Collapse() {
	gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
	vUV = vec2(0.5);
	vLayer = 0.0;
	vColor = vec3(0.0);
}

void main() {
	int src = gl_InstanceID / NOX_FLARE_MAX_LAYERS;
	int slot = gl_InstanceID % NOX_FLARE_MAX_LAYERS;
	NoxFlareSource s = flareSources[src];
	int style = int(s.posStyle.w + 0.5);
	float vis = flareVisibility[src];
	vec4 clip = uViewProj * vec4(s.posStyle.xyz, 1.0);
	if (style < 0 || style >= uStyleCount || clip.w <= 0.05 || vis < 0.002) {
		Collapse();
		return;
	}
	NoxFlareLayer L = flareLayers[style * NOX_FLARE_MAX_LAYERS + slot];
	if (L.flags.y < 0.5) {
		Collapse();
		return;
	}

	// Emission direction: full glare inside the beam, weaker from the side, none from behind.
	vec3 toCam = uCameraPos - s.posStyle.xyz;
	float dist = length(toCam);
	toCam /= max(dist, 1e-3);
	float facing = 1.0;
	if (dot(s.facingLuminance.xyz, s.facingLuminance.xyz) > 0.5) {
		float c = dot(s.facingLuminance.xyz, toCam);
		facing = mix(0.3, 1.0, smoothstep(-0.05, 0.45, c)) * smoothstep(-0.15, -0.02, c);
	}
	float distFade = 1.0 - smoothstep(uFadeStart, uFadeEnd, dist);

	// Base size from the projected source size, clamped. Below the floor the sprite keeps its size but
	// loses energy, so distant sources become faint points rather than large blobs.
	float focalPx = uProj[1][1] * uScreenSize.y * 0.5;
	float projPx = s.tintSize.w * focalPx / clip.w;
	float glowPx = clamp(projPx, uMinPixels, uMaxPixels);
	float energy = clamp(projPx / max(uMinPixels, 1e-3), 0.15, 1.0);
	float glare = smoothstep(0.6, 1.0, projPx / max(uMaxPixels, 1e-3));

	float layerGain = L.layerGainWidthGlareMin.y;
	float glareMin = L.layerGainWidthGlareMin.w;
	if (glareMin > 0.0) layerGain *= smoothstep(glareMin, glareMin + 0.4, glare);
	layerGain *= mix(1.0, glare, clamp(L.tintGlareWeight.w, 0.0, 1.0));
	float strength = vis * facing * distFade * energy * s.facingLuminance.w * uGain * layerGain;
	if (strength < 1e-6) {
		Collapse();
		return;
	}
	vec3 tint = L.tintGlareWeight.rgb * (L.flags.x > 0.5 ? s.tintSize.rgb : vec3(1.0));

	vec2 corner = kCorners[gl_VertexID];
	vec2 ndc = clip.xy / clip.w;
	float sizePx = glowPx * L.layerGainWidthGlareMin.z;
	gl_Position = vec4(ndc + corner * sizePx / uScreenSize, 0.0, 1.0);
	vUV = vec2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5);   // image top at screen up
	vLayer = L.layerGainWidthGlareMin.x;
	vColor = tint * strength;
}
