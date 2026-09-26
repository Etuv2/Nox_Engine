# AGENT.md — Nox Engine EA GIBS-Style Surfel GI Implementation Spec

**Project:** Real-Time Surfel-Based Global Illumination in OpenGL 4.6  
**Engine:** Nox Engine, custom C++ OpenGL deferred renderer  
**Target implementer:** autonomous coding agent or graphics-programming assistant  
**Primary goal:** implement an EA GIBS-inspired dynamic diffuse GI system using persistent GPU surfels, adapted to OpenGL 4.6 without relying on DXR/Vulkan RT.

---

## 0. Read This First

This file is the implementation contract for the agent. Do not treat the GI system as a single shader. Implement it as a renderer subsystem with persistent GPU resources, staged compute passes, debug visualisation, profiling, and fallback paths.

The reference technique is Electronic Arts’ **Global Illumination Based on Surfels (GIBS)** presented at SIGGRAPH Advances 2021. The original GIBS system uses hardware ray tracing, persistent surfels, G-buffer spawning, transform tracking, non-linear surfel acceleration, temporal irradiance accumulation, radial depth functions, ray guiding, ray binning, many-light sampling, spatial filtering, and probe clipmaps for transparent / non-deferred shading. Nox Engine targets OpenGL 4.6, so the implementation must preserve the architecture while replacing hardware ray tracing with an OpenGL-compatible hybrid trace path.

### Non-negotiable constraints

1. Use **C++ + OpenGL 4.6 + GLSL 460 compute shaders**.
2. Store persistent surfel data in **Shader Storage Buffer Objects (SSBOs)** using `std430` layout.
3. Avoid CPU readback in the frame loop except for optional debug counters using delayed query/readback.
4. Integrate as a **renderer subsystem** compatible with the existing deferred renderer and ECS-style scene update.
5. Keep all resource sizes bounded by user-configurable budgets.
6. Every major pass must have a debug view and GPU timing hook.
7. The first working milestone may use a uniform grid and simplified tracing, but the final target must include the complete GIBS-style architecture described below.

---

## 1. System Definition

### 1.1 What the system computes

The system computes **indirect diffuse illumination** by discretising visible and recently visible scene surfaces into persistent surfels. Each surfel approximates a small disk-shaped surface neighbourhood and caches diffuse irradiance over time. During final deferred lighting, shaded pixels gather nearby surfel irradiance as an additional indirect diffuse term.

The system should support:

- fully dynamic camera movement;
- dynamic lights;
- moving rigid meshes;
- skinned meshes via dominant transform / bone tracking where available;
- off-screen indirect lighting persistence;
- colour bleeding;
- emissive surfaces where material data exists;
- bounded memory and bounded ray budget;
- comparison against the existing Nox SSGI implementation.

### 1.2 What the system does not need to solve initially

The first implementation does **not** need physically exact path tracing, caustics, glossy multi-bounce GI, participating media, perfect transparent-object GI, or full ReSTIR. Those are stretch goals. The required result is a robust, measurable, dynamic, diffuse GI cache.

---

## 2. High-Level Rendering Frame

Implement the GI frame in this order. Each item is a concrete pass or CPU-side orchestration step.

```text
Existing Renderer:
  00. ECS / scene update
  01. G-buffer pass
      outputs: depth, normal, albedo, material, emissive, motion if available,
               entity/material/mesh/transform ID

Surfel GI Persistent Work:
  02. Update transform buffer for objects / bones
  03. Update live surfel world positions from local position + transform ID
  04. Recycle irrelevant surfels under memory pressure
  05. Clear surfel acceleration grid
  06. Insert live surfels into grid cells
  07. Build per-cell aggregate irradiance / fallback values

Surfel Spawn Work:
  08. Compute screen-space surfel coverage from G-buffer + grid
  09. Spawn new surfels from least-covered 16x16 screen tiles
  10. Initialise new surfel irradiance, radial depth, guide map, history
  11. Insert spawned surfels into the grid or rebuild affected grid range

Lighting Integration:
  12. Ray request pass: each live surfel requests rays according to variance, age, contribution
  13. Ray allocation pass: enforce global ray budget proportionally
  14. Ray generation pass: cosine / guided hemisphere sampling
  15. Ray binning and sorting pass for coherence
  16. Trace / gather pass: hybrid screen-space + software-BVH + surfel-grid fallback
  17. Direct-light / emissive / indirect-at-hit evaluation
  18. Temporal irradiance integration with adaptive MSME-style estimator
  19. Update radial depth function and ray-guiding map
  20. Spatial irradiance sharing / filtering when variance is high

Final Lighting:
  21. Deferred lighting apply pass samples surfel grid per pixel
  22. Composite indirect diffuse into renderer lighting buffer
  23. Optional TAA / denoise integration

Optional Transparent / Non-Deferred Support:
  24. Probe clipmap update
  25. Probe SH reconstruction for transparents / forward passes

Debug / Evaluation:
  26. Render debug overlays
  27. Record GPU timings and counters
```

---

## 3. Engine Integration Points

### 3.1 Required C++ classes

Create or extend the following renderer-side classes. Names may be adapted to the existing Nox style, but responsibilities must remain separate.

```cpp
class SurfelGIManager;
class SurfelGIPipeline;
class SurfelPool;
class SurfelGrid;
class SurfelRayQueue;
class SurfelLightSampler;
class SurfelProbeClipmap;       // optional target feature
class SurfelGIDebugRenderer;
class GpuTimerScope;
```

#### `SurfelGIManager`
Owns all public settings, GPU resources, pass order, debug modes, and integration with the deferred renderer.

Required methods:

```cpp
void Init(const SurfelGISettings& settings);
void Resize(uint32_t width, uint32_t height);
void Shutdown();
void BeginFrame(const Camera& camera, const SceneRenderData& scene);
void Execute(const GBuffer& gbuffer, const LightingData& lights);
void ApplyIndirect(const GBuffer& gbuffer, RenderTarget& lightingBuffer);
void RenderDebug(DebugDrawContext& ctx);
void ReloadShaders();
```

#### `SurfelPool`
Owns the persistent surfel SSBO and free-list stack.

Required methods:

```cpp
void Create(uint32_t maxSurfels);
void ResetFreeList();
GLuint GetSurfelBuffer() const;
GLuint GetFreeListBuffer() const;
GLuint GetCountersBuffer() const;
```

#### `SurfelGrid`
Owns the acceleration structure used for neighbour lookup, surfel insertion, final gathering, irradiance sharing, and ray fallback queries.

Required methods:

```cpp
void Create(const SurfelGridSettings& settings);
void Clear();
void Build(GLuint surfelBuffer, uint32_t maxSurfels);
void BuildCellAverages();
GLuint GetCellHeaderBuffer() const;
GLuint GetCellEntryBuffer() const;
```

