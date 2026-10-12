from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relpath: str) -> str:
    return (ROOT / relpath).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    resources_h = read("src/surfel_gi/SurfelGIResources.h")
    common = read("shaders/surfel_gi/common.glsl")
    coverage = read("shaders/surfel_gi/coverage.comp")
    spawn = read("shaders/surfel_gi/spawn.comp")
    recycle = read("shaders/surfel_gi/recycle_surfels.comp")
    integrate = read("shaders/surfel_gi/integrate.comp")

    require("uvec4 lifecycle;" in common and "glm::uvec4 lifecycle" in resources_h,
            "surfel records must store reference lifecycle state")
    for field in ("SurfelReferenceLife", "SurfelReferenceSleeping",
                  "SurfelReferenceSetLife", "SurfelReferenceSetSleeping"):
        require(field in recycle,
                f"reference lifecycle helper missing from shader policy: {field}")

    require("RecordProjectedValidation" in coverage and
            "atomicExchange(surfels[surfelID].lifecycle.z, 1u)" in coverage,
            "projected coverage must mark reference last-seen state")
    require("ReferenceCoverageForSurfel" in spawn and
            "atomicExchange(surfels[surfelID].lifecycle.z, 1u)" in spawn,
            "spawn coverage lookup must refresh surfels that cover current G-buffer candidates")

    require("SURFEL_REFERENCE_MAX_LIFE = 240u" in recycle and
            "SURFEL_REFERENCE_SLEEPING_MAX_LIFE = SURFEL_REFERENCE_MAX_LIFE / 4u" in recycle and
            "SURFEL_REFERENCE_REF_COUNT_THRESHOLD = 32u" in recycle,
            "recycling must use the Falcor life/sleep/ref-count thresholds")
    require("overCoverageRemoval" in recycle and
            "staleRemoval = life == 0u" in recycle and
            "hasEnoughRefCount" in recycle,
            "coverage, stale, and ref-count recycling must be explicit")
    require("lastSeen" in recycle and
            "life = SURFEL_REFERENCE_MAX_LIFE" in recycle,
            "valid visible surfels must refresh life before stale recycling")

    require("surfel.frameInfo = uvec4(uFrameIndex, uFrameIndex, uFrameIndex, uFrameIndex)" in spawn,
            "spawned surfels must record last-spawned frame for young-surface protection")
    require("surfel.lifecycle = uvec4(0u, 0u, 0u, 0u)" in spawn,
            "new surfels must initialize reference lifecycle state")
    require("surfel.lifecycle.y = max(surfel.lifecycle.y, 1u)" in integrate,
            "accepted irradiance samples must reinforce lifecycle success")


if __name__ == "__main__":
    main()
