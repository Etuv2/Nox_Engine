#version 460 core

in vec4 vColor;
in vec4 vStyle;
in vec4 vDisk0;
in vec4 vDisk1;
in vec4 vDisk2;
in float vClipDepth01;
out vec4 FragColor;

uniform mat4 uProjection;
uniform sampler2D uSceneDepthTex;
uniform int uUseDepthReject = 1;
uniform float uDepthRejectBias = 0.0015;

void main()
{
    vec2 p = gl_PointCoord * 2.0 - 1.0;
    vec3 centerView = vDisk0.xyz;
    float diskRadius = max(vDisk0.w, 0.001);
    vec3 normalView = normalize(vDisk1.xyz);
    float pointRadiusPx = max(vDisk1.w, 1.0);
    vec2 centerNdc = vDisk2.xy;
    vec2 viewportSize = max(vDisk2.zw, vec2(1.0));
    vec2 fragNdc = centerNdc + (p * pointRadiusPx * 2.0) / viewportSize;

    vec3 viewRay = normalize(vec3(fragNdc.x / uProjection[0][0], fragNdc.y / uProjection[1][1], -1.0));
    float denom = dot(normalView, viewRay);
    float planeT = abs(denom) > 1e-4 ? dot(normalView, centerView) / denom : -1.0;
    vec3 diskDelta = viewRay * planeT - centerView;
    float diskR2 = dot(diskDelta, diskDelta) / (diskRadius * diskRadius);

    float spriteR2 = dot(p, p);
    bool validDisk = planeT > 0.0 && !isnan(diskR2) && !isinf(diskR2);
    float r2 = validDisk ? diskR2 : spriteR2;
    if (r2 > 1.0 || spriteR2 > 1.0) {
        discard;
    }

    if (uUseDepthReject != 0) {
        vec2 uv = fragNdc * 0.5 + 0.5;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
            discard;
        }

        float sceneDepth = texture(uSceneDepthTex, uv).r;
        float surfelDepth = vClipDepth01;
        if (validDisk) {
            vec4 fragClip = uProjection * vec4(viewRay * planeT, 1.0);
            surfelDepth = fragClip.z / max(abs(fragClip.w), 1e-6) * 0.5 + 0.5;
        }

        if (sceneDepth > 0.0 && sceneDepth < 0.999999 &&
            surfelDepth > sceneDepth + uDepthRejectBias) {
            discard;
        }
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
