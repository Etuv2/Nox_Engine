#version 460 core
// Rain streaks (RainPass, docs/nfs2015/README.md M6). Instanced camera-facing quads stretched along
// the drop's velocity over the shutter time. Drops live in a box that wraps around the camera, so
// the particle count is constant and nothing is simulated on the CPU.

layout(location = 0) in vec2 aCorner; // x in [-1,1] across, y in [0,1] along the streak

uniform mat4 uViewProj;
uniform vec3 uCameraPos;
uniform vec3 uBoxSize;        // metres, centred on the camera
uniform float uTime;
uniform vec3 uFallVelocity;   // m/s (gravity + wind)
uniform float uStreakTime;    // seconds of motion visible in one streak
uniform float uDropWidth;     // metres
uniform vec3 uCameraVelocity; // m/s, so streaks lengthen with the car's speed

out vec2 vCorner;
out vec3 vWorldPos;
out float vFade;

float Hash11(float n) { return fract(sin(n * 12.9898) * 43758.5453); }

void main() {
	float id = float(gl_InstanceID);
	vec3 seed = vec3(Hash11(id * 1.37), Hash11(id * 2.11 + 3.1), Hash11(id * 0.71 + 7.7));
	float speedVariation = mix(0.85, 1.15, Hash11(id * 5.3));
	vec3 velocity = uFallVelocity * speedVariation;
	// Position in the wrapping box: seed offset plus fall, wrapped relative to the camera.
	vec3 p = seed * uBoxSize + velocity * uTime;
	vec3 local = mod(p - uCameraPos + uBoxSize * 0.5, uBoxSize) - uBoxSize * 0.5;
	vec3 head = uCameraPos + local;

	// Streak spans the motion relative to the camera during the shutter time.
	vec3 relative = velocity - uCameraVelocity;
	vec3 tail = head - relative * uStreakTime;
	vec3 along = head - tail;
	vec3 toCam = normalize(uCameraPos - head);
	vec3 side = cross(along, toCam);
	float sideLen = length(side);
	side = sideLen > 1e-5 ? side / sideLen : vec3(1.0, 0.0, 0.0);
	vec3 world = mix(tail, head, aCorner.y) + side * aCorner.x * uDropWidth;

	vCorner = aCorner;
	vWorldPos = world;
	// Fade near the box border (no popping) and very close to the lens.
	vec3 edge = 1.0 - abs(local) / (uBoxSize * 0.5);
	vFade = clamp(min(min(edge.x, edge.y), edge.z) * 4.0, 0.0, 1.0) * smoothstep(0.3, 1.2, length(local));
	gl_Position = uViewProj * vec4(world, 1.0);
}