#### `SurfelRayQueue`
Owns ray request, ray allocation, binning, sorted ray buffers, and hit buffers.

Required methods:

```cpp
void Create(uint32_t maxRays, uint32_t maxSurfels);
void Clear();
void RequestRays(GLuint surfelBuffer);
void AllocateRays(GLuint surfelBuffer, uint32_t globalBudget);
void GenerateRays(GLuint surfelBuffer, GLuint gridBuffer);
void BinAndSortRays();
void TraceRays(const TraceResources& traceResources);
void IntegrateHits(GLuint surfelBuffer);
```

### 3.2 G-buffer requirements

The current deferred G-buffer must expose enough data for surfel spawning and lighting:

| Field | Required | Purpose |
|---|---:|---|
| Depth | yes | Reconstruct world position and run screen-space tracing |
| World normal | yes | Initialise surfel normal and compute final gather weights |
| Albedo / base colour | yes | Indirect diffuse response and hit shading |
| Material flags | yes | reject sky, particles, decals, invalid surfaces; identify emissive surfaces |
| Emissive radiance | target | direct ray-hit contribution from emissive materials |
| Roughness / metallic | optional | reject metals or weight diffuse contribution correctly |
| Entity / mesh ID | yes | stable material and transform lookup |
| Transform ID | yes | surfel persistence on moving objects |
| Dominant bone ID | target | approximate surfel tracking on skinned meshes |
| Motion vector | optional | debug and future reprojection |

If Nox currently lacks transform IDs in the G-buffer, add a single unsigned integer render target or pack an ID into an existing integer material target. Do not pack IDs into normal/albedo floats unless there is no alternative.

### 3.3 Transform buffer

Maintain a GPU transform table updated each frame after ECS hierarchy propagation.

```glsl
struct GpuTransform
{
    mat4 worldFromLocal;
    mat4 localFromWorld;
    mat4 prevWorldFromLocal;
    uint flags;
    uint parentEntity;
    uint _pad0;
    uint _pad1;
};
```

Surfels store local-space position and normal plus a transform ID. Each frame, the update pass computes the surfel world position. For skinned geometry, use the dominant bone transform ID written during the G-buffer pass. If dominant-bone IDs are not available, spawn surfels only on rigid/deferred geometry until the animation path is implemented.

---

## 4. GPU Resource Layout

### 4.1 Settings

```cpp
struct SurfelGISettings
{
    bool enabled = true;
    uint32_t maxSurfels = 262144;
    uint32_t maxRayBudget = 262144;
    uint32_t spawnTileSize = 16;
    uint32_t maxSurfelsPerCell = 64;
    uint32_t maxGatherSurfelsPerPixel = 32;
    float targetSurfelScreenRadiusPx = 8.0f;
    float minSurfelRadius = 0.03f;
    float maxSurfelRadius = 5.0f;
    float spawnCoverageThreshold = 0.65f;
    float recyclePressureStart = 0.85f;
    float normalRejectCos = 0.25f;
    float finalGatherNormalCos = 0.15f;
    float radialDepthSigmaScale = 1.0f;
    float indirectIntensity = 1.0f;
    float skyMissRadianceMultiplier = 1.0f;
    bool useNonLinearGrid = true;
    bool useRadialDepth = true;
    bool useRayGuiding = true;
    bool useRayBinning = true;
    bool useIrradianceSharing = true;
    bool useScreenSpaceTrace = true;
    bool useSoftwareBVHTrace = true;
    bool useSurfelFallbackTrace = true;
};
```

### 4.2 Surfel SSBO struct

Use `std430`. Keep 16-byte alignment. Split very large optional data into separate buffers/textures to avoid bloating the core surfel record.

```glsl
struct Surfel
{
    // Disk in world space, updated every frame.
    vec4 worldPos_radius;        // xyz = world position, w = radius
    vec4 worldNormal_age;        // xyz = normal, w = age in frames

    // Persistent attachment.
    vec4 localPos_spawnRadius;   // xyz = local-space position, w = initial radius
    vec4 localNormal_flags;      // xyz = local-space normal, w = packed flags as floatBitsToUint

    // Material / identity.
    uvec4 ids;                   // x transformID, y entityID, z materialID, w generation
    vec4 albedo_life;            // rgb = diffuse albedo, a = life / confidence

    // Irradiance estimator.
    vec4 irradiance;             // rgb = long-term irradiance, a = confidence
    vec4 shortMean;              // rgb = short-term mean, a = short sample count
    vec4 shortM2;                // rgb = short-term variance accumulator / M2, a = requested rays

    // Usage and lifecycle.
    uvec4 frameInfo;             // x lastVisible, y lastContributed, z lastUpdated, w lastSpawned
    vec4 debug;                  // x variance luminance, y coverage, z last ray count, w recycle score
};
```

Flags:

```cpp
enum SurfelFlags : uint32_t
{
    SURFEL_DEAD           = 0u,
    SURFEL_ALIVE          = 1u << 0,
    SURFEL_NEW            = 1u << 1,
    SURFEL_DYNAMIC        = 1u << 2,
    SURFEL_SKINNED        = 1u << 3,
    SURFEL_EMISSIVE       = 1u << 4,
    SURFEL_INVALID        = 1u << 31
};
```

### 4.3 Free-list and counters

```glsl
layout(std430, binding = B_SURFEL_FREELIST) buffer SurfelFreeList
{
    uint freeIndices[];
};

struct SurfelCounters
{
    uint freeTop;
    uint liveCount;
    uint spawnedThisFrame;
    uint recycledThisFrame;
    uint requestedRays;
    uint allocatedRays;
    uint overflowSurfels;
    uint overflowGridEntries;
};
```

Spawning pops from `freeIndices` with an atomic decrement on `freeTop`. Recycling pushes back with an atomic increment. Initialise all indices once on startup and when resetting the GI system.

### 4.4 Radial depth data

EA GIBS uses a low-resolution radial depth function per surfel hemisphere to reduce light bleeding. Implement this as a separate buffer, not inside the main surfel struct.

Recommended layout:

```glsl
// 4x4 hemisphere texels per surfel. Each texel stores depth mean and depth squared mean.
struct RadialDepthTexel
{
    float meanDepth;
    float meanDepthSq;
};

layout(std430, binding = B_RADIAL_DEPTH) buffer SurfelRadialDepth
{
    RadialDepthTexel radialDepth[]; // index = surfelID * 16 + hemiTexel
};
```

MVP may store only `R16F meanDepth`; final target stores mean and mean-square so variance can be reconstructed.

### 4.5 Ray-guiding data

