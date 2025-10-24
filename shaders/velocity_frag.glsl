#version 330 core

in vec2 TexCoord;
in vec4 currentPos;
in vec4 prevPos;

layout (location = 0) out vec2 velocity;

uniform vec2 screenSize;

void main()
{
    // Convert to NDC space
    vec2 currentNDC = currentPos.xy / currentPos.w;
    vec2 prevNDC = prevPos.xy / prevPos.w;
    
    // Convert NDC to screen space [0,1]
    vec2 currentScreen = currentNDC * 0.5 + 0.5;
    vec2 prevScreen = prevNDC * 0.5 + 0.5;
    
    // Calculate motion vector in screen space
    vec2 motionVector = currentScreen - prevScreen;
    
    // Apply dilation for better coverage at object edges
    float motionLength = length(motionVector * screenSize);
    if (motionLength > 0.1) {
        // Normalize and extend the motion vector slightly
        vec2 motionDir = normalize(motionVector);
        float dilation = 1.2; // Extend by 20%
        motionVector = motionDir * (motionLength * dilation) / screenSize;
    }
    
    // Output velocity in UV space
    velocity = motionVector;
}