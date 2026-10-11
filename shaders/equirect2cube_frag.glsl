#version 460 core
out vec4 FragColor;
in vec3 localPos;
uniform sampler2D equirectangularMap;
const vec2 invAtan = vec2(0.1591, 0.3183);
vec2 sampleSphericalMap(vec3 v)
{
    // Compute UV coordinates for the equirectangular map
    vec2 uv;
    uv.x = atan(v.z, v.x) * invAtan.x + 0.5;
    // Rows are uploaded top-first (stb, no flip), so v = 0 is the image's top (zenith).
    uv.y = 0.5 - asin(v.y) * invAtan.y;
    return uv;
}
void main()
{
    vec3 n = normalize(localPos);
    vec2 uv = sampleSphericalMap(n);
    vec3 color = texture(equirectangularMap, uv).rgb;
    FragColor = vec4(color, 1.0);
}
