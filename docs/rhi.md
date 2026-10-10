# NOX RHI (rendering hardware interface)

All GPU access in the engine goes through `rhi::Device` (`src/rhi/RHI.h`). The OpenGL 4.6 backend
(`src/rhi/gl/GLDevice.cpp`) is the only code that calls OpenGL. `tools/CheckNoRawGL.ps1` fails if a
`gl*` call, GL type (`GLuint`, `GLenum`, ...), `GL_*` constant or GL header shows up anywhere else under `src/`.

```cpp
rhi::Device& gpu = rhi::Gpu();          // the engine's device (created by MainWindow)
gpu.BindFramebuffer(fbo);
gpu.SetViewport({ 0, 0, w, h });
gpu.UseProgram(program);
gpu.SetUniform(gpu.GetUniformLocation(program, "uExposure"), 1.0f);
gpu.BindTexture(0, colorTex);
gpu.Draw(rhi::Topology::TriangleStrip, 0, 4);
```

## Model

- **Immediate context.** Resources are created and updated through the device. State is set with
  explicit setters, resources are bound by slot, and draws and dispatches consume the current state.
  An explicit-API backend (Vulkan, D3D12) implements the same calls this way:
  - records them into a command buffer;
  - caches pipelines keyed by the accumulated raster/depth/blend state and program;
  - builds descriptor sets from the bound texture, image, storage and uniform slots;
  - turns `PipelineBarrier` bits into pipeline barriers.
- **Opaque handles.** `TextureHandle`, `BufferHandle`, `ProgramHandle`, `FramebufferHandle`,
  `VertexInputHandle`, `QueryHandle`. `{0}` means none. `kBackBuffer` is the window's framebuffer.
  Never interpret `.id`. It is only printable for logs.
- **Resizable textures.** Textures are resizable by default (`TextureDesc::immutable = false`):
  - Creation allocates `mipLevels` levels; `GenerateMipmaps` adds the chain.
  - `ResizeTexture` keeps the handle, so framebuffer attachments stay valid.
  - `immutable = true` gives fixed storage, and sampling clamps to the allocated levels.
- **Shaders** are GLSL 4.6. Loose uniforms are set by location on the bound program. A Vulkan backend
  maps them to a default uniform block via reflection (glslang relaxed Vulkan rules).
- **Window, context, present and Dear ImGui rendering** belong to the backend:
  `rhi::PrepareWindow`, `rhi::CreateDevice`, `Device::Present`, and `Device::ImGui*`.

## Engine wrappers on top of the RHI

| Wrapper | Purpose |
| --- | --- |
| `Texture` (`Texture::Builder`, `TextureFactory`) | Owned texture plus metadata; `ID()` returns the handle |
| `FrameBuffer` | Colour textures plus a depth texture, depth array or depth target; `GetFBO()`, `GetColorAttachment()` |
| `GpuBuffer` | Owned buffer (resizable or fixed storage, map, copy, `BindBase` by role) |
| `ComputeShader` | Compute program with dispatch, image/storage binding and uniform helpers |
| `ShaderLoader.h` | `CreateShaderProgram`, `CreateComputeShader`, `CreateShaderProgramWithGeometry`; return `rhi::ProgramHandle` |
| `ScreenQuad`, `MDIBatch`, `DefaultTextures`, `TextureArrayBuilder` | Helpers |

## Porting conventions (GL to RHI)

Ports are one-to-one: same operations, same order, same values. The bit-exact capture goldens must not change.

