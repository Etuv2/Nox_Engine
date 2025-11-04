#version 460 core

// Screen-space ray marching for SSGI - FIXED to match screen-space shadows approach
// Reference: https://www.ea.com/seed/news/seed-dd18-presentation-slides-raytracing

layout (local_size_x = 8, local_size_y = 8) in;

// Input textures
layout (binding = 0) uniform sampler2D depthTex;  // Scene depth (non-linear [0,1]) - FULL RESOLUTION
layout (binding = 1) uniform sampler2D normalTex;   // G-buffer normals (oct-encoded in RG, WORLD SPACE) - FULL RESOLUTION
layout (binding = 2) uniform sampler2D albedoTex;   // Surface albedo (unused in this pass - applied in lighting)
layout (binding = 3) uniform sampler2D randTex;  // Stochastic directions from previous pass (RGBA with random values) - WORKING RESOLUTION
layout (binding = 4) uniform sampler2D prevColor;   // Previous frame HDR color (for sampling hits) - WORKING RESOLUTION
layout (binding = 6) uniform samplerCube iblIrradiance; // IBL diffuse irradiance (fallback for misses)

// Output texture - RGBA16F: RGB = indirect irradiance, A = hit mask
layout (binding = 5, rgba16f) writeonly uniform image2D outSSGI;

// Uniforms
uniform mat4 invProj;       // Inverse projection matrix
uniform mat4 invView;       // Inverse view matrix (world from view)
uniform mat4 view;          // View matrix (view from world)
uniform mat4 proj;// Projection matrix
uniform float maxRayLenVS;  // Maximum ray length in view space units
uniform int numSteps;    // Number of ray marching steps
uniform float thickness;  // Surface thickness for intersection (view-space units)
uniform vec2 screenSize;    // Full resolution screen size
uniform vec2 workSize;    // Working resolution (may be half-res)
uniform float cameraNear;   // Camera near plane (unused - kept for compatibility)
uniform float cameraFar;    // Camera far plane (unused - kept for compatibility)
uniform int hasIBL;  // 0/1 flag for IBL availability
uniform float iblFallbackStrength; // IBL fallback blend strength

// ============================================================================
// CRITICAL FIX: Use exact same depth linearization as screen-space shadows
// ============================================================================
float LinearizeDepth(float d) {
    float z = d * 2.0 - 1.0;
    float A = proj[2][2];
    float B = proj[3][2];
    float C = proj[2][3]; 
    return B / (z * C - A); // negative in front of camera (view space Z convention)
}

// Octahedral decode for world-space normals (matches SSAO exactly)
vec3 octDecode(vec2 e) {
    vec2 f = e * 2.0 - 1.0;
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += (n.x >= 0.0 ? -t : t);
    n.y += (n.y >= 0.0 ? -t : t);
    return normalize(n);
}

// CRITICAL FIX: Use exact same reconstruction as screen-space shadows
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
    // Extract half-FOV from projection matrix
    float tanHalfFovy = 1.0 / proj[1][1];
    
    // View-space height at this depth
    float viewHeight = 2.0 * absViewZ * tanHalfFovy;
    
    // CRITICAL: Use WORKING resolution for pixel size calculation
    return viewHeight / max(workSize.y, 1.0);
}

