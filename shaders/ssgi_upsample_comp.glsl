#version 450 core
layout(local_size_x=8, local_size_y=8) in;

layout(binding=0, rgba16f) writeonly uniform image2D dst; // full-res output
uniform sampler2D lowRes;  // half-res input (use sampler for automatic filtering)
uniform sampler2D gDepth;  // full-res guidance
uniform sampler2D gNormal; // full-res guidance
uniform vec2 fullSize;     // full-resolution dimensions

vec3 decodeOct(vec2 e){ 
    vec3 n; 
    n.z = 1.0 - abs(e.x) - abs(e.y); 
    n.xy = n.z >= 0.0 ? e : (1.0 - abs(e.yx)) * sign(e.xy); 
    return normalize(n); 
}

void main(){
    ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
    if (gid.x >= int(fullSize.x) || gid.y >= int(fullSize.y)) return;
    
    // Use normalized UVs for sampling
    // Both full-res and half-res textures share the same [0,1] UV space
    vec2 uv = (vec2(gid) + 0.5) / fullSize;
    
    // Sample center from half-res using normalized UVs (no coordinate scaling needed)
    vec3 center = texture(lowRes, uv).rgb;

    // Get full-res guidance data at current pixel
    float centerDepth = texture(gDepth, uv).r;
    vec3  centerN = decodeOct(texture(gNormal, uv).rg);

    // Bilateral upsample using 3x3 neighborhood in half-res texture
    vec3 sum = vec3(0); 
    float wsum = 0.0;
    
    // Sample step size in UV space (covers neighborhood in half-res texture)
    vec2 halfResSize = textureSize(lowRes, 0);
    vec2 texelSize = 1.0 / halfResSize;
    
    // 3x3 bilateral filter
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            // Offset in half-res UV space
            vec2 offset = vec2(x, y) * texelSize;
            vec2 sampleUV = uv + offset;
            
            // Sample GI from half-res
            vec3 c = texture(lowRes, sampleUV).rgb;
            
            // Sample guidance from full-res (using same UV - they share UV space)
            float d = texture(gDepth, sampleUV).r;
            vec3  n = decodeOct(texture(gNormal, sampleUV).rg);
            
            // Bilateral weights based on depth and normal similarity
            float depthWeight = exp(-abs(d - centerDepth) * 200.0);
            float normalWeight = pow(max(dot(n, centerN), 0.0), 16.0);
            float w = depthWeight * normalWeight;
            
            sum += c * w; 
            wsum += w;
        }
    }
    
    // Output weighted average, fallback to center if weights are too small
    vec3 outc = (wsum > 1e-5) ? (sum / wsum) : center;
    imageStore(dst, gid, vec4(outc, 1.0));
}
