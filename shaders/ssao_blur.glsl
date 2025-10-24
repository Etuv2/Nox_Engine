#version 450 core
out float FragColor;

in vec2 TexCoord;

uniform sampler2D ssaoInput;
uniform sampler2D depthInput;
uniform sampler2D gNormal;

uniform vec2 texelSize;
uniform float depthThreshold = 0.02;
uniform float normalThreshold = 0.2;

// Decode oct-encoded normal from gNormal
vec3 DecodeNormalOct8(vec2 e) {
    e = e * 2.0 - 1.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        vec2 s = vec2(sign(e.x), sign(e.y));
        n.xy = (1.0 - abs(n.yx)) * s;
    }
    return normalize(n);
}

void main() {
    float centerAO = texture(ssaoInput, TexCoord).r;
    float centerDepth = texture(depthInput, TexCoord).r;
    vec3 centerNormal = DecodeNormalOct8(texture(gNormal, TexCoord).rg);

    float result = 0.0;
    float weightSum = 0.0;

    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            vec2 offset = vec2(x, y) * texelSize;
            vec2 sampleUV = TexCoord + offset;

            float sampleAO = texture(ssaoInput, sampleUV).r;
            float sampleDepth = texture(depthInput, sampleUV).r;
            vec3 sampleNormal = DecodeNormalOct8(texture(gNormal, sampleUV).rg);

            float depthDiff = abs(centerDepth - sampleDepth);
            float normalDiff = max(0.0, 1.0 - dot(centerNormal, sampleNormal));

            float weight = exp(-depthDiff / depthThreshold) * exp(-normalDiff / normalThreshold);
            result += sampleAO * weight;
            weightSum += weight;
        }
    }

    FragColor = result / max(weightSum, 1e-5);
}
