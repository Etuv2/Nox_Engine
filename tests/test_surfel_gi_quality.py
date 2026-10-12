from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(rel_path: str) -> str:
    return (ROOT / rel_path).read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b[\w\d_]+\s+{re.escape(name)}\s*\([^)]*\)\s*\{{", source)
    if not match:
        raise AssertionError(f"Function {name} not found")
    start = match.end()
    depth = 1
    pos = start
    while pos < len(source) and depth > 0:
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
        pos += 1
    if depth != 0:
        raise AssertionError(f"Function {name} body is not balanced")
    return source[start:pos - 1]


def parse_caps_tuple(source: str, tier: str) -> list[str]:
    match = re.search(
        rf"case\s+SurfelGIQualityTier::{tier}\s*:\s*(?:default\s*:\s*)?return\s*\{{([^}}]+)\}};",
        source,
        re.S,
    )
    if not match:
        raise AssertionError(f"Missing caps tuple for {tier}")
    return [part.strip() for part in match.group(1).split(",")]


def uint_token(token: str) -> int:
    match = re.search(r"(\d+)u?", token)
    if not match:
        raise AssertionError(f"Expected uint token, got {token!r}")
    return int(match.group(1))


def float_token(token: str) -> float:
    match = re.search(r"([0-9]+(?:\.[0-9]+)?)f?", token)
    if not match:
        raise AssertionError(f"Expected float token, got {token!r}")
    return float(match.group(1))


class SurfelGIQualityContracts(unittest.TestCase):
    def test_quality_caps_prioritize_transport_resolution_and_convergence(self) -> None:
        renderer = read("src/ModularRenderer.cpp")

        medium = parse_caps_tuple(renderer, "Medium")
        high = parse_caps_tuple(renderer, "High")
        ultra = parse_caps_tuple(renderer, "Ultra")

        self.assertGreaterEqual(uint_token(medium[0]), 65536)
        self.assertLessEqual(uint_token(medium[1]), 256)
        self.assertGreaterEqual(uint_token(medium[4]), 32)
        self.assertLessEqual(uint_token(medium[4]), 40)
        self.assertGreaterEqual(float_token(medium[6]), 0.32)
        self.assertLessEqual(float_token(medium[6]), 0.33)
        self.assertLessEqual(float_token(medium[7]), 0.25)
        self.assertEqual(uint_token(medium[13]), 1)
        self.assertLessEqual(uint_token(medium[14]), 1)
        self.assertLessEqual(uint_token(medium[17]), 64)
        self.assertEqual(medium[11], "false")

        self.assertGreaterEqual(uint_token(high[1]), 32768)
        self.assertGreaterEqual(uint_token(high[4]), 256)
        self.assertEqual(float_token(high[6]), 1.0)

        self.assertGreaterEqual(uint_token(ultra[0]), 262144)
        self.assertGreaterEqual(uint_token(ultra[1]), 65536)
        self.assertGreaterEqual(uint_token(ultra[4]), 512)
        self.assertEqual(float_token(ultra[6]), 1.0)
        self.assertIn("captureMode ? 1.0f", renderer)
        self.assertNotIn("std::max(caps.fallbackStrength, 0.65f)", renderer)
        self.assertIn(
            "settings.cellAverageFallbackStrength = captureMode ? std::min(settings.cellAverageFallbackStrength, caps.fallbackStrength) : settings.cellAverageFallbackStrength;",
            renderer,
        )

    def test_surfel_ui_reflects_effective_realtime_caps(self) -> None:
        renderer = read("src/ModularRenderer.cpp")

        self.assertIn("m_context.surfelGIMaxSurfels = static_cast<int>(settings.maxSurfels);", renderer)
        self.assertIn("m_context.surfelGIMaxRayBudget = static_cast<int>(settings.maxRayBudget);", renderer)
        self.assertIn("m_context.surfelGIMaxGatherSurfelsPerPixel = static_cast<int>(settings.maxGatherSurfelsPerPixel);", renderer)

    def test_validation_framebuffer_dump_captures_scene_before_imgui(self) -> None:
        core = read("src/Core.cpp")

        self.assertIn("NOX_FRAMEBUFFER_DUMP_DIR", core)
        self.assertIn("SaveBackbufferPNG", core)
        dump_index = core.index("MaybeDumpSceneFramebuffer")
        imgui_index = core.index("ImGui_ImplOpenGL3_NewFrame")
        self.assertLess(dump_index, imgui_index)

    def test_surfel_final_composite_uses_dedicated_response(self) -> None:
        deferred = read("shaders/deferred_lighting_frag.glsl")

        self.assertIn("vec3 ComputeSurfelGIResponse", deferred)
        self.assertIn("sourceIndex == 2", deferred)
        self.assertIn("ComputeSurfelGIResponse(indirectIrradiance", deferred)

    def test_medium_defaults_enable_core_transport_with_bounded_fallback(self) -> None:
        manager = read("src/passes/SurfelGIManager.h")
        context = read("src/RenderContext.h")
        imgui_h = read("src/ImGui/RenderingSettingsWindow.h")
        imgui_cpp = read("src/ImGui/RenderingSettingsWindow.cpp")
        renderer = read("src/ModularRenderer.cpp")

        self.assertIn("float cellAverageFallbackStrength = 0.20f;", manager)
        self.assertIn("uint32_t maxSurfels = 65536u;", manager)
        self.assertIn("uint32_t maxRayBudget = 192u;", manager)
        self.assertIn("uint32_t maxGatherSurfelsPerPixel = 32u;", manager)
        self.assertIn("uint32_t maxSpawnsPerFrame = 64u;", manager)
        self.assertIn("float finalGatherResolutionScale = 0.32f;", manager)
        self.assertIn("bool useRadialDepth = true;", manager)
        self.assertIn("bool useRayGuiding = true;", manager)
        self.assertIn("bool useScreenSpaceTrace = true;", manager)

        self.assertIn("float surfelGICellAverageFallbackStrength = 0.20f;", context)
        self.assertIn("int surfelGIMaxSurfels = 65536;", context)
        self.assertIn("int surfelGIMaxRayBudget = 192;", context)
        self.assertIn("int surfelGIMaxGatherSurfelsPerPixel = 32;", context)
        self.assertIn("int surfelGIMaxSpawnsPerFrame = 64;", context)
        self.assertIn("bool surfelGIUseRadialDepth = true;", context)
        self.assertIn("bool surfelGIUseRayGuiding = true;", context)
        self.assertIn("bool surfelGIUseScreenTrace = true;", context)

        self.assertIn("int m_surfelGIMaxSurfels = 65536;", imgui_h)
        self.assertIn("int m_surfelGIMaxRayBudget = 192;", imgui_h)
        self.assertIn("int m_surfelGIMaxGatherSurfelsPerPixel = 32;", imgui_h)
        self.assertIn("int m_surfelGIMaxSpawnsPerFrame = 64;", imgui_h)
        self.assertNotIn("m_surfelGIMaxSurfels = 262144;", imgui_cpp)
        self.assertNotIn("m_surfelGIMaxRayBudget = 65536;", imgui_cpp)
        self.assertNotIn("m_surfelGIMaxGatherSurfelsPerPixel = 512;", imgui_cpp)

        self.assertIn("fallbackStrength", renderer)
        self.assertIn("settings.cellAverageFallbackStrength", renderer)
        self.assertNotIn("0.55f, true, true, false, false", renderer)
        self.assertNotIn("0.65f, true, true, false, false", renderer)

    def test_lighting_changes_invalidate_stale_surfel_history_quickly(self) -> None:
        pipeline_cpp = read("src/passes/SurfelGIPipeline.cpp")
        pipeline_h = read("src/passes/SurfelGIPipeline.h")
        update = read("shaders/surfel_gi/update_surfels.comp")
        request = read("shaders/surfel_gi/request_rays.comp")
        allocate = read("shaders/surfel_gi/allocate_rays.comp")
        generate = read("shaders/surfel_gi/generate_rays.comp")
        integrate = read("shaders/surfel_gi/integrate.comp")
        temporal = read("shaders/surfel_gi/temporal_filter.comp")

        self.assertIn("m_lightingChangeFactor", pipeline_h)
        self.assertIn("m_lightingChangeResponse", pipeline_h)
        self.assertIn("m_lightingChangeHoldFrames", pipeline_h)
        self.assertIn("FindPrimaryDirectionalLight", pipeline_cpp)
        self.assertIn("BuildLightingSignature", pipeline_cpp)
        self.assertIn("ComputeLightingChangeFactor", pipeline_cpp)
        self.assertIn("m_lastLightingSignature = currentLightingSignature;", pipeline_cpp)
        self.assertGreaterEqual(pipeline_cpp.count('SetUniform1f(program, "uLightingChangeFactor", m_lightingChangeFactor);'), 4)
        self.assertIn('SetUniform1f(temporalProgram, "uLightingChangeFactor", m_lightingChangeFactor);', pipeline_cpp)

        self.assertIn("uniform float uLightingChangeFactor", update)
        self.assertIn("lightingReactive", update)
        self.assertIn("surfel.msme.y", update)
        self.assertIn("SURFEL_NEW", update)

        self.assertIn("uniform float uLightingChangeFactor", request)
        self.assertIn("lightingReactiveVisible", request)
        self.assertIn("SURFEL_REQUEST_LIGHTING_CHANGE_RAYS", request)

        self.assertIn("uniform float uLightingChangeFactor", allocate)
        self.assertIn("lightReactivePriority", allocate)

        self.assertIn("uniform float uLightingChangeFactor", generate)
        self.assertIn("SurfelGuideMixProbability", generate)
        self.assertIn("mix(SURFEL_GUIDE_MIX_PROBABILITY, 0.18", generate)

        self.assertIn("uniform float uLightingChangeFactor", integrate)
        self.assertIn("lightingChange", integrate)
        self.assertIn("retainedConfidence", integrate)
        self.assertIn("msmeData.inconsistency", integrate)
        self.assertIn("lightingCatchUpBlend", integrate)

        self.assertIn("uniform float uLightingChangeFactor", temporal)
        self.assertIn("lightingHistoryCap", temporal)
        self.assertIn("historyConfidence *= mix(1.0, 0.38, lightingChange);", temporal)

        trace = read("shaders/surfel_gi/trace_rays.comp")
        self.assertIn("uniform float uLightingChangeFactor", trace)
        self.assertIn("cacheTrust", trace)
        self.assertNotIn("bounced = max(bounced, liveBounce);", trace)

    def test_surfel_gi_has_no_professional_naming_split(self) -> None:
        searched = "\n".join(
            read(path)
            for path in (
                "src/RenderContext.h",
                "src/ModularRenderer.cpp",
                "src/ModularRenderer.h",
                "src/ImGui/RenderingSettingsWindow.cpp",
                "src/ImGui/RenderingSettingsWindow.h",
            )
        )
        self.assertNotRegex(searched, r"cleanSurfel|CleanSurfel|Clean Surfel|clean surfel|Clean surfel")
        self.assertIn("bool enableSurfelGI = false;", searched)
        self.assertIn("const SurfelGIFrameStats* GetSurfelGIStats() const", searched)
        self.assertIn("void SyncSurfelGISettings()", searched)

    def test_surfel_composite_preserves_bleed_chroma_under_direct_light(self) -> None:
        deferred = read("shaders/deferred_lighting_frag.glsl")
        preserve = function_body(deferred, "PreserveSurfelBleedAgainstDirect")

        self.assertIn("vec3 PreserveSurfelBleedAgainstDirect", deferred)
        self.assertIn("directWashout", preserve)
        self.assertIn("positiveChroma", preserve)
        self.assertIn("maxProtectedLum", preserve)
        self.assertIn(
            "vec3 fullSurfelContribution = PreserveSurfelBleedAgainstDirect(surfelContribution, directLighting + iblContribution);",
            deferred,
        )
        self.assertIn("color += ssgiContribution + lpvContribution + fullSurfelContribution;", deferred)

    def test_final_gather_fallback_can_fill_direct_gather_holes(self) -> None:
        apply = read("shaders/surfel_gi/apply_indirect.comp")
        bounded = function_body(apply, "BoundedFallbackBlend")
        clamp = function_body(apply, "ClampFallbackToDirectSupport")

        self.assertNotIn("sampleCount == 0u", bounded)
        self.assertIn("coverageGap", bounded)
        self.assertIn("candidateOrCellSupport", bounded)

        self.assertNotIn("directLum <= 1e-5", clamp)
        self.assertIn("coverageCap", clamp)
        self.assertIn("fallbackConfidence", clamp)

        self.assertNotIn("if (sumWeight <= 1e-5)", apply)
        self.assertRegex(apply, r"finalGIValid\s*=.*fallbackBlend.*bleedSupport")
        self.assertRegex(apply, r"confidence\s*=.*fallbackBlend.*bleedSupport")

    def test_colour_bleed_support_does_not_require_direct_gather(self) -> None:
        apply = read("shaders/surfel_gi/apply_indirect.comp")
        bleed_clamp = function_body(apply, "ClampColorBleedLift")
        bleed_weight = function_body(apply, "ReceiverSurfelColorBleedWeight")
        bleed_source = function_body(apply, "SurfelColorBleedIrradianceForReceiver")

        bleed_boost = re.search(r"SURFEL_CROSS_SURFACE_BLEED_BOOST\s*=\s*([0-9.]+)", apply)
        max_lift = re.search(r"SURFEL_CROSS_SURFACE_MAX_LIFT\s*=\s*([0-9.]+)", apply)
        chroma_gain = re.search(r"SURFEL_CROSS_SURFACE_CHROMA_GAIN\s*=\s*([0-9.]+)", apply)
        self.assertIsNotNone(bleed_boost)
        self.assertIsNotNone(max_lift)
        self.assertIsNotNone(chroma_gain)
        self.assertGreaterEqual(float(bleed_boost.group(1)), 1.10)
        self.assertLessEqual(float(bleed_boost.group(1)), 1.45)
        self.assertGreaterEqual(float(max_lift.group(1)), 0.32)
        self.assertLessEqual(float(max_lift.group(1)), 0.42)
        self.assertGreaterEqual(float(chroma_gain.group(1)), 1.45)
        self.assertLessEqual(float(chroma_gain.group(1)), 1.80)

        self.assertNotIn("smoothstep(0.01, 0.18, sumWeight)", apply)
        self.assertNotIn("baseLum * SURFEL_CROSS_SURFACE_MAX_LIFT + 0.012", bleed_clamp)
        self.assertIn("supportLum", bleed_clamp)
        self.assertIn("geometricSupport", bleed_clamp)
        self.assertIn("supportRadius * 1.55", bleed_weight)
        self.assertIn("normalizedDistance * normalizedDistance * 0.70", bleed_weight)
        self.assertNotIn("transferFacing <= 0.002", bleed_weight)
        self.assertIn("SURFEL_CROSS_SURFACE_BLEED_BOOST", bleed_source)

    def test_colour_bleed_is_promoted_for_final_composite(self) -> None:
        apply = read("shaders/surfel_gi/apply_indirect.comp")
        deferred = read("shaders/deferred_lighting_frag.glsl")
        final_promote = function_body(apply, "PromoteColorBleedForFinalComposite")
        surfel_response = function_body(deferred, "ComputeSurfelGIResponse")

        self.assertIn("SURFEL_FINAL_BLEED_VISIBILITY_BOOST", apply)
        self.assertIn("SURFEL_FINAL_BLEED_CHROMA_BOOST", apply)
        visibility_boost = re.search(r"SURFEL_FINAL_BLEED_VISIBILITY_BOOST\s*=\s*([0-9.]+)", apply)
        chroma_boost = re.search(r"SURFEL_FINAL_BLEED_CHROMA_BOOST\s*=\s*([0-9.]+)", apply)
        self.assertIsNotNone(visibility_boost)
        self.assertIsNotNone(chroma_boost)
        self.assertGreaterEqual(float(visibility_boost.group(1)), 1.0)
        self.assertLessEqual(float(visibility_boost.group(1)), 1.35)
        self.assertGreaterEqual(float(chroma_boost.group(1)), 0.20)
        self.assertLessEqual(float(chroma_boost.group(1)), 0.75)
        self.assertIn("colorBleed = PromoteColorBleedForFinalComposite(preliminaryIrradiance, colorBleed, bleedSupport);", apply)
        self.assertIn("positiveChroma", final_promote)
        self.assertIn("maxPromotedLum", final_promote)

        self.assertIn("indirectChromaSignal", surfel_response)
        self.assertIn("chromaVisibility", surfel_response)
        self.assertIn("receiverChromaResponseAlbedo", surfel_response)
        self.assertIn("chromaPreservedResponse", surfel_response)
        self.assertIn("maxChromaPreservedLum", surfel_response)
        self.assertNotIn("SURFEL_GI_RESPONSE_CHROMA_GAIN", deferred)
        self.assertNotIn("responseChroma", surfel_response)
        self.assertNotIn("chromaBoost", surfel_response)
        self.assertNotIn("maxResponseLum", surfel_response)
        self.assertIn("SURFEL_GI_RESPONSE_FORM_FACTOR_COMPENSATION", deferred)
        self.assertIn("energyCompensation", surfel_response)
        response_gain = re.search(r"SURFEL_GI_RESPONSE_FORM_FACTOR_COMPENSATION\s*=\s*([0-9.]+)", deferred)
        self.assertIsNotNone(response_gain)
        self.assertGreaterEqual(float(response_gain.group(1)), 16.0)
        self.assertLessEqual(float(response_gain.group(1)), 22.0)
        self.assertIn("diffuseAlbedo * INV_PI", surfel_response)
        self.assertNotIn("bleedResponseFloor", surfel_response)
        self.assertNotIn("effectiveDiffuseAlbedo", surfel_response)

    def test_surfel_debug_live_mode_draws_surface_aligned_flat_discs(self) -> None:
        debug_vert = read("shaders/surfel_gi/debug_surfels.vert")
        debug_frag = read("shaders/surfel_gi/debug_surfels.frag")
        pipeline = read("src/passes/SurfelGIPipeline.cpp")

        self.assertIn("glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4", pipeline)
        self.assertIn("SurfelBuildBasis(normal, tangent, bitangent);", debug_vert)
        self.assertIn("diskWorldPos = diskCenterWorld + (tangent * corner.x + bitangent * corner.y) * surfelRadius", debug_vert)
        self.assertIn("paperStyle = uDebugView == DEBUG_SURFEL_SPHERES", debug_vert)
        paper_scale = re.search(r"GIBS_PAPER_DISK_SCALE\s*=\s*([0-9.]+)", debug_vert)
        paper_min = re.search(r"GIBS_PAPER_MIN_RADIUS_PX\s*=\s*([0-9.]+)", debug_vert)
        paper_max = re.search(r"GIBS_PAPER_MAX_RADIUS_PX\s*=\s*([0-9.]+)", debug_vert)
        self.assertIsNotNone(paper_scale)
        self.assertIsNotNone(paper_min)
        self.assertIsNotNone(paper_max)
        self.assertLessEqual(float(paper_scale.group(1)), 0.95)
        self.assertLessEqual(float(paper_min.group(1)), 2.5)
        self.assertLessEqual(float(paper_max.group(1)), 9.0)
        self.assertNotIn("paperBillboard", debug_vert)
        self.assertNotIn("diskCenterView4.xyz + vec3(corner.x", debug_vert)
        self.assertIn("vDiskUV = corner;", debug_vert)
        self.assertIn("if (r2 > 1.0)", debug_frag)
        self.assertIn("discard;", debug_frag)
        self.assertRegex(debug_frag, r"paperAlpha\s*=.*0\.[0-7][0-9]")

    def test_final_composite_preserves_bleed_chroma_after_neutral_gather_mix(self) -> None:
        apply = read("shaders/surfel_gi/apply_indirect.comp")
        compose = function_body(apply, "ComposeColorBleedWithIrradiance")

        self.assertIn("bleedTint", compose)
        self.assertIn("chromaTransfer", compose)
        self.assertIn("tintBlend", compose)
        self.assertIn("tintedBase", compose)
        self.assertIn("maxAddedBleedLum", compose)
        self.assertIn("boundedBleed", compose)
        self.assertIn("targetLum", compose)
        self.assertNotIn("irradiance += colorBleed;", apply)
        self.assertIn(
            "vec3 irradiance = ComposeColorBleedWithIrradiance(preliminaryIrradiance, colorBleed, bleedSupport);",
            apply,
        )
        self.assertIn("imageStore(uOutIndirect, pixel, vec4(colorBleed, bleedSupport));", apply)

    def test_final_gather_converts_source_cache_to_bounce_not_neutral_irradiance_copy(self) -> None:
        apply = read("shaders/surfel_gi/apply_indirect.comp")
        gather_weight = function_body(apply, "ReceiverSurfelGatherWeight")
        gathered_irradiance = function_body(apply, "SurfelGatheredIrradianceForReceiver")

        self.assertIn("SURFEL_APPLY_COPLANAR_SELF_TRANSFER_SCALE", apply)
        self.assertIn("coplanarFacing = max(dot(receiverNormal, surfelNormal), 0.0) * SURFEL_APPLY_COPLANAR_SELF_TRANSFER_SCALE", gather_weight)
        self.assertNotIn("coplanarFacing = max(dot(receiverNormal, surfelNormal), 0.0) * 0.85", gather_weight)

        self.assertIn("sourceAlbedo", gathered_irradiance)
        self.assertIn("sourceDiffuseBounce", gathered_irradiance)
        self.assertIn("sameSurfaceAttenuation", gathered_irradiance)
        self.assertNotIn("return mix(transferIrradiance, cachedIrradiance, sameSurfaceBlend);", gathered_irradiance)

    def test_surfel_composite_remains_bounded_irradiance_not_white_lift(self) -> None:
        apply = read("shaders/surfel_gi/apply_indirect.comp")
        deferred = read("shaders/deferred_lighting_frag.glsl")
        compose = function_body(apply, "ComposeColorBleedWithIrradiance")
        surfel_response = function_body(deferred, "ComputeSurfelGIResponse")

        visibility_boost = re.search(r"SURFEL_FINAL_BLEED_VISIBILITY_BOOST\s*=\s*([0-9.]+)", apply)
        luma_scale = re.search(r"SURFEL_FINAL_BLEED_MAX_LUMA_SCALE\s*=\s*([0-9.]+)", apply)
        self.assertIsNotNone(visibility_boost)
        self.assertIsNotNone(luma_scale)
        self.assertLessEqual(float(visibility_boost.group(1)), 1.35)
        self.assertLessEqual(float(luma_scale.group(1)), 1.65)
        self.assertIn("maxAddedBleedLum", compose)
        self.assertIn("targetLum", compose)
        self.assertIn("maxChromaPreservedLum", surfel_response)
        self.assertNotIn("safeBase * neutralRetention + safeBleed + chromaCarriedBase", compose)

        self.assertIn("INV_PI", surfel_response)
        self.assertNotIn("preintegrated indirect diffuse response", surfel_response)
        self.assertNotIn("bleedResponseFloor", surfel_response)

    def test_ray_tracing_prefers_real_geometry_before_surfel_fallback(self) -> None:
        trace = read("shaders/surfel_gi/trace_rays.comp")
        trace_body = function_body(trace, "TraceSurfelGrid")

        screen_index = trace_body.index("TraceScreenSpace")
        bvh_index = trace_body.index("TraceSoftwareBVHRadiance")
        fallback_index = trace_body.index("uUseSurfelFallbackTrace")
        self.assertLess(screen_index, fallback_index)
        self.assertLess(bvh_index, fallback_index)
        self.assertNotIn("% SURFEL_TRACE_BVH_FALLBACK_PHASE_COUNT", trace_body)
        self.assertIn("uBVHTraceStride", trace)
        self.assertIn("useBVHThisRay", trace_body)
        self.assertIn("counters.allocatedRays", trace)
        self.assertNotIn("uint activeRayLimit = min(uActiveRayCount, uMaxRays);", trace)
        self.assertIn("hits[rayID].ids", trace)
        self.assertIn("sourceDirectSample ? 1u : 0u", trace)

    def test_source_direct_bootstrap_has_visible_transport_energy(self) -> None:
        trace = read("shaders/surfel_gi/trace_rays.comp")

        match = re.search(r"SURFEL_SOURCE_DIRECT_BOUNCE_SCALE\s*=\s*([0-9.]+)", trace)
        self.assertIsNotNone(match)
        self.assertGreaterEqual(float(match.group(1)), 1.0)
        self.assertLessEqual(float(match.group(1)), 2.5)

    def test_source_direct_bootstrap_is_deterministic_not_ray_count_scaled(self) -> None:
        trace = read("shaders/surfel_gi/trace_rays.comp")
        integrate = read("shaders/surfel_gi/integrate.comp")

        self.assertNotIn("sourceDirectEstimatorScale", trace)
        self.assertNotIn("encodedSourceBounce", trace)
        self.assertNotIn("* (samplePdf / sampleCos)", trace)
        self.assertIn("sourceDirectSample", trace)
        self.assertIn("gather.radiance = sourceDirectBounce;", trace)
        self.assertIn("hit ? gather.hitKind : SURFEL_TRACE_MISS_KIND", trace)

        self.assertIn("sourceDirectHit ? SurfelClampRadiance(hit.radiance_pdf.rgb)", integrate)
        self.assertIn(": SurfelClampRadiance(hit.radiance_pdf.rgb * (sampleCos / samplePdf));", integrate)

    def test_temporal_integration_and_sharing_allow_supported_lift(self) -> None:
        allocate = read("shaders/surfel_gi/allocate_rays.comp")
        integrate = read("shaders/surfel_gi/integrate.comp")
        sharing = read("shaders/surfel_gi/irradiance_sharing.comp")

        self.assertNotIn("(uMaxRayBudget * 3u) / 4u", allocate)
        self.assertIn("uint budget = uMaxRayBudget;", allocate)

        self.assertIn("hit.ids.w != 0u", integrate)
        self.assertIn("atomicAdd(counters.acceptedRaySamples", integrate)
        self.assertIn("atomicAdd(counters.temporalClampEvents", integrate)
        self.assertIn("preUpdateDeviation", integrate)

        self.assertNotIn("centerLum < 0.025 && neighborLum > 0.12 ? 0.0 : 1.0", sharing)
        self.assertIn("neighborSupport", sharing)
        self.assertIn("atomicAdd(counters.sharingAppliedSurfels", sharing)

    def test_surfel_msmE_state_is_persistent_and_used_for_integration(self) -> None:
        resources_h = read("src/surfel_gi/SurfelGIResources.h")
        common = read("shaders/surfel_gi/common.glsl")
        spawn = read("shaders/surfel_gi/spawn.comp")
        integrate = read("shaders/surfel_gi/integrate.comp")

        self.assertIn("glm::vec4 msme", resources_h)
        self.assertIn("offsetof(Surfel, msme)", resources_h)
        self.assertIn("vec4 msme;", common)

        self.assertIn('#include "msme.comp"', spawn)
        self.assertIn("SurfelMSMEInitialize(surfel", spawn)

        self.assertIn('#include "msme.comp"', integrate)
        self.assertIn("SurfelMSMELoad(surfel)", integrate)
        self.assertIn("SurfelMSME(estimatorSample", integrate)
        self.assertIn("SurfelMSMEStore(surfel", integrate)
        self.assertNotIn("mix(previousIrradiance, surfel.shortMean.rgb, alpha)", integrate)


if __name__ == "__main__":
    unittest.main()
