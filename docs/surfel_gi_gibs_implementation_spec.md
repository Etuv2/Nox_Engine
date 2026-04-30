# Surfel GI (GIBS) Implementation Spec for Coding Agents

This document is an implementation-oriented explanation of **Global Illumination Based on Surfels (GIBS)** as presented in the **SIGGRAPH Advances 2021 - Surfel GI** material, expanded into a full engineering spec. It is written for LLM coding agents and software engineers who need a practical, end-to-end plan rather than a high-level overview.

The original GIBS talk describes a production real-time **diffuse indirect illumination** system that combines:

- **dynamic surfelization of visible geometry**
- **persistent world-space caching of irradiance**
- **hardware ray tracing for sparse lighting queries**
- **temporal accumulation with variance-aware blending**
- **specialized data structures for scalable lookup and memory control**

The talk gives many of the core ideas, but it does **not** fully specify every data layout, threshold, equation, or pass boundary needed to build the system from scratch. This document therefore does two things:

1. It records the **source-backed details** from the talk and related surfel literature.
2. It proposes **implementation defaults** where the talk is silent.

Whenever a detail is directly from the talk or uploaded literature, it is marked **Source-backed**. Whenever it is a practical design choice needed to complete the implementation, it is marked **Recommended default**.

---

## 1. What problem GIBS solves

Traditional real-time GI techniques usually fall into one of these groups:

- **screen-space GI**: cheap, but loses all off-screen information and becomes unstable when the camera changes
- **probe grids / DDGI**: persistent and stable, but store lighting in empty space and can struggle with thin geometry and high-detail surfaces
- **voxel GI / voxel cone tracing**: fully dynamic, but expensive in memory and resolution, with common light leaking and thin-geometry issues
- **full path tracing**: physically faithful, but too expensive for mainstream real-time budgets unless heavily denoised and simplified

GIBS uses **surfels** instead. A surfel is a small oriented surface sample with:

- world position
- normal
- radius
- cached irradiance
- auxiliary state for temporal integration and artifact suppression

Instead of caching GI in volume space, GIBS caches it **on surfaces**. This avoids empty-space waste and lets the system spend rays only where geometry actually exists. The key idea is that surfels are:

- **spawned on demand from the G-buffer**
- **persistent across frames**
- **updated as attached geometry moves**
- **recycled when they become irrelevant**

That persistence is what makes the technique powerful: once a surface region has been surfelized and partially solved, the work is not thrown away just because the camera looks elsewhere. This is the major advantage over screen-space GI, and one of the major advantages over pure per-frame ray tracing.

**Source-backed:** the official EA material describes GIBS as a real-time indirect diffuse GI solution that combines ray tracing with discretized scene geometry to cache and amortize lighting calculations across time and space, requires no prebake, no special meshes, and no special UVs, and is built for arbitrary scene scale and dynamic content. The uploaded talk also states that surfels are spawned from the G-buffer, persist across frames, and are used to cache irradiance on opaque surfaces. [Refs 1, 2]

---

## 2. Core design goals

Your implementation should preserve these goals:

1. **Dynamic scene support**  
   No prebake. Geometry, lighting, characters, and emissives can move.

2. **Scale independence**  
   Near surfaces get denser surfels. Distant surfaces get fewer, larger surfels.

3. **Persistence**  
   Lighting should survive camera motion and continue converging over time.

4. **Ray budget control**  
   Expensive ray queries must be sparse and prioritized.

5. **Stable memory footprint**  
   Surfel memory must be fixed-capacity and GPU-managed.

6. **Leak resistance**  
   Surfels must not apply lighting through walls just because they are close in Euclidean distance.

7. **Agent-friendly modularity**  
   The codebase should split into clean passes and buffers so each subsystem can be debugged independently.

---

## 3. System overview

At a high level, implement GIBS as these stages every frame:

1. **Render G-buffer**
2. **Spawn new surfels from under-covered visible pixels**
3. **Update persistent surfel transforms**
4. **Recycle stale surfels**
5. **Insert surfels into the spatial acceleration structure**
6. **Estimate requested ray count per surfel**
7. **Allocate actual ray budget globally**
8. **Generate and bin rays**
9. **Trace rays**
10. **Shade ray hits**
11. **Update surfel irradiance and auxiliary data**
12. **Filter or share irradiance locally if needed**
13. **Apply surfel lighting to the main shading pass**
14. **Handle transparent / unsuitable geometry with probe fallback**

### 3.1 Minimal pass graph

```text
Depth Prepass / GBuffer
    -> Surfel Spawn Pass
    -> Surfel Transform Update Pass
    -> Surfel Recycle Pass
    -> Surfel Grid Clear + Build Pass
    -> Surfel Ray Request Pass
    -> Global Ray Allocation Pass
    -> Ray Generation Pass
    -> Ray Binning / Sorting Pass
    -> Ray Trace Pass
    -> Ray Hit Lighting Pass
    -> Surfel Integrate / MSME Pass
    -> Surfel Neighborhood Share / Filter Pass
    -> Final Lighting Apply Pass
    -> Transparent Probe Pass (parallel subsystem)
```

---

## 4. Surfel representation

A coding agent should treat surfels as persistent records, not temporary shading samples.

## 4.1 Required surfel fields

Use a GPU-compact struct similar to this.

