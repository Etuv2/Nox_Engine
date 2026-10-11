#version 460 core
// Motion vectors of static geometry and sky from the G-buffer depth (camera motion only).
// Same convention as velocity_frag.glsl: uv(current, unjittered) - uv(previous, unjittered).
// Moving / skinned items are rasterised over this by velocity_vert/frag.

in vec2 TexCoord;
layout(location = 0) out vec2 velocity;

uniform sampler2D uDepth;          // G-buffer depth (rendered with the jittered projection)
uniform mat4 uInvViewProjJittered; // current jittered clip -> world
uniform mat4 uViewProj;            // current unjittered world -> clip
uniform mat4 uPrevViewProj;        // previous unjittered world -> clip
uniform mat4 uSkyReprojection;     // current unjittered clip -> previous clip, rotation only

void main() {
    vec2 uv = TexCoord;
    float depth = texture(uDepth, uv).r;
    vec2 ndc = uv * 2.0 - 1.0;
    if (depth >= 1.0) {
        vec4 prevClip = uSkyReprojection * vec4(ndc, 1.0, 1.0);
        vec2 prevUV = (prevClip.xy / max(abs(prevClip.w), 1e-6) * sign(prevClip.w)) * 0.5 + 0.5;
        velocity = uv - prevUV;
        return;
    }
    vec4 world = uInvViewProjJittered * vec4(ndc, depth * 2.0 - 1.0, 1.0);
    world /= world.w;
    vec4 cur = uViewProj * world;
    vec4 prev = uPrevViewProj * world;
    if (cur.w <= 0.0 || prev.w <= 0.0) {
        velocity = vec2(0.0);
        return;
    }
    velocity = (cur.xy / cur.w - prev.xy / prev.w) * 0.5;
}
