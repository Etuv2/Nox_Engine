#version 460 core
// Generic particle billboards (ParticlePass, src/render/ParticleSystem.h). One instance per particle.

layout(location = 0) in vec2 aCorner;   // triangle strip corner, [-1,1]^2
layout(location = 1) in vec4 aPosSize;  // xyz world position (m), w billboard radius (m)
layout(location = 2) in vec4 aColor;    // rgb colour, a opacity (straight alpha, linear)
layout(location = 3) in vec4 aMisc;     // x roll (rad), y random seed [0,1), z normalised age, w unused

uniform mat4 uView;
uniform mat4 uViewProj;

out vec2 vCorner;   // unrotated corner: the disc mask and the texture lookup use it
out vec4 vColor;

void main() {
	float c = cos(aMisc.x);
	float s = sin(aMisc.x);
	vec2 rolled = vec2(c * aCorner.x - s * aCorner.y, s * aCorner.x + c * aCorner.y);
	vec3 right = vec3(uView[0][0], uView[1][0], uView[2][0]);
	vec3 up = vec3(uView[0][1], uView[1][1], uView[2][1]);
	vec3 world = aPosSize.xyz + (right * rolled.x + up * rolled.y) * aPosSize.w;
	vCorner = aCorner;
	vColor = aColor;
	gl_Position = uViewProj * vec4(world, 1.0);
}
