#version 460 core

in vec3 vWorldPos;
out vec4 FragColor;

uniform mat4 u_view;
uniform mat4 u_projection;
uniform vec3 u_gridCenter;
uniform float u_voxelSize;
uniform int u_gridResolution;

// Visualize LPV values
uniform sampler3D u_lpvTextureR;
uniform sampler3D u_lpvTextureG;
uniform sampler3D u_lpvTextureB;
uniform sampler3D u_geometryVolume;

vec3 WorldToUVW(vec3 worldPos){
    vec3 local = (worldPos - u_gridCenter) / u_voxelSize + vec3(u_gridResolution*0.5);
    return local/float(u_gridResolution);
}

void main(){
    vec3 uvw = WorldToUVW(vWorldPos);
    vec4 r = texture(u_lpvTextureR, uvw);
    vec4 g = texture(u_lpvTextureG, uvw);
    vec4 b = texture(u_lpvTextureB, uvw);
    vec3 e = vec3(r.x+g.x+b.x, r.y+g.y+b.y, r.z+g.z+b.z);
    float geom = texture(u_geometryVolume, uvw).r;
    vec3 col = e * (1.0-geom);
    FragColor = vec4(col, 0.35);
}
