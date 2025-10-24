#version 460 core

// Screen-space ray marching for SSGI
// Traces rays in cosine-weighted hemisphere to find indirect lighting

layout (local_size_x = 8, local_size_y = 8) in;

// Input textures
layout (binding = 0) uniform sampler2D depthTex;      // Scene depth
layout (binding = 1) uniform sampler2D normalTex;     // G-buffer normals (oct-encoded in RG)
layout (binding = 2) uniform sampler2D albedoTex;     // Surface albedo
layout (binding = 3) uniform sampler2D randTex;       // Random values from directions pass
layout (binding = 4) uniform sampler2D prevColor;     // Previous frame color (for sampling hits)

// Output texture - FIXED: use rgba16f instead of rgb16f (RGB not supported for images)
layout (binding = 5, rgba16f) writeonly uniform image2D outSSGI;

// Uniforms
uniform mat4 invProj;       // Inverse projection matrix
uniform mat4 invView;       // Inverse view matrix (world from view)
uniform mat4 view;          // View matrix (view from world) - NEW
uniform float maxRayLenVS;  // Maximum ray length in view space
uniform int numSteps;       // Number of ray marching steps
uniform float thickness;    // Thickness parameter for intersection
uniform vec2 screenSize;    // Full resolution screen size
uniform vec2 workSize;      // Working resolution (may be half-res)

// Octahedral decode matching lighting pass (expects input in [-1,1])
vec3 DecodeNormalOct8(vec2 e) {
    vec3 n;
    n.z = 1.0 - abs(e.x) - abs(e.y);
    n.xy = n.z >= 0.0 ? e.xy : (1.0 - abs(e.yx)) * sign(e.xy);
    return normalize(n);
}

// Reconstruct view-space position from UV and depth
vec3 reprojToView(vec2 uv, float depth) {
    // Depth is in [0,1], convert to NDC [-1,1]
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 vpos = invProj * ndc;
    vpos /= vpos.w;
    return vpos.xyz;
}

// Project view-space position to screen UV and return uv
vec2 projectToUV(vec3 vpos) {
    mat4 proj = inverse(invProj);
    vec4 clip = proj * vec4(vpos, 1.0);
    vec3 ndc = clip.xyz / clip.w;
    return ndc.xy * 0.5 + 0.5;
}

