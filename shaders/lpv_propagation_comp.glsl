#version 460 core

layout(local_size_x = 8, local_size_y = 8, local_size_z = 8) in;

// Inputs: float LPV textures (RGBA16F), one voxel stores 4 SH coeffs in RGBA
layout(binding = 0, rgba16f) readonly uniform image3D u_lpvR;
layout(binding = 1, rgba16f) readonly uniform image3D u_lpvG;
layout(binding = 2, rgba16f) readonly uniform image3D u_lpvB;

// Outputs: float LPV textures (RGBA16F)
layout(binding = 3, rgba16f) writeonly uniform image3D u_lpvOutR;
layout(binding = 4, rgba16f) writeonly uniform image3D u_lpvOutG;
layout(binding = 5, rgba16f) writeonly uniform image3D u_lpvOutB;

// Geometry volume (occlusion)
uniform sampler3D u_geometryVolume;

// Propagation parameters
uniform int   u_gridResolution;
uniform float u_attenuation = 0.9;
uniform float u_bias        = 0.1;
uniform bool  u_enableOcclusion = true;

// Direction vectors to 6-neighborhood
const vec3 directions[6] = vec3[](
    vec3( 1,  0,  0),
    vec3(-1,  0,  0),
    vec3( 0,  1,  0),
    vec3( 0, -1,  0),
    vec3( 0,  0,  1),
    vec3( 0,  0, -1)
);

// Evaluate and re-project using a simple first-order SH basis
vec4 projectDirToSH(vec3 dir) {
    const float c0 = 0.282095;      // Y00
    const float c1 = 0.488603;      // sqrt(3/(4*pi))
    return vec4(c0, c1*dir.x, c1*dir.y, c1*dir.z);
}

vec4 propagate(vec4 sh, vec3 dir) {
    vec4 basis = projectDirToSH(dir);
    float irradiance = max(dot(sh, basis), 0.0);
    return basis * irradiance;
}

void main() {
    ivec3 p = ivec3(gl_GlobalInvocationID.xyz);
    if (any(greaterThanEqual(p, ivec3(u_gridResolution)))) return;

    // Read current voxel
    vec4 curR = imageLoad(u_lpvR, p);
    vec4 curG = imageLoad(u_lpvG, p);
    vec4 curB = imageLoad(u_lpvB, p);

    vec4 accR = vec4(0.0);
    vec4 accG = vec4(0.0);
    vec4 accB = vec4(0.0);

    for (int i = 0; i < 6; ++i) {
        ivec3 np = p + ivec3(directions[i]);
        if (any(lessThan(np, ivec3(0))) || any(greaterThanEqual(np, ivec3(u_gridResolution)))) continue;

        vec4 nR = imageLoad(u_lpvR, np);
        vec4 nG = imageLoad(u_lpvG, np);
        vec4 nB = imageLoad(u_lpvB, np);

        float occ = 1.0;
        if (u_enableOcclusion) {
            vec3 uvw = (vec3(np) + 0.5) / float(u_gridResolution);
            occ = 1.0 - texture(u_geometryVolume, uvw).r;
        }

        vec3 dirToHere = -directions[i];
        accR += propagate(nR, dirToHere) * u_attenuation * occ;
        accG += propagate(nG, dirToHere) * u_attenuation * occ;
        accB += propagate(nB, dirToHere) * u_attenuation * occ;
    }

    vec4 outR = curR * (1.0 - u_attenuation * 0.5) + accR / 6.0;
    vec4 outG = curG * (1.0 - u_attenuation * 0.5) + accG / 6.0;
    vec4 outB = curB * (1.0 - u_attenuation * 0.5) + accB / 6.0;

    imageStore(u_lpvOutR, p, outR);
    imageStore(u_lpvOutG, p, outG);
    imageStore(u_lpvOutB, p, outB);
}
