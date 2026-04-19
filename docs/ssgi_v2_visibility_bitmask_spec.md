# SSGI v2 Visibility Bitmask Spec

**Path:** `docs/ssgi_v2_visibility_bitmask_spec.md`  
**Status:** Source of truth for all agents and passes implementing SSGI v2 visibility bitmask.

## 1) Scope and intent

This document defines the canonical contracts for SSGI v2 visibility bitmask integration. All pipeline passes, shader headers, debug tools, and agent-authored changes **must** comply with this spec.

Every pass header comment touching SSGI v2 dataflow **must include a link/reference** to:

`docs/ssgi_v2_visibility_bitmask_spec.md`

---

## 2) Coordinate-space conventions

These conventions are mandatory and globally consistent.

### 2.1 Depth encoding and linear depth convention

- **Primary depth input:** hardware depth buffer sampled from the current frame.
- **Stored depth semantic:** non-linear device depth in `[0, 1]` when sourced directly from depth texture.
- **Linearization output:** positive **view-space distance from camera** in world units.
- **Linear depth sign convention:**
  - Linear depth is always non-negative (`>= 0`).
  - `0` corresponds to camera origin.
  - Increasing value means farther from camera.
- Any pass requiring linear depth must explicitly linearize from device depth using the active projection parameters and must not assume clip-space linearity.

### 2.2 View-space handedness

- **Canonical view space:** right-handed view space with camera looking along **-Z**.
- Therefore:
  - Visible geometry in front of camera has `viewZ < 0`.
  - Linear depth uses `-viewZ` (positive distance).
- If a backend uses a different convention internally, adaptation must occur at pass boundary so all shared buffers follow this canonical convention.

### 2.3 Normal encoding/decoding

- **Working normal space:** view space.
- **Decoded normal range:** each component in `[-1, 1]` before normalization.
- **Normalization rule:** decoded normals must be renormalized in shader before use.
- **Storage contract:** if packed into UNORM textures, unpack with `n = packed * 2.0 - 1.0` then normalize.
- **Fallback invalid normal:** if decode length is near-zero, use `(0, 0, 1)` in tangent/local code path or `(0, 0, -1)` in view-facing path according to pass expectation; document the chosen fallback in pass-local comments.

---

## 3) Ownership boundaries by stage

### 3.1 Gather pass (SSGI v2 visibility bitmask gather)

**Owns:**
- Primary ray marching / sector visibility sampling.
- Visibility bitmask generation and per-pixel raw GI estimate.
- Current-frame confidence/validity outputs tied to sampling.

**Does not own:**
- Temporal reprojection decisions beyond writing data needed for reprojection.
- Spatial denoise filtering policy.
- Upscale policy.
- Final scene composition.

### 3.2 Reprojection pass

**Owns:**
- Motion-vector-based reprojection of **raw GI history domain data**.
- Disocclusion tests and history validity gate.
- History blend factor application (when enabled).

**Does not own:**
- New GI sampling.
- Spatial denoise kernels.
- Composite blending with lighting.

### 3.3 Denoise pass

**Owns:**
- Spatial and/or temporal-stabilized filtering on approved reprojection outputs.
- Edge-stopping logic using depth/normal thresholds.

**Does not own:**
- Reprojection validity logic (consumes it).
- Upscale strategy.
- Final composite tone/lighting policy.

### 3.4 Upscale pass

**Owns:**
- Resolution transfer from GI working resolution to target shading/composite resolution.
- Edge-aware reconstruction policy.

**Does not own:**
- New temporal accumulation.
- Scene-light compositing choices.

### 3.5 Composite pass

**Owns:**
- Integration of final SSGI contribution into lighting stack.
- Artistic intensity controls and clamps at composition time.

**Does not own:**
- Rebuilding GI from post-processed color.
- Reprojection or history writes.

---

## 4) Temporal ownership rules

### 4.1 Raw GI history (required path)

- History buffers store **raw/reprojection-domain GI**, not post-composited lighting.
- The reprojection pass is sole owner of reading prior raw GI history and writing next history state.
- Gather writes current-frame candidates; reprojection arbitrates history acceptance.

### 4.2 Optional radiance reinjection (optional path)

- Radiance reinjection is optional and must be feature-gated.
- If enabled, reinjected radiance is treated as auxiliary input to gather/denoise, **not** as authoritative replacement for raw GI history.
- Raw GI history remains canonical temporal source; reinjection must never mutate ownership boundaries.

### 4.3 Prohibited temporal coupling

- No pass may implicitly read/write another stage's private temporal buffers.
- Composite pass must not author any temporal GI state.

---

## 5) Buffer contracts

All buffers must declare format, resolution domain, mip policy, lifetime, and clear value at creation site.

