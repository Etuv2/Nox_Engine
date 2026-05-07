from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    apply_shader = read("shaders/surfel_gi/apply_indirect.comp")

    require("SURFEL_CROSS_SURFACE_BLEED_BOOST" in apply_shader,
            "final gather must have an explicit bounded cross-surface colour bleed term")
    require("ReceiverSurfelColorBleedWeight" in apply_shader,
            "final gather must compute a separate colour-bleed compatibility weight")
    require("SurfelColorBleedIrradianceForReceiver" in apply_shader,
            "colour bleed must use source surfel albedo and cached irradiance")
    require("SurfelColorBleedTint" in apply_shader and
            "SURFEL_CROSS_SURFACE_CHROMA_GAIN" in apply_shader,
            "colour bleed must preserve source-surface chroma instead of collapsing to grey indirect")
    require("sumBleedIrradiance" in apply_shader and "sumBleedWeight" in apply_shader,
            "final gather must accumulate colour bleed separately from same-surface interpolation")
    require("SurfelRadialDepthVisibility" in apply_shader and "bleedWeight *= radialVisibility" in apply_shader,
            "colour bleed must remain gated by radial-depth visibility")
    require("ClampColorBleedLift" in apply_shader,
            "colour bleed must be clamped so it cannot become a fake ambient flood")
    require("PromoteColorBleedForFinalComposite" in apply_shader,
            "colour bleed must be promoted enough to survive the final composite")
    require("ComposeColorBleedWithIrradiance" in apply_shader and
            "vec3 irradiance = ComposeColorBleedWithIrradiance(preliminaryIrradiance, colorBleed, bleedSupport);" in apply_shader,
            "final output must preserve supported colour-bleed chroma when mixing with neutral irradiance")
    require("irradiance += colorBleed" not in apply_shader,
            "final output must not bury colour bleed with a plain add into neutral irradiance")
    sharing_shader = read("shaders/surfel_gi/irradiance_sharing.comp")
    require("SURFEL_SHARING_MIN_CROSS_TRANSFER" in sharing_shader and
            "crossSurfaceCompatible" in sharing_shader and
            "transferFacing" in sharing_shader,
            "irradiance sharing must exchange bounded colour between compatible perpendicular surfels")


if __name__ == "__main__":
    main()
