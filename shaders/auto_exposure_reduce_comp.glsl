#version 460 core
// Auto exposure, step 2: percentile-trimmed average luminance -> EV100 -> temporal adaptation.
// Also used (uMode = 1) to commit last frame's exposure as this frame's pre-exposure.

layout(local_size_x = 64) in;

layout(std430, binding = 42) buffer ExposureState {
	float exposurePre;
	float exposureCurrent;
	float exposureEV100;
	float exposureAverageLuminance;
	float exposurePrevPre;
};
layout(std430, binding = 43) buffer ExposureHistogram { uint bins[64]; };

uniform int uMode;            // 0 = meter + adapt, 1 = commit (pre = current), 2 = reset to uResetEV
uniform float uMinLog2Lum;
uniform float uLog2LumRange;
uniform float uLowPercentile;
uniform float uHighPercentile;
uniform float uCompensationEV;
uniform float uMinEV;
uniform float uMaxEV;
uniform float uSpeedUp;
uniform float uSpeedDown;
uniform float uDeltaTime;
uniform float uResetEV;
uniform int uSnap;            // 1 = jump straight to the metered value (camera cut / first frame)

shared uint sBins[64];

float ExposureFromEV100(float ev) { return 1.0 / (1.2 * exp2(ev)); }

void main() {
	uint i = gl_LocalInvocationIndex;
	if (uMode == 1) {
		if (i == 0u) {
			exposurePrevPre = exposurePre;
			exposurePre = exposureCurrent;
		}
		return;
	}
	if (uMode == 2) {
		if (i == 0u) {
			exposureEV100 = uResetEV;
			exposureCurrent = ExposureFromEV100(uResetEV);
			exposurePre = exposureCurrent;
			exposurePrevPre = exposureCurrent;
		}
		if (i < 64u) bins[i] = 0u;
		return;
	}

	sBins[i] = bins[i];
	bins[i] = 0u; // ready for next frame
	barrier();
	if (i != 0u) return;

	// Bin 0 holds black pixels; it counts toward the percentile ranks but not the average.
	float total = 0.0;
	for (int b = 0; b < 64; ++b) total += float(sBins[b]);
	if (total <= 0.0) return;
	float lo = total * uLowPercentile;
	float hi = total * uHighPercentile;
	float acc = 0.0, sumLog = 0.0, weight = 0.0;
	for (int b = 0; b < 64; ++b) {
		float n = float(sBins[b]);
		float start = acc;
		acc += n;
		float inRange = max(min(acc, hi) - max(start, lo), 0.0);
		if (b == 0 || inRange <= 0.0) continue;
		float binLog2 = uMinLog2Lum + ((float(b) - 0.5) / 62.0) * uLog2LumRange;
		sumLog += binLog2 * inRange;
		weight += inRange;
	}
	float avgLog2 = (weight > 0.0) ? sumLog / weight : uMinLog2Lum;
	float avgLum = exp2(avgLog2);
	// Frostbite: EV100 = log2(Lavg * S / K), S = 100, K = 12.5. Compensation brightens.
	float target = clamp(log2(avgLum * 100.0 / 12.5) - uCompensationEV, uMinEV, uMaxEV);
	float current = exposureEV100;
	if (uSnap == 1 || isnan(current) || isinf(current)) {
		current = target;
	} else {
		// A higher target EV means the scene got brighter (adapt with uSpeedUp, like the eye's
		// fast light adaptation); a lower one means darker (slower dark adaptation).
		float rate = (target > current) ? uSpeedUp : uSpeedDown;
		current += (target - current) * (1.0 - exp(-uDeltaTime * rate));
	}
	exposureEV100 = current;
	exposureCurrent = ExposureFromEV100(current);
	exposureAverageLuminance = avgLum;
}
