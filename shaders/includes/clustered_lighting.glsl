#ifndef NOX_CLUSTERED_LIGHTING_GLSL
#define NOX_CLUSTERED_LIGHTING_GLSL

// Clustered light lists built by light_cluster_build_comp.glsl (LightClusterPass).
// View space is split into screen tiles x exponential depth slices; each cluster stores up to
// NOX_CLUSTER_MAX_LIGHTS punctual (point/spot) light indices. Directional lights are always the
// first `clusterNumDirectional` entries of the light buffer (LightManager sorts by type) and are
// evaluated for every pixel. See docs/nfs2015/README.md (M2).

#define NOX_CLUSTER_MAX_LIGHTS 128u

layout(std140, binding = 8) uniform ClusterParams {
	uvec4 clusterGrid;    // x, y, z cluster counts; w = tile size in pixels
	vec4  clusterZ;       // x = near, y = far, z = slice scale, w = slice bias
	uvec4 clusterLights;  // x = total lights, y = directional count, z = enabled, w = unused
	vec4  clusterScreen;  // x, y = render size in pixels
};

#ifndef NOX_CLUSTER_BUILD
layout(std430, binding = 40) readonly buffer ClusterLightCounts { uint clusterLightCount[]; };
layout(std430, binding = 41) readonly buffer ClusterLightIndices { uint clusterLightIndex[]; };
#endif

uint NoxClusterSlice(float viewDepth) {
	float slice = log(max(viewDepth, clusterZ.x)) * clusterZ.z - clusterZ.w;
	return uint(clamp(slice, 0.0, float(clusterGrid.z - 1u)));
}

uint NoxClusterIndex(vec2 fragCoord, float viewDepth) {
	uvec2 tile = min(uvec2(fragCoord) / max(clusterGrid.w, 1u), clusterGrid.xy - 1u);
	uint slice = NoxClusterSlice(viewDepth);
	return (slice * clusterGrid.y + tile.y) * clusterGrid.x + tile.x;
}

bool NoxClusteringEnabled() {
	return clusterLights.z != 0u;
}

#endif // NOX_CLUSTERED_LIGHTING_GLSL
