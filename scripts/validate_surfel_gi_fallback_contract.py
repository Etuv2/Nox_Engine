from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relpath: str) -> str:
    return (ROOT / relpath).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    apply_shader = read("shaders/surfel_gi/apply_indirect.comp")
    cell_average_shader = read("shaders/surfel_gi/cell_average.comp")
    spatial_filter_shader = read("shaders/surfel_gi/spatial_filter.comp")

    require("float CellAverageSupportConfidence(" in cell_average_shader,
            "cell averages must scale fallback confidence by surfel support")
    require("float CellAverageSignalConfidence(" in cell_average_shader,
            "cell averages must not advertise confident fallback for black/no-signal cells")
    require("packedConfidence = temporalConfidence * supportConfidence * signalConfidence" in cell_average_shader,
            "cell-average fallback confidence must combine temporal, support, and signal confidence")

    require("float BoundedFallbackBlend(" in apply_shader,
            "final gather must use an explicit bounded fallback blend")
    require("vec3 ClampFallbackToDirectSupport(" in apply_shader,
            "final gather must clamp fallback brightness to real direct surfel support")
    require("if (sumWeight <= 1e-5) {\n        imageStore(uOutIndirect, pixel, vec4(0.0));" in apply_shader,
            "final gather must output black when no real surfel support is gathered")
    require("mix(directIrradiance, boundedGridIrradiance, fallbackBlend)" in apply_shader,
            "cell-average fallback may only smooth from real direct surfel irradiance")
    require("if (SurfelLuminance(boundedGridIrradiance) <= 1e-5) {\n        fallbackBlend = 0.0;" in apply_shader,
            "a clamped-to-black fallback must not darken direct surfel irradiance")
    require("fallbackIrradiance" not in apply_shader and "fallbackWeight" not in apply_shader,
            "per-cell fallback accumulation must not bypass the bounded grid fallback contract")
    require("max(directBlend, fallbackConfidence)" not in apply_shader,
            "fallback confidence must not hide missing surfel support in the output alpha")

    require("if (centerConfidence <= 1e-4) {\n        imageStore(uOutIndirect, pixel, vec4(0.0));" in spatial_filter_shader,
            "spatial filter must not synthesize unsupported GI from neighboring pixels")
    require("centerConfidence > 1e-4 && SurfelLuminance(center.rgb) > 1e-5" not in spatial_filter_shader,
            "spatial filter must preserve valid dark center samples instead of treating them as holes")
    require("centerConfidence > 1e-4\n        ? exp(-colorDelta * colorDelta * 0.42)\n        : 1.0" not in spatial_filter_shader,
            "spatial filter must not disable color gating when the center has no signal")


if __name__ == "__main__":
    main()