```cpp
struct Surfel
{
    // identity / lifecycle
    uint32_t surfelId;
    uint32_t flags;              // alive, spawnedThisFrame, markedForRecycle, etc.
    uint32_t transformId;        // parent transform / bone id
    uint32_t lastVisibleFrame;
    uint32_t lastContributedFrame;
    uint32_t ageFrames;

    // geometric state
    float3 localPosition;        // stored in parent local space
    float3 localNormal;
    float3 worldPosition;
    float3 worldNormal;
    float  radiusWorld;

    // material / shading
    float3 albedo;
    float3 emissive;
    float  opacityClass;         // usually 1 for opaque surfels

    // irradiance state
    float3 irradianceMeanLong;   // long-term estimate
    float3 irradianceMeanShort;  // short-term estimate
    float3 irradianceVarShort;   // short-term variance estimate

    // requested / allocated work
    uint32_t requestedRays;
    uint32_t allocatedRays;

    // ray guiding
    uint16_t guidingScale;       // shared normalization scale
    uint8_t  guidingTex[6*6*3];  // example packed RGB; layout may vary

    // leak suppression
    // 4x4 radial depth moments per hemisphere texel is a good default
    float depthMean[16];
    float depthMeanSq[16];

    // grid / neighborhood cache
    uint32_t cellIndex;
    uint32_t cellLocalSlot;

    // optional
    float3 debugColor;
};
```

### 4.2 Notes

- **Source-backed:** the talk explicitly defines surfels by position, radius, and normal, and describes per-surfel irradiance caching, variance tracking, ray guiding, and a radial depth function with stored mean and depth-squared moments. It also states the default radial depth resolution is **4x4 texels per surfel hemisphere**, and the ray-guiding texture is typically **6x6 texels** plus a scale value. [Ref 1]
- **Recommended default:** store both local-space and world-space data. This makes transform updates simple and keeps spawn independent from later motion.
- **Recommended default:** keep albedo and emissive on the surfel if you want direct relighting or emissive bounce to be stable even when the original mesh is no longer visible.
- **Recommended default:** use SOA or split-buffer storage in production. The struct above is conceptual. For real GPU performance, separate hot and cold data.

---

## 5. Surfel spawning from the G-buffer

The source talk makes this one of the key features of GIBS.

### 5.1 Source-backed algorithm

The screen is divided into **16x16 tiles**. For each tile:

1. Determine which visible texel currently has the **least surfel coverage**.
2. If the coverage is below a randomized threshold, spawn a surfel from the G-buffer sample at that texel.
3. Continue only until the tile has sufficient coverage.
4. Newly spawned surfels are persistent and no longer tied to the current view except for later lifecycle management.

The talk also states that surfels are sized so that their **screen-space projection is roughly constant**. As the camera approaches geometry, surfels shrink and more are spawned. As the camera moves away, surfels grow and excess coverage can be removed. [Ref 1]

### 5.2 Recommended practical spawn definition

Define a target projected radius in pixels:

```cpp
const float targetSurfelRadiusPx = 6.0f;   // tune 4..8
```

Given a perspective projection with focal length `f_px` in pixels and G-buffer depth `z_view`, estimate world radius:

```cpp
radiusWorld = targetSurfelRadiusPx * z_view / f_px;
```

Use the G-buffer fields:

- world position from depth reconstruction
- geometric normal
- material id / transform id / bone id
- albedo and emissive
- object id if available

### 5.3 Coverage estimation

The talk says “least coverage currently” but does not publish the exact coverage metric. A workable implementation is:

```cpp
coverage(texel) =
    sum over nearby surfels S:
        projectedDiscCoverage(S, texel)
```

Use a disk approximation in screen space:

```cpp
projectedRadiusPx = f_px * surfel.radiusWorld / surfel.viewDepth;
contribution = saturate(1 - distance(texel, surfelScreenPos) / projectedRadiusPx);
coverage += contribution;
```

### 5.4 Spawn rule

A good rule that matches the talk’s intent:

```cpp
if (minCoverageInTile < randomThreshold(tile, frame))
    spawnSurfelFromMinCoverageTexel();
```

Where:

```cpp
randomThreshold = lerp(0.6f, 1.0f, blueNoise(tile, frame));
```

This randomization prevents structured popping and distributes spawn work across frames.

### 5.5 Spawn initialization

For each new surfel:

- pop an id from the free-stack
- set lifecycle state
- store parent `transformId`
- convert world position and normal into parent local space
- initialize long and short irradiance to zero or a cheap first estimate
- initialize short variance high
- initialize radial depth means to `radiusWorld * 2` or just `radiusWorld` diameter, matching the talk’s “initialize to the diameter of the surfel”
- clear guiding texture
- boost initial requested rays for rapid convergence

```cpp
surfel.flags = Alive | SpawnedThisFrame;
surfel.ageFrames = 0;
surfel.lastVisibleFrame = currentFrame;
surfel.lastContributedFrame = currentFrame;
surfel.irradianceMeanLong = float3(0);
surfel.irradianceMeanShort = float3(0);
surfel.irradianceVarShort = float3(largeValue);
surfel.requestedRays = initialRayBoost;
```

### 5.6 Duplicate suppression

The talk visually implies gap filling rather than unrestricted spawning. You still need hard duplicate suppression. Use either:

- coverage metric only, or
- coverage metric + nearest existing surfel test in world space and normal space

Suggested check:

```cpp
reject if exists surfel N in neighboring cells:
    distance(N.pos, candidate.pos) < 0.5 * min(N.radius, candidate.radius)
    && dot(N.normal, candidate.normal) > 0.95
```

---

## 6. Persistence and transform following

Persistence is one of the defining properties of GIBS.

### 6.1 Source-backed behavior

