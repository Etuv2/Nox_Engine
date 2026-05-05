from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relpath: str) -> str:
    return (ROOT / relpath).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    generate = read("shaders/surfel_gi/generate_rays.comp")
    trace = read("shaders/surfel_gi/trace_rays.comp")
    integrate = read("shaders/surfel_gi/integrate.comp")

    require("bool SurfelSelectGuideBin(" in generate,
            "ray guiding must choose a learned positive-radiance guide bin")
    require("if (!SurfelSelectGuideBin(surfelID, selectionTarget, totalWeight, selectedBin, selectedWeight))" in generate,
            "guided sampling must validate selection after the guide-bin search")
    require("!(selectedWeight > 0.0)" not in generate,
            "guided sampling must not reject before assigning selectedWeight")

    require("EvaluateHitEnvironmentIrradiance" in trace and
            "TraceSkyVisibilityAtSurface" in trace and
            "SkyOcclusionVisibilityFromHitDistance" in trace and
            "uSkyIrradianceMap" in trace and
            "TraceSoftwareBVH(origin, skyDir" in trace and
            "return hemisphereAccess * SkyOcclusionVisibilityFromHitDistance(skyHit.t);" in trace,
            "ray-hit environment lighting must come from skybox irradiance with distance-weighted BVH sky visibility")
    require("uSkyRadiance * max(rayDir.y, 0.0)" not in trace,
            "ray misses must not become simple horizon-weighted ambient irradiance samples")
    require("EvaluateMissEnvironmentRadiance" in trace and
            "uSkyEnvironmentMap" in trace and
            "SURFEL_TRACE_HIT_KIND_ENVIRONMENT" in trace,
            "unoccluded ray misses must accumulate directional skybox environment radiance, not be discarded or filled with flat ambient")
    require("directIrradiance + environmentIrradiance + cachedIndirectIrradiance" in trace,
            "hit radiance must include direct diffuse, visible skybox irradiance, and cached surfel indirect at the hit")
    require("emissiveRadiance" in trace and "emissiveSurfelHit" in trace,
            "surfel fallback hits must preserve emissive radiance")
    require("vec3 environmentIrradiance = EvaluateHitEnvironmentIrradiance(surfelPos, surfelNormal);" in trace and
            "directIrradiance + environmentIrradiance + cachedIndirectIrradiance" in trace,
            "surfel fallback hits must gather the same direct, environment, and cached indirect terms as BVH/screen hits")
    require("gather.radiance = vec3(0.0);" in trace,
            "invalid misses and nearest-cluster resets must still write explicit zero radiance")

    require("bool validSample = hit.normal_hitKind.w > 0.5;" in integrate,
            "temporal integration must accept real hit samples only")
    require("SurfelLuminance(max(hit.radiance_pdf.rgb, vec3(0.0))) > 1e-5" not in integrate,
            "temporal integration must not treat bright misses as valid samples")


if __name__ == "__main__":
    main()
