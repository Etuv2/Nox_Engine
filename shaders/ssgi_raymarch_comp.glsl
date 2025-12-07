#version 460 core
layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;


// INPUTS

uniform sampler2D gDepth;
uniform sampler2D gPackedNormalRM;  // RT0: oct normal (RG) + roughness (B) + metallic (A)
uniform sampler2D gAlbedoAO;        // RT1: albedo (RGB) + occlusion (A)
uniform sampler2D randTex;          // Stochastic direction texture from directions pass
uniform sampler2D prevColor;        // Previous frame HDR color for ray hit sampling
uniform samplerCube iblIrradiance;  // IBL irradiance cubemap for fallback


// OUTPUT

layout (rgba16f, binding = 0) writeonly uniform image2D outputImage; // RGB = irradiance, A = hit mask


// UNIFORMS

uniform mat4 invProj;               // Inverse projection matrix
uniform mat4 invView;               // Inverse view matrix (world from view)
uniform mat4 view;                  // View matrix (view from world)
uniform mat4 proj;                  // Projection matrix
uniform float maxRayLenVS;          // Maximum ray length in view-space units
uniform int numSteps;               // Number of ray marching steps
uniform float thickness;            // Surface thickness for intersection (view-space units)
uniform vec2 screenSize;            // Full resolution screen size
uniform vec2 workSize;              // Working resolution (may be half-res)
uniform float cameraNear;           // Camera near plane (unused - kept for compatibility)
uniform float cameraFar;            // Camera far plane (unused - kept for compatibility)
uniform int hasIBL;                 // 0/1 flag for IBL availability
uniform float iblFallbackStrength;  // IBL fallback blend strength


// FUNCTIONS


// Linearize depth from [0,1] non-linear to view-space Z
float LinearizeDepth(float d) {
	float z = d * 2.0 - 1.0;
	float A = proj[2][2];
	float B = proj[3][2];
	float C = proj[2][3];
	return B / (z * C - A); // Negative in front of camera (view space Z convention)
}

// FIXED: Octahedral decode with proper [0,1] to [-1,1] remapping (matches SSAO)
vec3 octDecode(vec2 e) {
	// Remap from [0,1] (texture storage) to [-1,1]
	e = e * 2.0 - 1.0;
	
	vec3 n;
	n.z = 1.0 - abs(e.x) - abs(e.y);
	
	if (n.z < 0.0) {
		// Handle lower hemisphere fold
		vec2 signE = sign(e);
		signE = mix(vec2(1.0), signE, step(vec2(0.0001), abs(e)));
		n.xy = (1.0 - abs(e.yx)) * signE;
	} else {
		n.xy = e.xy;
	}
	
	return normalize(n);
}

// Reconstruct view-space position from screen UV and depth
vec3 ReconstructVS(vec2 uv, float depth) {
	vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
	vec4 vs = invProj * ndc;
	return vs.xyz / vs.w;
}

// Project view-space position back to screen UV (matches screen-space shadows)
vec2 VS_to_UV(vec3 viewPos) {
	vec4 clipPos = proj * vec4(viewPos, 1.0);
	vec2 ndc = clipPos.xy / clipPos.w;
	return ndc * 0.5 + 0.5;
}

// CRITICAL FIX: Per-pixel view-space footprint for scale-independent stepping
float PixelSizeVS(float absViewZ) {
	float tanHalfFovy = 1.0 / proj[1][1];
	float viewHeight = 2.0 * absViewZ * tanHalfFovy;
	return viewHeight / max(workSize.y, 1.0);
}

// NEW: Edge-aware depth sampling (prevents cross-edge bleeding like screen-space shadows)
float SampleDepthEdgeAware(vec2 uv, float centerDepth, float edgeThreshold) {
	ivec2 size = textureSize(gDepth, 0);
	vec2 texel = 1.0 / vec2(size);

	vec2 st = uv * vec2(size) - 0.5;
	ivec2 ij = ivec2(floor(st));
	vec2 f = fract(st);

	float d00 = texelFetch(gDepth, clamp(ij, ivec2(0), size - 1), 0).r;
	float d10 = texelFetch(gDepth, clamp(ij + ivec2(1, 0), ivec2(0), size - 1), 0).r;
	float d01 = texelFetch(gDepth, clamp(ij + ivec2(0, 1), ivec2(0), size - 1), 0).r;
	float d11 = texelFetch(gDepth, clamp(ij + ivec2(1, 1), ivec2(0), size - 1), 0).r;

	// Reject bilinear if edge detected
	if (abs(d00 - d10) > edgeThreshold || abs(d01 - d11) > edgeThreshold) {
		return centerDepth;
	}

	float dx0 = mix(d00, d10, f.x);
	float dx1 = mix(d01, d11, f.x);
	return mix(dx0, dx1, f.y);
}

