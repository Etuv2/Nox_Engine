#version 460 core

// Temporal accumulation for SSGI stability
// Blends current frame with history using exponential moving average

layout (local_size_x = 8, local_size_y = 8) in;

// Input textures
layout (binding = 0) uniform sampler2D curSSGI;   // Current frame (denoised)
layout (binding = 1) uniform sampler2D prevSSGI;  // Previous frame SSGI

// Output texture - FIXED: use rgba16f instead of rgb16f
layout (binding = 2, rgba16f) writeonly uniform image2D outSSGI;

// Uniforms
uniform float alpha; // Blend factor (0..1, smaller = more history)

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    vec2 texel = vec2(id) / vec2(imageSize(outSSGI));
    
    // Sample current and previous frames
    vec3 current = texture(curSSGI, texel).rgb;
    vec3 history = texture(prevSSGI, texel).rgb;
    
    // Exponential moving average
    // result = lerp(current, history, alpha)
    // Lower alpha = more temporal stability (more ghosting)
    // Higher alpha = less ghosting (more noise)
    vec3 result = mix(current, history, alpha);
    
    imageStore(outSSGI, id, vec4(result, 1.0));
}
