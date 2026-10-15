"""Correctness contracts of the surfel GI shaders and host code (current design).

Checks the invariants that WP1 established: integrate reads only the ray slots generate_rays wrote, with an
ownership and frame check; requestedRays has one atomic type; debug.z has one writer; slice growth comes from
the setting; the independent reference reads no surfel state; WP2 allocation is three-phase largest remainder. Run: python scripts/validate_surfel_gi_correctness.py
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
    require('"uRequestReservation", 0u' in pipeline,
            "the host must seed requestedRays with zero: it is the sum of the requests, with no reservation")


def check_allocation_contract() -> None:
    """WP2: demand is shared out by largest remainder in three phases, with contiguous grants."""
    allocate = read("shaders/surfel_gi/allocate_rays.comp")
    generate = read("shaders/surfel_gi/generate_rays.comp")
    request = read("shaders/surfel_gi/request_rays.comp")
    pipeline = read("src/passes/SurfelGIPipeline.cpp")
    require("uPriorityPass" not in allocate and "uPriorityPass" not in pipeline,
            "the priority pass was removed by WP2; no shader or host code may use uPriorityPass")
    for phase in ("PhaseHistogram", "PhaseThreshold", "PhaseGrant"):
        require(phase in allocate, f"allocate_rays.comp must keep phase {phase}")
    require(len(re.findall(r"atomicAdd\(counters\.allocatedRays", allocate)) == 1,
            "allocatedRays must be reserved in one place (the grant phase)")
    require("uAllocPhase" in pipeline and "Dispatch(1u, 1u, 1u);" in pipeline,
            "the host must dispatch the threshold phase as one group, between histogram and grant")
    require("B_RAY_ALLOC_SCRATCH" in allocate and "B_RAY_ALLOC_SCRATCH 36" in read("shaders/surfel_gi/common.glsl"),
            "the allocation scratch must be one binding, defined once in common.glsl")
    require("SurfelGIBinding::RayAllocScratch" in pipeline,
            "the host must bind the allocation scratch buffer")
    require("#define SURFEL_REQUEST_MAX_RAYS_PER_SURFEL" not in request and
            "const uint SURFEL_REQUEST_MAX_RAYS_PER_SURFEL = 8u;" in request,
            "requests must be capped at the generated-ray count (8)")
    require("SURFEL_GENERATED_RAYS_PER_SURFEL" in generate and "request.firstRay + i" in generate,
            "generate_rays must write exactly the granted contiguous range, capped at the generated count")


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
    require(pipeline.count("rebuildGridAndAverages(true)") == 0,
            "no mid-frame grid rebuild should build cell averages; averages are built once at the end")
    require(pipeline.count("buildCellAverages();") == 2,
            "buildCellAverages is expected in its lambda definition and once at the end of Execute")


def check_reference_independence() -> None:
    reference = read("shaders/surfel_gi/reference_gi.comp")
    for forbidden in ("surfels[", "gridHeaders", "gridEntries", "B_SURFELS", "uMaxSurfels", "SurfelIsAlive"):
        require(forbidden not in reference, f"the independent reference must not use surfel state ({forbidden})")
    # Direct light at each hit and the next-event estimate now live in the shared path (surfel_scene_trace.glsl,
    # SurfelPathRadiance); the reference keeps its own path entry point and the sky miss term (plan section 14).
    require("ReferencePathIrradiance" in reference and "SurfelPathRadiance" in reference and
            "EvaluateMissEnvironmentRadiance" in reference,
            "the reference must use the path tracer (shared path), and the sky miss term")


def main() -> None:
    check_stale_slots()
    check_request_counter_atomics()
    check_allocation_contract()
    check_debug_z_single_writer()
    check_slice_growth_setting()
    check_grid_rebuilds()
    check_reference_independence()
    print("validate_surfel_gi_correctness: OK")


if __name__ == "__main__":
    main()
