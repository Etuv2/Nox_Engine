#ifndef NOX_LPV_COMMON_GLSL
#define NOX_LPV_COMMON_GLSL

// Shared conventions of the light propagation volume (Kaplanyan & Dachsbacher 2010).
//
// Every cell stores, per color channel, the radiant intensity I(w) (W/sr) of the light passing
// through it as 2-band real spherical harmonics, packed as vec4(L00, L1x, L1y, L1z) with the
// basis Y00 = 0.282095 and Y1 = 0.488603 * (x, y, z); w is the direction the light travels.
// Coefficients of two functions dotted together give the integral of their product.

const float LPV_SH_C0 = 0.282094792;             // Y00
const float LPV_SH_C1 = 0.488602512;             // Y1 / direction component
const float LPV_COSINE_LOBE_C0 = 0.886226925;    // SH projection of max(0, n.w): sqrt(pi) / 2
const float LPV_COSINE_LOBE_C1 = 1.023326708;    //                               sqrt(pi / 3)
const float LPV_INV_PI = 0.318309886;

// Solid angles subtended by a destination cell's faces as seen from the source cell center.
// One front face and four side faces per neighbour; 6 * (front + 4 * side) = 4 pi.
const float LPV_FRONT_FACE_SOLID_ANGLE = 0.400669694;
const float LPV_SIDE_FACE_SOLID_ANGLE = 0.423431334;

vec4 LPVSHBasis(vec3 dir) {
    return vec4(LPV_SH_C0, LPV_SH_C1 * dir);
}

// Clamped cosine lobe max(0, n.w) around n.
vec4 LPVCosineLobe(vec3 n) {
    return vec4(LPV_COSINE_LOBE_C0, LPV_COSINE_LOBE_C1 * n);
}

vec3 LPVRotate(vec3 v, vec4 q) {
    vec3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

vec3 LPVRotateInverse(vec3 v, vec4 q) {
    return LPVRotate(v, vec4(-q.xyz, q.w));
}

// Continuous grid coordinates: cell i spans [i, i + 1), its center is at i + 0.5.
vec3 LPVWorldToGrid(vec3 worldPos, vec3 gridCenter, vec4 gridOrientation, float voxelSize, int gridResolution) {
    vec3 localPos = LPVRotateInverse(worldPos - gridCenter, gridOrientation);
    return localPos / max(voxelSize, 1e-5) + vec3(float(gridResolution) * 0.5);
}

#endif
