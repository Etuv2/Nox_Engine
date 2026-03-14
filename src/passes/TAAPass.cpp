#include "TAAPass.h"
#include "../ShaderLoader.h"
#include "../FrameBuffer.h"
#include "../ScreenQuad.h"
#include "../SceneGraph.h"
#include "../Camera.h"
#include "../RenderContext.h"
#include <glm/gtc/type_ptr.hpp>
#include <iostream>

#pragma warning(disable: 4996)  // Suppress deprecated function warnings for SceneGraph legacy API

TAAPass::TAAPass() {}

TAAPass::~TAAPass() {
	if (m_velocityShader) glDeleteProgram(m_velocityShader);
	if (m_resolveShader) glDeleteProgram(m_resolveShader);
}

bool TAAPass::Initialize(RenderContext& context) {
	m_velocityShader = CreateShaderProgram("shaders/velocity_vert.glsl",
		"shaders/velocity_frag.glsl");
	m_resolveShader = CreateShaderProgram("shaders/fullscreen_vert.glsl",
		"shaders/taa_resolve.glsl");

	if (!m_velocityShader || !m_resolveShader) {
		std::cerr << "[TAAPass] Failed to create shaders.\n";
		return false;
	}

	// Create framebuffers
	m_velocityFBO = std::make_unique<FrameBuffer>(
		context.width, context.height,
		std::vector<GLenum>{ GL_RG16F },
		true, false
	);

	m_currentFBO = std::make_unique<FrameBuffer>(
		context.width, context.height,
		std::vector<GLenum>{ GL_RGB16F },
		false, false
	);

	m_historyFBO = std::make_unique<FrameBuffer>(
		context.width, context.height,
		std::vector<GLenum>{ GL_RGB16F },
		false, false
	);

	if (!m_velocityFBO->IsComplete() || !m_currentFBO->IsComplete() || !m_historyFBO->IsComplete()) {
		std::cerr << "[TAAPass] TAA framebuffers not complete!\n";
		return false;
	}

	// Expose velocity texture in context for other passes (e.g., SSGI)
	context.velocityTex = m_velocityFBO->GetColorAttachment(0);

	m_velocityUniforms.view = glGetUniformLocation(m_velocityShader, "view");
	m_velocityUniforms.projection = glGetUniformLocation(m_velocityShader, "projection");
	m_velocityUniforms.prevView = glGetUniformLocation(m_velocityShader, "prevView");
	m_velocityUniforms.prevProjection = glGetUniformLocation(m_velocityShader, "prevProjection");
	m_velocityUniforms.jitter = glGetUniformLocation(m_velocityShader, "jitter");
	m_velocityUniforms.prevJitter = glGetUniformLocation(m_velocityShader, "prevJitter");
	m_velocityUniforms.screenSize = glGetUniformLocation(m_velocityShader, "screenSize");

	m_resolveUniforms.currentFrame = glGetUniformLocation(m_resolveShader, "currentFrame");
	m_resolveUniforms.historyFrame = glGetUniformLocation(m_resolveShader, "historyFrame");
	m_resolveUniforms.velocityBuffer = glGetUniformLocation(m_resolveShader, "velocityBuffer");
	m_resolveUniforms.depthBuffer = glGetUniformLocation(m_resolveShader, "depthBuffer");
	m_resolveUniforms.gNormal = glGetUniformLocation(m_resolveShader, "gNormal");
	m_resolveUniforms.blendFactor = glGetUniformLocation(m_resolveShader, "blendFactor");
	m_resolveUniforms.varianceThreshold = glGetUniformLocation(m_resolveShader, "varianceThreshold");
	m_resolveUniforms.lumaWeight = glGetUniformLocation(m_resolveShader, "lumaWeight");
	m_resolveUniforms.useYCoCg = glGetUniformLocation(m_resolveShader, "useYCoCg");
	m_resolveUniforms.historyValid = glGetUniformLocation(m_resolveShader, "historyValid");
	m_resolveUniforms.screenSize = glGetUniformLocation(m_resolveShader, "screenSize");
	m_resolveUniforms.jitter = glGetUniformLocation(m_resolveShader, "jitter");
	m_resolveUniforms.depthThreshold = glGetUniformLocation(m_resolveShader, "depthThreshold");
	m_resolveUniforms.normalThreshold = glGetUniformLocation(m_resolveShader, "normalThreshold");
	m_resolveUniforms.edgeThreshold = glGetUniformLocation(m_resolveShader, "edgeThreshold");
	m_resolveUniforms.reactiveMaskStrength = glGetUniformLocation(m_resolveShader, "reactiveMaskStrength");

	if constexpr (VerboseLogging) {
		if (m_runtimeVerboseLogging) {
			std::cout << "[TAAPass] Initialized successfully.\n";
		}
	}
	return true;
}