float depthFromVS(vec3 vpos) {
    mat4 proj = inverse(invProj);
    vec4 clip = proj * vec4(vpos, 1.0);
    float ndcZ = clip.z / clip.w;        // [-1,1]
    return ndcZ * 0.5 + 0.5;             // [0,1]
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    
    // Early exit for out-of-bounds threads
    if (any(greaterThanEqual(id, ivec2(workSize)))) return;
    
    vec2 uvWork = (vec2(id) + 0.5) / workSize;
    vec2 uvFull = uvWork; // Simple mapping (works for both full and half-res)

    // Sample depth - skip sky pixels
    float depth = texelFetch(depthTex, ivec2(uvFull * screenSize), 0).r;
    if (depth >= 1.0) {
        imageStore(outSSGI, id, vec4(0.0));
        return;
    }

    // Reconstruct normal in view space
    vec2 enc = texelFetch(normalTex, ivec2(uvFull * screenSize), 0).rg;
    // Convert from [0,1] back to [-1,1] for oct decode
    vec3 nWorld = DecodeNormalOct8(enc * 2.0 - 1.0);
    // Transform world-space normal to view space
    vec3 nVS = normalize(mat3(view) * nWorld);

    // Get random values for cosine-weighted hemisphere sampling
    vec2 rands = texelFetch(randTex, ivec2(uvWork * workSize), 0).rg;
    
    // Build tangent basis around normal in view space
    vec3 bitangent = normalize(abs(nVS.z) < 0.999 ? 
                               vec3(-nVS.y, nVS.x, 0.0) : 
                               vec3(0.0, 1.0, 0.0));
    vec3 tangent = normalize(cross(bitangent, nVS));
    
    // Generate cosine-weighted direction
    float r = sqrt(rands.x);
    float phi = 6.2831853 * rands.y;

    // Create a jittered ray direction to break up banding artifacts
    vec3 jitter = vec3(rands.x - 0.5, rands.y - 0.5, 0.0) * 0.1;
    vec3 hemisphereDir = normalize(
        tangent * (r * cos(phi)) + 
        bitangent * (r * sin(phi)) + 
        nVS * sqrt(max(0.0, 1.0 - r * r))
    );
    vec3 dirVS = normalize(hemisphereDir + jitter);

    // Ray march in view space
    vec3 orgVS = reprojToView(uvFull, depth);
    float stepLen = maxRayLenVS / float(max(numSteps, 1));
    vec3 march = orgVS;
    vec3 hitVS = vec3(0.0);
    bool hit = false;

    for (int i = 0; i < numSteps; i++) {
        march += dirVS * stepLen;
        
        // Project to screen space
        vec2 uv = projectToUV(march);
        
        // Check if ray is off-screen
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) break;

        // Sample scene depth at this position
        float sceneDepth = textureLod(depthTex, uv, 0).r;
        if (sceneDepth >= 1.0) continue;

        // Compute ray depth
        float rayDepth = depthFromVS(march);

        // Self-intersection guard: require minimum travel in view space
        if (length(march - orgVS) < thickness) continue;
        
        // If our ray is deeper than the scene, we've crossed the surface
        if (rayDepth > sceneDepth + 1e-4) {
            hit = true;
            // Start refinement using binary search between previous and current march positions
            vec3 a = march - dirVS * stepLen;
            vec3 b = march;
            for (int j = 0; j < 4; ++j) {
                vec3 m = 0.5 * (a + b);
                vec2 mUV = projectToUV(m);
                float mScene = textureLod(depthTex, mUV, 0).r;
                float mRay   = depthFromVS(m);
                if (mRay > mScene) {
                    b = m; // still behind -> move closer to camera
                } else {
                    a = m; // in front -> move deeper
                }
            }
            hitVS = b; // final position just behind the surface
            break;
        }
    }

    // Refine intersection using binary search (if hit)
    if (hit) {
        // Sampled position in view space
        vec3 scenePosVS = hitVS;

        // Early out if the scene position is not valid
        if (scenePosVS.z >= 0.0) {
            hit = false;
        } else {
            // Perform binary search to refine the intersection point
            march = orgVS;
            vec3 stepDir = scenePosVS - orgVS;
            float stepDist = length(stepDir);
            stepDir /= max(stepDist, 1e-4);

            float tNear = 0.0;
            float tFar = stepDist;
            float tHit = 0.0;

            // Bisect to find the precise intersection
            for (int j = 0; j < 4; ++j) {
                tHit = (tNear + tFar) * 0.5;
                vec3 midPoint = orgVS + stepDir * tHit;
                vec2 midUV = projectToUV(midPoint);
                float midDepth = textureLod(depthTex, midUV, 0).r;
                vec3 midSceneVS = reprojToView(midUV, midDepth);

                if (midPoint.z < midSceneVS.z) {
                    tFar = tHit; // Move closer
                } else {
                    tNear = tHit; // Move away
                }
            }

            // Use the refined tHit to calculate the precise hit position
            march = orgVS + stepDir * tHit;
            hitVS = reprojToView(projectToUV(march), textureLod(depthTex, projectToUV(march), 0).r);
        }
    }

    // Sample indirect lighting if we hit something
    vec3 indirect = vec3(0.0);
    if (hit) {
        vec2 hitUV = projectToUV(hitVS);
        // Sample previous frame color at hit location
        vec3 srcColor = textureLod(prevColor, hitUV, 0).rgb;

        // Store pre-albedo indirect lighting (albedo applied in lighting pass)
        vec3 irradiance = srcColor;

        // Distance-based attenuation to stabilize and localize contribution
        float dist = length(hitVS - orgVS);
        float attenuation = 1.0 - smoothstep(0.0, maxRayLenVS, dist);
        irradiance *= attenuation * attenuation;

        // Cosine weighting with receiver normal to avoid light from grazing directions being over-strong
        float cosW = max(dot(nVS, normalize(hitVS - orgVS)), 0.0);
        irradiance *= cosW;

        indirect = irradiance;
    }

    imageStore(outSSGI, id, vec4(indirect, 1.0));
}