// Decode normal from oct-encoded 8-bit pair
vec3 DecodeNormalOct8(vec2 e) {
	vec3 n;
	n.z = 1.0 - abs(e.x) - abs(e.y);
	n.xy = n.z >= 0.0 ? e.xy : (1.0 - abs(e.yx)) * sign(e.xy);
	return normalize(n);
}


// MAIN

void main() {
	ivec2 id = ivec2(gl_GlobalInvocationID.xy);
	ivec2 dstSize = imageSize(outputImage);

	// Early exit for out-of-bounds threads
	if (any(greaterThanEqual(id, dstSize))) {
		imageStore(outputImage, id, vec4(0));
		return;
	}

	// Compute UVs (working resolution for output)
	vec2 uvWork = (vec2(id) + 0.5) / workSize;

	// Sample depth from full-resolution buffer
	float depthRaw = texture(gDepth, uvWork).r;
	if (depthRaw >= 0.999) {
		imageStore(outputImage, id, vec4(0.0));
		return;
	}


	// Reconstruct View-Space Position

	vec3 originVS = ReconstructVS(uvWork, depthRaw);
	float originDepthLinear = LinearizeDepth(depthRaw);

	// Validate: in right-handed view space, objects in front have negative Z
	if (originVS.z >= 0.0) {
		imageStore(outputImage, id, vec4(0.0));
		return;
	}


	// Transform Normal to View Space

	vec2 normalEnc = texture(gPackedNormalRM, uvWork).rg;
	vec3 normalWorld = octDecode(normalEnc);
	vec3 normalVS = normalize(mat3(view) * normalWorld);


	// Generate Cosine-Weighted Hemisphere Direction (Malley’s Method)

	vec2 rands = texture(randTex, uvWork).rg;

	float r = sqrt(rands.x);
	float phi = 6.283185307179586 * rands.y; // 2*PI
	vec3 diskSample = vec3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - rands.x)));

	// Build orthonormal basis in view space (Gram-Schmidt)
	vec3 up = abs(normalVS.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent = normalize(cross(up, normalVS));
	vec3 bitangent = cross(normalVS, tangent);

	// Transform disk sample to hemisphere
	vec3 directionVS = normalize(
		tangent * diskSample.x +
		bitangent * diskSample.y +
		normalVS * diskSample.z
	);

	// Reject invalid rays
	if (dot(directionVS, normalVS) < 0.01) {
		imageStore(outputImage, id, vec4(0.0));
		return;
	}


	//Screen-Space Ray Marching

	float pixelVS = PixelSizeVS(abs(originVS.z));

	float depthRatio = abs(originVS.z) / max(cameraNear, 0.1);
	float effectiveMaxRayLen = maxRayLenVS * sqrt(depthRatio);

	float stepLenVS = effectiveMaxRayLen / float(max(numSteps, 1));
	stepLenVS = max(stepLenVS, pixelVS * 0.75);

	float depthAdaptation = mix(0.8, 1.2, smoothstep(0.0, 100.0, depthRatio));
	float grazingFactor = abs(dot(directionVS, normalVS));
	float angleAdaptation = mix(0.8, 1.0, grazingFactor);

	stepLenVS *= depthAdaptation * angleAdaptation;
	stepLenVS *= (1.0 + 0.1 * (rands.x + rands.y - 1.0)); // Temporal jitter

	vec3 stepVS = directionVS * stepLenVS;
	vec3 rayPosVS = originVS;
	vec3 hitPosVS = vec3(0.0);
	bool foundHit = false;

	float closestMissDistance = 1e6;
	vec3 closestMissPosition = vec3(0.0);

	float adaptiveThickness = max(thickness, pixelVS * 1.5);

	for (int i = 0; i < numSteps; ++i) {
		rayPosVS += stepVS;

		vec2 sampleUV = VS_to_UV(rayPosVS);

		if (any(lessThan(sampleUV, vec2(0.0))) || any(greaterThan(sampleUV, vec2(1.0)))) {
			break;
		}

		float sampleDepth = SampleDepthEdgeAware(sampleUV, depthRaw, 0.01);
		if (sampleDepth >= 0.999) continue;

		vec3 surfaceVS = ReconstructVS(sampleUV, sampleDepth);

		float depthDifference = surfaceVS.z - rayPosVS.z;
		bool intersects = (depthDifference < 0.0) && (depthDifference > -adaptiveThickness * 2.0);

		if (!intersects && abs(depthDifference) < closestMissDistance) {
			closestMissDistance = abs(depthDifference);
			closestMissPosition = rayPosVS;
		}

		if (intersects) {
			foundHit = true;

			// Binary search refinement
			vec3 searchStart = rayPosVS - stepVS;
			vec3 searchEnd = rayPosVS;

			for (int refinement = 0; refinement < 6; ++refinement) {
				vec3 searchMid = 0.5 * (searchStart + searchEnd);
				vec2 midUV = VS_to_UV(searchMid);

				if (any(lessThan(midUV, vec2(0.0))) || any(greaterThan(midUV, vec2(1.0)))) break;

				float midDepth = texture(gDepth, midUV).r;
				if (midDepth >= 0.999) break;

				vec3 midSurfaceVS = ReconstructVS(midUV, midDepth);

				if (midSurfaceVS.z < searchMid.z) {
					searchEnd = searchMid;
				} else {
					searchStart = searchMid;
				}
			}

			hitPosVS = searchEnd;
			break;
		}
	}

	// Fallback to near-miss
	if (!foundHit && closestMissDistance < adaptiveThickness * 3.0) {
		foundHit = true;
		hitPosVS = closestMissPosition;
	}


	// Step 5: Sample Indirect Lighting from Hit Position

	vec3 indirectIrradiance = vec3(0.0);
	float validityMask = 0.0;

	if (foundHit) {
		vec2 hitUV = VS_to_UV(hitPosVS);

		if (all(greaterThanEqual(hitUV, vec2(0.0))) && all(lessThan(hitUV, vec2(1.0)))) {
			vec3 hitColor = texture(prevColor, hitUV).rgb;
			
			// CRITICAL FIX: hitColor is final lit radiance (HDR), clamp to prevent excessive energy
			// Real-time SSGI samples final frame buffer which includes direct + indirect + emissive
			// This can create feedback loops with very bright values
			hitColor = min(hitColor, vec3(10.0)); // Clamp to reasonable HDR range
			
			vec3 irradiance = hitColor;

			float depthRatio = abs(originVS.z) / max(cameraNear, 0.1);
			float effectiveMaxRayLen = maxRayLenVS * sqrt(depthRatio);

			float hitDistance = length(hitPosVS - originVS);
			float attenuation = 1.0 - smoothstep(0.0, effectiveMaxRayLen, hitDistance);
			attenuation *= attenuation;

			vec3 hitDirection = normalize(hitPosVS - originVS);
			float cosineAtReceiver = max(dot(normalVS, hitDirection), 0.0);

			// Apply geometric term (distance + angle falloff)
			irradiance *= attenuation * cosineAtReceiver;
			
			// CRITICAL FIX: Scale down by PI for proper energy conservation
			// We're treating this as incoming irradiance that will be integrated over hemisphere
			irradiance *= 0.318309886; // 1/PI
			
			float luminance = dot(irradiance, vec3(0.2126, 0.7152, 0.0722));
			if (luminance > 0.0001) {
				indirectIrradiance = irradiance;
				validityMask = 1.0;
			}
		}
	}


	// IBL Fallback for Misses

	if (hasIBL == 1 && validityMask < 0.5) {
		vec3 worldNormal = normalize(mat3(invView) * normalVS);
		vec3 iblIrr = texture(iblIrradiance, worldNormal).rgb;
		indirectIrradiance = mix(indirectIrradiance, iblIrr, iblFallbackStrength);
	}

	// Store result
	imageStore(outputImage, id, vec4(indirectIrradiance, validityMask));
}
