#include "RTPass.h"
#include "../RenderContext.h"
#include "../FrameBuffer.h"
#include "../ShaderLoader.h"
#include "../SceneGraph.h"
#include "../Camera.h"
#include "../ComputeShader.h"


RTPass::~RTPass()
{
}

bool RTPass::Initialize(RenderContext& context)
{
	std::cout << "[RTPass] Initializing Ray Tracing Pass..." << std::endl;
	// Create compute shader for ray tracing
	m_rtShader = std::make_unique<ComputeShader>();
	if (!m_rtShader->CreateFromFile("shaders/rt_bidirectional_comp.glsl")) {
		std::cerr << "[RTPass] Failed to compile rt_bidirectional_comp.glsl" << std::endl;
		return false;
	}


	// Allocate render target texture
	m_rtTexture = Texture::Builder::Texture2D(context.width, context.height, GL_RGBA16F)
		.Format(GL_RGBA).DataType(GL_HALF_FLOAT)
		.FilterMode(GL_LINEAR, GL_LINEAR)
		.WrapMode(GL_CLAMP_TO_EDGE)
		.Build();


	// Create framebuffer to hold ray traced output
	m_rtFBO = std::make_unique<FrameBuffer>(context.width, context.height);
}

void RTPass::Resize(RenderContext& context, int newWidth, int newHeight)
{
	if (newWidth == m_w && newHeight == m_h) {
		return;
	}
	m_w = newWidth;
	m_h = newHeight;
	m_renderResolutionScale = context.rtResolutionScale;

	// Resize ray traced output texture
	m_rtTexture->Resize(newWidth, newHeight);
	// Resize framebuffer
	m_rtFBO->Resize(newWidth, newHeight);
}
