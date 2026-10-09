#version 460 core
// Volumetric fog step 3: composite over the lit HDR image (before TAA).
// Blending: dst' = inScatter + dst * transmittance  (glBlendFunc(GL_ONE, GL_SRC_ALPHA)).
// The froxel grid stops at uVolumeFar; beyond it fog continues analytically with the volume's
// last-slice extinction so distant geometry and the sky still recede into haze.

#include "includes/volumetric_common.glsl"
#include "includes/exposure_common.glsl"

in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler3D uIntegrated;
uniform sampler2D uDepth;
uniform mat4 uProj;
uniform float uDistantExtinction; // 1/m beyond the volume
uniform vec3 uDistantInScatter;   // scene-referred radiance of the distant haze

void main() {
	float d = texture(uDepth, TexCoord).r;
	// glm::perspective: view z = -B / (ndcZ + A) (negative in front of the camera); depth = -z.
	float viewDepth = d >= 1.0 ? 1e6 : uProj[3][2] / ((d * 2.0 - 1.0) + uProj[2][2]);
	float w = VolumeDepthToSlice(viewDepth);
	// Integrated texel z stores the value at its slice's far boundary ((z + 1) / N).
	float texZ = min(w, 1.0) - 0.5 / float(uVolumeSize.z);
	vec4 fog = texture(uIntegrated, vec3(TexCoord, clamp(texZ, 0.0, 1.0)));
	if (viewDepth > uVolumeFar) {
		float extra = min(viewDepth, 2500.0) - uVolumeFar;
		float tail = exp(-uDistantExtinction * extra);
		fog.rgb += fog.a * uDistantInScatter * NoxPreExposure() * (1.0 - tail);
		fog.a *= tail;
	}
	FragColor = vec4(fog.rgb, fog.a);
}
