#ifndef NOX_WEATHER_COMMON_GLSL
#define NOX_WEATHER_COMMON_GLSL

// Wet surfaces (docs/nfs2015/README.md, M4). Applied in the G-buffer and forward passes before the
// material is packed, so every consumer (lighting, SSR, RT) sees the same wet material.
//
// Model (after Lagarde, "Water drop 2" 2013, adapted):
//   * Wet substrate: porous materials darken (water fills pores, more light is absorbed) and
//     get smoother, but keep part of their roughness variation so wet asphalt reads as broken,
//     stretched highlights rather than a mirror.
//   * Standing water (puddles): only on upward-facing surfaces of materials that opt in
//     (puddleResponse), placed by world-space noise plus a cavity term (crevices fill first).
//     Water is a smooth dielectric (IOR 1.33) over a flattened normal, with rain ripples.
//   * Rain beads: droplets on smooth coated materials (car paint, glass), in object space so
//     they stick to moving objects.

uniform float uWeatherWetness = 0.0;     // global wetness [0,1]
uniform float uWeatherPuddles = 0.0;     // standing-water coverage [0,1]
uniform float uWeatherRain = 0.0;        // rain intensity [0,1] (ripples, bead density)
uniform float uWeatherPuddleScale = 4.0; // metres per puddle noise cell
uniform float uWeatherTime = 0.0;        // seconds

float NoxHash12(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

vec2 NoxHash22(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.xx + p3.yz) * p3.zy);
}

vec3 NoxHash33(vec3 p) {
	p = fract(p * vec3(0.1031, 0.1030, 0.0973));
	p += dot(p, p.yxz + 33.33);
	return fract((p.xxy + p.yxx) * p.zyx);
}

float NoxValueNoise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	float a = NoxHash12(i);
	float b = NoxHash12(i + vec2(1.0, 0.0));
	float c = NoxHash12(i + vec2(0.0, 1.0));
	float d = NoxHash12(i + vec2(1.0, 1.0));
	return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float NoxFbm(vec2 p) {
	float v = 0.0;
	float a = 0.5;
	for (int i = 0; i < 4; ++i) {
		v += a * NoxValueNoise(p);
		p = p * 2.03 + vec2(17.1, 9.7);
		a *= 0.5;
	}
	return v;
}

// Expanding ring ripples from raindrops on standing water. Returns a tangent-space XY slope.
vec2 NoxRippleSlope(vec2 xz, float time, float cellSize) {
	vec2 cell = floor(xz / cellSize);
	vec2 slope = vec2(0.0);
	for (int j = -1; j <= 1; ++j) {
		for (int i = -1; i <= 1; ++i) {
			vec2 c = cell + vec2(i, j);
			vec2 rnd = NoxHash22(c);
			vec2 centre = (c + rnd) * cellSize;
			float period = 0.9 + rnd.x * 0.6;
			float t = fract(time / period + rnd.y);       // ring age [0,1)
			vec2 d = xz - centre;
			float r = length(d);
			float radius = t * cellSize * 1.2;
			float x = (r - radius) / (cellSize * 0.09);
			float ring = exp(-x * x) * sin(x * 3.14159) * (1.0 - t) * (1.0 - t);
			slope += (r > 1e-4 ? d / r : vec2(0.0)) * ring;
		}
	}
	return slope;
}

struct NoxWetResult {
	float puddle;       // standing water amount [0,1]
	float wetness;      // substrate wetness [0,1]
};

