# Principled Shading Contract (Refactor Source of Truth)

This document consolidates the constraints and parameter contract extracted from:
- docs/engine_refactor_execution.md
- docs/remaining_systems_completion_notes.md
- docs/transmission_completion_audit.md
- /memories/session/material_system_inspection.md

It is intentionally implementation-focused and scoped to the current engine architecture.

## 1. Pipeline Constraints

- Deferred, forward-transparent, and RT paths must share one canonical BRDF contract.
- Shared BRDF helpers live in shaders/includes/pbr_common.glsl and are the only source for core lobe evaluation.
- CPU-side canonical material data is MaterialDesc in src/MeshComponent.h.
- Transparent rendering keeps shared reflection/specular BRDF and adds a separate medium transmission term.
- G-buffer must preserve principled data required by deferred and RT consumers (clearcoat and principled extras are already allocated).

## 2. Canonical Material Parameters

Core metallic-roughness:
- baseColorFactor (vec4)
- metallicFactor [0,1]
- roughnessFactor [0,1]
- alphaCutoff [0,1]

Specular and clearcoat:
- specularFactor [0,1]
- specularColorFactor (vec3)
- clearcoatFactor [0,1]
- clearcoatRoughnessFactor [0,1]

Transmission and volume:
- transmissionFactor [0,1]
- ior >= 1.0
- thicknessFactor >= 0
- attenuationDistance >= 0 (infinity means no attenuation)
- attenuationColor (vec3)

Supporting factors:
- emissiveFactor (vec3)
- emissiveStrength >= 0
- occlusionStrength [0,1]
- normalScale >= 0

## 3. BRDF Model Requirements

The shared BRDF uses:
- GGX/Trowbridge-Reitz normal distribution function D
- Smith height-correlated visibility term V
- Schlick Fresnel F

Direct specular term:
- f_spec = D * V * F

Disney/Burley diffuse term:
- diffuse follows Burley diffuse response with optional subsurface blend path

Transmission weighting:
- k_t = transmissionFactor * (1 - F_avg(NdotV))

Clearcoat layering:
- clearcoat is a separate dielectric lobe with base-layer attenuation

## 4. Volume Transmission Requirements

Beer-Lambert attenuation:
- sigma_a = -log(attenuationColor) / attenuationDistance
- T = exp(-sigma_a * pathLength)

Practical contract for this engine:
- Forward transparent path composites with dual-source blending (`dst = color + dst * transmittance`): the background seen along the view ray is attenuated per channel by coverage, `transmission * (1 - F)`, base color and Beer-Lambert absorption. Refraction offsets are not simulated.
- RT path applies Beer-Lambert transmittance for refracted bounces when traversing medium thickness.
- Reflection/specular remains on shared BRDF path; transmission is additive and Fresnel-weighted.

## 5. Refactor Guardrails

- Prefer minimal changes and preserve existing pass interfaces unless required for correctness.
- Do not introduce new abstraction layers without clear reuse value.
- Keep behavior deterministic and consistent between deferred, forward, and RT where data availability overlaps.
- Avoid undocumented assumptions; if unavailable in a pass (for example, missing packed fields), use explicit documented defaults.
