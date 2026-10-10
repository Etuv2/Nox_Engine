#include "VehicleFXPass.h"
#include "../PresentTarget.h"
#include "../VehicleFX.h"
#include "VolumetricFogPass.h"
#include "ExposurePass.h"
#include "../ShaderLoader.h"
#include "../FrameBuffer.h"
#include "../RenderContext.h"
#include "../Camera.h"
#include <algorithm>
#include <cstddef>
#include <iostream>
#include <glm/gtc/type_ptr.hpp>

namespace {
// Corner attribute for a triangle strip: x across [-1,1], y along [-1,1].
const float kCorners[] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
constexpr GLuint kCornerLocation = 0;
constexpr GLuint kInstanceLocation = 1; // three consecutive vec4 attributes: 1, 2, 3
static_assert(sizeof(VehicleFX::RenderParticle) == 3 * sizeof(glm::vec4), "instance layout");
}

VehicleFXPass::~VehicleFXPass()
{
	if (m_shader) glDeleteProgram(m_shader);
	if (m_instanceVbo) glDeleteBuffers(1, &m_instanceVbo);
	if (m_cornerVbo) glDeleteBuffers(1, &m_cornerVbo);
	if (m_vao) glDeleteVertexArrays(1, &m_vao);
}

bool VehicleFXPass::Initialize(RenderContext&)
{
	m_shader = CreateShaderProgram("shaders/vehicle_fx_vert.glsl", "shaders/vehicle_fx_frag.glsl");
	if (!m_shader) {
		std::cerr << "[VehicleFXPass] Failed to create vehicle FX shader" << std::endl;
		return false;
	}
	glGenVertexArrays(1, &m_vao);
	glGenBuffers(1, &m_cornerVbo);
	glGenBuffers(1, &m_instanceVbo);

	glBindVertexArray(m_vao);
	glBindBuffer(GL_ARRAY_BUFFER, m_cornerVbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(kCorners), kCorners, GL_STATIC_DRAW);
	glEnableVertexAttribArray(kCornerLocation);
	glVertexAttribPointer(kCornerLocation, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

	glBindBuffer(GL_ARRAY_BUFFER, m_instanceVbo);
	for (GLuint i = 0; i < 3; ++i) {
		const GLuint loc = kInstanceLocation + i;
		glEnableVertexAttribArray(loc);
		glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, sizeof(VehicleFX::RenderParticle),
			reinterpret_cast<const void*>(i * sizeof(glm::vec4)));
		glVertexAttribDivisor(loc, 1);
	}
	glBindVertexArray(0);
	return true;
}

void VehicleFXPass::Execute(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>&,
	const std::shared_ptr<Camera>& camera,
	const std::shared_ptr<DirectionalLight>&,
	const std::shared_ptr<Skybox>&)
{
	VehicleFX::EnsureTestHarnessTick(ctx.deltaTime);
	const std::vector<VehicleFX::RenderParticle>& particles = VehicleFX::GetRenderParticles();
	if (particles.empty() || !camera || !ctx.hdrFBO || !ctx.gbufferFBO) {
		return;
	}

	// Stream the instance list: orphan the store every frame so the upload never stalls on the GPU.
	const size_t count = particles.size();
	if (count > m_instanceCapacity) {
		m_instanceCapacity = std::max(count, m_instanceCapacity * 2);
	}
	glBindBuffer(GL_ARRAY_BUFFER, m_instanceVbo);
	glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_instanceCapacity * sizeof(VehicleFX::RenderParticle)), nullptr, GL_STREAM_DRAW);
	glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(count * sizeof(VehicleFX::RenderParticle)), particles.data());

	const glm::vec3 camPos = glm::vec3(glm::inverse(ctx.view)[3]);
	const glm::mat4 viewProj = ctx.proj * ctx.view;

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

	auto set1f = [&](const char* n, float v) { const GLint l = glGetUniformLocation(m_shader, n); if (l >= 0) glUniform1f(l, v); };
	auto set3f = [&](const char* n, const glm::vec3& v) { const GLint l = glGetUniformLocation(m_shader, n); if (l >= 0) glUniform3fv(l, 1, glm::value_ptr(v)); };
	auto setM4 = [&](const char* n, const glm::mat4& m) { const GLint l = glGetUniformLocation(m_shader, n); if (l >= 0) glUniformMatrix4fv(l, 1, GL_FALSE, glm::value_ptr(m)); };

	setM4("uViewProj", viewProj);
	setM4("uView", ctx.view);
	setM4("uProj", ctx.proj);
	set3f("uCameraPos", camPos);
	set1f("uStreakTime", std::max(ctx.motionBlurExposureTime, 1.0f / 60.0f));
	set1f("uSoftRange", 0.6f);                         // metres of depth fade at intersections
	set1f("uBrightness", 1.0f);
	set3f("uSmokeTint", glm::vec3(1.0f, 0.84f, 0.66f)); // warm street-light tint on smoke
	set1f("uSmokeGain", 3.0f);
	set3f("uFogAlbedo", ctx.fogAlbedo);
	set3f("uAmbient", ctx.fogAmbientRadiance);
	{
		const GLint l = glGetUniformLocation(m_shader, "uScreenSize");
		if (l >= 0) glUniform2f(l, static_cast<float>(ctx.width), static_cast<float>(ctx.height));
	}

	// Scene depth (G-buffer) for the soft-particle fade.
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
	glUniform1i(glGetUniformLocation(m_shader, "uSceneDepth"), 1);

	// Froxel scattering volume for lamp and headlight light.
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

	glBindVertexArray(m_vao);
	glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(count));
	glBindVertexArray(0);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glDisable(GL_BLEND);
	glDepthMask(GL_TRUE);
	glEnable(GL_CULL_FACE);
	glBindFramebuffer(GL_FRAMEBUFFER, PresentTarget::Get());
}
