#version 460 core

// Screen-space ray marching for SSGI
// Traces rays in cosine-weighted hemisphere to find indirect lighting

layout (local_size_x =8, local_size_y =8) in;

// Input textures
layout (binding =0) uniform sampler2D depthTex; // Scene depth
layout (binding =1) uniform sampler2D normalTex; // G-buffer normals (oct-encoded in RG)
layout (binding =2) uniform sampler2D albedoTex; // Surface albedo
layout (binding =3) uniform sampler2D randTex; // Random values from directions pass
layout (binding =4) uniform sampler2D prevColor; // Previous frame color (for sampling hits)

// Output texture - FIXED: use rgba16f instead of rgb16f (RGB not supported for images)
layout (binding =5, rgba16f) writeonly uniform image2D outSSGI;

// Uniforms
uniform mat4 invProj; // Inverse projection matrix
uniform mat4 invView; // Inverse view matrix (world from view)
uniform mat4 view; // View matrix (view from world)
uniform mat4 proj; // Projection matrix
uniform float maxRayLenVS; // Maximum ray length in view space
uniform int numSteps; // Number of ray marching steps
uniform float thickness; // Thickness parameter for intersection
uniform vec2 screenSize; // Full resolution screen size
uniform vec2 workSize; // Working resolution (may be half-res)
uniform float cameraNear; // Camera near plane
uniform float cameraFar; // Camera far plane

// Linearize hardware depth to view-space Z (>0) then we negate to get right-handed view space (-Z forward)
float LinearizeDepth(float depth, float nearP, float farP) {
 float z = depth *2.0 -1.0;
 return (2.0 * nearP * farP) / (farP + nearP - z * (farP - nearP));
}

// Octahedral decode with proper fold
vec3 octDecode(vec2 e) {
 vec2 f = e *2.0 -1.0;
 vec3 n = vec3(f.x, f.y,1.0 - abs(f.x) - abs(f.y));
 float t = clamp(-n.z,0.0,1.0);
 n.x += (n.x >=0.0 ? -t : t);
 n.y += (n.y >=0.0 ? -t : t);
 return normalize(n);
}

// Reconstruct view-space position from UV and depth
vec3 reprojToView(vec2 uv, float depthRaw) {
 vec4 ndc = vec4(uv *2.0 -1.0, depthRaw *2.0 -1.0,1.0);
 vec4 vpos = invProj * ndc;
 vpos /= vpos.w;
 return vpos.xyz;
}

// Project view-space position to screen UV and return uv
vec2 projectToUV(vec3 vpos) {
 vec4 clip = proj * vec4(vpos,1.0);
 vec3 ndc = clip.xyz / clip.w;
 return ndc.xy *0.5 +0.5;
}

float depthFromVS(vec3 vpos) {
 vec4 clip = proj * vec4(vpos,1.0);
 float ndcZ = clip.z / clip.w; // [-1,1]
 return ndcZ *0.5 +0.5; // [0,1]
}

