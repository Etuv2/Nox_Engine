#ifndef NOX_VOLUMETRIC_COMMON_GLSL
#define NOX_VOLUMETRIC_COMMON_GLSL

// Froxel volumetric fog (VolumetricFogPass, docs/nfs2015/README.md M6), after Hillaire 2015,
// "Physically Based and Unified Volumetric Rendering in Frostbite": a camera-aligned froxel grid
// (screen tiles x exponential depth slices) holding in-scattered light and extinction, integrated
// front to back with the energy-conserving form and composited over the lit image.

uniform ivec3 uVolumeSize;   // froxels in x, y, z
uniform float uVolumeNear;   // metres
uniform float uVolumeFar;    // metres

// Exponential slice distribution: depth(w) = near * (far / near)^w, w in [0,1].
float VolumeSliceToDepth(float w) {
	return uVolumeNear * pow(uVolumeFar / uVolumeNear, clamp(w, 0.0, 1.0));
}

float VolumeDepthToSlice(float depth) {
	return log(max(depth, uVolumeNear) / uVolumeNear) / log(uVolumeFar / uVolumeNear);
}

#endif // NOX_VOLUMETRIC_COMMON_GLSL
