#version 460 core

in vec2 TexCoord;
in vec4 currentPos;
in vec4 prevPos;

layout (location = 0) out vec2 velocity;

void main()
{
    if (currentPos.w <= 0.0 || prevPos.w <= 0.0) {
        velocity = vec2(0.0);
        return;
    }

    // Convert to NDC space
    vec2 currentNDC = currentPos.xy / currentPos.w;
    vec2 prevNDC = prevPos.xy / prevPos.w;
    
    // Convert NDC to screen space [0,1]
    vec2 currentScreen = currentNDC * 0.5 + 0.5;
    vec2 prevScreen = prevNDC * 0.5 + 0.5;
    
    // Calculate motion vector in screen space
    vec2 motionVector = currentScreen - prevScreen;

    if (any(isnan(motionVector)) || any(isinf(motionVector))) {
        motionVector = vec2(0.0);
    }
    
    // Output velocity in UV space
    velocity = motionVector;
}