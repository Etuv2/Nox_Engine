#version 460 core

#include "includes/pbr_common.glsl"
#include "includes/screen_space_reconstruction.glsl"
#include "includes/lpv_common.glsl"

// Resolves the accumulated LPV into per-pixel irradiance for the lighting pass.
// A cell's intensity I(w) turns into incident radiance I(w) / s^2 (s = cell size), so a surface
// with normal n receives E = integral I(w) / s^2 * max(0, -n.w) dw: the dot product of the
// stored SH with the cosine lobe around -n (light arriving at the surface travels against n).
layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0) uniform sampler3D u_lpvTextureR;
layout(binding = 1) uniform sampler3D u_lpvTextureG;
layout(binding = 2) uniform sampler3D u_lpvTextureB;
layout(binding = 3) uniform sampler2D u_packedNormalRM;
layout(binding = 4) uniform sampler2D u_depth;

layout(binding = 0, rgba16f) writeonly uniform image2D u_outIndirect;

uniform mat4 u_invProjection;
uniform mat4 u_invView;
uniform vec3 u_gridCenter;
uniform float u_voxelSize;
uniform int u_gridResolution;
uniform vec4 u_gridOrientation;
uniform float u_giStrength;
uniform int u_normalsInWorldSpace;

void main() {
	ivec2 id = ivec2(gl_GlobalInvocationID.xy);
	ivec2 outputSize = imageSize(u_outIndirect);
	if (any(greaterThanEqual(id, outputSize))) {
		return;
	}

	vec2 uv = (vec2(id) + 0.5) / vec2(outputSize);
	float depth = textureLod(u_depth, uv, 0.0).r;
	if (depth >= 0.999999 || u_gridResolution <= 0 || u_giStrength <= 0.0001) {
		imageStore(u_outIndirect, id, vec4(0.0, 0.0, 0.0, 1.0));
		return;
	}

	vec3 viewPos = ReconstructViewPosition(uv, depth, u_invProjection);
	vec3 worldPos = (u_invView * vec4(viewPos, 1.0)).xyz;
	vec3 normal = DecodeNormalOct(textureLod(u_packedNormalRM, uv, 0.0).rg);
	if (u_normalsInWorldSpace == 0) {
		normal = mat3(u_invView) * normal;
	}
	if (length(normal) < 0.5 || any(isnan(normal))) {
		normal = vec3(0.0, 1.0, 0.0);
	}
	normal = normalize(normal);

	// Look up half a cell in front of the surface, like the VPLs were injected, so a surface does
	// not read the cell behind it (Kaplanyan & Dachsbacher 2010).
	vec3 gridPos = LPVWorldToGrid(worldPos + normal * (0.5 * u_voxelSize),
		u_gridCenter, u_gridOrientation, u_voxelSize, u_gridResolution);
	vec3 uvw = gridPos / float(max(u_gridResolution, 1));
	if (any(lessThan(uvw, vec3(0.0))) || any(greaterThan(uvw, vec3(1.0)))) {
		imageStore(u_outIndirect, id, vec4(0.0, 0.0, 0.0, 1.0));
		return;
	}

	vec4 shR = textureLod(u_lpvTextureR, uvw, 0.0);
	vec4 shG = textureLod(u_lpvTextureG, uvw, 0.0);
	vec4 shB = textureLod(u_lpvTextureB, uvw, 0.0);
	vec4 receiverLobe = LPVCosineLobe(-LPVRotateInverse(normal, u_gridOrientation));
	vec3 irradiance = max(vec3(dot(shR, receiverLobe), dot(shG, receiverLobe), dot(shB, receiverLobe)), vec3(0.0));
	irradiance *= u_giStrength / max(u_voxelSize * u_voxelSize, 1e-8);

	if (any(isnan(irradiance)) || any(isinf(irradiance))) {
		irradiance = vec3(0.0);
	}
	imageStore(u_outIndirect, id, vec4(irradiance, 1.0));
}
