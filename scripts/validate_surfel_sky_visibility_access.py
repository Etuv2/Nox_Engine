from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    trace = read("shaders/surfel_gi/trace_rays.comp")

    require("DiffuseSkyHemisphereAccess" in trace,
            "sky visibility must use a diffuse hemispherical access term")
    require("0.5 + 0.5 * dot(normal, skyDir)" in trace,
            "vertical surfaces must retain half-hemisphere sky access instead of near-zero smoothstep access")
    require("TraceSkyVisibilityDirection" in trace,
            "sky visibility must trace individual sky directions")
    require("bentSky = SurfelSafeNormalize(mix(skyUp, normal, 0.28))" in trace and
            "return clamp(TraceSkyVisibilityDirection(worldPos, normal, bentSky), 0.0, 1.0);" in trace,
            "real-time sky access must use one bounded bent-sky BVH probe instead of a fake ambient term or three extra traces")
    require("SkyOcclusionVisibilityFromHitDistance" in trace,
            "sky access must remain distance-occlusion weighted, not become an unoccluded ambient term")


if __name__ == "__main__":
    main()
