from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relpath: str) -> str:
    return (ROOT / relpath).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    common = read("shaders/surfel_gi/common.glsl")
    request = read("shaders/surfel_gi/request_rays.comp")
    allocate = read("shaders/surfel_gi/allocate_rays.comp")
    integrate = read("shaders/surfel_gi/integrate.comp")
    pipeline = read("src/passes/SurfelGIPipeline.cpp")

    require("SurfelSpawnTileStratum" in common and
            "SURFEL_SPAWN_TILE_STRATA_X" in common and
            "SURFEL_SPAWN_TILE_STRATA_Y" in common,
            "spawn tile sampling must use deterministic stratified coverage before temporal jitter")
    require("pixelIndex = SurfelHash(seed) % pixelCount" not in common,
            "spawn tile sampling must not choose every candidate from a full-tile random index")
    require("vec2 SurfelSpawnTileStratum(uvec2 tile" in common and
            "sampleIndex == 0u" not in common and
            "SURFEL_SPAWN_TILE_R2_STEP" in common and
            "Cranley" in common,
            "spawn tile sampling must not make candidate 0 a repeated tile-center winner on flat surfaces")

    require("float visible = exp(-0.08 * float(framesSinceVisible));" in request,
            "ray request priority must be driven by current visible age, not last ray-contribution age")
    require("bool brandNew = SurfelIsNew(surfel) && surfel.worldNormal_age.w < 8.0;" in request,
            "old unresolved surfels must leave the expensive brand-new bootstrap lane")
    require("bool recentlyVisible = framesSinceVisible <= 4u;" in request,
            "ray request must explicitly classify currently visible surfels")
    require("confidence < 0.82 || surfel.shortMean.a < 32.0 || variance > 0.025" in request,
            "visible low-history or high-variance surfels must be treated as unresolved")
    require("dormantLowVariance = !recentlyVisible" in request,
            "visible surfels must not enter dormant low-variance ray starvation")
    require("stableLowVariance = !recentlyVisible" in request,
            "visible surfels must not be phase-throttled until they are genuinely covered and confident")
    require("visibleUnresolved * 0.48" in request,
            "visible unresolved surfels need stronger importance than low-value maintenance work")

    require("bool recentlyVisible = framesSinceVisible <= 4u;" in allocate and
            "bool visibleStarving = recentlyVisible && framesSinceContribution > 4u" in allocate,
            "ray allocation priority must reserve budget for visible surfels that have not received recent rays")
    require("bool firstBootstrap = SurfelIsNew(surfel) && surfel.worldNormal_age.w < 4.0;" in allocate and
            "bool alternatingExtra = ((uFrameIndex + requestID) & 1u) == 0u;" in allocate and
            "priorityGrant = min(priorityGrant, requested);" in allocate,
            "priority ray allocation must breadth-fill visible surfels before giving repeated rays to the same winners")
    require("uint start = atomicAdd(counters.allocatedRays, desired);" in allocate and
            "atomicCompSwap(counters.allocatedRays" not in allocate,
            "ray allocation must reserve bounded ray ranges with a single atomic add instead of starving under CAS contention")
    require("surfel.frameInfo.x = uFrameIndex;" not in integrate and
            "surfel.frameInfo.y = uFrameIndex;" in integrate,
            "temporal integration must update last ray contribution, not forge the last visible coverage-touch frame")
    require("bool brandNew = SurfelIsNew(surfel) && surfel.worldNormal_age.w < 8.0;" in integrate and
            "bool lateBootstrap = SurfelIsNew(surfel) && !brandNew && surfel.shortMean.a <= 1.0 && surfel.irradiance.a <= 0.08;" in integrate and
            "SurfelMSMEInitialize(surfel, estimatorSample, initialVariance);" in integrate and
            "float bootstrapBase = mix(brandNew ? 0.03 : 0.02, brandNew ? 0.11 : 0.075, sourceDirectSupport);" in integrate,
            "old low-confidence seeded surfels need a bounded late-bootstrap MSME initialization path instead of starvation")

    require("[SurfelGIFrameCounters]" in pipeline and
            "coverageVisibleTileCount" in pipeline and
            "requestedRays" in pipeline and
            "allocatedRays" in pipeline,
            "subpass metrics must include counters needed to quantify coverage/ray starvation")


if __name__ == "__main__":
    main()