| OpenGL | RHI |
| --- | --- |
| `GLuint` texture / buffer / program / fbo / vao | `rhi::TextureHandle` / `BufferHandle` / `ProgramHandle` / `FramebufferHandle` / `VertexInputHandle` |
| `GLint` uniform location | `rhi::UniformLocation` |
| `GLenum` internal format | `rhi::Format` (`GL_RGBA16F` becomes `Format::RGBA16F`, `GL_DEPTH_COMPONENT24` becomes `Format::D24`, `GL_DEPTH24_STENCIL8` becomes `Format::D24S8`, `GL_SRGB8_ALPHA8` becomes `Format::SRGB8_A8`, `GL_R11F_G11F_B10F` becomes `Format::R11G11B10F`) |
| pixel format/type of uploads | `rhi::PixelFormat` / `rhi::PixelType` |
| `glGenTextures` + `glTexImage*` | `CreateTexture(desc)` (resizable) + `UpdateTexture` for data |
| `glTexStorage*` | `CreateTexture` with `immutable = true` |
| `glTexSubImage*` | `UpdateTexture(tex, region, fmt, type, data, alignment)` (cube face = layer `z`) |
| re-`glTexImage*` with a new size | `ResizeTexture` |
| `glTexParameteri` | `SetTextureFilter / MinFilter / MagFilter / Wrap / WrapAxis / BorderColor / Compare / MipRange / Anisotropy` |
| `glActiveTexture(GL_TEXTURE0+u)` + `glBindTexture(t, id)` / `glBindTextureUnit` | `BindTexture(u, tex)` (invalid handle unbinds) |
| `glBindImageTexture` | `BindImage(unit, tex, level, layered, layer, access, format)` |
| `glBindBufferBase/Range(GL_SHADER_STORAGE_BUFFER / GL_UNIFORM_BUFFER / GL_ATOMIC_COUNTER_BUFFER, ...)` | `BindStorageBuffer` / `BindUniformBuffer` / `BindAtomicCounterBuffer` |
| `glGenBuffers` + `glBufferData` | `CreateBuffer`; later `glBufferData` on the same buffer becomes `ReallocateBuffer` |
| `glBufferStorage` | `CreateBuffer` with `immutable = true` and `storage` flags |
| `glBufferSubData` / `glGetBufferSubData` / `glCopyBufferSubData` / `glClearBufferData` | `UpdateBuffer` / `ReadBuffer` / `CopyBuffer` / `ClearBuffer` |
| `glMapBuffer(Range)` / `glUnmapBuffer` / `glFlushMappedBufferRange` | `MapBuffer` / `UnmapBuffer` / `FlushMappedRange` |
| VAO + `glVertexAttrib(I)Pointer` + `glVertexAttribDivisor` + element buffer | `CreateVertexInput(desc)`; `BindVertexInput(handle)` (invalid = attribute-less draws) |
| `glDrawArrays(Instanced/BaseInstance)` | `Draw(topology, first, count, instances, baseInstance)` |
| `glDrawElements(...)` | `DrawIndexed(topology, count, indexType, byteOffset, instances, baseVertex, baseInstance)` |
| `glMultiDrawElementsIndirect` / `glDrawElementsIndirect` | `DrawIndexedIndirect(topology, indexType, argsBuffer, byteOffset, drawCount, stride)` |
| `glDispatchCompute(Indirect)` | `Dispatch` / `DispatchIndirect` |
| `glMemoryBarrier(bits)` | `PipelineBarrier(rhi::Barrier...)` (`GL_SHADER_IMAGE_ACCESS_BARRIER_BIT` becomes `BarrierImageAccess`, `GL_SHADER_STORAGE_BARRIER_BIT` becomes `BarrierStorage`, `GL_TEXTURE_FETCH_BARRIER_BIT` becomes `BarrierTextureFetch`, `GL_ALL_BARRIER_BITS` becomes `BarrierAll`) |
| `glGenFramebuffers` + `glFramebufferTexture*` | `CreateFramebuffer(desc)`; re-targeting becomes `SetAttachment(fbo, point, {tex, level, layer})` (cube face = layer) |
| `glDrawBuffers` / `glDrawBuffer(GL_NONE)` / `glReadBuffer` | `SetDrawBuffers(fbo, list, n)` / `SetDrawBuffers(fbo, nullptr, 0)` / `SetReadBuffer` |
| `glBindFramebuffer(GL_FRAMEBUFFER / DRAW / READ, x)` | `BindFramebuffer` / `BindDrawFramebuffer` / `BindReadFramebuffer`; former `0` becomes `PresentTarget::Get()` or `rhi::kBackBuffer` exactly as the old code meant |
| `glClearColor` + `glClear` | `SetClearColor` + `Clear(rhi::ClearColor \| rhi::ClearDepth \| rhi::ClearStencil)` |
| `glClearBuffer*` / `glClearTexImage` | `ClearColorAttachment` / `ClearDepthStencil` / `ClearTexture` |
| `glBlitFramebuffer` / `glReadPixels` / `glGetTexImage` / `glCopyImageSubData` | `BlitFramebuffer` / `ReadPixels` / `ReadTexture(Region)` / `CopyTexture` |
| `glViewport` / `glScissor` / `glEnable(GL_SCISSOR_TEST)` | `SetViewport` / `SetScissorRect` / `SetScissorEnabled` |
| `glEnable/glDisable(GL_DEPTH_TEST)`, `glDepthMask`, `glDepthFunc` | `SetDepthTest`, `SetDepthWrite`, `SetDepthFunc` |
| `GL_CULL_FACE`, `glCullFace`, `glFrontFace`, `glPolygonMode`, `glLineWidth` | `SetCullEnabled`, `SetCullFace`, `SetFrontFace`, `SetPolygonMode`, `SetLineWidth` |
| `GL_POLYGON_OFFSET_FILL`, `glPolygonOffset` | `SetPolygonOffsetEnabled`, `SetPolygonOffsetFactors` |
| `GL_BLEND`, `glBlendFunc(Separate)(i)`, `glBlendEquation(Separate)`, `glColorMask(i)`, `glEnablei(GL_BLEND, i)` | `SetBlendEnabled`, `SetBlendFunc(Separate)`, `SetBlendEquation(Separate)`, `SetColorMask`, `SetBlendEnabled(rt, ...)` |
| `GL_PROGRAM_POINT_SIZE` / `GL_DEPTH_CLAMP` / `GL_FRAMEBUFFER_SRGB` / `GL_TEXTURE_CUBE_MAP_SEAMLESS` / `GL_STENCIL_TEST` | `SetFeature(rhi::Feature::...)` |
| `glGet*` save/restore | `CaptureState()` / `RestoreState()`, `GetViewport`, `CurrentDrawFramebuffer`, `CurrentProgram`, `IsDepthTestEnabled`, `BoundTexture` |
| `glUseProgram` / `glGetUniformLocation` / `glUniform*` | `UseProgram` / `GetUniformLocation` / `SetUniform(loc, value)` (`glm` types) / `SetUniformArray` |
| `glGenQueries` / `glQueryCounter` / `glBeginQuery` / `glGetQueryObject*` | `CreateQuery(type)` / `WriteTimestamp` / `BeginQuery`,`EndQuery` / `GetQueryResult`, `IsQueryResultAvailable` |
| `glObjectLabel` / `glGetError` | `SetDebugName` / `PollError()` |
| `GLEW_*` capability checks | `gpu.Limits()` / `gpu.Info()` |
| `ImGui::Image((ImTextureID)(intptr_t)texId, ...)` | `ImGui::Image(gpu.ImGuiTextureId(tex), ...)` |