void TAAPass::Resize(RenderContext& context, int newWidth, int newHeight) {
	if (m_velocityFBO) m_velocityFBO->Resize(newWidth, newHeight);
	if (m_currentFBO) m_currentFBO->Resize(newWidth, newHeight);
	if (m_historyFBO) m_historyFBO->Resize(newWidth, newHeight);
	context.velocityTex = m_velocityFBO ? m_velocityFBO->GetColorAttachment(0) : 0;
	// Invalidate history on resize
	m_historyValid = false;
}

glm::vec2 TAAPass::GetJitter(int frameIndex, int pattern) {
	if (pattern == 0) {
		// Halton sequence - properly normalized to pixel space
		auto halton = [](int index, int base) -> float {
			float f = 1.0f;
			float r = 0.0f;
			while (index > 0) {
				f = f / base;
				r = r + f * (index % base);
				index = index / base;
			}
			return r;
			};

		// Generate in [-0.5, 0.5] range, then normalize to pixel space
		float x = (halton((frameIndex % 16) + 1, 2) - 0.5f);
		float y = (halton((frameIndex % 16) + 1, 3) - 0.5f);

		// Return in normalized screen space (not pixel space)
		// The shader will apply this as an offset in projection space
		return glm::vec2(x, y);
	}
	else {
		// Hammersley 8-sample pattern
		const glm::vec2 samples[8] = {
			glm::vec2(0.000000f, 0.000000f),
			glm::vec2(0.500000f, 0.333333f),
			glm::vec2(0.250000f, 0.666667f),
			glm::vec2(0.750000f, 0.111111f),
			glm::vec2(0.125000f, 0.444444f),
			glm::vec2(0.625000f, 0.777778f),
			glm::vec2(0.375000f, 0.222222f),
			glm::vec2(0.875000f, 0.555556f)
		};
		return samples[frameIndex % 8] - glm::vec2(0.5f);
	}
}

void TAAPass::Execute(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera,
	const std::shared_ptr<DirectionalLight>& dirLight,
	const std::shared_ptr<Skybox>& skybox) {
	if (!ctx.enableTAA) {
		// TAA disabled - just copy HDR to current
		m_currentFBO->Bind();
		glViewport(0, 0, ctx.width, ctx.height);
		glClear(GL_COLOR_BUFFER_BIT);

		glUseProgram(0); // Use a simple blit or copy shader if available
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, ctx.hdrFBO->GetColorAttachment(0));

		ctx.screenQuad->Render();
		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		// Still expose the FBO for SSGI
		ctx.taaFBO = m_currentFBO.get();
		return;
	}

	// Update jitter
	m_prevJitter = m_jitter;
	m_jitter = GetJitter(m_frameIndex, ctx.taaJitterPattern);
	m_frameIndex++;

	// Render velocity buffer
	RenderVelocity(ctx, sceneGraph, camera);

	// Resolve TAA
	ResolveTemporalAntiAliasing(ctx);

	// Swap current and history
	m_currentFBO.swap(m_historyFBO);
	m_historyValid = true;

	// Expose current FBO to context for SSGI to use
	ctx.taaFBO = m_currentFBO.get();
}

