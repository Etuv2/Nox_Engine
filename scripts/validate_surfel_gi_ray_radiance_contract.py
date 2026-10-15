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
    shared = read("shaders/surfel_gi/surfel_scene_trace.glsl")
    integrate = read("shaders/surfel_gi/integrate.comp")

    require("bool SurfelSelectGuideBin(" in generate,
            "ray guiding must choose a learned positive-radiance guide bin")
    require("if (!SurfelSelectGuideBin(surfelID, selectionTarget, totalWeight, selectedBin, selectedWeight))" in generate,
            "guided sampling must validate selection after the guide-bin search")
    require("!(selectedWeight > 0.0)" not in generate,
            "guided sampling must not reject before assigning selectedWeight")

    # Plan section 14: one sky estimator. A hit shades through the shared path; without a coplanar cache the path
    # continues, and the sky enters only at a miss (EvaluateMissEnvironmentRadiance, weighted by the throughput).
    require("ShadeHitRadiance(" in trace and "SurfelPathRadiance(" in trace and
            "TraceCachedIndirectAtSurface(" in trace,
            "hit radiance must come from the shared path, with the surfel cache answering where it has coplanar surfels")
    require("return radiance + throughput * EvaluateMissEnvironmentRadiance(direction);" in shared,
            "the sky must enter the path at a miss, weighted by the path throughput")
    require("uSkyRadiance * max(rayDir.y, 0.0)" not in trace,
            "ray misses must not become simple horizon-weighted ambient irradiance samples")
    require("EvaluateMissEnvironmentRadiance" in shared and
            "uSkyEnvironmentMap" in shared and
            "SURFEL_TRACE_HIT_KIND_ENVIRONMENT" in trace,
            "unoccluded ray misses must accumulate directional skybox environment radiance, not be discarded or filled with flat ambient")
    require("albedo * SURFEL_INV_PI * (direct + cached)" in shared and
            "albedo * SURFEL_INV_PI * (direct + nee)" in shared,
            "hit radiance must include direct diffuse, emitter next-event light and cached surfel indirect at the hit")
    require("emitterIrradiance" in integrate and "SurfelRelativeClamp(sampleIrradiance, sampleBoundLum)" in integrate,
            "the next-event estimate must join the ray samples, bounded by the relative clamp")
    require("gather.radiance = vec3(0.0);" in trace,
            "invalid misses and nearest-cluster resets must still write explicit zero radiance")

    require("if (hit.normal_hitKind.w <= 0.5) {" in integrate,
            "temporal integration must accept real hit samples only")
    require("SurfelLuminance(max(hit.radiance_pdf.rgb, vec3(0.0))) > 1e-5" not in integrate,
            "temporal integration must not treat bright misses as valid samples")


if __name__ == "__main__":
    main()
