"""Correctness contracts of the surfel GI shaders and host code (current design).

Checks the invariants that WP1 established: integrate reads only the ray slots generate_rays wrote, with an
ownership and frame check; requestedRays has one atomic type; debug.z has one writer; slice growth comes from
the setting; the independent reference reads no surfel state. Run: python scripts/validate_surfel_gi_correctness.py
"""
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def read(relpath: str) -> str:
    return (ROOT / relpath).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def check_stale_slots() -> None:
    integrate = read("shaders/surfel_gi/integrate.comp")
    generate = read("shaders/surfel_gi/generate_rays.comp")
    trace = read("shaders/surfel_gi/trace_rays.comp")
    common = read("shaders/surfel_gi/common.glsl")

    require("const uint SURFEL_GENERATED_RAYS_PER_SURFEL = 8u;" in common,
            "the generated-ray cap must be one shared constant in common.glsl")
    require("min(min(request.allocatedCount, SURFEL_GENERATED_RAYS_PER_SURFEL), uMaxRays - request.firstRay)" in integrate,
            "integrate must bound its loop by the generated count, not by allocatedCount")
    require("min(min(request.allocatedCount, SURFEL_GENERATED_RAYS_PER_SURFEL), uMaxRays - request.firstRay)" in generate,
            "generate_rays must use the same cap as integrate")
    require("hit.ids.x != request.surfelID || hit.ids.z != i || hit.ids.w != uFrameIndex" in integrate,
            "integrate must check the hit's surfel, slot and frame before using it")
    require("atomicAdd(counters.staleRaySlotCount, 1u);" in integrate,
            "integrate must count rejected hits in staleRaySlotCount")
    require("hits[rayID].ids = uvec4(ray.ids.x, hit ? gather.hitSurfelID : SURFEL_INVALID_INDEX, ray.ids.z, uFrameIndex);"
            in trace, "trace must stamp the frame index into hits[].ids.w")
    pipeline = read("src/passes/SurfelGIPipeline.cpp")
    require('SetUniform1ui(program, "uFrameIndex", m_frameIndex);' in pipeline and
            pipeline.count('"uActiveRayCount", rayBudget);') == 1,
            "the trace pass must set uFrameIndex next to uActiveRayCount")


def check_request_counter_atomics() -> None:
    request = read("shaders/surfel_gi/request_rays.comp")
    begin = read("shaders/surfel_gi/begin_frame.comp")
    atomics = re.findall(r"atomic\w+\(counters\.requestedRays", request)
    require(atomics == ["atomicAdd(counters.requestedRays"],
            f"requestedRays must be touched only by atomicAdd, found {atomics}")
    require("counters.requestedRays = uRequestReservation;" in begin,
            "begin_frame must seed requestedRays with the reservation")
    pipeline = read("src/passes/SurfelGIPipeline.cpp")
    require('"uRequestReservation", std::max(maxSurfels / 64u, 1u)' in pipeline,
            "the host must set uRequestReservation before begin_frame")


def check_debug_z_single_writer() -> None:
    update = read("shaders/surfel_gi/update_surfels.comp")
    integrate = read("shaders/surfel_gi/integrate.comp")
    spawn = read("shaders/surfel_gi/spawn.comp")
    require(len(re.findall(r"surfel\.debug\.z\s*=", update)) == 1,
            "update_surfels.comp must be the only writer of debug.z (target radius error)")
    require(re.search(r"surfel\.debug\.z\s*=", integrate) is None,
            "integrate.comp must not write debug.z (it stores allocation in debug.y)")
    require(re.search(r"surfel\.debug\.z\s*=", spawn) is None, "spawn.comp must not write debug.z")


def check_slice_growth_setting() -> None:
    common = read("shaders/surfel_gi/common.glsl")
    pipeline = read("src/passes/SurfelGIPipeline.cpp")
    require("uniform float uSurfelGridSliceGrowth" in common and "return uSurfelGridSliceGrowth;" in common,
            "slice growth must read the uniform, not a literal")
    require('SetUniform1f(program, "uSurfelGridSliceGrowth", gridSettings.sliceGrowth);' in pipeline,
            "the host must set slice growth from the grid settings")


def check_grid_rebuilds() -> None:
    pipeline = read("src/passes/SurfelGIPipeline.cpp")
    require(pipeline.count("rebuildGridAndAverages(") == 0 and pipeline.count("buildCellAverages") == 0,
            "the grid is built once per frame; the mid-frame rebuilds and the separate cell-average pass are gone")
    require(pipeline.count("buildGrid();") == 1,
            "buildGrid is expected once, at the end of Execute (count, block sums, scan and scatter)")
    for label in ("grid_count", "grid_scan", "grid_build"):
        require(f'timedDispatch("{label}"' in pipeline, f"grid pass {label} must be timed")


def check_reference_independence() -> None:
    reference = read("shaders/surfel_gi/reference_gi.comp")
    for forbidden in ("surfels[", "gridHeaders", "gridEntries", "B_SURFELS", "uMaxSurfels", "SurfelIsAlive"):
        require(forbidden not in reference, f"the independent reference must not use surfel state ({forbidden})")
    require("ReferencePathIrradiance" in reference and "EvaluateHitDirectIrradiance" in reference and
            "EvaluateMissEnvironmentRadiance" in reference,
            "the reference must use the path tracer, direct light at each hit, and the sky miss term")


def main() -> None:
    check_stale_slots()
    check_request_counter_atomics()
    check_debug_z_single_writer()
    check_slice_growth_setting()
    check_grid_rebuilds()
    check_reference_independence()
    print("validate_surfel_gi_correctness: OK")


if __name__ == "__main__":
    main()
