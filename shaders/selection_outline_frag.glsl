#version 460 core

// Editor selection outline (SelectionOutlinePass): a disc dilation of the mask from
// selection_mask_frag.glsl. Pixels inside the silhouette are left alone; pixels outside it within the
// outline width take the colour. Visible outlines win over occluded ones at the same pixel.

uniform sampler2D uMask;
uniform vec3 uSelectedColor;
uniform float uSelectedWidth;     // pixels, 1..4
uniform vec3 uHoverColor;
uniform float uHoverWidth;        // pixels
uniform float uOccludedAlpha;     // 0 hides occluded outlines
uniform bool uOccludedDashed;

out vec4 fragColor;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 size = textureSize(uMask, 0);
    vec4 centre = texelFetch(uMask, p, 0);
    if (centre.r + centre.g > 0.5) discard;   // inside the selection

    float selR2 = uSelectedWidth * uSelectedWidth + 0.25;
    float hovR2 = uHoverWidth * uHoverWidth + 0.25;
    int reach = int(ceil(max(uSelectedWidth, uHoverWidth)));

    float selVisible = 0.0;
    float selOccluded = 0.0;
    float hovVisible = 0.0;
    for (int dy = -reach; dy <= reach; ++dy) {
        for (int dx = -reach; dx <= reach; ++dx) {
            float d2 = float(dx * dx + dy * dy);
            if (d2 > max(selR2, hovR2)) continue;
            ivec2 q = clamp(p + ivec2(dx, dy), ivec2(0), size - ivec2(1));
            vec4 m = texelFetch(uMask, q, 0);
            if (d2 <= selR2) {
                selVisible = max(selVisible, m.r);
                selOccluded = max(selOccluded, m.g);
            }
            if (d2 <= hovR2) {
                hovVisible = max(hovVisible, m.b);
            }
        }
    }

    bool dash = !uOccludedDashed || fract((gl_FragCoord.x + gl_FragCoord.y) / 8.0) < 0.5;
    if (selVisible > 0.5) {
        fragColor = vec4(uSelectedColor, 1.0);
    } else if (selOccluded > 0.5 && uOccludedAlpha > 0.0 && dash) {
        fragColor = vec4(uSelectedColor, uOccludedAlpha);
    } else if (hovVisible > 0.5 && centre.b + centre.a < 0.5) {
        fragColor = vec4(uHoverColor, 0.6);
    } else {
        discard;
    }
}