The talk states that spawned surfels store a **transform identifier** for the geometry they are attached to. The G-buffer writes this id when the surfel is created. Frostbite keeps a **global transform buffer** that includes rigid transforms and skinned bones. Each frame, the surfel recomputes world position from:

- local position
- stored transform id
- current transform in the global transform buffer

For skinned meshes, the talk says the G-buffer writes the **bone with the highest skinning weight**, giving an effective one-bone approximation for surfel attachment. This is imperfect but “fairly forgiving” in practice. [Ref 1]

### 6.2 Update pass

Per frame:

```cpp
Transform T = globalTransformBuffer[surfel.transformId];
surfel.worldPosition = mul(T, float4(surfel.localPosition, 1)).xyz;
surfel.worldNormal   = normalize(mul((float3x3)T, surfel.localNormal));
surfel.ageFrames++;
```

### 6.3 Recommended defaults

- If the transform id is invalid because the source object was destroyed, mark the surfel for recycle.
- If transform motion is too large in one frame, reset short-term variance high so temporal estimators react faster.
- For high-quality characters, consider storing **two** bone ids plus weights. The talk uses one-bone for simplicity, but this is an implementation tradeoff, not a theoretical requirement.

---

## 7. Fixed-capacity memory and recycling

GIBS does not allow surfel count to grow unbounded.

### 7.1 Source-backed behavior

The talk states that all surfel resources are **allocated up front**, so the system has a fixed maximum surfel count. A GPU-side free stack stores available surfel ids.

- On spawn: decrement stack counter atomically and read an id.
- On recycle: increment stack counter atomically and write the freed id back.

Recycling probability uses factors including:

1. how many surfels are live
2. when the surfel last contributed to scene lighting
3. how far away the surfel is

The talk states these are combined and compared against a random value so less relevant surfels are more likely to be recycled. [Ref 1]

### 7.2 Recommended scoring function

A practical implementation:

```cpp
float livePressure   = liveSurfels / maxSurfels;
float staleVisible   = currentFrame - surfel.lastVisibleFrame;
float staleContrib   = currentFrame - surfel.lastContributedFrame;
float distanceScore  = saturate(distance(cameraPos, surfel.worldPosition) / maxKeepDistance);
float agePenalty     = saturate(surfel.ageFrames / warmupFrames);

float recycleScore =
      0.35f * livePressure
    + 0.20f * saturate(staleVisible / 120.0f)
    + 0.25f * saturate(staleContrib / 120.0f)
    + 0.20f * distanceScore
    - 0.10f * (1.0f - agePenalty);
```

Then:

```cpp
if (blueNoise(surfelId, currentFrame) < recycleScore)
    recycleSurfel(surfelId);
```

### 7.3 Hard recycle conditions

Also force recycle if:

- source transform no longer exists
- surfel is behind a far cull distance and memory pressure is high
- surfel never received meaningful contribution for many frames
- procedural / temporary geometry invalidated it

### 7.4 Avoiding destructive thrash

Do **not** recycle surfels aggressively just because they are off-screen for a short time. Persistence is the point of the algorithm. Prefer probabilistic pressure-driven recycling over hard screen-visibility eviction.

---

## 8. Spatial acceleration structure

This is the other defining subsystem of GIBS.

### 8.1 Why a uniform grid is not enough

The talk explains that the original PICA PICA implementation used a uniform grid, but this breaks down for large environments because distant surfels become large in world space if their projected screen size is held constant. To avoid discontinuities, the uniform cells would need to become huge, destroying lookup efficiency. [Ref 1]

### 8.2 Source-backed GIBS design

The production solution keeps:

- a **uniform grid** near the camera
- plus **trapezoidal grids** along each principal axis outside that center region

The slices get thicker with distance. This matches perspective scaling, so the grid keeps roughly constant “surfels per cell” behavior in screen space. The talk describes this as a **non-linear acceleration structure** whose trapezoidal grids behave like regular grids under a non-linear transform. [Ref 1]

### 8.3 Recommended implementation model

Use a camera-centered piecewise structure.

#### Region A: near uniform cube

```cpp
centerExtent = 16 m to 32 m per axis
baseCellSize = chosen from target projected surfel size at near-mid depth
uniformResolution = e.g. 32^3 or 48^3 logical cells
```

#### Region B: axis-aligned outer frusta / trapezoids

For each axis direction `+X, -X, +Y, -Y, +Z, -Z`, define slices whose thickness increases geometrically:

```cpp
sliceThickness[i] = baseCellSize * pow(growthFactor, i)
growthFactor      = 1.15 .. 1.30
```

Store prefix sums to convert from world distance to slice index.

Each slice can itself be subdivided in the two orthogonal axes at a lower effective resolution so that projected cell size remains near constant.

### 8.5 Cell contents

Each cell should contain:

- count of surfels
- start offset into a compact surfel index list
- optional averaged irradiance
- optional averaged normal
- optional visibility / light metadata
- optional neighbor aggregation data

```cpp
struct SurfelCell
{
    uint32_t start;
    uint32_t count;
    float3   avgIrradiance;
    float    avgWeight;
    float3   avgNormal;
    float    pad0;
};
```

### 8.6 Insertion

The talk says surfels are inserted every frame and also inserted into **immediate neighboring cells** if their radius overlaps those cells. It also guarantees surfel radius is never larger than a cell side in the original regular-grid explanation. [Ref 1]

Recommended procedure:

1. Compute primary cell index from world position.
2. Insert surfel into that cell.
3. For each adjacent cell:
   - if surfel sphere overlaps cell AABB, insert there too.

This is important for lookup continuity and avoids discontinuities at cell boundaries.