EA GIBS tracks a compact hemispherical radiance map per surfel, typically described as a 6x6 function plus a scale value. Use a separate buffer.

```glsl
// MVP: luminance only. Target: RGB or RGBE-like encoding.
struct GuideCell
{
    uint packedRadiance; // R8/G8/B8/confidence or luminance8 + flags
};

layout(std430, binding = B_GUIDE_MAP) buffer SurfelGuideMap
{
    GuideCell guideCells[]; // index = surfelID * 36 + guideCell
};

layout(std430, binding = B_GUIDE_SCALE) buffer SurfelGuideScale
{
    vec4 guideScale[]; // rgb scale, a confidence / total luminance
};
```

For MVP, use 36 `float` luminance values per surfel if memory permits. Convert to compact packed storage once functionality is correct.

### 4.6 Grid resources

Support two modes:

1. **MVP uniform grid:** easier to implement, sufficient to get visible GI working.
2. **Target non-linear GIBS grid:** central uniform region plus six axis-aligned trapezoidal / non-linear regions with cell size increasing with distance from the camera.

Grid header:

```glsl
struct SurfelCellHeader
{
    uint firstEntry;
    uint entryCount;
    uint averagePackedIrradiance;
    uint flags;
};

struct SurfelCellEntry
{
    uint surfelID;
    uint next;       // optional linked-list mode
};
```

Prefer fixed entry ranges or append-buffer ranges over linked lists for cache coherence. Use overflow counters and debug views.

### 4.7 Ray buffers

```glsl
struct SurfelRayRequest
{
    uint surfelID;
    uint requestedCount;
    uint allocatedCount;
    uint firstRay;
};

struct SurfelRay
{
    vec4 origin_tMin;
    vec4 direction_tMax;
    uvec4 ids;       // x surfelID, y binID, z sampleIndex, w flags
    vec4 throughput; // rgb = estimator throughput, a = pdf
};

struct SurfelRayHit
{
    vec4 position_t;
    vec4 normal_hitKind;
    vec4 radiance_pdf;
    uvec4 ids;       // surfelID, materialID, primitiveID, flags
};
```

---

## 5. Pass Specifications

## 5.1 Pass 02 — Transform Buffer Update

### Purpose
Upload all object and bone transforms needed by surfels.

### CPU responsibilities

- After ECS hierarchy propagation, write world matrices to a persistently mapped buffer or update with `glBufferSubData` / orphaning.
- Maintain stable transform IDs. IDs must not change every frame.
- For destroyed objects, mark transform entry invalid so attached surfels can be recycled.

### Agent tasks

- Add transform ID allocation to renderable entities.
- Add optional dominant bone ID path for skinned meshes.
- Add G-buffer output for transform ID.

---

## 5.2 Pass 03 — Update Live Surfels

### Purpose
Move surfels with their parent transform.

### GLSL behaviour

For each surfel:

1. Skip if not alive.
2. Fetch `GpuTransform` by `transformID`.
3. If transform invalid, mark surfel for recycling.
4. Compute:

```glsl
worldPos = (worldFromLocal * vec4(localPos, 1.0)).xyz;
worldNormal = normalize((transpose(localFromWorld) * vec4(localNormal, 0.0)).xyz);
```

5. Update age and frame metadata.
6. Recompute radius so screen projection remains roughly constant:

```glsl
float worldUnitsPerPixel = ComputeWorldUnitsPerPixel(camera, worldPos);
float radius = clamp(settings.targetSurfelScreenRadiusPx * worldUnitsPerPixel,
                     settings.minSurfelRadius,
                     settings.maxSurfelRadius);
```

7. Mark surfels outside hard distance / invalid transform for recycling, not immediate deletion unless memory pressure is critical.

---

## 5.3 Pass 04 — Recycling

### Purpose
Bound memory and discard irrelevant surfels without destroying useful cached lighting unnecessarily.

### Recycle score

Compute a probability-like score:

```glsl
float livePressure = saturate((liveCount / maxSurfels - recyclePressureStart) /
                              (1.0 - recyclePressureStart));
float distanceScore = saturate((distanceToCamera - recycleNear) / (recycleFar - recycleNear));
float staleScore = saturate((frameIndex - lastContributedFrame) / staleFrameThreshold);
float invisibleScore = saturate((frameIndex - lastVisibleFrame) / invisibleFrameThreshold);
float oversizeScore = saturate((radius - desiredRadius * 2.0) / desiredRadius);

float recycleScore = weighted_sum(livePressure, distanceScore, staleScore,
                                  invisibleScore, oversizeScore);
```

Recycle if:

```glsl
recycleScore > BlueNoiseOrHash01(surfelID, frameIndex)
```

### Required behaviour

- Always recycle if parent transform is invalid.
- Recycle more aggressively under free-list pressure.
- Never recycle all surfels simply because they are off-screen; persistence is core to the technique.
- Push recycled IDs back to free-list on GPU.

---

## 5.4 Pass 05 / 06 — Grid Clear and Build

### Purpose
Enable fast lookup of nearby surfels for coverage, final gathering, filtering, and approximate tracing.

### MVP uniform grid

Use a camera-centred world-space grid:

```cpp
gridResolution = ivec3(64, 64, 64);
gridExtent = vec3(80.0f); // tune by scene scale
cellSize = 2.0f * gridExtent / gridResolution;
```

Each live surfel inserts into its primary cell and immediate neighbours if its radius overlaps cell boundaries. Ensure radius is never larger than one cell side in uniform-grid MVP.

### Target non-linear grid

Implement the GIBS-style grid:

- central uniform cube around the camera;
- six surrounding axis regions: +X, -X, +Y, -Y, +Z, -Z;
- depth slices grow in thickness with distance from centre;
- lateral cell width grows with slice depth so projected cell size remains approximately constant;
- mapping must be deterministic and reversible enough for lookup.

Suggested mapping:

```glsl
GridCoord MapWorldToNonLinearGrid(vec3 worldPos)
{
    vec3 p = worldPos - cameraPos;

    if (InsideCentralGrid(p))
        return MapCentralUniform(p);

    int axis = DominantAbsAxis(p);       // 0 x, 1 y, 2 z
    int sign = p[axis] >= 0.0 ? 1 : -1;
    float d = abs(p[axis]) - centerHalfExtent;

    // Log/exponential depth slicing approximates growing trapezoid thickness.
    int slice = clamp(int(floor(log2(1.0 + d / baseCellSize) * sliceScale)), 0, sliceCount - 1);
    float sliceCellSize = baseCellSize * pow(sliceGrowth, float(slice));

    vec2 lateral = RemainingAxes(p, axis);
    ivec2 uv = ivec2(floor(lateral / sliceCellSize + lateralGridHalf));

    return EncodeAxisGrid(axis, sign, slice, uv);
}
```

