#version 460 core

in vec2 TexCoord;
in vec4 currentPos;
in vec4 prevPos;

layout (location = 0) out vec2 velocity;

// Moving-items-only mode (static motion comes from velocity_static_frag.glsl): no depth buffer is
// used; fragments behind the visible G-buffer surface are discarded instead.
uniform bool uDepthFromGBuffer = false;
uniform sampler2D uGBufferDepth;
uniform vec2 uGBufferDepthParams;  // jittered proj [2][2], [3][2] (linearises the G-buffer depth)

void main()
{
    if (currentPos.w <= 0.0 || prevPos.w <= 0.0) {
        velocity = vec2(0.0);
        return;
    }

    if (uDepthFromGBuffer) {
        float d = texelFetch(uGBufferDepth, ivec2(gl_FragCoord.xy), 0).r;
        float visible = d >= 1.0 ? 1e30 : uGBufferDepthParams.y / min(d * 2.0 - 1.0 + uGBufferDepthParams.x, -1e-7);
        if (currentPos.w > visible * 1.01 + 0.02) discard;
    }

    // Convert to NDC space
    vec2 currentNDC = currentPos.xy / currentPos.w;
    vec2 prevNDC = prevPos.xy / prevPos.w;
    
    // Convert NDC to screen space [0,1]
    vec2 currentScreen = currentNDC * 0.5 + 0.5;
    vec2 prevScreen = prevNDC * 0.5 + 0.5;
    
    // Calculate motion vector in screen space
    vec2 motionVector = currentScreen - prevScreen;

    if (any(isnan(motionVector)) || any(isinf(motionVector))) {
        motionVector = vec2(0.0);
    }
    
    // Output velocity in UV space
    velocity = motionVector;
}