void TAAPass::RenderVelocity(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera) {
	if (!sceneGraph || !camera) return;

	m_velocityFBO->Bind();
	glViewport(0, 0, ctx.width, ctx.height);
	glEnable(GL_DEPTH_TEST);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glUseProgram(m_velocityShader);

	// Upload current and previous matrices
	if (m_velocityUniforms.view >= 0) glUniformMatrix4fv(m_velocityUniforms.view,
		1, GL_FALSE, glm::value_ptr(ctx.view));
	if (m_velocityUniforms.projection >= 0) glUniformMatrix4fv(m_velocityUniforms.projection,
		1, GL_FALSE, glm::value_ptr(ctx.proj));

	// Previous matrices (stored in context)
	if (m_velocityUniforms.prevView >= 0) glUniformMatrix4fv(m_velocityUniforms.prevView,
		1, GL_FALSE, glm::value_ptr(ctx.prevView));
	if (m_velocityUniforms.prevProjection >= 0) glUniformMatrix4fv(m_velocityUniforms.prevProjection,
		1, GL_FALSE, glm::value_ptr(ctx.prevProj));

	// Upload jitter
	if (m_velocityUniforms.jitter >= 0) glUniform2fv(m_velocityUniforms.jitter,
		1, glm::value_ptr(m_jitter));
	if (m_velocityUniforms.prevJitter >= 0) glUniform2fv(m_velocityUniforms.prevJitter,
		1, glm::value_ptr(m_prevJitter));

	if (m_velocityUniforms.screenSize >= 0) glUniform2f(m_velocityUniforms.screenSize,
		static_cast<float>(ctx.width), static_cast<float>(ctx.height));

	// Render scene for motion vectors
	sceneGraph->DrawVelocity(m_velocityShader);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	// Update context velocity texture each frame
	ctx.velocityTex = m_velocityFBO->GetColorAttachment(0);
}

void TAAPass::ResolveTemporalAntiAliasing(RenderContext& ctx) {
	m_currentFBO->Bind();
	glViewport(0, 0, ctx.width, ctx.height);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(m_resolveShader);

	// Bind current frame (HDR output)
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, ctx.hdrFBO->GetColorAttachment(0));
	if (m_resolveUniforms.currentFrame >= 0) glUniform1i(m_resolveUniforms.currentFrame, 0);

	// Bind history
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, m_historyFBO->GetColorAttachment(0));
	if (m_resolveUniforms.historyFrame >= 0) glUniform1i(m_resolveUniforms.historyFrame, 1);

	// Bind velocity
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, m_velocityFBO->GetColorAttachment(0));
	if (m_resolveUniforms.velocityBuffer >= 0) glUniform1i(m_resolveUniforms.velocityBuffer, 2);

	// Bind depth for disocclusion detection
	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
	if (m_resolveUniforms.depthBuffer >= 0) glUniform1i(m_resolveUniforms.depthBuffer, 3);

	// Bind normal for rejection
	glActiveTexture(GL_TEXTURE4);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0));
	if (m_resolveUniforms.gNormal >= 0) glUniform1i(m_resolveUniforms.gNormal, 4);

	// Upload TAA parameters
	if (m_resolveUniforms.blendFactor >= 0) glUniform1f(m_resolveUniforms.blendFactor, ctx.taaBlendFactor);
	if (m_resolveUniforms.varianceThreshold >= 0) glUniform1f(m_resolveUniforms.varianceThreshold, ctx.taaVarianceThreshold);
	if (m_resolveUniforms.lumaWeight >= 0) glUniform1f(m_resolveUniforms.lumaWeight, ctx.taaLumaWeight);
	if (m_resolveUniforms.useYCoCg >= 0) glUniform1i(m_resolveUniforms.useYCoCg, ctx.taaUseYCoCg ? 1 : 0);
	if (m_resolveUniforms.historyValid >= 0) glUniform1i(m_resolveUniforms.historyValid, m_historyValid ? 1 : 0);
	if (m_resolveUniforms.screenSize >= 0) glUniform2f(m_resolveUniforms.screenSize,
		static_cast<float>(ctx.width), static_cast<float>(ctx.height));
	if (m_resolveUniforms.jitter >= 0) glUniform2fv(m_resolveUniforms.jitter,
		1, glm::value_ptr(m_jitter));

	// Enhanced quality parameters
	if (m_resolveUniforms.depthThreshold >= 0) glUniform1f(m_resolveUniforms.depthThreshold, ctx.taaDepthThreshold);
	if (m_resolveUniforms.normalThreshold >= 0) glUniform1f(m_resolveUniforms.normalThreshold, ctx.taaNormalThreshold);
	if (m_resolveUniforms.edgeThreshold >= 0) glUniform1f(m_resolveUniforms.edgeThreshold, ctx.taaEdgeThreshold);
	if (m_resolveUniforms.reactiveMaskStrength >= 0) glUniform1f(m_resolveUniforms.reactiveMaskStrength, ctx.taaReactiveMaskStrength);

	ctx.screenQuad->Render();

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