Do not spend excessive time perfecting the final mapping before the MVP works. Build uniform grid first, then replace it with non-linear mapping behind the same `SurfelGrid` interface.

### Cell averages

After insertion, compute per-cell average irradiance and average normal. The apply pass uses this as fallback when individual surfel weights do not cover a pixel sufficiently.

---

## 5.5 Pass 08 / 09 — Coverage and Spawning

### Purpose
Spawn surfels from the G-buffer where screen-space coverage is insufficient.

### Algorithm

The screen is divided into **16x16 tiles**.

For each tile:

1. Iterate candidate pixels in the tile.
2. Reject invalid pixels: sky, transparent, non-diffuse-only if desired, backfaces, decals if unsupported.
3. Reconstruct world position and normal.
4. Query nearby surfels using the grid.
5. Compute current coverage:

```glsl
float SurfelCoverage(vec3 p, vec3 n, Surfel s)
{
    vec3 d = p - s.worldPos;
    float dist2 = dot(d, d);
    float r2 = s.radius * s.radius;
    float disk = exp(-dist2 / max(r2, 1e-4));
    float normal = saturate((dot(n, s.normal) - normalRejectCos) / (1.0 - normalRejectCos));
    float plane = exp(-abs(dot(d, s.normal)) / max(s.radius * 0.25, 1e-4));
    return disk * normal * plane;
}
```

6. Track the pixel with the lowest coverage.
7. If lowest coverage is below a randomised threshold, spawn one surfel from that pixel.

### Randomised threshold

```glsl
float threshold = settings.spawnCoverageThreshold + 0.15 * (BlueNoise(tileID, frame) - 0.5);
if (lowestCoverage < threshold) SpawnSurfel(lowestPixel);
```

### Spawn initialisation

New surfel fields:

- world position from depth;
- world normal from G-buffer;
- local position = `localFromWorld * worldPos`;
- local normal = inverse-transpose transform;
- radius from target screen projection;
- albedo/material/entity/transform IDs;
- irradiance initialised from rough environment / direct ambient / neighbour average;
- short mean = initial irradiance;
- variance high enough to request more rays at first;
- age = 0;
- frameInfo set to current frame;
- radial depth initialised to surfel diameter;
- guide map initialised to uniform cosine/luminance.

### Required rejection rules

Do not spawn surfels on:

- sky/background;
- water / transparent materials unless explicitly supported;
- particles and decals;
- invalid entity ID;
- surfaces below minimum normal confidence;
- pixels with depth discontinuity if normal/position derivatives indicate unstable geometry.

---

## 5.6 Pass 12 / 13 — Ray Request and Allocation

### Purpose
Adapt ray count per surfel while enforcing a global ray budget.

### Ray request score

```glsl
float varianceScore = saturate(luminance(shortM2.rgb) * varianceScale);
float newScore = IsNewSurfel ? 1.0 : 0.0;
float contributionScore = saturate(1.0 - (frameIndex - lastContributedFrame) / contributionFalloffFrames);
float staleScore = saturate((frameIndex - lastUpdatedFrame) / staleUpdateFrames);

float desired = baseRays
              + newScore * newSurfelExtraRays
              + varianceScore * varianceExtraRays
              + contributionScore * contributionExtraRays
              + staleScore * staleExtraRays;
```

The request pass sums all requested rays using atomics. The allocation pass computes:

```glsl
allocated = max(minRaysForAliveSurfels,
                floor(requested * globalRayBudget / max(totalRequested, 1)));
```

Low-variance surfels may become nearly dormant and shoot only occasional detection rays.

---

## 5.7 Pass 14 — Ray Generation and Ray Guiding

### Purpose
Generate hemisphere rays from surfels according to cosine-weighted sampling and learned guide maps.

### MVP sampling

Use cosine-weighted hemisphere sampling around surfel normal:

```glsl
vec3 dir = CosineSampleHemisphere(rand2, surfel.normal);
float pdf = max(dot(dir, surfel.normal), 0.0) / PI;
```

### Target guided sampling

Use a mixture distribution:

```glsl
float guideProbability = saturate(guideConfidence);
if (rand < guideProbability)
    dir = SampleGuideMap6x6(surfelID, rand2, out guidedPdf);
else
    dir = CosineSampleHemisphere(rand2, surfel.normal, out cosinePdf);

float finalPdf = mix(cosinePdf, guidedPdf, guideProbability);
```

Guide map sampling:

1. Map surfel hemisphere to a 2D 6x6 domain.
2. Interpret stored guide values as an unnormalised discrete PDF.
3. Generate a uniform random number multiplied by sum of guide values.
4. Walk cells in deterministic order until cumulative sum exceeds the random value.
5. Jitter inside selected cell.
6. Map UV back to hemisphere direction.
7. Return PDF including cell probability and hemisphere mapping term.

Update guide map after ray hits using incoming radiance luminance/RGB. Normalise guide values periodically or store per-surfel scale so 8-bit cells remain useful.

---

## 5.8 Pass 15 — Ray Binning and Sorting

### Purpose
Improve trace coherence by sorting rays with similar origin cells and directions.

### Bin ID

```glsl
uint spatial = FlattenCellCoord(surfelCellCoord);
uint dirBin = OctahedralDirectionBin(ray.direction, DIR_BIN_RES); // e.g. 8x8
uint binID = spatial * DIR_BIN_COUNT + dirBin;
```

### Passes

1. Count rays per bin.
2. Prefix sum bin counts to offsets.
3. Scatter rays to sorted buffer.
4. Trace sorted rays.

MVP may skip sorting. Target implementation should include it behind `settings.useRayBinning`.

---

## 5.9 Pass 16 — OpenGL Hybrid Trace

### Purpose
EA GIBS uses hardware ray tracing. Nox OpenGL needs a compatible substitute. Implement a hybrid trace path with clearly separated backends.

### Trace backends

#### Backend A — Screen-space trace

Use depth-buffer / Hi-Z ray marching for short rays and visible geometry:

- project ray origin and direction to screen;
- march in view space or clip space;
- compare against depth pyramid;
- return hit position, normal, material if hit;
- reject unreliable hits near screen edges and depth discontinuities.

Pros: cheap, captures visible detail.  
Cons: cannot see off-screen or occluded geometry.

#### Backend B — Software BVH trace

Build or upload a simple GPU BVH over static opaque meshes, with optional dynamic mesh update later.

Minimum viable BVH:

```glsl
struct BvhNode
{
    vec4 boundsMin_leftFirst;
    vec4 boundsMax_count;
};

struct TriangleData
{
    vec4 p0;
    vec4 p1;
    vec4 p2;
    vec4 n0_material;
    vec4 n1_entity;
    vec4 n2_flags;
};
```