### 8.7 Build approach

Use a GPU two-pass build:

1. **Count pass**
   - each surfel computes all overlapped cells
   - atomic increment per cell count

2. **Prefix-sum pass**
   - exclusive scan on counts to get `start`

3. **Scatter pass**
   - each surfel computes overlapped cells again
   - atomic increment cell-local cursor
   - write surfel index into compact index array

---

## 9. Applying surfel lighting to pixels

The talk explicitly describes this pass.

### 9.1 Source-backed behavior

In the final pass:

1. reconstruct world position of each shaded pixel
2. find the containing cell
3. fetch the first `N` surfels in that cell
4. accumulate their irradiance weighted by:
   - distance from surfel to shaded point
   - orientation of surfel relative to shaded point and pixel normal/orientation
5. if the total contribution of these surfels is less than one, add the weighted average irradiance of the grid cell as fallback

This cell-average fallback helps fill gaps and stabilize lighting. [Ref 1]

### 9.2 Recommended weight function

For pixel `x` with normal `n_x`, and surfel `s`:

```cpp
float3 d     = x - s.worldPosition;
float  dist2 = dot(d, d);
float  dist  = sqrt(dist2);
float3 dir   = d / max(dist, 1e-5);

float w_dist   = saturate(1.0f - dist / s.radiusWorld);
float w_surfel = saturate(dot(s.worldNormal, dir));
float w_pixel  = saturate(dot(n_x, -dir));
float w = w_dist * w_surfel * w_pixel;
```

Then:

```cpp
accum += w * s.irradianceMeanLong;
sumW  += w;
```

If `sumW < 1` then blend in the cell average:

```cpp
accum += (1 - sumW) * cell.avgIrradiance;
```

### 9.3 Practical notes

- Clamp to first `N` surfels for cost control. Start with 16 or 24.
- Sort cell surfels by recency, contribution, or distance if needed.
- Use TAA after this pass; the talk repeatedly assumes temporal filtering exists.

---

## 10. Leak suppression with a radial depth function

This is one of the most important implementation details from the talk.

### 10.1 The problem

A surfel outside a wall can incorrectly light geometry on the inside if the two surfaces are close enough spatially. Since a surfel by itself has only a point, normal, and radius, it does not inherently know about nearby occluding geometry.

### 10.2 Source-backed solution

The talk solves this with a **radial depth function**:

- initialize the depth function to the **diameter** of the surfel
- as rays are traced for irradiance gathering, if they hit geometry within the surfel diameter, update the depth function
- store both:
  - moving average of depth
  - moving average of depth squared
- reconstruct mean and variance
- use **Chebyshev’s inequality** for a smooth depth test
- default resolution is **4x4 texels per surfel hemisphere**

The talk explicitly states this is inspired by **Variance Shadow Maps** and **DDGI**, but only stores depth in the surfel-local hemisphere within the surfel diameter. [Ref 1]

### 10.3 Recommended parameterization

Represent the hemisphere in local surfel tangent space using octahedral or hemi-octahedral mapping.

```cpp
float2 uv = HemiOctEncode(localHitDirection);
int2   ij = clamp(int2(uv * 4.0), 0, 3);
int    k  = ij.y * 4 + ij.x;
```

Update moments with exponential moving average:

```cpp
float alpha = 0.1f; // fast enough to react, stable enough not to flicker
depthMean[k]   = lerp(depthMean[k],   hitDistance, alpha);
depthMeanSq[k] = lerp(depthMeanSq[k], hitDistance * hitDistance, alpha);
```

### 10.4 Chebyshev visibility test

For candidate shading point distance `t` along local direction bucket `k`:

```cpp
float mu   = depthMean[k];
float mu2  = depthMeanSq[k];
float var  = max(mu2 - mu * mu, 1e-4f);

float d = t - mu;
float p = var / (var + d * d);  // upper-bound style
```

Then use:

```cpp
float occlusionFactor = (t <= mu) ? 1.0f : p;
```

Interpretation:

- near expected depth -> accepted
- well behind expected depth -> rejected or strongly attenuated

### 10.5 Usage in final apply pass

When evaluating a surfel’s contribution to a shaded point:

1. transform pixel position into surfel-local tangent space
2. find hemisphere bucket
3. compute radial distance
4. use moment test to attenuate the contribution

```cpp
w *= occlusionFactor;
```

This is the main anti-bleed mechanism.

---

## 11. Irradiance integration on surfels

This is where the actual GI is solved.

### 11.1 Source-backed behavior

For each surfel, rays are shot into the scene. At ray hit points, the system evaluates:

- direct diffuse lighting
- shadowing against scene lights
- existing surfel lighting at the hit point

The talk says this yields **effectively infinite bounce over time**, as long as surfel coverage exists. [Ref 1]

This is the crucial recursive idea:

- ray hits read from the *previously solved* surfel cache
- the new sample is written back into the surfel cache
- repeated over frames, multi-bounce energy propagates through the scene

### 11.2 One-sample estimate

For surfel `s`, each ray sample contributes an estimate of incoming diffuse irradiance:

```cpp
L_sample =
    directDiffuseAtHit(hitPoint, hitNormal)
  + surfelIndirectAtHit(hitPoint, hitNormal);
```

If sampling is cosine-weighted around the surfel normal, the Monte Carlo estimator for diffuse irradiance can be simplified:

```cpp
E_hat = (1 / M) * sum_i L_sample_i
```

if the cosine-weighted BRDF/pdf cancellation is already accounted for.

Otherwise the general estimator is:

```cpp
E_hat = (1 / M) * sum_i [ L_i * max(0, dot(n_s, wi)) / pdf(wi) ]
```

For cosine-weighted hemisphere sampling with `pdf = cos(theta)/pi`, this becomes:

```cpp
E_hat = pi * mean(L_i)
```

### 11.3 Recommended storage choice

Store **diffuse irradiance RGB** on each surfel, not outgoing radiance. This matches the talk’s language and makes final apply cheaper.

---

## 12. Temporal accumulation with variance-aware blending

This is the heart of the convergence strategy.

### 12.1 Source-backed behavior

The talk says they use a **modified moving average estimator**. A normal moving average forces a fixed blend tradeoff between responsiveness and convergence. GIBS instead tracks:

- a longer-term moving average
- a shorter-term mean
- a shorter-term variance

The short-term statistics modulate the blend factor of the long-term accumulator. This allows the system to:

- react quickly when lighting changes
- converge smoothly when the scene is stable

The literature review identifies this as a **multi-scale mean estimator (MSME)**. [Refs 1, 6]

### 12.2 Recommended implementation

Let:

- `L_t` = newly estimated irradiance this frame
- `M_long` = long-term mean
- `M_short` = short-term mean
- `V_short` = short-term variance

Update short-term statistics with larger alpha:

```cpp
const float aShort = 0.2f;
M_short = lerp(M_short, L_t, aShort);
float3 delta = L_t - M_short;
V_short = lerp(V_short, delta * delta, aShort);
```

Now derive a reactivity scalar from variance magnitude:

```cpp
float varianceScalar = max_component(V_short);
float reactive = saturate(varianceScalar / varianceScale);
```

Convert to long-term alpha:

```cpp
float aLong = lerp(alphaStable, alphaReactive, reactive);
// e.g. alphaStable = 0.02, alphaReactive = 0.35
```

Then update:

```cpp
M_long = lerp(M_long, L_t, aLong);
```

### 12.3 Reset triggers

Force `reactive = 1` or reinitialize short-term stats if:

- surfel just spawned
- source transform changed sharply
- surfel moved between cells
- local visibility classification changed a lot
- emissive state changed

---

## 13. Adaptive ray budgeting

The talk makes clear that surfels do not all get the same number of rays.

### 13.1 Source-backed behavior

Requested ray counts depend on:

- variance
- how recently the surfel contributed to visible lighting
- whether it just spawned

Content creators can set a **global total ray budget**. The system first runs a ray-count pass to accumulate requested work, then allocates actual rays proportionally in a later pass. Surfels with low variance can go nearly dormant and only send enough rays to detect change. [Ref 1]

### 13.2 Recommended request formula

```cpp
float varianceScore   = saturate(max_component(surfel.irradianceVarShort) / varianceScale);
float visibleScore    = exp(-0.02f * (currentFrame - surfel.lastContributedFrame));
float spawnScore      = surfel.flags & SpawnedThisFrame ? 1.0f : 0.0f;
float ageScore        = saturate(surfel.ageFrames / 16.0f);

float importance =
      0.50f * varianceScore
    + 0.25f * visibleScore
    + 0.25f * spawnScore;

importance *= lerp(1.5f, 1.0f, ageScore);

surfel.requestedRays = clamp(
    int(round(lerp(minRays, maxRays, importance))),
    minRays,
    maxRays);
```

Good starting range:

```cpp
minRays = 1;
maxRays = 8;
spawnBoost = 12 for first few frames if budget allows;
```

### 13.3 Budget normalization

If total requested rays exceed the global budget:

```cpp
allocated = floor(requested * globalBudget / totalRequested);
```

Preserve at least one ray for eligible surfels when possible.

---

## 14. Ray guiding

This is another major convergence accelerator in the talk.

### 14.1 Source-backed behavior

The talk notes that cosine-lobe importance sampling is not always enough. When most light comes from a narrow or unusual direction, many cosine-sampled rays are wasted. Their solution is per-surfel **ray guiding**:

- map the surfel hemisphere to a quad
- store a compact radiance function
- default storage is **6x6 texels**, **8 bits per component**, plus a **16-bit scale**
- normalize after each iteration to track relative radiance
- when the function is sufficiently populated, use it to guide future rays
- sample by inverse CDF style walking over the discrete distribution
- return both direction and PDF

The talk explicitly walks through importance sampling using the cumulative sum over the discrete 2D map. [Ref 1]

### 14.2 Recommended data model

Store a 6x6 luminance or RGB guide over the local surfel hemisphere using hemi-octahedral mapping.

For each traced ray that returns radiance `L` from direction `wi_local`:

1. map `wi_local` to `uv`
2. find texel
3. accumulate radiance estimate
4. renormalize occasionally

### 14.3 Updating the guide

Use EMA per texel:

```cpp
guide[k] = lerp(guide[k], incomingRadiance, guideAlpha);
```

Or accumulate and renormalize over a short window.

### 14.4 Sampling from the guide

Let `w[k]` be positive weights over the 36 texels.

```cpp
float sumW = sum_k w[k];
float u = random01() * sumW;

float accum = 0;
for k in 0..35:
    accum += w[k];
    if (accum >= u) { choose k; break; }
```

Then:

- sample a jittered point inside texel `k`
- map back to hemisphere direction
- pdf = `w[k] / sumW * 1 / texelSolidAngleApprox`

### 14.5 Mixture sampling

Do not fully replace cosine sampling. Use a mixture:

```cpp
with probability pGuide: sample from guide
else: sample cosine hemisphere
```

Then the combined PDF is:

