from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relpath: str) -> str:
    return (ROOT / relpath).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    generate_rays = read("shaders/surfel_gi/generate_rays.comp")
    sharing = read("shaders/surfel_gi/irradiance_sharing.comp")
    radial_depth = read("shaders/surfel_gi/radial_depth_update.comp")

    require("bool SurfelSelectGuideBin(" in generate_rays,
            "generate_rays.comp must select guide bins through an explicit helper")
    require("if (!SurfelSelectGuideBin(surfelID, selectionTarget, totalWeight, selectedBin, selectedWeight))" in generate_rays,
            "guided sampling must select a positive learned-radiance bin before fallback")
    require("!(selectedWeight > 0.0)" not in generate_rays,
            "guided sampling must not test selectedWeight before bin selection")
    require("mix(cosinePdf, guidedPdf, SURFEL_GUIDE_MIX_PROBABILITY)" in generate_rays,
            "guided and fallback rays must use the same bounded mixture PDF")

    for token in (
        "SharingConnectedSurfaceWeight",
        "SURFEL_SHARING_MIN_NORMAL_DOT",
        "SURFEL_SHARING_MAX_PLANE_SEPARATION",
        "SURFEL_SHARING_MIN_RADIAL_VISIBILITY",
        "SharingPreserveGradientWeight",
        "SharingDarknessPreservationBlend",
    ):
        require(token in sharing,
                f"irradiance_sharing.comp missing compatibility token: {token}")
    require("if (normalDot < SURFEL_SHARING_MIN_NORMAL_DOT)" in sharing,
            "sharing must hard-reject incompatible normals")
    require("if (max(centerPlane, neighborPlane) > planeLimit)" in sharing,
            "sharing must hard-reject large plane separation")
    require("if (visibility < SURFEL_SHARING_MIN_RADIAL_VISIBILITY)" in sharing,
            "sharing must hard-reject radial-depth occlusion")
    require("if (connectedWeight <= 1e-5)" in sharing,
            "sharing must hard-reject disconnected surfaces")
    require("SharingDarknessPreservationBlend(" in sharing and
            "surfel.irradiance.rgb = SurfelClampSharedIrradiance(mix(" in sharing,
            "sharing must preserve darkness while blending compatible neighbors")
    require("surfel.frameInfo.y = max(surfel.frameInfo.y, uFrameIndex)" not in sharing,
            "sharing must not masquerade as a ray contribution or it defeats stable-surface ray throttling")
    require("uSharingPhaseCount" in sharing and
            "uSharingPhaseIndex" in sharing and
            "surfelID % sharingPhaseCount" in sharing,
            "sharing must support phased execution so the real-time preset avoids full-field sharing spikes")

    require("minReliableDepth" in radial_depth and
            "dot(hitNormal, -toHitDir) <= 0.02" in radial_depth,
            "radial depth updates must reject unreliable near/self and backfacing hits")


if __name__ == "__main__":
    main()