Trace with an explicit stack in compute shader. Keep this path optional and budgeted. If BVH work is too large for the first milestone, implement screen-space + surfel fallback first, but leave the interface intact.

#### Backend C — Surfel-grid fallback trace

Approximate ray hits by intersecting ray against surfel disks in grid cells:

```glsl
bool IntersectSurfelDisk(Ray r, Surfel s, out float t)
{
    float denom = dot(r.dir, s.normal);
    if (abs(denom) < 1e-4) return false;
    t = dot(s.worldPos - r.origin, s.normal) / denom;
    if (t < r.tMin || t > r.tMax) return false;
    vec3 p = r.origin + t * r.dir;
    return length(p - s.worldPos) <= s.radius;
}
```

Pros: sees persistent off-screen surfels.  
Cons: approximate, can miss thin geometry, can self-intersect if bias is wrong.

### Trace priority

```text
1. If the software BVH is available, trace every ray with it: its hit or miss is exact.
   A miss samples the sky/environment (black without a skybox) and is a valid sample.
2. Otherwise try the screen-space trace for a near hit,
3. then the surfel-grid fallback.
4. If neither resolves the ray, it counts as sky only when a sky is bound; otherwise it is
   unresolved and the integrator skips it (it may have hit geometry the screen and grid
   cannot see, so counting it as black would bias irradiance low).
```

Every resolved ray has equal weight in the irradiance estimate. Hit-kind reliability may steer
ray guiding, but weighting samples by how they were traced biases the mean.

### Hit bias

Offset ray origin:

```glsl
origin = surfel.worldPos + surfel.normal * max(0.01, surfel.radius * 0.05);
```

Avoid self-intersection by ignoring the source surfel ID and nearby same-surface surfels for very small `t`.

---

## 5.10 Pass 17 — Hit Lighting Evaluation

### Purpose
Compute incoming radiance seen by the surfel ray.

At each hit point:

```glsl
Li = DirectDiffuse(hit) + Emissive(hit) + ExistingSurfelIndirect(hit);
```

#### Direct diffuse

Use engine light data. For each selected light:

```glsl
float NoL = max(dot(hitNormal, lightDir), 0.0);
vec3 direct = lightRadiance * NoL * visibility;
```

Visibility options:

1. existing shadow maps for directional/spot lights;
2. software shadow ray if BVH is available;
3. unshadowed approximation for early MVP;
4. many-light stochastic sampling for high light counts.

#### Existing surfel indirect at hit

Query surfel grid at hit point and gather irradiance exactly like final apply, but with lower max surfel count to keep tracing cheap. This is how multi-bounce / effectively infinite-bounce lighting appears over time.

#### Sample estimator

For diffuse irradiance at the source surfel:

```glsl
float cosTheta = max(dot(rayDir, surfel.normal), 0.0);
vec3 sampleIrradiance = Li * cosTheta / max(rayPdf, 1e-5);
```

Use radiometric consistency. If Nox’s lighting buffer stores diffuse radiance rather than irradiance, document the convention and keep it consistent through apply.

---

## 5.11 Pass 18 — Adaptive Temporal Integration

### Purpose
Accumulate irradiance across frames while reacting quickly to scene changes.

EA GIBS describes a modified moving average with long-term mean plus short-term mean/variance. Implement this simplified MSME-style estimator:

```glsl
void IntegrateSurfelSample(inout Surfel s, vec3 x)
{
    // Short-term exponential mean.
    float aShort = 0.20;
    vec3 oldShort = s.shortMean.rgb;
    s.shortMean.rgb = mix(s.shortMean.rgb, x, aShort);

    // Short-term variance proxy.
    vec3 delta = x - oldShort;
    s.shortM2.rgb = mix(s.shortM2.rgb, delta * delta, aShort);

    float varLum = luminance(s.shortM2.rgb);
    float diffLum = luminance(abs(s.shortMean.rgb - s.irradiance.rgb));

    float reactive = saturate(varLum * varianceReactiveScale + diffLum * differenceReactiveScale);
    float alpha = mix(alphaStable, alphaReactive, reactive);

    // New surfels converge quickly.
    if (IsNewSurfel(s)) alpha = max(alpha, alphaNewSurfel);

    s.irradiance.rgb = mix(s.irradiance.rgb, s.shortMean.rgb, alpha);
    s.irradiance.a = saturate(s.irradiance.a + confidenceGain);
    s.debug.x = varLum;
}
```

Recommended starting values:

```cpp
alphaStable = 0.02f;
alphaReactive = 0.35f;
alphaNewSurfel = 0.60f;
varianceReactiveScale = 4.0f;
differenceReactiveScale = 2.0f;
```

Clamp extreme radiance to reduce fireflies:

```glsl
x = min(x, vec3(maxSampleIrradiance));
```

Do not hide severe errors with excessive clamping. Add debug view for clamped samples.

---

## 5.12 Pass 19 — Radial Depth Function

### Purpose
Reduce light leaking through walls and across nearby disconnected surfaces.

Each surfel stores a low-resolution depth distribution over its normal hemisphere. During ray integration, if the ray hits geometry within approximately the surfel diameter, update the radial depth texel corresponding to ray direction.

### Hemisphere mapping

Use octahedral or polar mapping. Keep mapping consistent for update and lookup.

```glsl
ivec2 texel = HemiDirectionTo4x4Texel(localDir);
uint idx = surfelID * 16 + texel.y * 4 + texel.x;
```

### Update

```glsl
float d = min(hitDistance, surfel.radius * 2.0);
float a = 0.05;
mean = mix(mean, d, a);
meanSq = mix(meanSq, d * d, a);
```

Initialise `mean = surfel.radius * 2.0`, `meanSq = mean * mean`.

### Visibility test during final gather

When a receiver point samples a surfel:

1. Compute vector from surfel to receiver.
2. Map direction to radial depth texel.
3. Fetch mean and variance.
4. If receiver distance is greater than mean, attenuate using a Chebyshev-style bound:

```glsl
float variance = max(meanSq - mean * mean, minVariance);
float delta = receiverDistance - mean;
float pMax = variance / (variance + delta * delta);
float visibility = (receiverDistance <= mean) ? 1.0 : saturate(pMax);
visibility = smoothstep(0.05, 1.0, visibility);
```

Multiply surfel contribution by `visibility`. Add debug mode showing radial-depth rejection.

---

## 5.13 Pass 20 — Irradiance Sharing / Spatial Filtering

### Purpose
Reduce blotchy variance by sharing irradiance between neighbouring surfels.

For each high-variance surfel:

1. Query neighbouring surfels in the same grid cell and adjacent cells.
2. Reject neighbours with incompatible normals, large plane distance, or radial depth occlusion.
3. Compute weighted average:

```glsl
float w = normalWeight * distanceWeight * planeWeight * confidenceWeight;
```

4. Blend only when variance is high:

```glsl
float shareAmount = saturate(varLum * shareScale) * maxShareAmount;
s.irradiance.rgb = mix(s.irradiance.rgb, neighbourAverage, shareAmount);
```

Do not over-filter. Preserve colour bleeding gradients and contact contrast.

---

## 5.14 Pass 21 / 22 — Final Deferred Apply

### Purpose
Add surfel GI to the lighting buffer.

For each shaded pixel:

1. Reconstruct world position and normal.
2. Map world position to surfel grid cell.
3. Fetch up to `maxGatherSurfelsPerPixel` surfels from cell and neighbours if needed.
4. Accumulate irradiance:

```glsl
vec3 indirectIrradiance = vec3(0.0);
float totalWeight = 0.0;

for each surfel s:
    vec3 d = pixelPos - s.worldPos;
    float dist = length(d);
    float radius = s.radius;
    float distanceW = exp(-(dist * dist) / max(radius * radius, 1e-4));
    float normalW = saturate((dot(pixelNormal, s.normal) - finalGatherNormalCos) /
                             (1.0 - finalGatherNormalCos));
    float planeW = exp(-abs(dot(d, s.normal)) / max(radius * 0.33, 1e-4));
    float radialW = useRadialDepth ? RadialDepthVisibility(s, pixelPos) : 1.0;
    float w = distanceW * normalW * planeW * radialW * s.irradiance.a;
    indirectIrradiance += s.irradiance.rgb * w;
    totalWeight += w;
```

5. If `totalWeight < 1`, blend in cell-average irradiance:

```glsl
indirectIrradiance += cellAverage * max(0.0, 1.0 - totalWeight);
totalWeight = max(totalWeight, 1.0);
```

6. Convert irradiance to outgoing diffuse contribution:

```glsl
vec3 indirectDiffuse = albedo * indirectIrradiance / PI;
lightingBuffer.rgb += settings.indirectIntensity * indirectDiffuse;
```

If the engine’s PBR convention already folds `1/PI` into diffuse BRDF elsewhere, avoid double division. Document the convention in code comments.

---

## 6. Many-Light Sampling

### 6.1 MVP reservoir light sampling

When many point/spot lights exist, do not evaluate all lights at each ray hit. Use stochastic reservoir-style selection.

For each ray hit:

1. Randomly sample `N = 4..8` lights from the active light list.
2. Compute weight:

```glsl
weight = lightIntensity * attenuation * max(dot(hitNormal, lightDir), 0.0);
```

3. Keep one winner with weighted reservoir update.
4. Trace or query visibility for the winner only.
5. Normalise contribution by selected PDF.

### 6.2 Target stochastic lightcuts

Build a light tree each frame or when lights change:

1. Store light positions in view space for precision.
2. Morton-sort lights by position.
3. Build tree bottom-up.
4. Each internal node stores bounds, total intensity, representative colour, and child indices.
5. At hit point, choose a cut with 2–8 nodes based on error/importance.
6. Stochastically descend from each cut node to one light using child probabilities.
7. Evaluate chosen lights with visibility.

Lightcuts are a target feature because they converge faster in many-light stress scenes, but they are more complex and more expensive than reservoir selection.

### 6.3 Future ReSTIR-like extension

Store selected light reservoirs per surfel or per grid cell and resample spatially/temporally. This is a stretch goal, not required for the first final artefact.

---

## 7. Probe Clipmaps for Transparent / Non-Deferred Objects

Surfels spawn from opaque G-buffer data, so they do not naturally support transparent or forward-rendered objects. EA GIBS uses ray-traced probes for arbitrary-location irradiance. Implement this only after the opaque surfel system is stable.

### 7.1 Probe data

Use low-order spherical harmonics, e.g. 3 bands / 9 coefficients per RGB channel.

```glsl
struct ProbeSH
{
    vec4 shR0; // pack coefficients across multiple vec4s
    vec4 shR1;
    vec4 shR2;
    vec4 shG0;
    vec4 shG1;
    vec4 shG2;
    vec4 shB0;
    vec4 shB1;
    vec4 shB2;
    vec4 variance;
};
```

### 7.2 Clipmap structure

Use nested volumes centred on the camera:

```cpp
levels = 3 or 4;
resolutionPerLevel = 16^3 or 24^3;
levelWorldSize[i] = baseSize * pow(2, i);
```

When the camera moves outside a level’s centre region:

1. Shift probe volume indices.
2. Preserve existing probes where possible.
3. Initialise newly exposed probes from the next coarser level by interpolation.
4. Update only a subset of probes per frame.

### 7.3 Probe integration

For selected probes per frame:

1. Shoot a few rays over the sphere.
2. Evaluate incident radiance via the same trace/hit-light path as surfels.
3. Project sample to SH.
4. Accumulate with the same adaptive estimator.
5. Apply de-ringing/windowing to reduce SH banding.

### 7.4 Probe sampling

Forward or transparent shaders:

1. Select highest-detail clipmap containing world position.
2. Either blend two nearest levels near borders or use blue-noise dithered level selection.
3. Reconstruct irradiance from SH using surface normal.
4. Add diffuse/transparent lighting contribution.

---

## 8. Debug Views

Implement these debug modes before optimising:

| Mode | Description |
|---|---|
| `SurfelSpheres` | draw surfel disks/spheres coloured by irradiance |
| `SurfelNormals` | surfel normal visualisation |
| `SurfelAge` | age / persistence heatmap |
| `SurfelVariance` | short-term variance, blue low, red high |
| `SurfelCoverage` | screen-space coverage per tile |
| `SurfelGridCells` | grid cell boundaries / selected cell under cursor |
| `CellOccupancy` | heatmap of surfels per cell |
| `SpawnRecycle` | green spawned, red recycled |
| `RayCounts` | requested / allocated rays per surfel |
| `RayGuide` | selected surfel’s 6x6 guide map |
| `RadialDepth` | radial depth visibility/rejection |
| `IndirectOnly` | final indirect diffuse only |
| `IndirectDifferenceVsSSGI` | compare against current SSGI |
| `ProbeClipmapLevel` | transparent probe sample level, if implemented |
| `Timings` | per-pass GPU timings overlay |

Add hotkeys or ImGui controls to switch modes and edit core settings live.

---

## 9. Performance Rules

