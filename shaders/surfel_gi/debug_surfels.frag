#version 460 core

in vec4 vColor;
in vec4 vStyle;
in vec4 vDisk0;
in vec4 vDisk1;
in vec4 vDisk2;
in vec2 vDiskUV;
out vec4 FragColor;

uniform mat4 uProjection;
uniform sampler2D uSceneDepthTex;
uniform int uUseDepthReject = 1;
uniform float uDepthRejectBias = 0.0015;

void main()
{
    vec2 p = vDiskUV;
    vec3 normalView = normalize(vDisk1.xyz);
    float r2 = dot(p, p);
    if (r2 > 1.0) {
        discard;
    }

    if (uUseDepthReject != 0) {
        vec2 uv = gl_FragCoord.xy / max(vDisk2.zw, vec2(1.0));
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
            discard;
        }

        float sceneDepth = texture(uSceneDepthTex, uv).r;
        float surfelDepth = gl_FragCoord.z;
        if (sceneDepth > 0.0 && sceneDepth < 0.999999 &&
            surfelDepth > sceneDepth + uDepthRejectBias) {
            discard;
        }
    }

    if (vStyle.w > 0.5) {
        float softEdge = 1.0 - smoothstep(0.82, 1.0, r2);
        float paperAlpha = vColor.a * softEdge * 0.96;
        float facing = abs(dot(normalView, vec3(0.0, 0.0, 1.0)));
        vec3 color = vColor.rgb * mix(0.86, 1.04, facing);
        color = mix(color, color * 1.08, 1.0 - smoothstep(0.0, 0.72, r2));
        FragColor = vec4(color, paperAlpha);
        return;
    }

    float edge = 1.0 - smoothstep(0.65, 1.0, r2);
    float rim = smoothstep(0.48, 0.82, r2) * (1.0 - smoothstep(0.76, 1.0, r2));
    float centerDot = 1.0 - smoothstep(0.0, 0.08, r2);
    float facing = abs(dot(normalView, vec3(0.0, 0.0, 1.0)));
    float recycledCross = vStyle.y * max(
        1.0 - smoothstep(0.0, 0.12, abs(p.x - p.y)),
        1.0 - smoothstep(0.0, 0.12, abs(p.x + p.y))
    );
    vec3 color = mix(vColor.rgb, vColor.rgb * 0.35, smoothstep(0.72, 1.0, r2) * 0.35);
    color = mix(color, vec3(1.0), rim * max(vStyle.x, 0.18) * 0.55);
    color = mix(color, vec3(1.0), centerDot * 0.35);
    color *= mix(0.72, 1.0, facing);
    color = mix(color, vec3(1.0, 0.18, 0.08), recycledCross * 0.85);
    FragColor = vec4(color, vColor.a * edge);
}