### 5.1 Naming and contract template

Each SSGI v2 buffer declaration should document:

- **Name**
- **Format**
- **Resolution domain:** full / half / quarter (relative to main render size)
- **Mip policy:** none / full-chain / explicit levels
- **Lifetime:** transient-per-frame / ping-pong persistent / multi-frame persistent
- **Clear value**

### 5.2 Canonical buffer set

Recommended canonical minimum:

1. **`SSGI_V2_RawGI_Current`**
   - Format: `RGBA16F` (or `RGB16F` where alpha unused)
   - Resolution: half (default)
   - Mips: none
   - Lifetime: transient-per-frame
   - Clear: `(0,0,0,0)`

2. **`SSGI_V2_VisibilityBitmask`**
   - Format: `R32_UINT` (preferred) or `RG16_UINT` equivalent packing
   - Resolution: half (match gather domain)
   - Mips: none
   - Lifetime: transient-per-frame
   - Clear: `0`

3. **`SSGI_V2_HistoryGI_A/B`** (ping-pong)
   - Format: `RGBA16F`
   - Resolution: half (match reprojection domain)
   - Mips: none
   - Lifetime: persistent across frames
   - Clear: `(0,0,0,0)` on reset/disocclusion-full-reset

4. **`SSGI_V2_HistoryValidity_A/B`**
   - Format: `R8_UNORM` or `R16F`
   - Resolution: half
   - Mips: none
   - Lifetime: persistent ping-pong
   - Clear: `0`

5. **`SSGI_V2_Denoised`**
   - Format: `RGBA16F`
   - Resolution: half
   - Mips: optional (explicit if used by upscale)
   - Lifetime: transient-per-frame
   - Clear: `(0,0,0,0)`

6. **`SSGI_V2_Upscaled`**
   - Format: `RGBA16F` (or scene-linear compatible composite format)
   - Resolution: full
   - Mips: none
   - Lifetime: transient until composite completes
   - Clear: `(0,0,0,0)`

### 5.3 Reset behavior

On camera cut / history reset event:
- Clear both history GI targets and validity targets to contract clear values before next reprojection.
- Treat first frame after reset as no-history-valid.

---

## 6) Debug output schema

Debug outputs must be stable and machine-comparable where practical.

### 6.1 Required debug views

1. **Raw GI radiance** (`DebugSSGIv2_RawGI`)
   - Expected range: scene-dependent, typically `[0, 10]` scene-linear.

2. **Visibility bit count / occupancy** (`DebugSSGIv2_BitmaskOccupancy`)
   - Expected normalized range: `[0,1]` where `1 = all sectors set`.

3. **History validity** (`DebugSSGIv2_HistoryValidity`)
   - Expected range: `[0,1]`.

4. **Reprojection confidence** (`DebugSSGIv2_ReprojectionWeight`)
   - Expected range: `[0,1]`.

5. **Denoised GI** (`DebugSSGIv2_Denoised`)
   - Expected range: scene-dependent, should preserve energy trends from Raw GI with reduced variance.

6. **Upscaled GI** (`DebugSSGIv2_Upscaled`)
   - Expected range: scene-dependent, visually aligned with full-res geometry edges.

### 6.2 Out-of-range handling

- Debug shaders should optionally highlight NaN/Inf and negative radiance (if disallowed by pipeline policy) in magenta.
- Any normalized diagnostic channel must clamp display to `[0,1]` while preserving raw values in telemetry buffers when available.

---

## 7) Quality defaults (mandatory defaults)

Unless a platform/profile explicitly overrides, use:

- **Slices:** `4`
- **Steps:** `4`
- **Radius:** `4.0`
- **Thickness:** `0.5`
- **Sectors:** `32`
- **Jitter:** low-discrepancy sequence (e.g., R2/Hammersley class), deterministic per-frame index with wraparound.

These defaults are the baseline for parity and debugging.

---

## 8) Explicit non-goals (hard constraints)

1. **No reuse of legacy gather logic/history/composite assumptions.**
   - Legacy SSGI behavior may inform reference comparisons only; it must not define v2 contracts.

2. **No sampling of post-processed backbuffer as GI source.**
   - GI source inputs must come from pre-postprocess scene-linear data and dedicated GI buffers only.

---

## 9) Pass header requirement (enforcement)

Every SSGI v2-related pass source/header comment must include a line equivalent to:

- `Spec: docs/ssgi_v2_visibility_bitmask_spec.md`

PRs that add or modify SSGI v2 passes without this reference are non-compliant.

---

## 10) Change control

- Any change to SSGI v2 inter-pass contracts must update this document in the same change set.
- If implementation and spec conflict, treat this document as authoritative until explicitly revised.
