#version 460 core

in vec2 vDiscUV;
in vec4 vColor;
in vec3 vWorldPos;
in vec3 vWorldNormal;

out vec4 FragColor;

uniform sampler2D uDepthTex;
uniform vec2 uScreenSize;
uniform int uDebugMode;
uniform vec3 uCameraPos;

void main()
{
    float d = dot(vDiscUV, vDiscUV);
    if (d > 1.0) {
        discard;
    }

    vec2 uv = gl_FragCoord.xy / max(uScreenSize, vec2(1.0));
    float sceneDepth = texture(uDepthTex, uv).r;
    if (sceneDepth >= 0.9999 || gl_FragCoord.z > sceneDepth + 0.0025) {
        discard;
    }

    float edge = smoothstep(1.0, 0.74, d);
    float rim = smoothstep(0.92, 1.0, d);
    float center = 1.0 - smoothstep(0.0, 0.72, d);
    float normalFacing = clamp(abs(dot(normalize(vWorldNormal), normalize(uCameraPos - vWorldPos))) * 0.35 + 0.65, 0.0, 1.0);
    vec3 color = vColor.rgb * normalFacing;

    if (uDebugMode == 1) {
        color = mix(color, vec3(1.0), center * 0.12);
        color = mix(color, color * 0.55, rim * 0.35);
    }

    FragColor = vec4(color, vColor.a * edge);
}