```cpp
pdf = pGuide * pdfGuide + (1 - pGuide) * pdfCosine;
```

Start with `pGuide = 0` until the guide has enough confidence. Then ramp toward `0.5 .. 0.8`.

---

## 15. Irradiance sharing between neighboring surfels

The talk mentions this explicitly as a way to reduce perceived noise.

### 15.1 Source-backed behavior

Since surfels are independent, noise can remain blotchy. The talk says the acceleration structure is used to share information among neighboring surfels, especially when variance is high. This significantly reduces perceived noise. [Ref 1]

### 15.2 Recommended filter

Only share when the target surfel’s variance is high. Gather neighbors from the same or adjacent cells and use a bilateral-like filter:

```cpp
w = w_dist * w_normal * w_radius * w_variance;
```

Suggested terms:

```cpp
w_dist    = exp(-dist2 / (2 * sigmaPos2));
w_normal  = pow(saturate(dot(n_i, n_j)), 16);
w_radius  = saturate(min(r_i, r_j) / max(r_i, r_j));
w_var     = saturate(var_j / (var_i + var_j + eps));
```

Then blend neighbor irradiance into the short-term estimate, not directly into the long-term state.

---

## 16. Ray binning / sorting for traversal coherence

The talk says unordered rays hurt cache behavior and traversal efficiency, so they use a **ray binning** strategy similar to Battlefield 5. Rays are binned by position and orientation. The surfel cell coordinate converted to 1D acts as the dominant spatial component of the bin index. Then a second directional component is added from the shot ray direction. Reordering is done in multiple passes using bin counts and offsets. [Ref 1]

### 16.1 Recommended implementation

For each generated ray:

```cpp
uint spatialHash = surfel.cellIndex;
uint dirHash     = QuantizeDirectionOct(rayDir, dirBins);
uint binIndex    = spatialHash * dirBins + dirHash;
```

Then:

1. count rays per bin
2. prefix sum counts
3. scatter rays into a sorted ray array

This improves BVH / RTAS coherence, especially on GPUs.

---

## 17. Many-light sampling at ray hits

The talk includes substantial production detail here.

### 17.1 Source-backed options

At ray hits, direct diffuse lighting must be computed against potentially many lights. The talk discusses:

- **brute-force random light sampling**: too noisy for many lights
- **stochastic lightcuts**
- **reservoir sampling** inspired by ReSTIR

The lightcuts implementation:

- stores light positions in view space
- sorts lights by Morton code
- builds a tree bottom-up where internal nodes store combined bounds and intensities
- chooses a cut with a small node limit (typically **2-8 nodes**)
- then traverses stochastically to leaf lights using importance weights

The reservoir approach:

- randomly samples **4-8 lights**
- chooses a single winner using weighted reservoir logic
- normalizes the resulting PDF by total sampled weight

The talk says reservoir sampling is cheaper on consoles in their setup, while stochastic lightcuts can converge faster but be more expensive. [Ref 1]

### 17.2 Recommended advice

If building a first implementation:

- start with reservoir light sampling
- defer lightcuts until the rest of GIBS is stable

At each ray hit:

```cpp
for i in 1..NcandidateLights:
    sample random light
    compute importance weight
    update reservoir
trace shadow ray to winning light
evaluate direct diffuse if visible
```

This keeps the integration system simple.

---

## 18. Transparency and “geometry that does not fit surfels”

The talk says surfels are a good fit for **opaque surfaces**, but not for transparent geometry because surfels are spawned from the G-buffer, which is naturally biased toward the primary visible opaque surface. Their fallback is a **probe volume** solved with a similar temporal integrator.

### 18.1 Source-backed design

For transparents:

- use ray-traced probes instead of surfels
- gather diffuse radiance into probes
- project probe radiance to **spherical harmonics**
- accumulate those SH coefficients across frames using the same adaptive MSME-style integrator
- store results in a **probe volume**
- solve scale using **volume clipmaps**
- shift clipmaps when the camera moves
- initialize newly exposed probes from higher levels by interpolation
- blend or dither between adjacent clip levels to avoid boundary popping
- the talk mentions **blue-noise dithered sampling** as a cheaper alternative to two-level blending

It also states the integrator computes the number of rays to shoot based on variance, and Sloan’s deringing window is used to reduce SH ringing. [Ref 1]

### 18.2 Recommended boundary of responsibility

Do **not** force surfels to solve transparency directly unless you are deliberately researching a new extension. Use:

- GIBS surfels for opaque diffuse GI
- clipmapped SH probes for transparency / foliage / unsupported cases

---

## 19. What the original technique does not fully specify

A coding agent must know which parts are not fully public.

The talk does **not** fully publish:

- exact spawn threshold equations
- exact coverage metric
- exact non-linear grid mapping function
- exact MSME equations and parameters
- exact irradiance-sharing filter
- exact many-light PDFs and error heuristics
- exact final shading weight function
- exact memory layouts used in Frostbite

Therefore a complete implementation must contain deliberate engineering defaults. That is normal. The implementation is still faithful if it preserves the same architecture:

- G-buffer opportunistic surfel spawn
- persistence
- transform-following
- fixed-capacity recycling
- non-linear spatial lookup
- sparse ray solving
- variance-aware temporal accumulation
- ray guiding
- radial depth leak suppression
- probe fallback for transparency

---

## 20. A complete recommended frame algorithm

This is the most practical section for coding agents.

### 20.1 Pass A: G-buffer

Output at least:

- depth
- world normal
- albedo
- emissive
- transform id / bone id
- object id
- motion vectors if available

### 20.2 Pass B: transform update

