#version 460 core
// Dual-filter (Bjorge 2015, "dual Kawase") 2:1 downsample for the bloom chain: centre x4 plus four
// bilinear taps one source texel away on the diagonals, so every source texel contributes and small
// bright sources do not alias (shimmer) as they move.
in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D image;

void main()
{
    vec2 t = 1.0 / vec2(textureSize(image, 0)); // source texel
    vec3 sum = texture(image, TexCoord).rgb * 4.0;
    sum += texture(image, TexCoord + vec2(-t.x, -t.y)).rgb;
    sum += texture(image, TexCoord + vec2( t.x, -t.y)).rgb;
    sum += texture(image, TexCoord + vec2(-t.x,  t.y)).rgb;
    sum += texture(image, TexCoord + vec2( t.x,  t.y)).rgb;
    FragColor = vec4(sum * 0.125, 1.0);
}
