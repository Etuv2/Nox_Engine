from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    allocate = read("shaders/surfel_gi/allocate_rays.comp")
    pipeline = read("src/passes/SurfelGIPipeline.cpp")

    require("readonly buffer SurfelBuffer" in allocate,
            "ray allocator must inspect surfel state instead of treating all ray requests equally")
    require("uniform uint uPriorityPass" in allocate,
            "ray allocator must run a priority pass before the maintenance pass")
    require("bool IsPriorityRaySurfel" in allocate,
            "ray allocator must classify new, visible, unresolved surfels as high priority")
    require("recentlyVisible" in allocate and "lowConfidence" in allocate and "SurfelIsNew" in allocate,
            "priority classifier must consider visible surfel age, confidence, and new surfel state")
    require("if (uPriorityPass != 0u)" in allocate,
            "priority allocation pass must have an explicit high-priority branch")
    require("ReserveRays(requested, budget" in allocate,
            "priority pass must allocate the full requested ray count before proportional maintenance allocation")
    require("!prioritySurfel" in allocate,
            "maintenance allocation pass must skip surfels already handled by the priority pass")
    require('SetUniform1ui(program, "uPriorityPass", 1u)' in pipeline and
            'SetUniform1ui(program, "uPriorityPass", 0u)' in pipeline,
            "pipeline must dispatch ray allocation as priority then maintenance")
    require('timedDispatch("ray_allocation"' in pipeline,
            "priority allocation must remain covered by the ray_allocation GPU timing label")


if __name__ == "__main__":
    main()
