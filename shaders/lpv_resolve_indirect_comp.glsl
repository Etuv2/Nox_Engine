#version 460 core

#include "includes/pbr_common.glsl"
#include "includes/screen_space_reconstruction.glsl"

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

vec3 RotateVector(vec3 v, vec4 q) {
	vec3 qxyz = q.xyz;
	float qw = q.w;
	vec3 t = 2.0 * cross(qxyz, v);
	return v + qw * t + cross(qxyz, t);
}

vec3 RotateVectorInverse(vec3 v, vec4 q) {
	return RotateVector(v, vec4(-q.xyz, q.w));
}

vec3 WorldToVoxelUVW(vec3 worldPos) {
	vec3 localPos = RotateVectorInverse(worldPos - u_gridCenter, u_gridOrientation);
	vec3 voxelPos = (localPos / max(u_voxelSize, 1e-5)) + vec3(float(u_gridResolution) * 0.5);
	return voxelPos / max(float(u_gridResolution), 1.0);
}

vec3 EvaluateSH(vec4 shR, vec4 shG, vec4 shB, vec3 normal) {
	vec4 shBasis = vec4(
		0.282095,
		0.488603 * normal.x,
		0.488603 * normal.y,
		0.488603 * normal.z
	);

	return max(vec3(dot(shR, shBasis), dot(shG, shBasis), dot(shB, shBasis)), vec3(0.0));
}

void main() {
	ivec2 id = ivec2(gl_GlobalInvocationID.xy);
	ivec2 outputSize = imageSize(u_outIndirect);
	if (any(greaterThanEqual(id, outputSize))) {
		return;
	}

	vec2 uv = (vec2(id) + 0.5) / vec2(outputSize);
	float depth = textureLod(u_depth, uv, 0.0).r;
	if (depth >= 0.999999 || u_gridResolution <= 0 || u_giStrength <= 0.0001) {
		imageStore(u_outIndirect, id, vec4(0.0));
		return;
	}

	vec3 viewPos = ReconstructViewPosition(uv, depth, u_invProjection);
	vec3 worldPos = (u_invView * vec4(viewPos, 1.0)).xyz;
	vec3 normal = DecodeNormalOct(textureLod(u_packedNormalRM, uv, 0.0).rg);
	if (u_normalsInWorldSpace == 0) {
		normal = normalize(mat3(u_invView) * normal);
	}
	if (length(normal) < 0.5 || any(isnan(normal))) {
		normal = vec3(0.0, 1.0, 0.0);
	}

	vec3 uvw = WorldToVoxelUVW(worldPos);
	if (any(lessThan(uvw, vec3(0.0))) || any(greaterThan(uvw, vec3(1.0)))) {
		imageStore(u_outIndirect, id, vec4(0.0));
		return;
	}

	vec4 shR = textureLod(u_lpvTextureR, uvw, 0.0);
	vec4 shG = textureLod(u_lpvTextureG, uvw, 0.0);
	vec4 shB = textureLod(u_lpvTextureB, uvw, 0.0);
	vec3 localNormal = RotateVectorInverse(normalize(normal), u_gridOrientation);
	vec3 irradiance = EvaluateSH(shR, shG, shB, normalize(localNormal)) * u_giStrength;

	imageStore(u_outIndirect, id, vec4(max(irradiance, vec3(0.0)), 1.0));
}
