#version 460 core
// Auto exposure, step 1: centre-weighted histogram of log2 scene luminance (docs/nfs2015 M3).
// The input image is pre-exposed, so luminance is divided by preExposure to recover cd/m^2.

layout(local_size_x = 16, local_size_y = 16) in;

layout(std430, binding = 42) readonly buffer ExposureStateIn {
	float exposurePre;
	float exposureCurrent;
	float exposureEV100;
	float exposureAverageLuminance;
	float exposurePrevPre;
};
layout(std430, binding = 43) buffer ExposureHistogram { uint bins[64]; };

uniform sampler2D uColor;
uniform ivec2 uSampleGrid;   // number of samples in x/y (sub-sampled image)
uniform float uMinLog2Lum;   // log2 cd/m^2 at bin 1
uniform float uLog2LumRange; // log2 range covered by bins 1..63

shared uint sBins[64];

void main() {
	uint local = gl_LocalInvocationIndex;
	if (local < 64u) sBins[local] = 0u;
	barrier();

	ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
	if (all(lessThan(gid, uSampleGrid))) {
		vec2 uv = (vec2(gid) + 0.5) / vec2(uSampleGrid);
		vec3 c = textureLod(uColor, uv, 0.0).rgb / max(exposurePre, 1e-12);
		float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
		uint bin = 0u;
		if (lum > 1e-6) {
			float t = clamp((log2(lum) - uMinLog2Lum) / uLog2LumRange, 0.0, 1.0);
			bin = uint(t * 62.0 + 1.0);
		}
		// Centre weighting: the middle of the frame counts up to 4x the corners.
		vec2 d = uv * 2.0 - 1.0;
		uint weight = uint(mix(4.0, 1.0, clamp(dot(d, d) * 0.5, 0.0, 1.0)) + 0.5);
		atomicAdd(sBins[bin], weight);
	}
	barrier();
	if (local < 64u && sBins[local] != 0u) atomicAdd(bins[local], sBins[local]);
}
