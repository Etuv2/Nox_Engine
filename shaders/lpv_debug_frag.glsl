#version 460 core

// Debug visualization fragment shader for the LPV grid.
// lpv_debug_vert.glsl emits one point per sampled voxel and already resolves its color
// (energy, occlusion or empty) and energy-scaled point size; this stage only shapes the sprite.

in vec4 v_color;
out vec4 FragColor;

void main() {
    // Round sprites instead of squares so dense grids stay readable.
    vec2 fromCenter = gl_PointCoord * 2.0 - 1.0;
    if (dot(fromCenter, fromCenter) > 1.0) {
        discard;
    }
    FragColor = v_color;
}
