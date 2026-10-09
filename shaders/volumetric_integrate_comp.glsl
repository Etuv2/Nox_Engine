#version 460 core
// Volumetric fog step 2: front-to-back integration along each froxel column.
// Energy-conserving form (Hillaire 2015): Sint = (S - S * exp(-sigma_t * dz)) / sigma_t.
// Output RGBA16F per froxel: rgb = in-scattered radiance from the camera to the froxel's far
// boundary (pre-exposed), a = transmittance over the same span.

layout(local_size_x = 8, local_size_y = 8) in;

#include "includes/volumetric_common.glsl"

layout(rgba16f, binding = 0) uniform writeonly image3D uIntegrated;
uniform sampler3D uScattering;

void main() {
	ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(xy, uVolumeSize.xy))) return;
	vec3 accum = vec3(0.0);
	float transmittance = 1.0;
	float prevDepth = 0.0;
	for (int z = 0; z < uVolumeSize.z; ++z) {
		float depth = VolumeSliceToDepth(float(z + 1) / float(uVolumeSize.z));
		float dz = depth - prevDepth;
		prevDepth = depth;
		vec4 se = texelFetch(uScattering, ivec3(xy, z), 0);
		float sigmaT = max(se.a, 1e-6);
		float sliceTransmittance = exp(-sigmaT * dz);
		vec3 sliceScatter = (se.rgb - se.rgb * sliceTransmittance) / sigmaT;
		accum += transmittance * sliceScatter;
		transmittance *= sliceTransmittance;
		imageStore(uIntegrated, ivec3(xy, z), vec4(accum, transmittance));
	}
}
