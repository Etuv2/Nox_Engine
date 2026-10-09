#version 460 core
// Builds per-cluster punctual light lists (docs/nfs2015/README.md, M2).
// One thread per cluster; lights are streamed through shared memory in batches. Point lights are
// tested as spheres against the cluster AABB, spot lights additionally with a cone-vs-sphere test
// against the cluster's bounding sphere.

layout(local_size_x = 64) in;

#define NOX_CLUSTER_BUILD
#include "includes/clustered_lighting.glsl"

struct LightData {
	vec4 position;
	vec4 direction;
	vec4 color;
	vec4 attenuation;
	vec4 shadowData;
	vec4 spotData;
	vec4 areaData;
	vec4 sampling;
};
layout(std430, binding = 0) readonly buffer LightDataBuffer { LightData lights[]; };
layout(std430, binding = 40) writeonly buffer ClusterLightCountsOut { uint outCount[]; };
layout(std430, binding = 41) writeonly buffer ClusterLightIndicesOut { uint outIndex[]; };

uniform mat4 uView;
uniform vec2 uTanHalfFov; // tan(fovX/2), tan(fovY/2)

shared vec4 sSphere[64];   // view-space centre + radius
shared vec4 sCone[64];     // view-space direction + cos(outer); w < -1.5 for point lights

float SliceDepth(uint slice) {
	// Inverse of NoxClusterSlice: depth = exp((slice + bias) / scale)
	return exp((float(slice) + clusterZ.w) / clusterZ.z);
}

bool SphereAABB(vec3 c, float r, vec3 mn, vec3 mx) {
	vec3 d = max(max(mn - c, c - mx), vec3(0.0));
	return dot(d, d) <= r * r;
}

bool ConeSphere(vec3 origin, vec3 dir, float range, float cosHalf, vec3 c, float r) {
	vec3 v = c - origin;
	float vLenSq = dot(v, v);
	float v1Len = dot(v, dir);
	float sinHalf = sqrt(max(1.0 - cosHalf * cosHalf, 0.0));
	float distClosest = cosHalf * sqrt(max(vLenSq - v1Len * v1Len, 0.0)) - v1Len * sinHalf;
	bool angleCull = distClosest > r;
	bool frontCull = v1Len > r + range;
	bool backCull = v1Len < -r;
	return !(angleCull || frontCull || backCull);
}

void main() {
	uint clusterCount = clusterGrid.x * clusterGrid.y * clusterGrid.z;
	uint ci = gl_GlobalInvocationID.x;
	bool clusterActive = ci < clusterCount;

	// Cluster bounds in view space (camera looks down -Z; depths are positive distances).
	vec3 mn = vec3(0.0), mx = vec3(0.0);
	if (clusterActive) {
		uint tx = ci % clusterGrid.x;
		uint ty = (ci / clusterGrid.x) % clusterGrid.y;
		uint tz = ci / (clusterGrid.x * clusterGrid.y);
		vec2 px0 = vec2(tx, ty) * float(clusterGrid.w);
		vec2 px1 = min(px0 + float(clusterGrid.w), clusterScreen.xy);
		vec2 ndc0 = px0 / clusterScreen.xy * 2.0 - 1.0;
		vec2 ndc1 = px1 / clusterScreen.xy * 2.0 - 1.0;
		float z0 = (tz == 0u) ? clusterZ.x : SliceDepth(tz);
		float z1 = (tz + 1u >= clusterGrid.z) ? clusterZ.y : SliceDepth(tz + 1u);
		mn = vec3(1e30);
		mx = vec3(-1e30);
		for (int k = 0; k < 8; ++k) {
			vec2 ndc = vec2((k & 1) != 0 ? ndc1.x : ndc0.x, (k & 2) != 0 ? ndc1.y : ndc0.y);
			float z = (k & 4) != 0 ? z1 : z0;
			vec3 p = vec3(ndc * uTanHalfFov * z, -z);
			mn = min(mn, p);
			mx = max(mx, p);
		}
	}
	vec3 sc = (mn + mx) * 0.5;
	float sr = length(mx - mn) * 0.5;

	uint count = 0u;
	uint total = clusterLights.x;
	uint first = clusterLights.y; // directional lights are not clustered
	for (uint base = first; base < total; base += 64u) {
		uint li = base + gl_LocalInvocationID.x;
		if (li < total) {
			LightData L = lights[li];
			vec3 pv = (uView * vec4(L.position.xyz, 1.0)).xyz;
			float range = L.attenuation.w;
			sSphere[gl_LocalInvocationID.x] = vec4(pv, range);
			if (int(L.position.w + 0.5) == 2) {
				vec3 dv = normalize(mat3(uView) * L.direction.xyz);
				float outerCos = (L.spotData.y > 0.0) ? L.spotData.y : cos(radians(30.0));
				sCone[gl_LocalInvocationID.x] = vec4(dv, outerCos);
			} else {
				sCone[gl_LocalInvocationID.x] = vec4(0.0, 0.0, 0.0, -2.0);
			}
		}
		barrier();
		uint batch = min(64u, total - base);
		if (clusterActive) {
			for (uint j = 0u; j < batch; ++j) {
				vec4 s = sSphere[j];
				if (!SphereAABB(s.xyz, s.w, mn, mx)) continue;
				vec4 cone = sCone[j];
				if (cone.w > -1.5 && !ConeSphere(s.xyz, cone.xyz, s.w, cone.w, sc, sr)) continue;
				if (count < NOX_CLUSTER_MAX_LIGHTS) {
					outIndex[ci * NOX_CLUSTER_MAX_LIGHTS + count] = base + j;
					++count;
				}
			}
		}
		barrier();
	}
	if (clusterActive) {
		outCount[ci] = count;
	}
}
