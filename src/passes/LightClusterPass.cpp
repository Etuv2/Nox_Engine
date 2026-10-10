#include "LightClusterPass.h"
#include "../ComputeShader.h"
#include "../RenderContext.h"
#include "../LightManager.h"
#include "../Camera.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <glm/gtc/type_ptr.hpp>

namespace {
	struct ClusterParamsStd140 {
		uint32_t grid[4];
		float z[4];
		uint32_t lights[4];
		float screen[4];
	};

	// Buffer-data semantics: allocates the buffer on first use, then reallocates the same handle.
	void SetBufferData(rhi::BufferHandle& buffer, size_t size, rhi::BufferUsage usage) {
		rhi::Device& gpu = rhi::Gpu();
		if (buffer) {
			gpu.ReallocateBuffer(buffer, size, nullptr, usage);
		}
		else {
			rhi::BufferDesc desc;
			desc.size = size;
			desc.usage = usage;
			buffer = gpu.CreateBuffer(desc);
		}
	}
}

LightClusterPass::LightClusterPass() = default;

LightClusterPass::~LightClusterPass()
{
	if (m_paramsUBO && rhi::HasGpu()) rhi::Gpu().DestroyBuffer(m_paramsUBO);
	if (m_countSSBO && rhi::HasGpu()) rhi::Gpu().DestroyBuffer(m_countSSBO);
	if (m_indexSSBO && rhi::HasGpu()) rhi::Gpu().DestroyBuffer(m_indexSSBO);
}

bool LightClusterPass::Initialize(RenderContext& context)
{
	m_buildShader = std::make_unique<ComputeShader>();
	if (!m_buildShader->CreateFromFile("shaders/light_cluster_build_comp.glsl")) {
		std::cerr << "[LightClusterPass] Failed to compile light_cluster_build_comp.glsl" << std::endl;
		return false;
	}
	SetBufferData(m_paramsUBO, sizeof(ClusterParamsStd140), rhi::BufferUsage::DynamicDraw);
	AllocateBuffers(context.width, context.height);
	context.clusterParamsUBO = m_paramsUBO;
	context.clusterCountSSBO = m_countSSBO;
	context.clusterIndexSSBO = m_indexSSBO;
	return true;
}

void LightClusterPass::AllocateBuffers(int width, int height)
{
	m_gridX = (static_cast<unsigned>(std::max(width, 1)) + kTileSize - 1u) / kTileSize;
	m_gridY = (static_cast<unsigned>(std::max(height, 1)) + kTileSize - 1u) / kTileSize;
	const size_t clusters = static_cast<size_t>(m_gridX) * m_gridY * kSlices;
	SetBufferData(m_countSSBO, clusters * sizeof(uint32_t), rhi::BufferUsage::DynamicCopy);
	SetBufferData(m_indexSSBO, clusters * kMaxLightsPerCluster * sizeof(uint32_t), rhi::BufferUsage::DynamicCopy);
}

void LightClusterPass::Resize(RenderContext& context, int newWidth, int newHeight)
{
	AllocateBuffers(newWidth, newHeight);
	context.clusterCountSSBO = m_countSSBO;
	context.clusterIndexSSBO = m_indexSSBO;
}

void LightClusterPass::Execute(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>&,
	const std::shared_ptr<Camera>& camera,
	const std::shared_ptr<DirectionalLight>&,
	const std::shared_ptr<Skybox>&)
{
	if (!ctx.lightManager || !camera) {
		return;
	}
	const unsigned totalLights = static_cast<unsigned>(std::max(ctx.lightManager->GetActiveLightCount(), 0));
	const unsigned directional = static_cast<unsigned>(std::max(ctx.lightManager->GetActiveDirectionalLightCount(), 0));
	const float nearPlane = std::max(camera->GetCameraNearPlane(), 0.01f);
	const float farPlane = std::max(camera->GetCameraFarPlane(), nearPlane * 2.0f);
	const float logRatio = std::log(farPlane / nearPlane);

	ClusterParamsStd140 params{};
	params.grid[0] = m_gridX;
	params.grid[1] = m_gridY;
	params.grid[2] = kSlices;
	params.grid[3] = kTileSize;
	params.z[0] = nearPlane;
	params.z[1] = farPlane;
	params.z[2] = static_cast<float>(kSlices) / logRatio;
	params.z[3] = static_cast<float>(kSlices) * std::log(nearPlane) / logRatio;
	params.lights[0] = totalLights;
	params.lights[1] = directional;
	params.lights[2] = ctx.enableClusteredLighting ? 1u : 0u;
	params.screen[0] = static_cast<float>(ctx.width);
	params.screen[1] = static_cast<float>(ctx.height);
	rhi::Gpu().UpdateBuffer(m_paramsUBO, 0, sizeof(params), &params);
	ctx.clusterParamsUBO = m_paramsUBO;
	ctx.clusterCountSSBO = m_countSSBO;
	ctx.clusterIndexSSBO = m_indexSSBO;

	if (!ctx.enableClusteredLighting || !m_buildShader || !m_buildShader->IsValid()) {
		return;
	}

	rhi::Device& gpu = rhi::Gpu();
	const rhi::ProgramHandle program = m_buildShader->GetProgramID();
	gpu.UseProgram(program);
	gpu.BindUniformBuffer(kParamsBinding, m_paramsUBO);
	gpu.BindStorageBuffer(0, ctx.lightManager->GetLightDataSSBO());
	gpu.BindStorageBuffer(kCountBinding, m_countSSBO);
	gpu.BindStorageBuffer(kIndexBinding, m_indexSSBO);
	// The cluster grid uses the unjittered frustum; jitter is sub-pixel and covered by the
	// conservative bounds.
	const glm::vec2 tanHalfFov(1.0f / ctx.proj[0][0], 1.0f / ctx.proj[1][1]);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uView"), ctx.view);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uTanHalfFov"), tanHalfFov);
	const unsigned clusters = m_gridX * m_gridY * kSlices;
	gpu.Dispatch((clusters + 63u) / 64u, 1, 1);
	gpu.PipelineBarrier(rhi::BarrierStorage);
}

void LightClusterPass::BindForShading(const RenderContext& ctx)
{
	rhi::Device& gpu = rhi::Gpu();
	if (ctx.clusterParamsUBO) gpu.BindUniformBuffer(kParamsBinding, ctx.clusterParamsUBO);
	if (ctx.clusterCountSSBO) gpu.BindStorageBuffer(kCountBinding, ctx.clusterCountSSBO);
	if (ctx.clusterIndexSSBO) gpu.BindStorageBuffer(kIndexBinding, ctx.clusterIndexSSBO);
}