// Modifies albedo / roughness / normal / IOR in place.
//   worldPos, geoNormal : world-space position and unperturbed vertex normal
//   cavity              : 0 = exposed, 1 = deep crevice (from AO / albedo)
NoxWetResult NoxApplyWetness(vec3 worldPos, vec3 geoNormal, float cavity,
	float wetResponse, float puddleResponse, float metallic,
	inout vec3 albedo, inout float roughness, inout vec3 N, inout float ior) {
	NoxWetResult r;
	r.puddle = 0.0;
	r.wetness = 0.0;
	float wet = clamp(uWeatherWetness * wetResponse, 0.0, 1.0);
	if (wet <= 0.0) {
		return r;
	}

	// Large-scale breakup so the road dries unevenly (tyre lines, sheltered areas).
	float breakup = NoxFbm(worldPos.xz * 0.35 + 3.7);
	float localWet = wet * smoothstep(0.15, 0.55, breakup + 0.25 * wet);
	float upFacing = smoothstep(0.75, 0.95, geoNormal.y);

	// Standing water.
	float puddle = 0.0;
	if (puddleResponse > 0.0 && uWeatherPuddles > 0.0 && upFacing > 0.0) {
		float n = NoxFbm(worldPos.xz / max(uWeatherPuddleScale, 0.1));
		float threshold = mix(0.85, 0.35, clamp(uWeatherPuddles, 0.0, 1.0));
		float height = n + cavity * 0.18;
		puddle = smoothstep(threshold, threshold + 0.05, height) * puddleResponse * upFacing * wet;
		// Damp ring around every puddle.
		localWet = max(localWet, smoothstep(threshold - 0.12, threshold, height) * wet);
	}

	// Wet substrate (Lagarde): porous dielectrics darken up to ~50 %; everything smooths, and water
	// filling micro-crevices flattens fine normal detail (keeps the coarse breakup).
	N = normalize(mix(N, geoNormal, localWet * 0.65));
	float porosity = clamp((roughness - 0.35) * 2.0, 0.0, 1.0) * (1.0 - metallic);
	albedo *= mix(1.0, mix(0.85, 0.45, porosity), localWet);
	// A continuous water film: rough asphalt (~0.9) drops to ~0.2, keeping relative variation so
	// highlights stay broken and stretched instead of mirror-like (standing water goes lower below).
	float wetRoughness = clamp(roughness * 0.22, 0.05, 0.25);
	roughness = mix(roughness, wetRoughness, localWet * 0.95);

	if (puddle > 0.0) {
		vec3 flatN = normalize(mix(N, geoNormal, puddle));
		if (uWeatherRain > 0.0) {
			vec2 slope = NoxRippleSlope(worldPos.xz, uWeatherTime, 0.35) * 0.55 +
				NoxRippleSlope(worldPos.xz + 7.3, uWeatherTime * 1.13, 0.19) * 0.35;
			slope *= uWeatherRain * puddle;
			flatN = normalize(flatN + vec3(slope.x, 0.0, slope.y));
		}
		N = flatN;
		albedo *= mix(1.0, 0.6, puddle);
		roughness = mix(roughness, 0.02, puddle);
		ior = mix(ior, 1.33, puddle);
	}

	r.puddle = puddle;
	r.wetness = localWet;
	return r;
}

// Rain beads on smooth coated surfaces. objectPos is in object-local metres, so drops move with
// the object. Returns bead coverage and perturbs N toward each droplet's dome normal.
float NoxApplyRainBeads(vec3 objectPos, vec3 geoNormal, float beadResponse, inout vec3 N, inout float roughness) {
	float amount = clamp(beadResponse * max(uWeatherRain, uWeatherWetness * 0.5), 0.0, 1.0);
	if (amount <= 0.0) {
		return 0.0;
	}
	// Triplanar projection on the dominant axis of the object-space normal.
	vec3 an = abs(geoNormal);
	vec2 p = an.y > max(an.x, an.z) ? objectPos.xz : (an.x > an.z ? objectPos.zy : objectPos.xy);
	// Fade drops out before they shrink below ~2 pixels: sub-pixel mirror-bright drops alias under
	// TAA jitter and read as sparkle (drop radius is 15-38 % of a cell).
	const float cellSize = 0.012;
	float footprint = length(fwidth(p));
	float lod = 1.0 - smoothstep(cellSize * 0.1, cellSize * 0.25, footprint);
	if (lod <= 0.0) {
		return 0.0;
	}
	vec2 cell = floor(p / cellSize);
	float coverage = 0.0;
	vec2 bestOffset = vec2(0.0);
	float bestRadius = 1.0;
	for (int j = -1; j <= 1; ++j) {
		for (int i = -1; i <= 1; ++i) {
			vec2 c = cell + vec2(i, j);
			vec2 rnd = NoxHash22(c * 1.37 + 11.0);
			if (rnd.x > 0.12 + 0.3 * amount) continue; // sparse: density follows rain amount
			vec2 centre = (c + 0.2 + 0.6 * rnd) * cellSize;
			float radius = cellSize * mix(0.15, 0.38, NoxHash12(c + 5.1));
			vec2 d = p - centre;
			float dist = length(d);
			if (dist < radius) {
				coverage = 1.0;
				bestOffset = d;
				bestRadius = radius;
			}
		}
	}
	coverage *= lod * amount;
	if (coverage > 0.0) {
		// Dome normal: tilt toward the drop edge, in the projected plane.
		vec2 s = bestOffset / bestRadius;
		float h = sqrt(max(1.0 - dot(s, s), 0.0));
		vec3 t1 = normalize(cross(abs(geoNormal.y) < 0.99 ? vec3(0, 1, 0) : vec3(1, 0, 0), N));
		vec3 t2 = cross(N, t1);
		vec3 domeN = normalize(N * h + (t1 * s.x + t2 * s.y) * 0.9);
		N = normalize(mix(N, domeN, coverage));
		roughness = mix(roughness, 0.02, coverage);
	}
	return coverage;
}

#endif // NOX_WEATHER_COMMON_GLSL
