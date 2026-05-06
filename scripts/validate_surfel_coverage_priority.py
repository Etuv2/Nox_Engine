from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def read(relpath: str) -> str:
    return (ROOT / relpath).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b[\w\s]+\s+{re.escape(name)}\s*\([^)]*\)\s*\{{", source)
    require(match is not None, f"missing function: {name}")
    start = match.end()
    depth = 1
    i = start
    while i < len(source) and depth > 0:
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
        i += 1
    require(depth == 0, f"could not parse function body: {name}")
    return source[start : i - 1]


def main() -> None:
    spawn = read("shaders/surfel_gi/spawn.comp")
    apply = read("shaders/surfel_gi/apply_indirect.comp")
    pipeline_cpp = read("src/passes/SurfelGIPipeline.cpp")
    coverage = read("shaders/surfel_gi/coverage.comp")
    common = read("shaders/surfel_gi/common.glsl")

    require("layout(binding = B_COVERAGE_TILES, std430) buffer SurfelCoverageTileBuffer" in spawn,
            "spawn shader must write persistent per-tile coverage diagnostics")
    require("SurfelCoverageTile" in common and "uvec4 state" in common and "uvec4 pixel" in common,
            "coverage tile struct must retain the existing shared diagnostic fields")
    require("vec2 SurfelSpawnTileStratum(uvec2 tile" in common,
            "spawn tile strata must be decorrelated per tile so flat surfaces do not seed repeated screen-tile centers")
    require("sampleIndex == 0u" not in common,
            "spawn candidate 0 must not be a special near-center sample that wins flat-surface ties into visible strips")
    require("SURFEL_SPAWN_TILE_R2_STEP" in common and
            "Cranley" in common and
            "stratumSize" not in common.split("uvec2 SurfelSpawnTilePixel", 1)[1],
            "spawn candidates must use full-tile low-discrepancy jitter, not a small fixed set of horizontal/vertical strata")
    require("SURFEL_COVERAGE_TILE_FINAL_GI_VALID" in common,
            "coverage tile flags must include final-gather GI validity")
    require("layout(binding = B_COVERAGE_TILES, std430) buffer SurfelCoverageTileBuffer" in apply and
            "MarkCoverageTileFinalGI" in apply and
            "atomicOr(coverageTiles[tileIndex].state.z, SURFEL_COVERAGE_TILE_FINAL_GI_VALID)" in apply,
            "final gather must mark per-tile GI validity only when real surfel support reaches the receiver")
    require("SetUniform2ui(program, \"uCoverageTileCount\"" in pipeline_cpp,
            "pipeline must pass coverage tile dimensions to the final-gather diagnostics")
    require("uniform uint uSpawnCandidateCount" in spawn and
            "clamp(uSpawnCandidateCount, 1u, SURFEL_SPAWN_TILE_CANDIDATES)" in spawn and
            "SetUniform1ui(program, \"uSpawnCandidateCount\"" in pipeline_cpp,
            "coverage scheduler must keep full candidate sweeps for fast fill while allowing cheaper stable maintenance")
    require("uCoveragePassMode" in coverage and
            "SURFEL_COVERAGE_PASS_RESET" in coverage and
            "SURFEL_COVERAGE_PASS_PROJECT" in coverage,
            "coverage.comp must implement explicit reset and surfel projection/classification modes")
    require("ValidateProjectedSurfelCoverage" in coverage and
            "depthConsistent" in coverage and
            "normalCompatible" in coverage and
            "materialCompatible" in coverage and
            "radiusCompatible" in coverage,
            "coverage projection must validate depth, normal, material, and radius compatibility")
    require('"coverage_reset"' in pipeline_cpp and '"coverage_project"' in pipeline_cpp,
            "pipeline must dispatch dedicated coverage reset and projected-surfel classification passes before spawning")

    evaluate_body = function_body(spawn, "EvaluateSpawnCandidate")
    require("candidate.rejectReason" in spawn and "candidate.nearbySurfelCount" in spawn,
            "spawn candidates must carry invalid/rejection reason and nearby-surface count diagnostics")
    require("SURFEL_SPAWN_REJECT_INVALID_DEPTH" in spawn and "SURFEL_SPAWN_REJECT_OUTSIDE_GRID" in spawn,
            "spawn shader must define stable per-tile rejection reason codes")
    require(re.search(r"candidate\.rejectReason\s*=\s*SURFEL_SPAWN_REJECT_INVALID_DEPTH", evaluate_body),
            "invalid depth rejection must be recorded on the tile candidate")
    require(re.search(r"candidate\.rejectReason\s*=\s*SURFEL_SPAWN_REJECT_OUTSIDE_GRID", evaluate_body),
            "outside-grid rejection must be recorded on the tile candidate")

    require("out uint nearbySurfelCount" in spawn,
            "coverage evaluation must return nearby surfel count for diagnostics")
    require("++nearbySurfelCount" in spawn,
            "coverage evaluation must count nearby valid surfel tests")

    store_body = function_body(spawn, "StoreCoverageTile")
    require("bool highPriority = underCovered &&" in store_body,
            "visible under-covered tiles must become high priority in the same StoreCoverageTile call")
    require("unresolvedAge >= SURFEL_COVERAGE_PRIORITY_AGE" in store_body,
            "unresolved age must drive high priority for stationary under-covered tiles")
    require("SURFEL_COVERAGE_MOTION_BOOST_AGE" in store_body,
            "camera movement may boost priority without replacing age-driven escalation")
    require("atomicAdd(counters.highPriorityTileCount, 1u)" in store_body,
            "high priority tile counter must be incremented from final per-tile priority")
    require("tileState.lowestCoverage.y = float(candidate.nearbySurfelCount)" in store_body,
            "tile diagnostics must record nearby surfel count in an existing field")
    require("tileState.lowestCoverage.z = candidate.initialCellConfidence" in store_body,
            "tile diagnostics must record final GI validity/confidence in an existing field")
    require("tileState.pixel.w = candidate.rejectReason" in store_body,
            "tile diagnostics must record invalid/rejection reason in an existing field")
    require("tileState.pixel.z = spawned ? 1u : 0u" in store_body,
            "tile diagnostics must record spawned-this-frame, not only a lifetime total")
    require("tileState.state.w = uFrameIndex" in store_body,
            "tile diagnostics must record updated-this-frame for visible candidates")

    main_body = function_body(spawn, "main")
    require("PermutedSpawnTileIndex" in spawn and
            "SpawnTilePermutationStride" in spawn and
            "uint workItem = gl_GlobalInvocationID.x" in main_body and
            "uint tileIndex = PermutedSpawnTileIndex(workItem, totalTileCount, uFrameIndex, passIndex)" in main_body,
            "spawn budget arbitration must visit screen tiles through a per-frame permutation instead of row-major order")
    require("StoreCoverageTile(tileIndex, bestCandidate.valid, bestCandidate, effectiveCoverageThreshold, spawned)" in main_body,
            "main must let StoreCoverageTile compute final high priority from current visibility, coverage, age, and motion")
    require("SurfelCandidateSelectionPriority" in spawn and
            "candidatePriority" in main_body and
            "currentScore -= candidatePriority" in main_body,
            "flat or near-flat tiles must use a deterministic per-candidate tie breaker instead of always keeping candidate 0")
    require("bestCandidate.coverage = max(bestCandidate.coverage, uCoverageThreshold)" not in main_body,
            "stationary tiles must continue measuring real coverage after initial seeding")
    require("previousUnresolvedAge >= 2u || uCameraMotionBoost != 0u" not in main_body,
            "priority must not be based only on previous age before current under-coverage is known")


if __name__ == "__main__":
    main()
