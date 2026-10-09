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
}

LightClusterPass::LightClusterPass() = default;

LightClusterPass::~LightClusterPass()
{
	if (m_paramsUBO) glDeleteBuffers(1, &m_paramsUBO);
	if (m_countSSBO) glDeleteBuffers(1, &m_countSSBO);
	if (m_indexSSBO) glDeleteBuffers(1, &m_indexSSBO);
}

bool LightClusterPass::Initialize(RenderContext& context)
{
	m_buildShader = std::make_unique<ComputeShader>();
	if (!m_buildShader->CreateFromFile("shaders/light_cluster_build_comp.glsl")) {
		std::cerr << "[LightClusterPass] Failed to compile light_cluster_build_comp.glsl" << std::endl;
		return false;
	}
	glGenBuffers(1, &m_paramsUBO);
	glBindBuffer(GL_UNIFORM_BUFFER, m_paramsUBO);
	glBufferData(GL_UNIFORM_BUFFER, sizeof(ClusterParamsStd140), nullptr, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
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
	if (!m_countSSBO) glGenBuffers(1, &m_countSSBO);
	if (!m_indexSSBO) glGenBuffers(1, &m_indexSSBO);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_countSSBO);
	glBufferData(GL_SHADER_STORAGE_BUFFER, clusters * sizeof(uint32_t), nullptr, GL_DYNAMIC_COPY);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_indexSSBO);
	glBufferData(GL_SHADER_STORAGE_BUFFER, clusters * kMaxLightsPerCluster * sizeof(uint32_t), nullptr, GL_DYNAMIC_COPY);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
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
	glBindBuffer(GL_UNIFORM_BUFFER, m_paramsUBO);
	glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(params), &params);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
	ctx.clusterParamsUBO = m_paramsUBO;
	ctx.clusterCountSSBO = m_countSSBO;
	ctx.clusterIndexSSBO = m_indexSSBO;

	if (!ctx.enableClusteredLighting || !m_buildShader || !m_buildShader->IsValid()) {
		return;
	}

	const GLuint program = m_buildShader->GetProgramID();
	glUseProgram(program);
	glBindBufferBase(GL_UNIFORM_BUFFER, kParamsBinding, m_paramsUBO);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ctx.lightManager->GetLightDataSSBO());
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kCountBinding, m_countSSBO);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kIndexBinding, m_indexSSBO);
	// The cluster grid uses the unjittered frustum; jitter is sub-pixel and covered by the
	// conservative bounds.
	const glm::vec2 tanHalfFov(1.0f / ctx.proj[0][0], 1.0f / ctx.proj[1][1]);
	glUniformMatrix4fv(glGetUniformLocation(program, "uView"), 1, GL_FALSE, glm::value_ptr(ctx.view));
	glUniform2fv(glGetUniformLocation(program, "uTanHalfFov"), 1, glm::value_ptr(tanHalfFov));
	const unsigned clusters = m_gridX * m_gridY * kSlices;
	glDispatchCompute((clusters + 63u) / 64u, 1, 1);
	glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
}

void LightClusterPass::BindForShading(const RenderContext& ctx)
{
	if (ctx.clusterParamsUBO) glBindBufferBase(GL_UNIFORM_BUFFER, kParamsBinding, ctx.clusterParamsUBO);
	if (ctx.clusterCountSSBO) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kCountBinding, ctx.clusterCountSSBO);
	if (ctx.clusterIndexSSBO) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kIndexBinding, ctx.clusterIndexSSBO);
}