1. All GPU buffers must be preallocated at startup or resolution change.
2. No per-frame heap allocation in renderer hot path.
3. Use `glMemoryBarrier` correctly after compute passes writing SSBO/image data.
4. Prefer ping-pong buffers for ray queues and temporary reductions.
5. Tune compute workgroups; start with 64 or 128 threads per group.
6. Keep separate GPU timers for spawn, update, grid build, ray work, filtering, and apply.
7. Track overflows for surfel pool, grid entries, ray queue, and light list.
8. Add quality tiers:

```text
Low:     fewer surfels, uniform grid, no ray guiding, small ray budget
Medium:  non-linear grid, radial depth, ray budget scaling
High:    ray guiding, ray binning, irradiance sharing, software BVH
Ultra:   many-light tree, probes, higher resolution / budgets
```

---

## 10. Milestone Plan for Agent

### Milestone A — Renderer plumbing and data

- Add `SurfelGIManager` and settings.
- Create SSBOs: surfel pool, free list, counters.
- Add transform ID to G-buffer.
- Add transform buffer.
- Add shader reload and debug UI.
- Acceptance: renderer runs with GI enabled but no visible GI yet; debug counters valid.

### Milestone B — Persistent surfel spawning

- Implement 16x16 tile coverage pass.
- Spawn surfels from G-buffer.
- Update surfel world positions from transform buffer.
- Implement free-list pop/push and simple recycling.
- Debug draw surfel disks.
- Acceptance: surfels cover visible opaque geometry, persist when camera moves, and follow moving rigid objects.

### Milestone C — Uniform grid and final gather

- Build uniform grid.
- Insert surfels and compute cell averages.
- Implement final deferred apply from surfel irradiance.
- Initialise surfels with simple ambient/skylight so gather can be seen.
- Acceptance: indirect-only debug view shows stable surfel-based lighting field.

### Milestone D — Ray integration MVP

- Generate cosine hemisphere rays.
- Implement screen-space ray trace.
- Evaluate direct light at hit point, initially unshadowed or shadow-map based.
- Integrate irradiance temporally.
- Acceptance: colour bleeding works in a Cornell-box-style test; moving light causes surfel irradiance to update.

### Milestone E — OpenGL off-screen tracing

- Add surfel-grid fallback trace.
- Add software BVH trace if feasible within project time.
- Use existing surfel indirect at hit for multi-bounce over time.
- Acceptance: off-screen coloured wall continues contributing after it leaves screen, unlike SSGI.

### Milestone F — Artifact mitigation
- Implement radial depth mean / variance.
- Add radial depth test in final apply and neighbour sharing.
- Acceptance: wall light-leak test visibly improves with radial depth enabled.

### Milestone G — Adaptive quality

- Implement variance-driven ray requests.
- Enforce global ray budget.
- Add ray guiding 6x6 map.
- Add ray binning/sorting if time permits.
- Acceptance: stable scene ray cost drops after convergence; new/high-variance surfels get more rays.

### Milestone H — Many lights and probes

- Add reservoir light sampling.
- Add stochastic lightcuts only if reservoir is insufficient.
- Add probe clipmaps for transparent / forward objects as stretch.
- Acceptance: many-light test converges better than brute-force random light sampling under equal ray budget.

### Milestone I — Evaluation and polish

- Add controlled test scenes.
- Add GPU timing capture.
- Add comparison screenshots vs SSGI and direct-only.
- Add documentation and known limitations.
- Acceptance: artefact is stable enough for video demonstration and dissertation evaluation.

---

## 11. Required Test Scenes

Create or configure these scenes inside Nox:

1. **Cornell colour-bleed box**  
   White box, red wall, blue/green wall, one area/light source. Demonstrates bounce colour.

2. **Off-screen bounce test**  
   Bright coloured wall starts visible, then camera turns away. SSGI should lose contribution; surfel GI should persist.

3. **Thin wall leak test**  
   Bright light on one side of a wall, dark room on other side. Radial depth should reduce leaking.

4. **Moving rigid object test**  
   Coloured cube moves near white floor. Surfels should follow transform and update lighting.

5. **Dynamic light test**  
   Light turns on/off or changes colour. Adaptive estimator should react faster than a fixed low-alpha average.

6. **Many light stress test**  
   100–1000 small lights. Compare random light sampling, reservoir sampling, and lightcuts if implemented.

7. **Transparency/probe test**  
   Only if probe clipmaps are implemented. Transparent object samples indirect irradiance without spawning surfels on itself.

---

## 12. Acceptance Criteria

### Functional acceptance

- GI can be toggled on/off at runtime.
- Surfel pool is bounded and does not grow dynamically.
- Surfels spawn from G-buffer coverage gaps.
- Surfels persist across frames and are recycled gradually.
- Surfels follow rigid transforms using transform IDs.
- Grid lookup supports final gather and debug occupancy views.
- Indirect diffuse lighting appears in deferred output.
- Ray integration updates surfel irradiance over time.
- Dynamic lights affect indirect lighting.
- Off-screen indirect contribution persists better than SSGI.
- Radial depth reduces at least one obvious light-leak test.

### Technical acceptance

- Uses OpenGL 4.6 compute shaders and SSBOs.
- Uses explicit memory barriers between dependent compute/render passes.
- All major buffers are named with `glObjectLabel` where debug contexts support it.
- Shader compilation errors include file name and pass name.
- Per-pass timings are recorded.
- Overflow counters are visible in debug UI.
- No synchronous CPU readbacks in normal rendering mode.

### Evaluation acceptance

The implementation must support measurements for:

- total GI GPU time;
- per-pass GPU time;
- live surfel count;
- spawned/recycled surfels per frame;
- requested/allocated rays per frame;
- average rays per live surfel;
- grid occupancy distribution;
- visual comparison vs direct-only and SSGI;
- temporal stability under light/camera movement.

---

## 13. Known Risks and Required Fallbacks

| Risk | Symptom | Required fallback |
|---|---|---|
| Software BVH too slow | ray pass dominates frame | use screen-space + surfel fallback, lower ray budget |
| Uniform grid scale issues | distant surfels overfill cells | implement non-linear grid or multiple grid cascades |
| Light leaking | bright surfels affect hidden surfaces | radial depth test, normal/plane rejection, smaller radius |
| Temporal lag | GI reacts too slowly | increase reactive alpha from variance/difference |
| Noise/blotches | sparse rays visible | irradiance sharing, ray guiding, TAA, higher budget |
| Overspawning | too many surfels on detailed geometry | stronger coverage threshold, normal variance rules, recycle pressure |
| Surfels detach from skinned mesh | lighting floats near characters | dominant bone IDs or reject skinned spawning until implemented |
| GPU memory pressure | allocation failure / slow frame | lower `maxSurfels`, compact guide/radial storage |

---

## 14. Implementation Notes for OpenGL 4.6

