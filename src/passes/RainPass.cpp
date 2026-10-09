#include "RainPass.h"
#include "../PresentTarget.h"
#include "VolumetricFogPass.h"
#include "ExposurePass.h"
#include "../ShaderLoader.h"
#include "../FrameBuffer.h"
#include "../RenderContext.h"
#include "../Camera.h"
#include <algorithm>
#include <iostream>
#include <glm/gtc/type_ptr.hpp>

RainPass::~RainPass()
{
	if (m_shader) glDeleteProgram(m_shader);
	if (m_vbo) glDeleteBuffers(1, &m_vbo);
	if (m_vao) glDeleteVertexArrays(1, &m_vao);
}

bool RainPass::Initialize(RenderContext&)
{
	m_shader = CreateShaderProgram("shaders/rain_vert.glsl", "shaders/rain_frag.glsl");
	if (!m_shader) {
		std::cerr << "[RainPass] Failed to create rain shader" << std::endl;
		return false;
	}
	const float corners[] = { -1.0f, 0.0f, 1.0f, 0.0f, -1.0f, 1.0f, 1.0f, 1.0f };
	glGenVertexArrays(1, &m_vao);
	glGenBuffers(1, &m_vbo);
	glBindVertexArray(m_vao);
	glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
	glBindVertexArray(0);
	return true;
}

void RainPass::Execute(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>&,
	const std::shared_ptr<Camera>& camera,
	const std::shared_ptr<DirectionalLight>&,
	const std::shared_ptr<Skybox>&)
{
	if (ctx.weatherRain <= 0.0f || ctx.rainDropCount <= 0 || !camera || !ctx.hdrFBO) {
		m_havePrev = false;
		return;
	}
	const glm::vec3 camPos = glm::vec3(glm::inverse(ctx.view)[3]);
	const float dt = std::max(ctx.deltaTime, 1e-4f);
	const glm::vec3 camVelocity = m_havePrev ? (camPos - m_prevCameraPos) / dt : glm::vec3(0.0f);
	m_prevCameraPos = camPos;
	m_havePrev = true;

	ctx.hdrFBO->Bind();
	glViewport(0, 0, ctx.width, ctx.height);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_FALSE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // premultiplied
	glDisable(GL_CULL_FACE);

	glUseProgram(m_shader);
	ExposurePass::Bind(ctx);
	const glm::mat4 viewProj = ctx.proj * ctx.view;
	auto set1f = [&](const char* n, float v) { const GLint l = glGetUniformLocation(m_shader, n); if (l >= 0) glUniform1f(l, v); };
	auto set3f = [&](const char* n, const glm::vec3& v) { const GLint l = glGetUniformLocation(m_shader, n); if (l >= 0) glUniform3fv(l, 1, glm::value_ptr(v)); };
	glUniformMatrix4fv(glGetUniformLocation(m_shader, "uViewProj"), 1, GL_FALSE, glm::value_ptr(viewProj));
	glUniformMatrix4fv(glGetUniformLocation(m_shader, "uView"), 1, GL_FALSE, glm::value_ptr(ctx.view));
	set3f("uCameraPos", camPos);
	set3f("uBoxSize", glm::vec3(36.0f, 22.0f, 36.0f));
	set1f("uTime", ctx.timeSeconds);
	set3f("uFallVelocity", glm::vec3(ctx.fogWind.x, -9.0f, ctx.fogWind.z));
	set1f("uStreakTime", std::max(ctx.motionBlurExposureTime, 1.0f / 60.0f));
	set1f("uDropWidth", 0.006f);
	set3f("uCameraVelocity", camVelocity);
	set3f("uFogAlbedo", ctx.fogAlbedo);
	set3f("uAmbient", ctx.fogAmbientRadiance);
	set1f("uOpacity", std::clamp(ctx.weatherRain, 0.0f, 1.0f));
	set1f("uBrightness", ctx.rainBrightness);

	const bool hasVolume = m_fog && ctx.enableVolumetricFog && m_fog->GetScatteringVolume() != 0;
	glUniform1i(glGetUniformLocation(m_shader, "uHasVolume"), hasVolume ? 1 : 0);
	if (hasVolume) {
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_3D, m_fog->GetScatteringVolume());
		glUniform1i(glGetUniformLocation(m_shader, "uScattering"), 0);
		const glm::ivec3 size = m_fog->GetVolumeSize();
		glUniform3i(glGetUniformLocation(m_shader, "uVolumeSize"), size.x, size.y, size.z);
		set1f("uVolumeNear", m_fog->GetVolumeNear());
		set1f("uVolumeFar", m_fog->GetVolumeFar());
	}

	const int drops = static_cast<int>(static_cast<float>(ctx.rainDropCount) * std::clamp(ctx.weatherRain, 0.0f, 1.0f));
	glBindVertexArray(m_vao);
	glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, std::max(drops, 0));
	glBindVertexArray(0);

	glDisable(GL_BLEND);
	glDepthMask(GL_TRUE);
	glEnable(GL_CULL_FACE);
	glBindFramebuffer(GL_FRAMEBUFFER, PresentTarget::Get());
}
