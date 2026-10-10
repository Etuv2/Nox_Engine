#version 460 core
// VehicleFX billboards (VehicleFXPass, src/VehicleFX.h). One instance per particle.
// Smoke and mist are camera-facing discs rolled by the particle's angle. Droplets are streaks along
// their screen-projected motion over the shutter time.

layout(location = 0) in vec2 aCorner;     // triangle strip corner, [-1,1]^2
layout(location = 1) in vec4 aPosRadius;  // xyz world position (m), w radius (m)
layout(location = 2) in vec4 aShading;    // x opacity, y roll (rad), z seed, w kind (0 smoke, 1 droplet, 2 mist)
layout(location = 3) in vec4 aVelocity;   // xyz velocity (m/s), w normalised age

uniform mat4 uView;
uniform mat4 uViewProj;
uniform vec3 uCameraPos;
uniform float uStreakTime; // seconds of droplet motion drawn as a streak

out vec2 vCorner;
out vec3 vWorldPos;
out float vViewDepth;
out float vOpacity;
out float vSeed;
out float vAge;
flat out int vKind;

void main() {
	vec3 pos = aPosRadius.xyz;
	float radius = aPosRadius.w;
	int kind = int(aShading.w + 0.5);

	vec3 right = vec3(uView[0][0], uView[1][0], uView[2][0]);
	vec3 up = vec3(uView[0][1], uView[1][1], uView[2][1]);
	vec3 toCam = normalize(uCameraPos - pos);

	vec2 corner = aCorner;
	vec3 offset;
	if (kind == 1) {
		vec3 motion = aVelocity.xyz * uStreakTime;
		motion -= toCam * dot(motion, toCam);
		float len = length(motion);
		vec3 dir = len > 1e-6 ? motion / len : up;
		vec3 side = normalize(cross(toCam, dir));
		float halfLen = radius * 1.5 + min(len, radius * 20.0) * 0.5;
		offset = dir * (corner.y * halfLen) + side * (corner.x * radius);
	} else {
		float c = cos(aShading.y);
		float s = sin(aShading.y);
		corner = vec2(c * aCorner.x - s * aCorner.y, s * aCorner.x + c * aCorner.y);
		offset = (right * corner.x + up * corner.y) * radius;
	}

	vec3 world = pos + offset;
	vec4 viewPos = uView * vec4(world, 1.0);
	vCorner = corner;
	vWorldPos = world;
	vViewDepth = -viewPos.z;
	vOpacity = aShading.x;
	vSeed = aShading.z;
	vAge = aVelocity.w;
	vKind = kind;
	gl_Position = uViewProj * vec4(world, 1.0);
}