### 14.1 Barriers

Use barriers after each write pass before subsequent reads. Typical examples:

```cpp
glDispatchCompute(groupsX, groupsY, groupsZ);
glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
```

Use more specific barriers where possible. Do not rely on implicit ordering between compute and draw calls.

### 14.2 SSBO binding discipline

Define shared binding indices in one header used by C++ and GLSL generation:

```cpp
enum SurfelGIBindings
{
    B_SURFELS = 10,
    B_SURFEL_FREELIST = 11,
    B_SURFEL_COUNTERS = 12,
    B_GRID_HEADERS = 13,
    B_GRID_ENTRIES = 14,
    B_RAY_REQUESTS = 15,
    B_RAYS_IN = 16,
    B_RAYS_SORTED = 17,
    B_RAY_HITS = 18,
    B_RADIAL_DEPTH = 19,
    B_GUIDE_MAP = 20,
    B_GUIDE_SCALE = 21,
    B_TRANSFORMS = 22,
    B_LIGHTS = 23,
    B_BVH_NODES = 24,
    B_BVH_TRIANGLES = 25
};
```

### 14.3 Shader organisation

Recommended shader files:

```text
assets/shaders/surfel_gi/common.glsl
assets/shaders/surfel_gi/update_surfels.comp
assets/shaders/surfel_gi/recycle_surfels.comp
assets/shaders/surfel_gi/clear_grid.comp
assets/shaders/surfel_gi/build_grid.comp
assets/shaders/surfel_gi/cell_average.comp
assets/shaders/surfel_gi/coverage.comp
assets/shaders/surfel_gi/spawn.comp
assets/shaders/surfel_gi/request_rays.comp
assets/shaders/surfel_gi/allocate_rays.comp
assets/shaders/surfel_gi/generate_rays.comp
assets/shaders/surfel_gi/bin_rays.comp
assets/shaders/surfel_gi/prefix_sum.comp
assets/shaders/surfel_gi/scatter_rays.comp
assets/shaders/surfel_gi/trace_rays.comp
assets/shaders/surfel_gi/integrate.comp
assets/shaders/surfel_gi/filter.comp
assets/shaders/surfel_gi/apply_indirect.frag
assets/shaders/surfel_gi/debug_surfels.vert
assets/shaders/surfel_gi/debug_surfels.frag
```

---

## 15. Agent Coding Rules

1. Implement in small passes. Do not write all shaders at once without testing.
2. After every pass, add a debug counter or debug visualisation.
3. Keep the renderer working if GI resources fail to initialise; disable GI and log an error.
4. Use deterministic random seeds for reproducible evaluation. Use blue-noise textures if available; otherwise hash-based RNG.
5. Comment every approximation that differs from EA’s hardware-ray-traced version.
6. Keep all tuning constants in `SurfelGISettings`, not hardcoded inside shaders, unless compile-time constants are required for buffer layout.
7. Add asserts for SSBO sizes and OpenGL limits.
8. Do not remove the existing SSGI path; it is needed for comparison and potential hybridisation.
9. No hidden external dependencies unless explicitly approved.
10. Produce screenshots and timing logs for each milestone.

---

## 16. Future Extensions

Only implement these after the required system is stable:

- hybrid SSGI + surfel GI, using SSGI for fine visible detail and surfels for off-screen persistence;
- ReSTIR-like light reservoir reuse per surfel or per grid cell;
- probe clipmaps with SH for transparents and forward shading;
- hardware ray tracing backend if the renderer later moves to Vulkan/DXR;
- surfel compaction pass to improve cache locality;
- per-material GI controls;
- specular GI approximation using probes or radiance cache;
- editor tooling to inspect surfel lifetime and per-object GI contribution.

---

## 17. Reference Notes

Use these as conceptual anchors while coding:

- **Kajiya rendering equation:** establishes GI as recursive light transport; this implementation approximates the diffuse indirect part with a persistent surface cache.
- **Pfister et al. surfels:** a surfel is a point/surface element with position, normal, colour/material, and radius-like support; Nox uses surfels as irradiance cache elements, not as primary rendering primitives.
- **EA GIBS 2021:** dynamic G-buffer surfel spawning, persistent surfel cache, transform tracking, non-linear grid, radial depth, adaptive integration, ray guiding, ray binning, many-light sampling, and probe clipmaps.
- **Zhang 2023 surfel renderer:** supports the idea of storing surfel attributes in GPU buffers and adapting surfel lifecycle for static/dynamic objects.
- **OpenGL 4.6 / GLSL 460:** compute shaders can write images, SSBOs, and atomic counters; SSBOs are writable and suitable for large GPU-side surfel pools.

---

## 18. Bibliography / Source Trail

- Electronic Arts SEED. “SIGGRAPH 21: Global Illumination Based on Surfels.” EA official site, 2021.  
  https://www.ea.com/seed/news/siggraph21-global-illumination-surfels
- Electronic Arts SEED. “SEED Presentations at SIGGRAPH 2021.” EA official site, 2021.  
  https://www.ea.com/seed/news/seed-siggraph-2021
- Halen, H., Brinck, A., Hayward, K., & Bei, X. “Global Illumination Based on Surfels.” SIGGRAPH Advances in Real-Time Rendering in Games, 2021.
- Electronic Arts. “GIBS Lighting Technology in EA SPORTS College Football 25.” EA Technology, 2024.  
  https://www.ea.com/technology/news/gibs-lighting-ea-sports-college-football-25
- Pfister, H., Zwicker, M., van Baar, J., & Gross, M. “Surfels: Surface Elements as Rendering Primitives.” SIGGRAPH 2000.
- Kajiya, J. T. “The Rendering Equation.” SIGGRAPH 1986.
- Zhang, H. “Design and Implementation of a Global Illumination Rendering System Based on Surfels.” ICIIBMS 2023.
- Khronos Group. “The OpenGL Shading Language, Version 4.60.”
- Khronos OpenGL Wiki. “Shader Storage Buffer Object.”

---

## 19. Final Deliverable Checklist

Before declaring the GI system complete, ensure the repository contains:

- [ ] C++ subsystem classes and renderer integration.
- [ ] GLSL compute/fragment/debug shaders.
- [ ] Configurable `SurfelGISettings` with UI controls.
- [ ] Debug views listed in section 8.
- [ ] Test scenes listed in section 11.
- [ ] GPU timing overlay.
- [ ] Screenshot set: direct-only, SSGI, surfel GI, indirect-only, debug surfels.
- [ ] Performance table at minimum/medium/high settings.
- [ ] Known limitations document.
- [ ] Dissertation-ready technical notes explaining deviations from EA GIBS due to OpenGL/no hardware RT.