Update all live surfels from their stored local-space attachment.

### 20.3 Pass C: spawn candidates

Per 16x16 screen tile:

- estimate coverage
- find least-covered visible texel
- if below threshold, claim a surfel from the free stack
- initialize it

### 20.4 Pass D: recycle

Score all surfels and recycle some under memory pressure.

### 20.5 Pass E: build acceleration structure

- clear cell counts
- count insertions
- prefix sum
- scatter surfel ids
- compute cell averages

### 20.6 Pass F: requested rays

Compute local requested rays per surfel from:

- variance
- visibility / contribution recency
- spawned status

### 20.7 Pass G: allocate global budget

Normalize requests to the global frame ray budget.

### 20.8 Pass H: generate rays

For each surfel:

- decide sample count
- use mixture of cosine sampling + guiding
- store ray origin, direction, surfel id, pdf, and metadata

### 20.9 Pass I: bin rays

Sort rays for traversal coherence.

### 20.10 Pass J: trace rays

Use hardware ray tracing if available. If not, use software BVH / hybrid solution.

### 20.11 Pass K: shade hits

At each hit:

- evaluate direct diffuse with light sampling
- optionally shoot one shadow ray to selected light
- sample current surfel cache at the hit point
- return total incoming radiance estimate

### 20.12 Pass L: update surfels

Per surfel:

- reduce all sample returns into an irradiance estimate
- update guiding texture
- update radial depth moments
- update short/long estimators
- update requested-ray statistics
- update contribution timestamps

### 20.13 Pass M: neighborhood share

If variance high, borrow irradiance from neighbors.

### 20.14 Pass N: final apply

For every shaded pixel:

- reconstruct world position and normal
- find cell
- gather first `N` surfels
- weight by distance, normal alignment, and radial-depth test
- fill remaining weight with cell average irradiance

### 20.15 Pass O: transparent probe solve

Run probe clipmaps in parallel or at a lower frequency.

---

## 21. Pseudocode skeleton

```cpp
void UpdateSurfelGI(FrameContext& ctx)
{
    RenderGBuffer(ctx);

    UpdatePersistentSurfels(ctx);
    SpawnSurfelsFromGBuffer(ctx);
    RecycleSurfels(ctx);

    BuildSurfelAcceleration(ctx);

    ComputeRequestedRayCounts(ctx);
    AllocateRayBudget(ctx);

    GenerateSurfelRays(ctx);
    BinAndSortRays(ctx);
    TraceRays(ctx);
    ShadeRayHits(ctx);

    IntegrateSurfelIrradiance(ctx);
    ShareIrradianceAcrossNeighbors(ctx);

    SolveTransparentProbeFallback(ctx);
}
```

```cpp
float3 SolveSurfelIndirectAtPixel(PixelData p)
{
    SurfelCell cell = LookupCell(p.worldPos);

    float3 accum = 0;
    float  sumW  = 0;

    for (uint i = 0; i < min(cell.count, MAX_SURFELS_PER_PIXEL); ++i)
    {
        Surfel s = Surfels[SurfelIndices[cell.start + i]];
        float w = ComputeSurfelApplyWeight(s, p.worldPos, p.normal);

        if (w > 0)
        {
            w *= RadialDepthOcclusion(s, p.worldPos);
            accum += w * s.irradianceMeanLong;
            sumW  += w;
        }
    }

    if (sumW < 1.0f)
        accum += (1.0f - sumW) * cell.avgIrradiance;

    return accum;
}
```

---

## 22. Validation checklist

A coding agent should not consider the implementation done until these tests pass.

### 22.1 Spawn / persistence

- surfels appear only in under-covered regions
- camera pan away and back returns to previously solved lighting faster than a cold start
- distant geometry uses larger surfels and fewer of them

### 22.2 Transform following

- rigid objects drag attached surfels correctly
- skinned characters keep approximate lighting coherence
- destroying the source object invalidates attached surfels safely

### 22.3 Recycling

- fixed max surfel count is never exceeded
- free stack remains coherent under stress
- no spawn/recycle thrash during normal camera movement

### 22.4 Acceleration structure

- lookup cost stays bounded as world size grows
- average surfels per cell stays roughly stable in screen space
- neighbor overlap insertion removes boundary seams

### 22.5 Leak suppression

- bright exterior surfels do not light dark interiors through thin walls
- disabling radial depth moments visibly reintroduces the problem
- 4x4 depth moments are enough in common scenes

### 22.6 Temporal accumulation

- static scenes converge to low-noise results
- light changes react quickly
- spawned surfels converge faster than dormant ones

### 22.7 Ray guiding

- guiding outperforms pure cosine sampling in directional-light-entry scenes
- mixed guide + cosine sampling remains unbiased relative to the chosen estimator

### 22.8 Final apply

- missing coverage is softened by cell-average fallback
- no obvious popping at cell boundaries
- weighting remains stable under motion

### 22.9 Transparent probe fallback

- transparent geometry receives plausible indirect diffuse
- probe clip levels transition without visible hard seams

---

## 23. Common failure modes and fixes

### 23.1 Surfel overspawn

**Symptoms:** too many surfels on high-detail or noisy geometry  
**Fixes:**

- raise coverage threshold
- clamp spawn count per tile
- use normal/depth discontinuity aware coverage
- combine with SSGI for near-screen detail, as the talk suggests as a future direction

### 23.2 Light bleeding

**Symptoms:** bright surfels affect geometry through walls  
**Fixes:**

- enable radial depth moments
- reduce surfel radius
- tighten final apply weight
- avoid cross-surface sharing in neighborhood filter

