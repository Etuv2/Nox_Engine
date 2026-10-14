#version 460 core

// Editor selection mask (SelectionOutlinePass). The pass draws selected/hovered mesh items with their
// own depth buffer, so only the front-most selected fragment per pixel reaches this shader. That
// fragment is "visible" when it is not behind the G-buffer depth (other geometry), else "occluded".
// Output: R = selected visible, G = selected occluded, B = hovered visible, A = hovered occluded.

uniform sampler2D uSceneDepth;   // G-buffer depth of the frame (unit TextureUnits::SELECTION_SCENE_DEPTH)
uniform vec2 uProjAB;            // jittered projection [2][2] and [3][2]; linearises depth
uniform int uRole;               // 0 = selected, 1 = hovered

out vec4 fragColor;

float LinearDepth(float z01)
{
    // Eye-space distance for a standard perspective projection (uProjAB.y is negative).
    return uProjAB.y / (z01 * 2.0 - 1.0 + uProjAB.x);
}

void main()
{
    float fragDist = LinearDepth(gl_FragCoord.z);
    float sceneZ = texelFetch(uSceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    float sceneDist = sceneZ >= 1.0 ? 1e30 : LinearDepth(sceneZ);
    // Small relative tolerance: a surface is never occluded by its own G-buffer copy.
    bool visible = fragDist <= sceneDist * 1.002 + 0.02;
    vec2 v = visible ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    fragColor = uRole == 0 ? vec4(v, 0.0, 0.0) : vec4(0.0, 0.0, v);
}
