// Standalone tests for the directional cascade math in src/ShadowMapper.cpp.
// No GL context is required. Build and run from the repository root:
//
//   g++ -std=c++20 -O1 -Iinclude -Isrc tests/ShadowMapperTests.cpp src/ShadowMapper.cpp -o shadow_mapper_tests
//   ./shadow_mapper_tests
//
// (On case-sensitive file systems point -I at a directory where "glm" resolves to include/GLM.)

#include "ShadowMapper.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* what)
{
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL: %s\n", what);
    }
}

struct CameraSetup {
    glm::vec3 position;
    float yawDeg;
    float pitchDeg;
};

glm::mat4 MakeView(const CameraSetup& cam)
{
    const float yaw = glm::radians(cam.yawDeg);
    const float pitch = glm::radians(cam.pitchDeg);
    const glm::vec3 forward(std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw));
    return glm::lookAt(cam.position, cam.position + forward, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::vec3 ProjectToShadowUV(const glm::mat4& lightSpace, const glm::vec3& world)
{
    const glm::vec4 clip = lightSpace * glm::vec4(world, 1.0f);
    return glm::vec3(clip) / clip.w * 0.5f + 0.5f;
}

// Every point of the view-frustum slice (including the blend overlap in front of it) must land
// inside the cascade's shadow map, depth included.
void TestCascadeCoversFrustumSlice()
{
    std::printf("cascade coverage\n");
    const float nearPlane = 0.1f;
    const float farPlane = 300.0f;
    const float aspect = 16.0f / 9.0f;
    const int resolution = 2048;
    const glm::vec3 lightDir = glm::normalize(glm::vec3(-0.5f, -1.0f, -0.5f));
    const std::vector<float> splits = ShadowMapper::ComputeCascadeSplits(nearPlane, farPlane, 4, 0.85f);

    std::mt19937 rng(7);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    for (float fov : { 45.0f, 90.0f, 110.0f }) {
        for (int trial = 0; trial < 16; ++trial) {
            const CameraSetup cam{ glm::vec3(unit(rng) * 400.0f - 200.0f, unit(rng) * 40.0f, unit(rng) * 400.0f - 200.0f),
                unit(rng) * 360.0f, unit(rng) * 170.0f - 85.0f };
            const glm::mat4 view = MakeView(cam);
            const glm::mat4 invView = glm::inverse(view);
            const float tanY = std::tan(glm::radians(fov) * 0.5f);

            float cascadeNear = nearPlane;
            for (int c = 0; c < 4; ++c) {
                const float cascadeFar = splits[c];
                const glm::mat4 ls = ShadowMapper::ComputeCascadeLightSpace(
                    cascadeNear, cascadeFar, view, glm::vec3(0.0f), lightDir, aspect, fov, c, resolution);
                const float sliceStart = (c == 0) ? cascadeNear
                    : std::max(0.01f, cascadeNear - (cascadeFar - cascadeNear) * ShadowMapper::kCascadeBlendOverlap);

                bool inside = true;
                for (int s = 0; s < 400 && inside; ++s) {
                    const float depth = sliceStart + (cascadeFar - sliceStart) * unit(rng);
                    const float x = (unit(rng) * 2.0f - 1.0f) * tanY * aspect * depth;
                    const float y = (unit(rng) * 2.0f - 1.0f) * tanY * depth;
                    const glm::vec3 world = glm::vec3(invView * glm::vec4(x, y, -depth, 1.0f));
                    const glm::vec3 uv = ProjectToShadowUV(ls, world);
                    inside = uv.x > 0.0f && uv.x < 1.0f && uv.y > 0.0f && uv.y < 1.0f && uv.z >= 0.0f && uv.z <= 1.0f;
                }
                Check(inside, "frustum slice point projected outside its cascade");
                cascadeNear = cascadeFar;
            }
        }
    }
}

// Translating or rotating the camera must not change the cascade's texel size, and translating
// it must move the shadow map window by whole texels only (no sub-texel crawl).
void TestCascadeTexelStability()
{
    std::printf("cascade texel stability\n");
    const int resolution = 2048;
    const glm::vec3 lightDir = glm::normalize(glm::vec3(0.3f, -1.0f, 0.2f));
    const glm::vec3 probe(12.345f, 0.5f, -7.89f);

    const glm::mat4 reference = ShadowMapper::ComputeCascadeLightSpace(
        0.1f, 20.0f, MakeView({ glm::vec3(0.0f, 2.0f, 0.0f), 30.0f, -10.0f }), glm::vec3(0.0f), lightDir, 1.6f, 70.0f, 0, resolution);
    const glm::vec3 refTexel = ProjectToShadowUV(reference, probe) * float(resolution);
    const float refScale = glm::length(glm::vec3(reference[0][0], reference[1][0], reference[2][0]));

    std::mt19937 rng(11);
    std::uniform_real_distribution<float> offset(-3.0f, 3.0f);
    std::uniform_real_distribution<float> angle(0.0f, 360.0f);
    bool scaleStable = true;
    bool gridStable = true;
    for (int i = 0; i < 200; ++i) {
        const CameraSetup cam{ glm::vec3(offset(rng), 2.0f + offset(rng) * 0.1f, offset(rng)), angle(rng), -10.0f + offset(rng) };
        const glm::mat4 ls = ShadowMapper::ComputeCascadeLightSpace(
            0.1f, 20.0f, MakeView(cam), glm::vec3(0.0f), lightDir, 1.6f, 70.0f, 0, resolution);
        const float scale = glm::length(glm::vec3(ls[0][0], ls[1][0], ls[2][0]));
        scaleStable = scaleStable && std::abs(scale - refScale) <= refScale * 1e-6f;

        const glm::vec3 texel = ProjectToShadowUV(ls, probe) * float(resolution);
        const glm::vec2 delta = glm::vec2(texel) - glm::vec2(refTexel);
        const glm::vec2 fractional = glm::abs(delta - glm::round(delta));
        gridStable = gridStable && fractional.x < 1e-2f && fractional.y < 1e-2f;
    }
    Check(scaleStable, "cascade texel size changed while the camera moved");
    Check(gridStable, "cascade window moved by a fraction of a texel");
}

void TestCasterCulling()
{
    std::printf("caster culling\n");
    const glm::vec3 lightDir = glm::normalize(glm::vec3(0.0f, -1.0f, 0.0f));
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f, 2.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 ls = ShadowMapper::ComputeCascadeLightSpace(0.1f, 10.0f, view, glm::vec3(0.0f), lightDir, 1.0f, 60.0f, 0, 1024);

    const glm::vec3 uvCenter = glm::vec3(glm::inverse(ls) * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    Check(ShadowMapper::SphereIntersectsShadowCasterVolume(ls, uvCenter, 0.5f), "caster at the cascade center rejected");
    // Far above the cascade (towards the light, beyond the near plane): must still cast.
    Check(ShadowMapper::SphereIntersectsShadowCasterVolume(ls, uvCenter + glm::vec3(0.0f, 500.0f, 0.0f), 0.5f),
        "caster between the light and the near plane rejected");
    // Far below the cascade (beyond the far plane): cannot shadow anything in it.
    Check(!ShadowMapper::SphereIntersectsShadowCasterVolume(ls, uvCenter - glm::vec3(0.0f, 500.0f, 0.0f), 0.5f),
        "caster behind the far plane accepted");
    // Large object whose center is well outside the window but whose bounds overlap it.
    Check(ShadowMapper::SphereIntersectsShadowCasterVolume(ls, uvCenter + glm::vec3(60.0f, 0.0f, 0.0f), 80.0f),
        "large overlapping caster with an off-window center rejected");
    Check(!ShadowMapper::SphereIntersectsShadowCasterVolume(ls, uvCenter + glm::vec3(60.0f, 0.0f, 0.0f), 1.0f),
        "small caster outside the window accepted");

    const glm::mat4 spot = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 20.0f)
        * glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    Check(ShadowMapper::SphereIntersectsShadowCasterVolume(spot, glm::vec3(0.0f, 0.0f, -10.0f), 0.5f), "spot caster on axis rejected");
    Check(!ShadowMapper::SphereIntersectsShadowCasterVolume(spot, glm::vec3(0.0f, 0.0f, 10.0f), 0.5f), "spot caster behind the light accepted");
    Check(!ShadowMapper::SphereIntersectsShadowCasterVolume(spot, glm::vec3(0.0f, 0.0f, -30.0f), 0.5f), "spot caster beyond range accepted");
}

void TestSliceSphereBoundsCorners()
{
    std::printf("frustum slice sphere\n");
    for (float fov : { 30.0f, 60.0f, 90.0f, 120.0f }) {
        for (float aspect : { 1.0f, 16.0f / 9.0f, 2.4f }) {
            for (auto [n, f] : { std::pair{ 0.1f, 5.0f }, std::pair{ 5.0f, 40.0f }, std::pair{ 40.0f, 300.0f } }) {
                const auto sphere = ShadowMapper::ComputeFrustumSliceSphere(n, f, fov, aspect);
                const float tanY = std::tan(glm::radians(fov) * 0.5f);
                const float tanX = tanY * aspect;
                bool bounded = true;
                for (float z : { n, f }) {
                    const float dz = z - sphere.centerDistance;
                    const float lateral2 = (z * tanX) * (z * tanX) + (z * tanY) * (z * tanY);
                    bounded = bounded && std::sqrt(dz * dz + lateral2) <= sphere.radius * (1.0f + 1e-5f);
                }
                Check(bounded, "frustum slice corner outside its bounding sphere");
            }
        }
    }
}

} // namespace

int main()
{
    TestSliceSphereBoundsCorners();
    TestCascadeCoversFrustumSlice();
    TestCascadeTexelStability();
    TestCasterCulling();
    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all ShadowMapper checks passed\n");
    return 0;
}
