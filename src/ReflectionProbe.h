#pragma once
#include <glm/glm.hpp>

/*
* Local reflection probe (docs/nfs2015/README.md, M5): a cubemap captured at `position` and used,
* parallax corrected against its box, for pixels inside [boxMin, boxMax]. Weights fade to zero over
* `blendDistance` metres inside the box border so neighbouring probes cross-fade.
* NFS (2015) used manually placed, parallax-corrected cubemaps (128x128 faces, 41 resident) with SSR
* on top (Fredriksson & Scott 2016); NOX captures static probes once per scene load.
*/
struct ReflectionProbeDesc {
	glm::vec3 position{ 0.0f };
	glm::vec3 boxMin{ -10.0f };
	glm::vec3 boxMax{ 10.0f };
	float blendDistance = 2.0f;
	float intensity = 1.0f;
};