// NEW: Edge-aware depth sampling (prevents cross-edge bleeding like screen-space shadows)
float SampleDepthEdgeAware(vec2 uv, float centerDepth, float edgeThreshold) {
 ivec2 size = textureSize(depthTex, 0);
    vec2 texel = 1.0 / vec2(size);

    vec2 st = uv * vec2(size) - 0.5;
    ivec2 ij = ivec2(floor(st));
    vec2 f = fract(st);

    float d00 = texelFetch(depthTex, clamp(ij, ivec2(0), size - 1), 0).r;
  float d10 = texelFetch(depthTex, clamp(ij + ivec2(1,0), ivec2(0), size - 1), 0).r;
    float d01 = texelFetch(depthTex, clamp(ij + ivec2(0,1), ivec2(0), size - 1), 0).r;
    float d11 = texelFetch(depthTex, clamp(ij + ivec2(1,1), ivec2(0), size - 1), 0).r;

    // Reject bilinear if edge detected
    if (abs(d00 - d10) > edgeThreshold || abs(d01 - d11) > edgeThreshold)
        return centerDepth;

    float dx0 = mix(d00, d10, f.x);
    float dx1 = mix(d01, d11, f.x);
    return mix(dx0, dx1, f.y);
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 dstSize = imageSize(outSSGI);
    
    // Early exit for out-of-bounds threads
    if (any(greaterThanEqual(id, dstSize))) {
        imageStore(outSSGI, id, vec4(0));
        return;
    }
    
    // CRITICAL FIX: Compute UVs correctly - working resolution for output
    vec2 uvWork = (vec2(id) + 0.5) / workSize;
    
    // Sample depth from FULL RESOLUTION buffer using work UV (maps correctly)
    float depthRaw = texture(depthTex, uvWork).r;
    if (depthRaw >= 0.999) {
        imageStore(outSSGI, id, vec4(0.0));
     return;
    }
    
    // ========================================================================
    // Step 1: Reconstruct View-Space Position (matches screen-space shadows)
    // ========================================================================
    vec3 originVS = ReconstructVS(uvWork, depthRaw);
    
    // CRITICAL: Linearize depth for proper distance checks
    float originDepthLinear = LinearizeDepth(depthRaw);
    
    // Validate: in right-handed view space, objects in front have negative Z
    if (originVS.z >= 0.0) {
        imageStore(outSSGI, id, vec4(0.0));
     return;
    }
    
// ========================================================================
    // Step 2: Transform Normal to View Space
    // ========================================================================
    vec2 normalEnc = texture(normalTex, uvWork).rg;
    vec3 normalWorld = octDecode(normalEnc);
  
    // Transform to view space for hemisphere sampling
    vec3 normalVS = normalize(mat3(view) * normalWorld);
    
    // ========================================================================
    // Step 3: Generate Cosine-Weighted Hemisphere Direction (Malley's Method)
    // ========================================================================
  vec2 rands = texture(randTex, uvWork).rg;
 
    // Malley's method: map uniform disk to cosine-weighted hemisphere
    float r = sqrt(rands.x);
    float phi = 6.283185307179586 * rands.y; // 2*PI
    vec3 diskSample = vec3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - rands.x)));
    
    // Build orthonormal basis in view space (Gram-Schmidt)
    vec3 up = abs(normalVS.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, normalVS));
    vec3 bitangent = cross(normalVS, tangent);
 
    // Transform disk sample to hemisphere using TBN matrix
    vec3 directionVS = normalize(
        tangent * diskSample.x + 
        bitangent * diskSample.y + 
        normalVS * diskSample.z
    );
    
    // Validate: ensure ray points AWAY from surface
    if (dot(directionVS, normalVS) < 0.01) {
   imageStore(outSSGI, id, vec4(0.0));
        return;
    }
    
    // ========================================================================
    // Step 4: Screen-Space Ray Marching (matches screen-space shadows logic)
    // ========================================================================

    // CRITICAL FIX: Adaptive step sizing based on pixel footprint
    float pixelVS = PixelSizeVS(abs(originVS.z));
    
    // Base step size in view-space units
    float stepLenVS = maxRayLenVS / float(max(numSteps, 1));
  
    // CRITICAL: Ensure minimum step is tied to pixel footprint
    stepLenVS = max(stepLenVS, pixelVS * 0.75);
    
    // Depth adaptation: closer surfaces need finer steps
    float depthAdaptation = mix(0.6, 1.4, smoothstep(1.0, 10.0, abs(originVS.z)));
    
    // Angular adaptation: grazing angles need finer steps
    float grazingFactor = abs(dot(directionVS, normalVS));
    float angleAdaptation = mix(0.8, 1.0, grazingFactor);
    
    stepLenVS *= depthAdaptation * angleAdaptation;
    
    // Minimal jitter for temporal stability (Sachdeva's insight)
    stepLenVS *= (1.0 + 0.1 * (rands.x + rands.y - 1.0));
    
    // Compute step vector in view space
    vec3 stepVS = directionVS * stepLenVS;
 
    // Ray marching state
    vec3 rayPosVS = originVS;
    vec3 hitPosVS = vec3(0.0);
    bool foundHit = false;
    
    // Track near-misses for fallback
    float closestMissDistance = 1e6;
    vec3 closestMissPosition = vec3(0.0);

    // CRITICAL FIX: Adaptive thickness based on depth and pixel footprint
    float adaptiveThickness = max(thickness, pixelVS * 1.5);
    
    // March ray through view space
  for (int i = 0; i < numSteps; ++i) {
        rayPosVS += stepVS;
        
        // Project to screen space
        vec2 sampleUV = VS_to_UV(rayPosVS);
        
 // Early exit if ray leaves screen
    if (any(lessThan(sampleUV, vec2(0.0))) || any(greaterThan(sampleUV, vec2(1.0)))) {
    break;
        }
        
  // CRITICAL FIX: Use edge-aware sampling to prevent cross-edge artifacts
        float sampleDepth = SampleDepthEdgeAware(sampleUV, depthRaw, 0.01);
        if (sampleDepth >= 0.999) continue; // Skip sky
    
        // Reconstruct surface position at sample point
        vec3 surfaceVS = ReconstructVS(sampleUV, sampleDepth);
        
        // CRITICAL FIX: Use linearized depth for proper thickness comparison
   float surfaceDepthLinear = LinearizeDepth(sampleDepth);
   float rayDepthLinear = LinearizeDepth(VS_to_UV(rayPosVS).x); // Linearize ray depth
        
   // Intersection test using linearized depths (matches screen-space shadows)
        float depthDifference = surfaceVS.z - rayPosVS.z;
  bool intersects = (depthDifference < 0.0) && (depthDifference > -adaptiveThickness * 2.0);
        
    // Track near-misses for fallback
      if (!intersects && abs(depthDifference) < closestMissDistance) {
    closestMissDistance = abs(depthDifference);
       closestMissPosition = rayPosVS;
        }
        
        if (intersects) {
            foundHit = true;
      
            // Binary search refinement for sub-pixel accuracy
            vec3 searchStart = rayPosVS - stepVS;
     vec3 searchEnd = rayPosVS;
    
  for (int refinement = 0; refinement < 6; ++refinement) {
           vec3 searchMid = 0.5 * (searchStart + searchEnd);
     vec2 midUV = VS_to_UV(searchMid);
     
        // Bounds check
      if (any(lessThan(midUV, vec2(0.0))) || any(greaterThan(midUV, vec2(1.0)))) {
                  break;
            }

            float midDepth = texture(depthTex, midUV).r;
     if (midDepth >= 0.999) break;
       
    vec3 midSurfaceVS = ReconstructVS(midUV, midDepth);
                
        // Binary search: adjust interval based on depth comparison
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
    
    // Fallback to near-miss if within reasonable distance
    if (!foundHit && closestMissDistance < adaptiveThickness * 3.0) {
   foundHit = true;
     hitPosVS = closestMissPosition;
    }
    
    // ========================================================================
    // Step 5: Sample Indirect Lighting from Hit Position
    // ========================================================================
    
    vec3 indirectIrradiance = vec3(0.0);
    float validityMask = 0.0;
    
    if (foundHit) {
        vec2 hitUV = VS_to_UV(hitPosVS);

        // Bounds check
        if (all(greaterThanEqual(hitUV, vec2(0.0))) && all(lessThan(hitUV, vec2(1.0)))) {
        // Sample previous frame's HDR color (WORKING RESOLUTION)
     vec3 hitColor = texture(prevColor, hitUV).rgb;
       
            // We're computing IRRADIANCE, not radiance
     vec3 irradiance = hitColor;
       
            // Distance-based attenuation with quadratic falloff
          float hitDistance = length(hitPosVS - originVS);
       float attenuation = 1.0 - smoothstep(0.0, maxRayLenVS, hitDistance);
 attenuation = attenuation * attenuation;
     
  // Geometric attenuation: Lambert's cosine law at receiver
        vec3 hitDirection = normalize(hitPosVS - originVS);
            float cosineAtReceiver = max(dot(normalVS, hitDirection), 0.0);
  
 // Apply attenuation
            irradiance *= attenuation * cosineAtReceiver;
            
            // Validation: reject very dark samples
            float luminance = dot(irradiance, vec3(0.2126, 0.7152, 0.0722));
     if (luminance > 0.0001) {
           indirectIrradiance = irradiance;
           validityMask = 1.0;
            }
        }
    }
    
    // ========================================================================
    // Step 6: IBL Fallback for Misses
    // ========================================================================
    if (hasIBL == 1 && validityMask < 0.5) {
        // Sample irradiance using world normal
        vec3 worldNormal = normalize(mat3(invView) * normalVS);
     vec3 iblIrr = texture(iblIrradiance, worldNormal).rgb;
      indirectIrradiance = mix(indirectIrradiance, iblIrr, iblFallbackStrength);
    }
  
    // Store result: RGB = indirect irradiance, A = validity mask
    imageStore(outSSGI, id, vec4(indirectIrradiance, validityMask));
}
