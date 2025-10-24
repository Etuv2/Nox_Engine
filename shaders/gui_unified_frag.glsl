#version 450 core
in vec2 TexCoord;
in vec2 FragPos;
out vec4 FragColor;

// Render mode flags
uniform int renderMode; // 0=solid, 1=gradient, 2=bevel
uniform int gradientType; // 0=horizontal, 1=vertical, 2=radial

// Common properties
uniform vec4 baseColor;
uniform vec2 rectSize; // Width and height of the rectangle

// Gradient properties
uniform vec4 startColor;
uniform vec4 endColor;
uniform vec2 gradientCenter;
uniform float gradientRadius;

// Bevel properties
uniform vec4 highlightColor;
uniform vec4 shadowColor;
uniform float cornerRadius;
uniform float bevelSize;

// SDF for rounded rectangles
float roundedBoxSDF(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

vec4 calculateGradient() {
    float t = 0.0;
    
    if (gradientType == 0) {
        // Linear horizontal
        t = TexCoord.x;
    } else if (gradientType == 1) {
        // Linear vertical
        t = TexCoord.y;
    } else if (gradientType == 2) {
        // Radial
        vec2 dist = TexCoord - gradientCenter;
        float len = length(dist);
        t = clamp(len / gradientRadius, 0.0, 1.0);
    }
    
    return mix(startColor, endColor, t);
}

vec4 calculateBevel() {
    // Convert UV to centered coordinates
    vec2 uv = TexCoord * 2.0 - 1.0;
    vec2 size = rectSize * 0.5;
    
    // Calculate SDF for rounded rectangle
    float sdf = roundedBoxSDF(uv * size, size - cornerRadius, cornerRadius);
    
    // Base color
    vec4 color = baseColor;
    
    // Add bevel effect
    vec2 bevelUV = TexCoord;
    float bevelFactor = bevelSize / min(rectSize.x, rectSize.y);
    
    // Top and left edges (highlight)
    float topEdge = smoothstep(0.0, bevelFactor, 1.0 - bevelUV.y);
    float leftEdge = smoothstep(0.0, bevelFactor, bevelUV.x);
    float highlight = max(topEdge, leftEdge);
    
    // Bottom and right edges (shadow)
    float bottomEdge = smoothstep(0.0, bevelFactor, bevelUV.y);
    float rightEdge = smoothstep(0.0, bevelFactor, 1.0 - bevelUV.x);
    float shadow = max(bottomEdge, rightEdge);
    
    // Apply bevel lighting
    color = mix(color, highlightColor, highlight * highlightColor.a);
    color = mix(color, shadowColor, shadow * shadowColor.a);
    
    // Apply rounded corners
    if (cornerRadius > 0.0) {
        float alpha = 1.0 - smoothstep(-1.0, 0.0, sdf);
        color.a *= alpha;
    }
    
    return color;
}

void main() {
    vec4 finalColor;
    
    if (renderMode == 0) {
        // Solid color
        finalColor = baseColor;
    } else if (renderMode == 1) {
        // Gradient
        finalColor = calculateGradient();
    } else if (renderMode == 2) {
        // Bevel
        finalColor = calculateBevel();
    } else {
        // Fallback
        finalColor = baseColor;
    }
    
    FragColor = finalColor;
}