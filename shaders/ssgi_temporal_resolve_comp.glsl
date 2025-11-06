#version 460 core

// Temporal accumulation for SSGI stability
// Implements exponential moving average with reprojection and rejection heuristics

layout (local_size_x = 8, local_size_y = 8) in;

// Input textures - all at working resolution (half-res by default)
layout (binding = 0) uniform sampler2D curSSGI;     // Current frame (denoised, rgba with a=mask)
layout (binding = 1) uniform sampler2D prevSSGI;    // Previous frame SSGI
layout (binding = 2) uniform sampler2D velocityTex; // Motion vectors in UV space (full res)
layout (binding = 3) uniform sampler2D depthTex;    // Current depth (for rejection, full res)
layout (binding = 4) uniform sampler2D normalTex;   // Current normals (oct-encoded RG, full res)

// Output (working resolution)
layout (binding = 5, rgba16f) writeonly uniform image2D outSSGI;

// Uniforms
uniform float alpha;           // Base blend factor (0..1) — small values (0.1–0.2) favor history
uniform float depthThreshold;  // Depth rejection threshold
uniform float normalThreshold; // Normal rejection threshold

vec3 octDecode(vec2 e) {
    e = e * 2.0 - 1.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        vec2 s = vec2(sign(e.x), sign(e.y));
        n.xy = (1.0 - abs(n.yx)) * s;
    }
    return normalize(n);
}

float luma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

vec3 decodeNormalOct8(vec2 e) {
    vec3 n;
    n.z = 1.0 - abs(e.x) - abs(e.y);
    n.xy = n.z >= 0.0 ? e.xy : (1.0 - abs(e.yx)) * sign(e.xy);
    return normalize(n);
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = imageSize(outSSGI);

    if (any(greaterThanEqual(id, sz))) return;

    vec2 uvWork = (vec2(id) + 0.5) / vec2(sz);
    vec2 uvScreen = uvWork;

    vec4 current = texture(curSSGI, uvWork);
    vec2 velocity = texture(velocityTex, uvScreen).rg;

    vec2 prevUV = uvWork - velocity;

    bool historyValid =
        all(greaterThanEqual(prevUV, vec2(0.005))) &&
        all(lessThan(prevUV, vec2(0.995)));

    vec4 history = historyValid ? texture(prevSSGI, prevUV) : vec4(0.0);

    float confidence = 1.0;

    if (historyValid) {
        float currDepth = texture(depthTex, uvScreen).r;
        vec3 currNormal = octDecode(texture(normalTex, uvScreen).rg);

        vec2 prevScreenUV = prevUV;
        float prevDepth = texture(depthTex, prevScreenUV).r;
        vec3 prevNormal = octDecode(texture(normalTex, prevScreenUV).rg);

        float depthDiff = abs(currDepth - prevDepth);
        float depthConfidence = exp(-depthDiff / depthThreshold);
        confidence *= depthConfidence;

        float normalDot = dot(currNormal, prevNormal);
        float normalConfidence = exp(-max(0.0, 1.0 - normalDot) / normalThreshold);

        if (normalDot < (1.0 - normalThreshold * 3.0)) {
            normalConfidence = 0.0;
        }

        confidence *= normalConfidence;

        float velLen = length(velocity * vec2(sz));
        float motionConfidence = exp(-velLen * 0.04);
        confidence *= motionConfidence;
    } else {
        confidence = 0.0;
    }

    vec3 minColor = vec3(1e9);
    vec3 maxColor = vec3(-1e9);

    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            vec2 nUV = uvWork + vec2(i, j) / vec2(sz);
            nUV = clamp(nUV, vec2(0.0), vec2(1.0));
            vec3 c = texture(curSSGI, nUV).rgb;

            minColor = min(minColor, c);
            maxColor = max(maxColor, c);
        }
    }

    vec3 historyClamped = clamp(history.rgb, minColor, maxColor);

    float adaptiveAlpha = alpha;
    adaptiveAlpha = mix(0.9, adaptiveAlpha, confidence);

    float lumaCur = luma(current.rgb);
    float lumaHist = luma(historyClamped);
    float lumaChange = abs(lumaCur - lumaHist) / max(lumaCur, 0.001);

    adaptiveAlpha = mix(adaptiveAlpha, 0.8, clamp(lumaChange * 5.0, 0.0, 1.0));
    adaptiveAlpha = clamp(adaptiveAlpha, 0.05, 0.95);

    vec3 result = mix(historyClamped, current.rgb, adaptiveAlpha);
    float outMask = mix(history.a, current.a, adaptiveAlpha);

    result = max(result, vec3(0.0));
    if (any(isnan(result)) || any(isinf(result))) {
        result = current.rgb;
    }

    outMask = clamp(outMask, 0.0, 1.0);

    imageStore(outSSGI, id, vec4(result, outMask));
}