void main() {
 ivec2 id = ivec2(gl_GlobalInvocationID.xy);
 ivec2 dstSize = imageSize(outSSGI);
 // Early exit for out-of-bounds threads
 if (any(greaterThanEqual(id, dstSize))) return;
 
 vec2 uvWork = (vec2(id) +0.5) / workSize; // normalized in working resolution
 vec2 uvFull = uvWork; // normalized UVs are resolution independent

 // Sample depth - skip sky pixels
 float depthRaw = texture(depthTex, uvFull).r;
 if (depthRaw >=1.0) {
 imageStore(outSSGI, id, vec4(0.0));
 return;
 }

 // Reconstruct normal in view space (G-buffer stores oct-encoded normals in world space)
 vec2 enc = textureLod(normalTex, uvFull,0.0).rg;
 vec3 nWorld = octDecode(enc);
 vec3 nVS = normalize(mat3(view) * nWorld);

 // Get random values for cosine-weighted hemisphere sampling
 vec2 rands = texture(randTex, uvWork).rg;
 
 // Build tangent basis around normal in view space
 vec3 bitangent = normalize(abs(nVS.z) <0.999 ? 
 vec3(-nVS.y, nVS.x,0.0) : 
 vec3(0.0,1.0,0.0));
 vec3 tangent = normalize(cross(bitangent, nVS));
 
 // Generate cosine-weighted direction
 float r = sqrt(rands.x);
 float phi =6.2831853 * rands.y;

 // Create a jittered ray direction to break up banding artifacts
 vec3 jitter = vec3(rands.x -0.5, rands.y -0.5,0.0) *0.1;
 vec3 hemisphereDir = normalize(
 tangent * (r * cos(phi)) + 
 bitangent * (r * sin(phi)) + 
 nVS * sqrt(max(0.0,1.0 - r * r))
 );
 vec3 dirVS = normalize(hemisphereDir + jitter);

 // Ray origin in view space
 float viewZ = -LinearizeDepth(depthRaw, cameraNear, cameraFar);
 vec4 ndc = vec4(uvFull *2.0 -1.0, depthRaw *2.0 -1.0,1.0);
 vec4 vposH = invProj * ndc;
 vec3 orgVS = vposH.xyz / vposH.w;

 // Ray march in view space
 float stepLen = maxRayLenVS / float(max(numSteps,1));
 vec3 marchVS = orgVS;
 vec3 hitVS = vec3(0.0);
 bool hit = false;

 for (int i =0; i < numSteps; i++) {
 marchVS += dirVS * stepLen;
 
 // Project to screen space
 vec2 uv = projectToUV(marchVS);
 
 // Check if ray is off-screen
 if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) break;

 // Sample scene depth at this position
 float dRaw = texture(depthTex, uv).r;
 if (dRaw >=1.0) continue;

 // Linearized hit depth (negative Z forward in view space)
 float hitZ = -LinearizeDepth(dRaw, cameraNear, cameraFar);

 // Distance-aware thickness for stability
 float eps = max(thickness,0.001 * abs(marchVS.z));

 // If our ray is behind the scene surface in view space, we've crossed the surface
 bool intersect = (hitZ > marchVS.z - eps); // flip to '<' if +Z forward is used
 if (intersect) {
 hit = true;
 // Binary search refinement between previous and current march positions
 vec3 a = marchVS - dirVS * stepLen;
 vec3 b = marchVS;
 for (int j =0; j <4; ++j) {
 vec3 m =0.5 * (a + b);
 vec2 mUV = projectToUV(m);
 float mRaw = texture(depthTex, mUV).r;
 float mZ = -LinearizeDepth(mRaw, cameraNear, cameraFar);
 if (mZ > m.z) {
 b = m; // still behind -> move closer to camera
 } else {
 a = m; // in front -> move deeper
 }
 }
 hitVS = b; // final position just behind the surface
 break;
 }
 }

 // Sample indirect lighting if we hit something
 vec3 indirect = vec3(0.0);
 if (hit) {
 vec2 hitUV = projectToUV(hitVS);
 // Sample previous frame color at hit location (HDR pre-tonemap)
 vec3 srcColor = texture(prevColor, hitUV).rgb;

 // Store pre-albedo indirect lighting (albedo applied in lighting pass)
 vec3 irradiance = srcColor;

 // Distance-based attenuation to stabilize and localize contribution
 float dist = length(hitVS - orgVS);
 float attenuation =1.0 - smoothstep(0.0, maxRayLenVS, dist);
 irradiance *= attenuation * attenuation;

 // Cosine weighting with receiver normal
 float cosW = max(dot(nVS, normalize(hitVS - orgVS)),0.0);
 irradiance *= cosW;

 indirect = irradiance;
 }

 imageStore(outSSGI, id, vec4(indirect,1.0));
}
