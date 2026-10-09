#version 460 core
// Motion blur step 1/2 (McGuire et al. 2012, "A Reconstruction Filter for Plausible Motion Blur").
// uPass 0: per-tile dominant (longest) velocity in pixels. uPass 1: 3x3 neighbourhood max of tiles.

layout(local_size_x = 8, local_size_y = 8) in;

layout(rg16f, binding = 0) uniform writeonly image2D uOut;
uniform sampler2D uVelocity;   // pass 0: full-res motion vectors (uv units)
uniform sampler2D uTiles;      // pass 1: tile max
uniform int uPass;
uniform int uTileSize;
uniform vec2 uFullSize;
uniform float uVelocityScale;  // shutter scale (exposure time / frame time)
uniform float uMaxBlurPixels;

vec2 ClampLength(vec2 v, float maxLen) {
	float len = length(v);
	return len > maxLen ? v * (maxLen / len) : v;
}

void main() {
	ivec2 cell = ivec2(gl_GlobalInvocationID.xy);
	ivec2 outSize = imageSize(uOut);
	if (any(greaterThanEqual(cell, outSize))) return;
	vec2 best = vec2(0.0);
	float bestLen = 0.0;
	if (uPass == 0) {
		ivec2 origin = cell * uTileSize;
		for (int y = 0; y < uTileSize; ++y) {
			for (int x = 0; x < uTileSize; ++x) {
				ivec2 p = min(origin + ivec2(x, y), ivec2(uFullSize) - 1);
				vec2 v = ClampLength(texelFetch(uVelocity, p, 0).rg * uFullSize * uVelocityScale, uMaxBlurPixels);
				float l = dot(v, v);
				if (l > bestLen) { bestLen = l; best = v; }
			}
		}
	} else {
		for (int y = -1; y <= 1; ++y) {
			for (int x = -1; x <= 1; ++x) {
				vec2 v = texelFetch(uTiles, clamp(cell + ivec2(x, y), ivec2(0), outSize - 1), 0).rg;
				float l = dot(v, v);
				if (l > bestLen) { bestLen = l; best = v; }
			}
		}
	}
	imageStore(uOut, cell, vec4(best, 0.0, 0.0));
}
