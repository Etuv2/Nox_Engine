#version 460 core
// Surface decals (DecalPass): instanced quads blended into the G-buffer.

layout(location = 0) in vec2 aCorner;        // [-1, 1]^2
layout(location = 1) in vec4 iCenterLayer;   // xyz centre, w texture layer
layout(location = 2) in vec4 iAxisU;         // xyz half extent along image right
layout(location = 3) in vec4 iAxisV;         // xyz half extent along image up
layout(location = 4) in vec4 iUvRect;        // (u0, v0, u1, v1), top-left image coordinates
layout(location = 5) in vec4 iTint;          // rgb albedo multiplier, a coverage multiplier

uniform mat4 uViewProj;

out vec3 vWorldPos;
out vec2 vUV;
flat out vec3 vNormal;
flat out float vLayer;
flat out vec4 vTint;

void main() {
	vec3 p = iCenterLayer.xyz + iAxisU.xyz * aCorner.x + iAxisV.xyz * aCorner.y;
	vec2 t = aCorner * 0.5 + 0.5;
	// Layers are stored with the image's top row first: +V (image up) samples v0.
	vUV = vec2(mix(iUvRect.x, iUvRect.z, t.x), mix(iUvRect.w, iUvRect.y, t.y));
	vWorldPos = p;
	vNormal = normalize(cross(iAxisU.xyz, iAxisV.xyz));
	vLayer = iCenterLayer.w;
	vTint = iTint;
	gl_Position = uViewProj * vec4(p, 1.0);
}