### 23.3 Temporal lag / ghosting

**Symptoms:** irradiance takes too long to respond  
**Fixes:**

- increase short-term alpha
- increase reactive long-term alpha
- raise initial variance for spawned / moved surfels

### 23.4 Noise remains blotchy

**Symptoms:** independent surfels converge at visibly different speeds  
**Fixes:**

- enable irradiance sharing under high variance
- improve ray guiding
- raise minimum rays for visible surfels

### 23.5 Poor traversal performance

**Symptoms:** ray pass takes too long even with low ray count  
**Fixes:**

- bin rays by position + direction
- reduce divergence in hit shading
- compact active surfel list
- reduce dynamic geometry rebuild cost

---

## 24. Recommended implementation order for an engine team

This sequence minimizes integration pain.

1. **Opaque surfel storage + spawn**
2. **Persistent transforms**
3. **Uniform-grid prototype**
4. **Final-apply pass from cached irradiance only**
5. **Simple cosine-sampled ray solve**
6. **Temporal accumulation**
7. **Adaptive ray budget**
8. **Ray binning**
9. **Radial depth leak suppression**
10. **Ray guiding**
11. **Neighborhood irradiance sharing**
12. **Many-light sampling**
13. **Transparent probe clipmaps**
14. **Replace prototype grid with the non-linear production structure**

This order matters. Do not start with many-light sampling or probe clipmaps before the opaque surfel pipeline is stable.

---

## 25. What is faithful to GIBS and what is an adaptation

### Faithful to the published 2021 talk

- G-buffer opportunistic surfel spawn
- persistent surfels
- transform-attachment to geometry and one-bone skinned support
- fixed-capacity surfel memory with GPU stack recycle
- non-linear camera-centered surfel lookup structure
- final apply by gathering nearby surfels and falling back to cell average
- radial depth moments for leak suppression
- variance-aware temporal accumulation
- adaptive per-surfel ray counts under global ray budget
- per-surfel ray guiding
- irradiance sharing across neighboring surfels
- probe clipmap fallback for transparency

### Adaptations required for a complete implementation

- exact equations for spawn coverage
- exact non-linear grid mapping
- exact accumulation coefficients
- exact final weighting function
- exact many-light integrator details
- exact buffer packing

That distinction should remain explicit in code comments and technical docs.

---

## 26. Practical advice for OpenGL 4.6 or non-RT implementations

The original GIBS talk assumes hardware ray tracing. If you implement in OpenGL 4.6 without RT cores, keep the architecture but replace the ray backend.

### 26.1 Keep

- surfels
- persistence
- adaptive budgets
- temporal accumulation
- radial depth
- guiding
- final apply

### 26.2 Replace

- hardware RTAS traversal with:
  - software BVH traversal in compute
  - or a hybrid of screen-space tracing for short-range hits plus software BVH for off-screen rays

### 26.3 Expect

- fewer rays per frame
- heavier reliance on temporal reuse
- more importance from ray guiding and irradiance sharing

This is still viable because surfel GI is designed to amortize sparse ray work over time.

---

## 27. Final condensed spec

If an agent needs the shortest possible interpretation:

> Build a fixed-capacity GPU surfel cache on opaque surfaces. Spawn surfels from under-covered G-buffer regions using 16x16 screen tiles and constant projected surfel size. Keep surfels persistent across frames by storing parent transform ids and local-space attachment. Recycle them probabilistically under memory pressure using contribution recency and distance. Insert surfels every frame into a camera-centered non-linear grid with fine central cells and coarser outer trapezoidal slices. For each surfel, request a ray count based on variance and visibility importance, normalize against a global frame budget, generate a mixture of cosine and guided rays, bin them for traversal coherence, trace them, and evaluate direct lighting plus previously cached surfel lighting at hit points. Accumulate the returned irradiance with a variance-aware multi-scale temporal estimator. Maintain a 4x4 radial depth-moment field per surfel hemisphere and use a Chebyshev-style test during final apply to prevent light leaking through walls. At shading time, reconstruct the pixel world position, fetch nearby surfels from the containing cell, weight their irradiance by distance and orientation, attenuate with the radial depth test, and fill missing weight with cell-average irradiance. Use a clipmapped SH probe volume as fallback for transparency and geometry not suitable for surfels.

---

## References

1. **SIGGRAPH Advances 2021 - Surfel GI** (uploaded course slides / transcript extract). This is the primary source for the production GIBS architecture, including spawning, persistence, recycling, non-linear acceleration, radial depth moments, adaptive ray counts, ray guiding, irradiance sharing, many-light sampling, and transparent probe fallback.
2. **EA SEED / Frostbite SIGGRAPH 2021 pages** describing GIBS as a real-time indirect diffuse GI method based on surfels, used in Frostbite and designed for dynamic scenes and arbitrary scale.
3. **Pfister et al., 2000, Surfels: Surface Elements as Rendering Primitives.** Foundational surfel paper. Important for the geometric idea of surfels as surface elements, even though modern GIBS uses them as irradiance caches rather than direct rendering primitives.
4. **Kajiya, 1986, The Rendering Equation.** The theoretical basis of the indirect illumination integral that GIBS approximates.
5. **Sloan, Kautz, Snyder, 2002, Precomputed Radiance Transfer.** Relevant for the SH-based probe fallback and for the general idea of transport caching.
6. **Zhang, 2023, Design and implementation of a global illumination rendering system based on Surfels.** Useful as an implementation-oriented companion and for practical adaptations where static and dynamic surfels are treated differently.
