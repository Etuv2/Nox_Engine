from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    trace = read("shaders/surfel_gi/trace_rays.comp")

    require("const uint maxCandidates = 32u;" in trace and
            "const uint perCellBudget = 2u;" in trace,
            "BVH hit albedo recovery must sample enough nearby surfels to recover textured material colour")
    require("confidence = smoothstep(0.01, 0.22, sumWeight);" in trace,
            "cached albedo confidence must converge quickly from nearby compatible surfel support")
    require("neutralBVH" in trace and "coloredCache" in trace and "cachedAlbedoBlend" in trace,
            "BVH hit albedo blending must prefer coloured surfel evidence when triangle material is neutral")
    require("TraceVisibleGBufferAlbedoAtSurface" in trace and
            "visibleAlbedoConfidence" in trace and
            "depthConfidence * normalConfidence * albedoSignal" in trace,
            "BVH hits that project onto the visible G-buffer must recover exact textured albedo with depth/normal validation")
    require("albedo = mix(albedo, cachedAlbedo, cachedAlbedoBlend);" in trace,
            "BVH hit radiance must use the colour-aware cached albedo blend")


if __name__ == "__main__":
    main()
